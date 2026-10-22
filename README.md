# Termux:Wayland

[![Nightly build](https://github.com/HomuHomu833/termux-wayland/actions/workflows/debug_build.yml/badge.svg?branch=master)](https://github.com/HomuHomu833/termux-wayland/actions/workflows/debug_build.yml)

A Wayland compositor for Termux: a fork of [Termux:X11](https://github.com/termux/termux-x11) with the X
server replaced by a native Wayland compositor. The app is Termux:X11's: the same activity, renderer,
preferences, extra keys bar, touch/trackpad modes, stylus and hardware keyboard handling. X11 programs
run through Xwayland like on any Wayland desktop.

> **Status: work in progress.** The compositor is tested on Linux against real clients (GTK3, foot,
> weston-simple-shm, weston-simple-egl on llvmpipe, rootful Xwayland); the APK builds. It has not been
> run on a phone yet.

## How it works

Like Termux:X11, the server runs inside Termux's own process (`termux-wayland`, started through
`app_process`), and the app only shows its picture and sends input back:

- **Server** (`lorie/src/main/cpp/compositor`, `lorie/cmdentrypoint.c`): libwayland-server and a
  compositor that draws every window with GLES into an `AHardwareBuffer` "root window", one frame per
  display vsync (`AChoreographer`). Two root buffers alternate, so the app never shows a half-drawn frame.
- **App**: Termux:X11's activity draws the root buffer and the cursor, and sends touch, mouse, stylus,
  keys, text and clipboard over the same socket protocol as before.

Client buffers:

| Client renders with | Buffers | How they reach the screen |
|---|---|---|
| llvmpipe, lavapipe, any CPU renderer | `wl_shm` | copied into `AHardwareBuffer`s sampled by GLES (`glTexSubImage2D` fallback) |
| a GPU driver with a DRM render node (RadeonSI, RADV, Freedreno/Turnip...) | `linux-dmabuf` v4, linear | imported with `EGL_EXT_image_dma_buf_import` if Android's EGL can, otherwise mmapped and copied |

dma-bufs are polled for their write fences (implicit sync) before they are shown: Android's GLES
ignores implicit fences, which is what made Mesa clients flicker on Termux:X11.

## Protocols

`wl_compositor` 6, `wl_subcompositor`, `wl_shm`, `wl_output` 4, `wl_seat` 8 (pointer, keyboard, touch),
`wl_data_device_manager` 3 (clipboard, synced with Android's), `xdg_wm_base` 6, `zxdg_decoration_manager_v1`
(client-side), `zxdg_output_manager_v1` 3, `wp_viewporter`, `zwp_linux_dmabuf_v1` 4,
`zwp_text_input_v3` (shows the soft keyboard when a text field is focused), `zwp_relative_pointer_v1`,
`zwp_pointer_constraints_v1` (pointer locks capture the mouse, for games).

Windows: top-level windows fill the screen (maximized, or fullscreen when they ask), dialogs and
fixed-size windows float centered over their parent, the focused window is on top. Alt+Tab and
Alt+Shift+Tab cycle windows, Alt+F4 closes one. For a desktop with movable windows, run a Wayland
compositor such as labwc or sway inside it as a client.

Not yet: rootless Xwayland (it needs `xwayland_shell_v1` and an X window manager; rootful Xwayland
works), drag and drop, primary selection, fractional scaling, buffer transforms.

## Setup

Requires Android 8 or later.

1. Install the APK (`termux-wayland-universal-debug.apk`, or the sharedUid variant to keep Termux from
   being throttled while the app is on screen) and the companion package (`termux-wayland-*-all.deb`,
   `dpkg -i` it in Termux) from the latest [build](https://github.com/HomuHomu833/termux-wayland/actions/workflows/debug_build.yml).
2. In Termux: `termux-wayland -- <command>`, or `termux-wayland &` and start clients yourself:

```sh
termux-wayland &
am start -n com.termux.wayland/.MainActivity
export XDG_RUNTIME_DIR=$TMPDIR WAYLAND_DISPLAY=wayland-0
```

In a proot or chroot distribution, bind Termux's `$TMPDIR` to the container's `/tmp`
(`proot-distro login --shared-tmp`) and use `XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0`.
X11 programs: `Xwayland :1 -geometry 1920x1080 & DISPLAY=:1 xfce4-session`.

### Options

```
termux-wayland [options] [-- command args...]
  -scale N          output scale (default: the app's preference, else from the screen density)
  -socket NAME      Wayland socket in $XDG_RUNTIME_DIR (default: first free wayland-N)
  -keymap FILE      XKB keymap for clients (xkbcli compile-keymap --layout ...); default us
  -no-dmabuf        no linux-dmabuf: GPU clients fall back to wl_shm
  -no-dmabuf-import copy dma-bufs instead of importing them into EGL
  -no-ahb           upload textures with glTexSubImage2D instead of AHardwareBuffers
  -xstartup "cmd"   run cmd with WAYLAND_DISPLAY set; the server exits with it
  -- command args   same, without a shell
```

Settings and controls are Termux:X11's (`termux-wayland-preference` works the same), plus:

- **Output → Wayland output scale**: `auto` (from the screen density) or 1-4. Toolkits that support
  scaling (GTK, Qt, foot...) draw sharp at that scale.
- **Keyboard → Show the keyboard when a text field is focused** (text-input-v3 clients).
- **Pointer → Capture the pointer when a game locks it**.

## Building

```sh
git clone --recurse-submodules https://github.com/HomuHomu833/termux-wayland
cd termux-wayland
./gradlew assembleDebug
```

Native dependencies are submodules built by CMake: libwayland 1.23.1 (server side only) and libffi 3.4.6.
The protocol glue in `lorie/src/main/cpp/protocols` is generated by `protocols/generate.sh`
(wayland-scanner, wayland-protocols); the built-in keymap by `compositor/gen-keymap.sh`.

`host/` builds the compositor for Linux (`twl-headless`): it renders off screen with Mesa, writes every
frame to a PPM, and takes the app's input events on a socket, so it can be tested with real clients:

```sh
cmake -S host -B build-host -G Ninja && ninja -C build-host
build-host/twl-headless --socket wl-test --size 1280x720 --scale 2 --dump /tmp/frame.ppm --events /tmp/twl-ev &
WAYLAND_DISPLAY=wl-test gtk3-widget-factory
```

## License

GPL-3.0, like Termux:X11 (see `LICENSE`). libwayland and libffi are MIT.
