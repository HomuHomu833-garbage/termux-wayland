#!/bin/sh
# Regenerates the protocol glue in this directory (committed, so the NDK build needs no
# wayland-scanner). Needs wayland-scanner and wayland-protocols (Debian/Ubuntu packages).
set -e
cd "$(dirname "$0")"
WP=${WAYLAND_PROTOCOLS:-$(pkg-config --variable=pkgdatadir wayland-protocols)}
gen() { # name xml (wayland-server-protocol.h is the name libwayland includes)
	wayland-scanner server-header "$2" "$1-protocol.h"
	wayland-scanner private-code "$2" "$1-protocol.c"
}
gen wayland-server ../wayland/protocol/wayland.xml
gen xdg-shell "$WP/stable/xdg-shell/xdg-shell.xml"
gen viewporter "$WP/stable/viewporter/viewporter.xml"
gen linux-dmabuf-v1 "$WP/stable/linux-dmabuf/linux-dmabuf-v1.xml"
gen xdg-output-unstable-v1 "$WP/unstable/xdg-output/xdg-output-unstable-v1.xml"
gen xdg-decoration-unstable-v1 "$WP/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
gen text-input-unstable-v3 "$WP/unstable/text-input/text-input-unstable-v3.xml"
gen relative-pointer-unstable-v1 "$WP/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
gen pointer-constraints-unstable-v1 "$WP/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
gen primary-selection-unstable-v1 "$WP/unstable/primary-selection/primary-selection-unstable-v1.xml"
