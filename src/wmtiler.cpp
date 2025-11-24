#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <iterator>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

struct DesktopLayout {
    int marginLeft = 0;
    int marginRight = 0;
    int marginTop = 0;
    int marginBottom = 0;
    int gap = 0;
};

struct Config {
    bool daemon = false;
    DesktopLayout defaults{};
    DesktopLayout tiledDefaults{};
    bool hasTiledDefaults = false;
    std::map<unsigned long, DesktopLayout> perDesktop;
    std::set<unsigned long> tiledDesktops;
    std::chrono::milliseconds debounce{200};
    std::string commandSocket = "/tmp/wmtiler.sock";
    bool sendCommand = false;
    std::string commandToSend;
};

Display* g_display = nullptr;
std::map<unsigned long, std::vector<Window>> g_windowOrder;
std::atomic<bool> g_interrupted{false};

enum class CommandType { MoveLeft, MoveRight };

struct PendingCommand {
    CommandType type;
};

std::mutex g_commandMutex;
std::deque<PendingCommand> g_commandQueue;
std::thread g_commandThread;
int g_commandServerFd = -1;
std::string g_commandSocketPath;

void pushCommand(CommandType type) {
    std::lock_guard<std::mutex> lock(g_commandMutex);
    g_commandQueue.push_back(PendingCommand{type});
}

std::vector<PendingCommand> pullCommands() {
    std::lock_guard<std::mutex> lock(g_commandMutex);
    std::vector<PendingCommand> out(g_commandQueue.begin(), g_commandQueue.end());
    g_commandQueue.clear();
    return out;
}

DesktopLayout layoutForDesktop(const Config& cfg, unsigned long desktop);

std::string trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

std::optional<CommandType> parseCommandString(const std::string& text) {
    if (text == "move-left") {
        return CommandType::MoveLeft;
    }
    if (text == "move-right") {
        return CommandType::MoveRight;
    }
    return std::nullopt;
}

int createCommandServer(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        close(fd);
        unlink(path.c_str());
        return -1;
    }
    return fd;
}

void commandListenerLoop() {
    while (!g_interrupted) {
        int client = accept(g_commandServerFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (g_interrupted) {
                break;
            }
            continue;
        }
        char buffer[128];
        ssize_t len = read(client, buffer, sizeof(buffer) - 1);
        if (len > 0) {
            buffer[len] = '\0';
            auto cmd = trim(buffer);
            if (auto parsed = parseCommandString(cmd)) {
                pushCommand(*parsed);
            }
        }
        close(client);
    }
}

void stopCommandServer() {
    if (g_commandServerFd >= 0) {
        close(g_commandServerFd);
        g_commandServerFd = -1;
    }
    if (!g_commandSocketPath.empty()) {
        unlink(g_commandSocketPath.c_str());
    }
    if (g_commandThread.joinable()) {
        g_commandThread.join();
    }
}

bool sendIpcCommand(const Config& cfg) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Failed to create command socket\n";
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (cfg.commandSocket.size() >= sizeof(addr.sun_path)) {
        std::cerr << "Command socket path is too long\n";
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, cfg.commandSocket.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to " << cfg.commandSocket
                  << ". Make sure the daemon is running.\n";
        close(fd);
        return false;
    }
    std::string payload = cfg.commandToSend;
    payload.push_back('\n');
    ssize_t written = write(fd, payload.data(), payload.size());
    close(fd);
    if (written != static_cast<ssize_t>(payload.size())) {
        std::cerr << "Failed to send the full command payload\n";
        return false;
    }
    return true;
}

[[noreturn]] void fail(const std::string& msg) {
    throw std::runtime_error(msg);
}

class AtomCache {
public:
    explicit AtomCache(Display* dpy) : display_(dpy) {}

    Atom get(const char* name) {
        auto it = cache_.find(name);
        if (it != cache_.end()) {
            return it->second;
        }
        Atom atom = XInternAtom(display_, name, False);
        cache_.emplace(name, atom);
        return atom;
    }

private:
    Display* display_;
    std::map<std::string, Atom> cache_;
};

struct MotifHints {
    unsigned long flags = 2;       // MWM_HINTS_DECORATIONS
    unsigned long functions = 0;
    unsigned long decorations = 0; // remove all decorations
    long inputMode = 0;
    unsigned long status = 0;
};

template <typename T>
class XOwnedProperty {
public:
    XOwnedProperty() = default;
    XOwnedProperty(unsigned char* data, unsigned long length)
        : data_(reinterpret_cast<T*>(data)), size_(length) {}
    ~XOwnedProperty() { reset(); }

    XOwnedProperty(const XOwnedProperty&) = delete;
    XOwnedProperty& operator=(const XOwnedProperty&) = delete;

    XOwnedProperty(XOwnedProperty&& other) noexcept { swap(other); }
    XOwnedProperty& operator=(XOwnedProperty&& other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }

    T* data() const { return data_; }
    unsigned long size() const { return size_; }
    explicit operator bool() const { return data_ != nullptr; }

private:
    void reset() {
        if (data_) {
            XFree(data_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    void swap(XOwnedProperty& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
    }

    T* data_ = nullptr;
    unsigned long size_ = 0;
};

XOwnedProperty<unsigned long> getCardinalProperty(Display* dpy, Window win, Atom prop) {
    Atom actualType;
    int actualFormat;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy,
                           win,
                           prop,
                           0,
                           (~0L),
                           False,
                           XA_CARDINAL,
                           &actualType,
                           &actualFormat,
                           &itemCount,
                           &bytesAfter,
                           &data) != Success) {
        return {};
    }
    if (actualType != XA_CARDINAL || actualFormat != 32) {
        if (data) {
            XFree(data);
        }
        return {};
    }
    return {data, itemCount};
}

std::optional<unsigned long> getCardinal(Display* dpy, Window win, Atom prop) {
    auto owned = getCardinalProperty(dpy, win, prop);
    if (!owned || owned.size() == 0) {
        return std::nullopt;
    }
    return owned.data()[0];
}

std::vector<Window> getWindowList(Display* dpy, Window root, Atom prop) {
    Atom actualType;
    int actualFormat;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    std::vector<Window> result;
    if (XGetWindowProperty(dpy,
                           root,
                           prop,
                           0,
                           (~0L),
                           False,
                           XA_WINDOW,
                           &actualType,
                           &actualFormat,
                           &itemCount,
                           &bytesAfter,
                           &data) != Success) {
        return result;
    }
    if (actualType == XA_WINDOW && actualFormat == 32 && data) {
        auto values = reinterpret_cast<Window*>(data);
        result.assign(values, values + itemCount);
    }
    if (data) {
        XFree(data);
    }
    return result;
}

std::optional<Window> getActiveWindow(Display* dpy, Window root, AtomCache& atoms) {
    Atom actualType;
    int actualFormat;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy,
                           root,
                           atoms.get("_NET_ACTIVE_WINDOW"),
                           0,
                           (~0L),
                           False,
                           XA_WINDOW,
                           &actualType,
                           &actualFormat,
                           &itemCount,
                           &bytesAfter,
                           &data) != Success) {
        return std::nullopt;
    }
    std::optional<Window> result;
    if (actualType == XA_WINDOW && actualFormat == 32 && itemCount >= 1 && data) {
        result = reinterpret_cast<Window*>(data)[0];
    }
    if (data) {
        XFree(data);
    }
    return result;
}

std::optional<unsigned long> getWindowDesktop(Display* dpy, Window win, AtomCache& atoms) {
    auto owned = getCardinalProperty(dpy, win, atoms.get("_NET_WM_DESKTOP"));
    if (!owned || owned.size() == 0) {
        return std::nullopt;
    }
    constexpr unsigned long kDesktopSticky = 0xFFFFFFFF;
    auto desktop = owned.data()[0];
    if (desktop == kDesktopSticky) {
        return std::nullopt;
    }
    return desktop;
}

bool isDockOrDesktop(Display* dpy, Window win, AtomCache& atoms) {
    Atom actualType;
    int actualFormat;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy,
                           win,
                           atoms.get("_NET_WM_WINDOW_TYPE"),
                           0,
                           (~0L),
                           False,
                           XA_ATOM,
                           &actualType,
                           &actualFormat,
                           &itemCount,
                           &bytesAfter,
                           &data) != Success) {
        return false;
    }
    bool result = false;
    if (actualType == XA_ATOM && actualFormat == 32) {
        Atom* types = reinterpret_cast<Atom*>(data);
        Atom dock = atoms.get("_NET_WM_WINDOW_TYPE_DOCK");
        Atom desktop = atoms.get("_NET_WM_WINDOW_TYPE_DESKTOP");
        for (unsigned long i = 0; i < itemCount; ++i) {
            if (types[i] == dock || types[i] == desktop) {
                result = true;
                break;
            }
        }
    }
    if (data) {
        XFree(data);
    }
    return result;
}

std::vector<Window> collectWindows(Display* dpy,
                                   Window root,
                                   unsigned long desktop,
                                   AtomCache& atoms) {
    auto list = getWindowList(dpy, root, atoms.get("_NET_CLIENT_LIST_STACKING"));
    std::vector<Window> filtered;
    for (auto win : list) {
        if (isDockOrDesktop(dpy, win, atoms)) {
            continue;
        }
        auto winDesktop = getWindowDesktop(dpy, win, atoms);
        if (winDesktop && *winDesktop == desktop) {
            XWindowAttributes attrs;
            if (!XGetWindowAttributes(dpy, win, &attrs)) {
                continue;
            }
            if (attrs.map_state != IsViewable) {
                continue;
            }
            filtered.emplace_back(win);
        }
    }
    return filtered;
}

std::vector<Window> stableOrder(unsigned long desktop, const std::vector<Window>& current) {
    auto& stored = g_windowOrder[desktop];
    std::vector<Window> result;
    result.reserve(current.size());
    std::unordered_set<Window> remaining(current.begin(), current.end());

    for (auto win : stored) {
        if (remaining.erase(win) > 0) {
            result.push_back(win);
        }
    }
    for (auto win : current) {
        if (remaining.erase(win) > 0) {
            result.push_back(win);
        }
    }
    stored = result;
    return result;
}

void sendNetWMState(Display* dpy, Window root, Window win, AtomCache& atoms, long action, Atom first, Atom second) {
    XEvent xev{};
    xev.xclient.type = ClientMessage;
    xev.xclient.serial = 0;
    xev.xclient.send_event = True;
    xev.xclient.message_type = atoms.get("_NET_WM_STATE");
    xev.xclient.window = win;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = action;
    xev.xclient.data.l[1] = first;
    xev.xclient.data.l[2] = second;
    xev.xclient.data.l[3] = 0;
    xev.xclient.data.l[4] = 0;
    long mask = SubstructureRedirectMask | SubstructureNotifyMask;
    XSendEvent(dpy, root, False, mask, &xev);
}

void unmaximizeWindow(Display* dpy, Window root, Window win, AtomCache& atoms) {
    auto horz = atoms.get("_NET_WM_STATE_MAXIMIZED_HORZ");
    auto vert = atoms.get("_NET_WM_STATE_MAXIMIZED_VERT");
    sendNetWMState(dpy, root, win, atoms, 0, horz, vert); // 0 == _NET_WM_STATE_REMOVE
}

void removeDecorations(Display* dpy, Window win, AtomCache& atoms) {
    MotifHints hints{};
    XChangeProperty(dpy,
                    win,
                    atoms.get("_MOTIF_WM_HINTS"),
                    atoms.get("_MOTIF_WM_HINTS"),
                    32,
                    PropModeReplace,
                    reinterpret_cast<unsigned char*>(&hints),
                    5);
}

struct Rect {
    int x;
    int y;
    int width;
    int height;
};

std::vector<int> distribute(int total, int slots) {
    std::vector<int> result;
    if (slots <= 0) {
        return result;
    }
    int base = total / slots;
    int remainder = total - base * slots;
    for (int i = 0; i < slots; ++i) {
        result.push_back(base + (i < remainder ? 1 : 0));
    }
    return result;
}

std::vector<int> buildRows(int count) {
    switch (count) {
        case 1:
            return {1};
        case 2:
            return {2};
        case 3:
            return {3};
        case 4:
            return {2, 2};
        case 5:
            return {2, 3};
        case 6:
            return {3, 3};
        default:
            break;
    }
    std::vector<int> rows;
    int remaining = count;
    while (remaining > 0) {
        int cols = remaining >= 3 ? 3 : remaining;
        rows.push_back(cols);
        remaining -= cols;
    }
    return rows;
}

std::vector<Rect> computePositions(int count, int screenW, int screenH, const DesktopLayout& layout) {
    std::vector<Rect> result;
    if (count <= 0) {
        return result;
    }
    auto rows = buildRows(count);
    int usableWidth = std::max(0, screenW - layout.marginLeft - layout.marginRight);
    int totalVertical = std::max(0, screenH - layout.marginTop - layout.marginBottom);
    int usableHeight = totalVertical - layout.gap * static_cast<int>(rows.size() - 1);
    if (usableHeight < 0) {
        usableHeight = 0;
    }
    auto rowHeights = distribute(usableHeight, static_cast<int>(rows.size()));

    int remaining = count;
    int y = layout.marginTop;
    for (size_t rowIdx = 0; rowIdx < rows.size() && remaining > 0; ++rowIdx) {
        int cols = std::min(rows[rowIdx], remaining);
        int rowWidth = usableWidth - layout.gap * (cols - 1);
        if (rowWidth < 0) {
            rowWidth = 0;
        }
        auto widths = distribute(rowWidth, cols);
        int x = layout.marginLeft;
        for (int width : widths) {
            result.push_back(Rect{x, y, width, rowHeights[rowIdx]});
            x += width + layout.gap;
            if (--remaining == 0) {
                break;
            }
        }
        y += rowHeights[rowIdx] + layout.gap;
    }
    return result;
}

void applyGeometry(Display* dpy, Window win, const Rect& rect) {
    XWindowChanges changes{};
    changes.x = rect.x;
    changes.y = rect.y;
    changes.width = rect.width;
    changes.height = rect.height;
    XConfigureWindow(dpy, win, CWX | CWY | CWWidth | CWHeight, &changes);
}

void tileWindows(Display* dpy,
                 Window root,
                 unsigned long desktop,
                 AtomCache& atoms,
                 const DesktopLayout& layout) {
    int screen = DefaultScreen(dpy);
    int screenW = DisplayWidth(dpy, screen);
    int screenH = DisplayHeight(dpy, screen);
    auto windows = collectWindows(dpy, root, desktop, atoms);
    if (windows.empty()) {
        g_windowOrder.erase(desktop);
        return;
    }
    auto ordered = stableOrder(desktop, windows);
    auto positions = computePositions(static_cast<int>(ordered.size()), screenW, screenH, layout);
    for (size_t i = 0; i < ordered.size() && i < positions.size(); ++i) {
        unmaximizeWindow(dpy, root, ordered[i], atoms);
        removeDecorations(dpy, ordered[i], atoms);
        applyGeometry(dpy, ordered[i], positions[i]);
    }
    XFlush(dpy);
}

std::set<unsigned long> defaultTiledDesktops(Display* dpy, Window root, AtomCache& atoms) {
    std::set<unsigned long> result;
    auto total = getCardinal(dpy, root, atoms.get("_NET_NUMBER_OF_DESKTOPS"));
    if (!total || *total <= 1) {
        result.insert(0);
        return result;
    }
    for (unsigned long desk = 1; desk < *total; ++desk) {
        result.insert(desk);
    }
    return result;
}

unsigned long currentDesktop(Display* dpy, Window root, AtomCache& atoms) {
    auto desktop = getCardinal(dpy, root, atoms.get("_NET_CURRENT_DESKTOP"));
    if (!desktop) {
        return 0;
    }
    return *desktop;
}

bool shouldTile(unsigned long desktop, const Config& cfg) {
    return cfg.tiledDesktops.empty() || cfg.tiledDesktops.count(desktop) > 0;
}

DesktopLayout layoutForDesktop(const Config& cfg, unsigned long desktop) {
    auto it = cfg.perDesktop.find(desktop);
    if (it != cfg.perDesktop.end()) {
        return it->second;
    }
    if (cfg.hasTiledDefaults) {
        return cfg.tiledDefaults;
    }
    return cfg.defaults;
}

bool moveActiveWindow(Display* dpy,
                      Window root,
                      unsigned long desktop,
                      AtomCache& atoms,
                      const Config& cfg,
                      bool forward) {
    auto windows = collectWindows(dpy, root, desktop, atoms);
    if (windows.empty()) {
        g_windowOrder.erase(desktop);
        return false;
    }
    auto ordered = stableOrder(desktop, windows);
    auto active = getActiveWindow(dpy, root, atoms);
    if (!active) {
        return false;
    }
    auto it = std::find(ordered.begin(), ordered.end(), *active);
    if (it == ordered.end()) {
        return false;
    }
    if (forward) {
        if (std::next(it) == ordered.end()) {
            return false;
        }
        std::iter_swap(it, std::next(it));
    } else {
        if (it == ordered.begin()) {
            return false;
        }
        std::iter_swap(it, std::prev(it));
    }
    g_windowOrder[desktop] = ordered;
    tileWindows(dpy, root, desktop, atoms, layoutForDesktop(cfg, desktop));
    return true;
}

void runOnce(Display* dpy, Window root, AtomCache& atoms, const Config& cfg) {
    auto desktop = currentDesktop(dpy, root, atoms);
    if (!shouldTile(desktop, cfg)) {
        return;
    }
    tileWindows(dpy, root, desktop, atoms, layoutForDesktop(cfg, desktop));
}

void handleSignal(int) {
    g_interrupted = true;
}

void processPendingCommands(Display* dpy, Window root, AtomCache& atoms, const Config& cfg) {
    auto commands = pullCommands();
    for (const auto& cmd : commands) {
        auto desktop = currentDesktop(dpy, root, atoms);
        if (!shouldTile(desktop, cfg)) {
            continue;
        }
        switch (cmd.type) {
            case CommandType::MoveLeft:
                moveActiveWindow(dpy, root, desktop, atoms, cfg, false);
                break;
            case CommandType::MoveRight:
                moveActiveWindow(dpy, root, desktop, atoms, cfg, true);
                break;
        }
    }
}

void runDaemon(Display* dpy, Window root, AtomCache& atoms, const Config& cfg) {
    XSelectInput(dpy,
                 root,
                 PropertyChangeMask | SubstructureNotifyMask | StructureNotifyMask);
    auto schedule = std::optional<std::chrono::steady_clock::time_point>{};

    if (!cfg.commandSocket.empty()) {
        g_commandSocketPath = cfg.commandSocket;
        g_commandServerFd = createCommandServer(cfg.commandSocket);
        if (g_commandServerFd >= 0) {
            g_commandThread = std::thread(commandListenerLoop);
        } else {
            std::cerr << "Warning: failed to create command socket "
                      << cfg.commandSocket << '\n';
        }
    }

    runOnce(dpy, root, atoms, cfg);

    while (!g_interrupted) {
        processPendingCommands(dpy, root, atoms, cfg);
        while (!g_interrupted && XPending(dpy)) {
            XEvent event;
            XNextEvent(dpy, &event);
            switch (event.type) {
                case PropertyNotify:
                case CreateNotify:
                case DestroyNotify:
                case ConfigureNotify:
                    schedule = std::chrono::steady_clock::now() + cfg.debounce;
                    break;
                default:
                    break;
            }
        }

        if (schedule && std::chrono::steady_clock::now() >= *schedule) {
            schedule.reset();
            auto desktop = currentDesktop(dpy, root, atoms);
            if (shouldTile(desktop, cfg)) {
                tileWindows(dpy, root, desktop, atoms, layoutForDesktop(cfg, desktop));
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    processPendingCommands(dpy, root, atoms, cfg);
    stopCommandServer();
}

std::set<unsigned long> parseDesktopList(const std::string& value) {
    std::set<unsigned long> result;
    std::stringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        try {
            auto desktop = static_cast<unsigned long>(std::stoul(token));
            result.insert(desktop);
        } catch (const std::exception&) {
            std::cerr << "Warning: failed to parse desktop number: " << token << '\n';
        }
    }
    return result;
}

void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "  --daemon                 Run in background and watch X11 events\n"
              << "  --tile-desktops 1,2,3    Comma-separated list of desktops to tile\n"
              << "  --margin-x <px>          Default horizontal margin applied to both sides\n"
              << "  --margin-left <px>       Default left margin\n"
              << "  --margin-right <px>      Default right margin\n"
              << "  --margin-top <px>        Default top margin\n"
              << "  --margin-bottom <px>     Default bottom margin\n"
              << "  --gap <px>               Default gap between windows\n"
              << "  --desktop-config N:top,right,bottom,left,gap      Per-desktop override\n"
              << "  --desktop-default-config top,right,bottom,left,gap Default for tiled desktops\n"
              << "  --command-socket <path>  Path to the UNIX socket (default /tmp/wmtiler.sock)\n"
              << "  --move-left              Send \"move-left\" command to a running daemon\n"
              << "  --move-right             Send \"move-right\" command to a running daemon\n"
              << "  --help                   Show this message\n";
}

bool parseIntArg(const char* value, int& out) {
    if (!value) {
        return false;
    }
    try {
        out = std::stoi(value);
        return true;
    } catch (...) {
        return false;
    }
}

DesktopLayout parseLayoutSpec(const std::string& spec) {
    DesktopLayout layout{};
    std::string normalized = spec;
    std::replace(normalized.begin(), normalized.end(), ':', ',');
    std::stringstream ss(normalized);
    std::string token;
    int values[5] = {0, 0, 0, 0, 0};
    int idx = 0;
    while (idx < 5 && std::getline(ss, token, ',')) {
        try {
            values[idx++] = std::stoi(token);
        } catch (...) {
            throw std::runtime_error("Invalid value in desktop config: " + token);
        }
    }
    if (idx != 5 || std::getline(ss, token, ',')) {
        throw std::runtime_error("Layout spec must contain 5 integers: top,right,bottom,left,gap");
    }
    layout.marginTop = values[0];
    layout.marginRight = values[1];
    layout.marginBottom = values[2];
    layout.marginLeft = values[3];
    layout.gap = values[4];
    return layout;
}

Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--daemon") {
            cfg.daemon = true;
        } else if (arg == "--tile-desktops") {
            if (i + 1 >= argc) {
                fail("--tile-desktops expects a comma-separated list");
            }
            cfg.tiledDesktops = parseDesktopList(argv[++i]);
        } else if (arg == "--margin-x") {
            int value = 0;
            if (i + 1 >= argc || !parseIntArg(argv[++i], value)) {
                fail("Invalid value for --margin-x");
            }
            cfg.defaults.marginLeft = value;
            cfg.defaults.marginRight = value;
        } else if (arg == "--margin-left") {
            if (i + 1 >= argc || !parseIntArg(argv[++i], cfg.defaults.marginLeft)) {
                fail("Invalid value for --margin-left");
            }
        } else if (arg == "--margin-right") {
            if (i + 1 >= argc || !parseIntArg(argv[++i], cfg.defaults.marginRight)) {
                fail("Invalid value for --margin-right");
            }
        } else if (arg == "--margin-top") {
            if (i + 1 >= argc || !parseIntArg(argv[++i], cfg.defaults.marginTop)) {
                fail("Invalid value for --margin-top");
            }
        } else if (arg == "--margin-bottom") {
            if (i + 1 >= argc || !parseIntArg(argv[++i], cfg.defaults.marginBottom)) {
                fail("Invalid value for --margin-bottom");
            }
        } else if (arg == "--gap") {
            if (i + 1 >= argc || !parseIntArg(argv[++i], cfg.defaults.gap)) {
                fail("Invalid value for --gap");
            }
        } else if (arg == "--desktop-default-config") {
            if (i + 1 >= argc) {
                fail("--desktop-default-config expects top,right,bottom,left,gap");
            }
            cfg.tiledDefaults = parseLayoutSpec(argv[++i]);
            cfg.hasTiledDefaults = true;
        } else if (arg == "--command-socket") {
            if (i + 1 >= argc) {
                fail("--command-socket expects a path");
            }
            cfg.commandSocket = argv[++i];
        } else if (arg == "--move-left") {
            cfg.sendCommand = true;
            cfg.commandToSend = "move-left";
        } else if (arg == "--move-right") {
            cfg.sendCommand = true;
            cfg.commandToSend = "move-right";
        } else if (arg == "--desktop-config") {
            if (i + 1 >= argc) {
                fail("--desktop-config expects N:top,right,bottom,left,gap");
            }
            std::string value = argv[++i];
            auto colon = value.find(':');
            if (colon == std::string::npos) {
                fail("Format for --desktop-config is N:top,right,bottom,left,gap");
            }
            auto deskStr = value.substr(0, colon);
            auto layoutStr = value.substr(colon + 1);
            unsigned long desk = std::stoul(deskStr);
            cfg.perDesktop[desk] = parseLayoutSpec(layoutStr);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n\n";
            printUsage(argv[0]);
            std::exit(1);
        }
    }
    return cfg;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Config cfg = parseArgs(argc, argv);
        if (cfg.sendCommand) {
            if (cfg.daemon) {
                fail("Cannot use --daemon together with --move-* commands");
            }
            return sendIpcCommand(cfg) ? 0 : 1;
        }
        g_display = XOpenDisplay(nullptr);
        if (!g_display) {
            fail("Failed to connect to X server. Is DISPLAY set?");
        }
        AtomCache atoms(g_display);
        Window root = DefaultRootWindow(g_display);
        if (cfg.tiledDesktops.empty()) {
            cfg.tiledDesktops = defaultTiledDesktops(g_display, root, atoms);
        }

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        if (cfg.daemon) {
            std::cout << "Starting wmtiler in daemon mode. Tiled desktops: ";
            for (auto desk : cfg.tiledDesktops) {
                std::cout << desk << ' ';
            }
            std::cout << std::endl;
            runDaemon(g_display, root, atoms, cfg);
        } else {
            runOnce(g_display, root, atoms, cfg);
        }

        XCloseDisplay(g_display);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        if (g_display) {
            XCloseDisplay(g_display);
        }
        return 1;
    }
}

