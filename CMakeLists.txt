cmake_minimum_required(VERSION 3.16)
project(wmtiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(wmtiler src/wmtiler.cpp)
target_link_libraries(wmtiler PRIVATE X11)

