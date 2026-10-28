#!/data/data/com.termux/files/usr/bin/bash
# Package what build-xwayland.sh installed ($PREFIX/opt/xwayland-glamor) as a Termux .deb:
# termux-wayland-xwayland, which termux-wayland-x11 picks up by itself.
#   bash package-xwayland.sh <out dir> [version suffix]
# The CI builds it in Termux's Docker image; on a phone it works the same after build-xwayland.sh.
set -euo pipefail
OUT=${1:?usage: package-xwayland.sh <out dir> [version suffix]}
O=$PREFIX/opt/xwayland-glamor
[ -x "$O/bin/Xwayland" ] || { echo "No $O/bin/Xwayland: run build-xwayland.sh first"; exit 1; }
VER=$("$O/bin/Xwayland" -version 2>&1 | sed -n 's/.*Xwayland Version \([0-9.]*\).*/\1/p' | head -1)
VER=${VER:-0}${2:+-$2}
ARCH=$(dpkg --print-architecture)

D=$(mktemp -d)
chmod 755 "$D"
trap 'rm -rf "$D"' EXIT
mkdir -p "$D/DEBIAN" "$D$O"
# Xwayland itself and its data; libxshmfence is linked in statically, its headers and .a stay out.
cp -a "$O/bin" "$D$O/"
[ -d "$O/share" ] && cp -a "$O/share" "$D$O/"
rm -rf "$D$O/share/man"
strip --strip-unneeded "$D$O/bin/Xwayland" 2>/dev/null || true

cat >"$D/DEBIAN/control" <<EOF
Package: termux-wayland-xwayland
Version: $VER
Architecture: $ARCH
Maintainer: termux-wayland
Installed-Size: $(du -sk "$D$O" | cut -f1)
Depends: libandroid-shmem, libdrm, libepoxy, libpixman, libwayland, libx11, libxau, libxcvt, libxfont2, libxinerama, libxkbfile, mesa, openssl, xkeyboard-config, xorg-protocol-txt, xorg-xkbcomp, libtirpc
Homepage: https://github.com/HomuHomu833/termux-wayland
Description: Xwayland with glamor for Termux:Wayland
 Termux's Xwayland is built without glamor, so X11 programs get no GPU. This one has glamor,
 a futex libxshmfence (DRI3 from proot/chroot programs) and Termux:Wayland fixes (output refresh
 rate, no fake modes, a Present fence hang). termux-wayland-x11 uses it when installed;
 TERMUX_WAYLAND_NO_GLAMOR=1 goes back to Termux's Xwayland.
EOF
mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz --build "$D" "$OUT/termux-wayland-xwayland_${VER}_${ARCH}.deb"
