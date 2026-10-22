cmake_minimum_required(VERSION 3.22)
project(lorie C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE RelWithDebInfo)

# ---- libffi (libwayland-server calls request handlers through ffi_call) ----

set(FFI ${CMAKE_CURRENT_SOURCE_DIR}/libffi)
set(FFI_GEN ${CMAKE_CURRENT_BINARY_DIR}/libffi)
if (CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    set(TARGET AARCH64)
    set(FFI_ARCH_SOURCES src/aarch64/ffi.c src/aarch64/sysv.S)
    set(FFI_ARCH_DIR aarch64)
    set(HAVE_LONG_DOUBLE 1)
    set(SIZEOF_LONG_DOUBLE 16)
elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    set(TARGET ARM)
    set(FFI_ARCH_SOURCES src/arm/ffi.c src/arm/sysv.S)
    set(FFI_ARCH_DIR arm)
    set(HAVE_LONG_DOUBLE 0)
    set(SIZEOF_LONG_DOUBLE 8)
elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
    set(TARGET X86_64)
    set(FFI_ARCH_SOURCES src/x86/ffi64.c src/x86/unix64.S src/x86/ffiw64.c src/x86/win64.S)
    set(FFI_ARCH_DIR x86)
    set(HAVE_LONG_DOUBLE 1)
    set(SIZEOF_LONG_DOUBLE 16)
elseif (CMAKE_ANDROID_ARCH_ABI STREQUAL "x86")
    set(TARGET X86)
    set(FFI_ARCH_SOURCES src/x86/ffi.c src/x86/sysv.S)
    set(FFI_ARCH_DIR x86)
    set(HAVE_LONG_DOUBLE 1)
    set(SIZEOF_LONG_DOUBLE 12)
else ()
    message(FATAL_ERROR "unsupported ABI ${CMAKE_ANDROID_ARCH_ABI}")
endif ()
set(VERSION 3.4.6)
set(HAVE_LONG_DOUBLE_VARIANT 0)
set(FFI_EXEC_TRAMPOLINE_TABLE 0)
configure_file(${FFI}/include/ffi.h.in ${FFI_GEN}/ffi.h @ONLY)
configure_file(${FFI}/src/${FFI_ARCH_DIR}/ffitarget.h ${FFI_GEN}/ffitarget.h COPYONLY)
file(WRITE ${FFI_GEN}/fficonfig.h.new "
#define STDC_HEADERS 1
#define HAVE_ALLOCA_H 1
#define HAVE_MEMCPY 1
#define HAVE_MKOSTEMP 1
#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE 1
#define HAVE_AS_CFI_PSEUDO_OP 1
#define HAVE_AS_X86_PCREL 1
#define HAVE_AS_X86_64_UNWIND_SECTION_TYPE 1
#define EH_FRAME_FLAGS \"a\"
#define FFI_MMAP_EXEC_WRIT 1
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE ${SIZEOF_LONG_DOUBLE}
#if ${HAVE_LONG_DOUBLE}
#define HAVE_LONG_DOUBLE 1
#endif
#define PACKAGE \"libffi\"
#define VERSION \"${VERSION}\"
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name) .hidden name
#else
#define FFI_HIDDEN __attribute__ ((visibility (\"hidden\")))
#endif
")
configure_file(${FFI_GEN}/fficonfig.h.new ${FFI_GEN}/fficonfig.h COPYONLY)
list(TRANSFORM FFI_ARCH_SOURCES PREPEND ${FFI}/)
add_library(ffi STATIC
        ${FFI}/src/prep_cif.c
        ${FFI}/src/types.c
        ${FFI}/src/raw_api.c
        ${FFI}/src/java_raw_api.c
        ${FFI}/src/closures.c
        ${FFI_ARCH_SOURCES})
target_include_directories(ffi PUBLIC ${FFI_GEN} PRIVATE ${FFI}/include ${FFI}/src/${FFI_ARCH_DIR})
target_compile_options(ffi PRIVATE -fPIC -fvisibility=hidden -Wno-deprecated-declarations)

# ---- libwayland-server ----

set(WL ${CMAKE_CURRENT_SOURCE_DIR}/wayland)
set(WL_GEN ${CMAKE_CURRENT_BINARY_DIR}/wayland)
set(WAYLAND_VERSION_MAJOR 1)
set(WAYLAND_VERSION_MINOR 23)
set(WAYLAND_VERSION_MICRO 1)
set(WAYLAND_VERSION 1.23.1)
configure_file(${WL}/src/wayland-version.h.in ${WL_GEN}/wayland-version.h @ONLY)
file(WRITE ${WL_GEN}/config.h.new "
#define HAVE_ACCEPT4 1
#define HAVE_MKOSTEMP 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_PRCTL 1
#define HAVE_STRNDUP 1
#define HAVE_SYS_PRCTL_H 1
#define HAVE_XUCRED_CR_PID 0
#define HAVE_BROKEN_MSG_CMSG_CLOEXEC 0
#define PACKAGE \"wayland\"
#define PACKAGE_VERSION \"${WAYLAND_VERSION}\"
")
configure_file(${WL_GEN}/config.h.new ${WL_GEN}/config.h COPYONLY)
add_library(wayland-server STATIC
        ${WL}/src/wayland-server.c
        ${WL}/src/wayland-shm.c
        ${WL}/src/event-loop.c
        ${WL}/src/connection.c
        ${WL}/src/wayland-os.c
        ${WL}/src/wayland-util.c
        protocols/wayland-server-protocol.c)
# wayland-os.c includes "../config.h": found through ${WL_GEN}/src/..
file(MAKE_DIRECTORY ${WL_GEN}/src)
target_include_directories(wayland-server PUBLIC ${WL}/src ${WL_GEN} protocols PRIVATE ${WL_GEN}/src)
target_compile_options(wayland-server PRIVATE -include config.h -fvisibility=hidden -Wno-unused-parameter)
target_link_libraries(wayland-server PRIVATE ffi)

# ---- the compositor, the server glue and the activity side (Termux:X11's renderer) ----

file(GLOB COMPOSITOR_SOURCES compositor/*.c)
file(GLOB PROTOCOL_SOURCES protocols/*-protocol.c)
list(REMOVE_ITEM PROTOCOL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wayland-server-protocol.c)

add_library(Xlorie SHARED
        ${COMPOSITOR_SOURCES}
        ${PROTOCOL_SOURCES}
        lorie/cmdentrypoint.c
        lorie/renderer.cpp
        lorie/buffer.c
        lorie/activity.cpp)
target_include_directories(Xlorie PRIVATE lorie compositor protocols)
target_compile_options(Xlorie PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function -Wno-missing-braces
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
# -nostdlib++ keeps this shared object free of any libc++ dependency; renderer.cpp and activity.cpp
# are restricted to a runtime-free subset of C++ (no exceptions, no RTTI, no STL) to make that possible.
target_link_options(Xlorie PRIVATE "-Wl,--as-needed" "-Wl,--no-undefined" "-fvisibility=hidden" "-nostdlib++")
target_link_libraries(Xlorie wayland-server ffi android mediandk log m EGL GLESv2)
