# wmtiler

wmtiler is a lightweight tiling helper for Openbox (and other EWMH-compliant window managers). It removes maximization/decorations and snaps windows to predefined layouts. You can run it once for the current desktop or keep it alive as a daemon that reacts to X11 events.

## Requirements

- `cmake >= 3.16`
- `g++` (or any C++20-capable compiler)
- `libx11-dev`

Install on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libx11-dev
```

## Build

```bash
cd /home/admax/Projects/snap
cmake -S . -B build
cmake --build build
```

The binary is produced at `build/wmtiler`.

## Usage

- Single-shot layout:

  ```bash
  ./build/wmtiler
  ```

- Daemon mode (perfect for `~/.config/openbox/autostart`):

  ```bash
  ./build/wmtiler --daemon --tile-desktops 1,2,3 &
  ```

Desktop `0` stays stacking-only, while desktops `1..3` are automatically retiled whenever windows change or you switch to them.

## Global margins

All margins default to zero. Adjust them for desktops that do not have their own profile:

```
--margin-x <px>      set both left and right margins
--margin-left <px>   left margin only
--margin-right <px>  right margin only
--margin-top <px>    top margin
--margin-bottom <px> bottom margin
--gap <px>           spacing between windows
```

Example: `--margin-x 6 --margin-top 6 --margin-bottom 42 --gap 6`.

## Per-desktop configuration

Override margins for a specific desktop with:

```
--desktop-config N:top,right,bottom,left,gap
```

Numbers may be separated with either `:` or `,`—keep the order `top,right,bottom,left,gap`. Supply multiple flags for multiple desktops. A desktop without its own profile falls back to the global defaults (or zero). You can also define a base profile for all tiled desktops via `--desktop-default-config top,right,bottom,left,gap`; individual `--desktop-config` entries still take precedence.

Example:

```bash
./build/wmtiler \
  --daemon \
  --tile-desktops 1,2 \
  --desktop-default-config 8,8,32,8,8 \
  --desktop-config 1:6,6,42,6,6 \
  --desktop-config 2:12,16,48,10,10 \
  &
```

- Desktop `0` is absent from `--tile-desktops`, so it remains stacking.
- `--desktop-default-config 8,8,32,8,8` defines the baseline padding (top/right/bottom/left/gap) for any tiled desktop without an explicit profile.
- Desktop `1` gets top=6/right=6/bottom=42/left=6 with gap 6.
- Desktop `2` gets top=12/right=16/bottom=48/left=10 with gap 10.

## Window order

wmtiler remembers the window order per desktop the moment the layout is first applied. Changing focus no longer shuffles anything; only opening or closing windows alters the list, with new windows appended to the end.

## Hotkeys / IPC

In daemon mode wmtiler opens a UNIX socket (`/tmp/wmtiler.sock` by default). Send commands through it to move the active window relative to others:

- `<path-to-wmtiler>/wmtiler --move-left`
- `<path-to-wmtiler>/wmtiler --move-right`

Need a different socket path (multi-user setups, sandboxes, etc.)? Pass `--command-socket /path/to.sock` to the daemon **and** to the command you trigger from hotkeys.

Example Openbox bindings (`~/.config/openbox/rc.xml`):

```xml
<keybind key="W-Left">
  <action name="Execute">
    <command><path-to-wmtiler>/wmtiler --move-left</command>
  </action>
</keybind>
<keybind key="W-Right">
  <action name="Execute">
    <command><path-to-wmtiler>/wmtiler --move-right</command>
  </action>
</keybind>
```

Make sure the daemon is already running (`--daemon`). Commands targeting stacking desktops are ignored.

## Openbox autostart tip

```
~/.config/openbox/autostart
--------------------------------
<path-to-wmtiler>/wmtiler --daemon --tile-desktops 1,2 --desktop-config 1:6,6,42,6,6 &
```

Remove the desktop from `--tile-desktops` (or stop the wmtiler process) to temporarily disable tiling on that workspace.

