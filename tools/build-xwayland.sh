#!/data/data/com.termux/files/usr/bin/bash
# Build Xwayland with glamor for Termux:Wayland, on a Termux-native Mesa with GBM:
# Termux's xwayland package has no glamor (-Dglamor=false), so it offers no DRI3 and X11 programs
# get no GPU (GLX: "failed to create drisw screen", desktops show the GPU as Unknown).
#   bash build-xwayland.sh            installs to $PREFIX/opt/xwayland-glamor
#   GBM_PREFIX=<mesa prefix>          where gbm.pc/dri.pc come from (default: termux-xclipse940-linux's
#                                     RadeonSI, $PREFIX/opt/radeonsi-xclipse, else Termux's own)
# Same version, Android patches and options as termux-packages' x11-packages/xwayland, plus glamor.
# termux-wayland-x11 runs it (through termux-gpu, when installed) instead of Termux's Xwayland.
set -euo pipefail
VER=${VER:-24.1.14}
SHA=4eb6b98d678299a4b96138d886345b294130695760345ef84596225a8224078a
S=$HOME/src/xwayland-$VER
O=$PREFIX/opt/xwayland-glamor
RAW=https://raw.githubusercontent.com/termux/termux-packages/master/x11-packages/xwayland

pkg install -y libandroid-shmem libdrm libepoxy libpciaccess libpixman libwayland libwayland-protocols \
	libx11 libxau libxcvt libxfont2 libxinerama libxkbfile libxshmfence openssl xkeyboard-config \
	xorg-protocol-txt xorg-xkbcomp xorgproto xtrans libtirpc ninja pkg-config >/dev/null
command -v meson >/dev/null || pip install -q meson

if [ ! -d "$S" ]; then
	mkdir -p "$HOME/src"
	curl -sSLf -o "$S.tar.xz" "https://xorg.freedesktop.org/releases/individual/xserver/xwayland-$VER.tar.xz"
	echo "$SHA  $S.tar.xz" | sha256sum -c - >/dev/null
	tar -xf "$S.tar.xz" -C "$HOME/src" && rm "$S.tar.xz"
	for p in hw_xwayland_xwayland-shm stubmain.c use_ashmem xorg-server-1.20.1_Xext_xf86bigfont.c \
		xorg-server-1.20.1_os_access.c xorg-server-1.20.1_os_backtrace.c xorg-server-1.20.1_os_osinit.c \
		xorg-server-1.20.1_os_utils.c xorg-server-1.20.1_xkb_ddxLoad.c; do
		curl -sSLf "$RAW/$p.patch" | sed "s#@TERMUX_PREFIX@#$PREFIX#g" | patch -d "$S" -p1 -s
	done
fi

# Termux's shm patch defines a static memfd_create, which clashes with libc's declaration once
# newer functions are declared (-D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__, for reallocarray): rename.
grep -q xwl_memfd_create "$S/hw/xwayland/xwayland-shm.c" ||
	sed -i 's/^static inline int memfd_create(/#define memfd_create xwl_memfd_create\n&/' "$S/hw/xwayland/xwayland-shm.c"

# gbm.pc and dri.pc from GBM_PREFIX, the rest from Termux.
GBM_PREFIX=${GBM_PREFIX:-$PREFIX/opt/radeonsi-xclipse}
export PKG_CONFIG_PATH=$GBM_PREFIX/lib/pkgconfig:$PREFIX/lib/pkgconfig
pkg-config --exists gbm || { echo "No gbm.pc: build a Mesa with GBM, or set GBM_PREFIX"; exit 1; }
FLAGS="-D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ -fcommon -fPIC -DFNDELAY=O_NDELAY -Wno-int-to-pointer-cast -Wno-implicit-function-declaration -I$PREFIX/include/libdrm"
[ -f "$S/build/build.ninja" ] || rm -rf "$S/build"
[ -f "$S/build/build.ninja" ] || CFLAGS="$FLAGS" LDFLAGS="-landroid-shmem" meson setup "$S/build" "$S" \
	--prefix="$O" --buildtype=release \
	-Dmitshm=true -Dxres=true -Dxv=true -Dsecure-rpc=false -Dscreensaver=true -Dxdmcp=true -Dglx=true \
	-Ddri3=true -Dxinerama=true -Dxace=true -Dxcsecurity=true -Dxf86bigfont=true -Ddrm=true \
	-Dglamor=true -Dxvfb=false -Dlibunwind=false -Dipv6=true -Dsha1=libcrypto \
	-Ddefault_font_path="$PREFIX/share/fonts" -Dxkb_dir="$PREFIX/share/X11/xkb" \
	-Dxkb_bin_dir="$PREFIX/bin" -Dxkb_output_dir="$PREFIX/var/lib/xkb"
ninja -C "$S/build" install >/dev/null
"$O/bin/Xwayland" -help 2>&1 | grep -i glamor | head -3
