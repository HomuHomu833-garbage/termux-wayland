# Linux test build of the compositor (twl-headless), against the system's libwayland-server
# and Mesa EGL. Not part of the APK.
cmake_minimum_required(VERSION 3.18)
project(twl-headless C)
set(CMAKE_C_STANDARD 11)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED wayland-server egl glesv2)

set(CPP ${CMAKE_CURRENT_SOURCE_DIR}/../lorie/src/main/cpp)
file(GLOB COMPOSITOR ${CPP}/compositor/*.c)
file(GLOB PROTOCOLS ${CPP}/protocols/*-protocol.c)
list(REMOVE_ITEM PROTOCOLS ${CPP}/protocols/wayland-server-protocol.c) # libwayland-server has it

add_executable(twl-headless headless.c ${COMPOSITOR} ${PROTOCOLS})
target_include_directories(twl-headless PRIVATE ${CPP}/protocols ${CPP}/compositor ${DEPS_INCLUDE_DIRS})
target_compile_options(twl-headless PRIVATE -Wall -Wno-unused-parameter -g -fsanitize=address,undefined)
target_link_options(twl-headless PRIVATE -fsanitize=address,undefined)
target_link_libraries(twl-headless ${DEPS_LIBRARIES})
