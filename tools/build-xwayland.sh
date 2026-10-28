#!/data/data/com.termux/files/usr/bin/bash
# Build Xwayland with glamor for Termux:Wayland, on a Termux-native Mesa with GBM:
# Termux's xwayland package has no glamor (-Dglamor=false), so it offers no DRI3 and X11 programs
# get no GPU (GLX: "failed to create drisw screen", desktops show the GPU as Unknown).
#   bash build-xwayland.sh            installs to $PREFIX/opt/xwayland-glamor
#   GBM_PREFIX=<mesa prefix>          a Mesa built for Termux with GBM, where gbm.pc/dri.pc come
#                                     from (default: $PREFIX, Termux's own)
# Same version, Android patches and options as termux-packages' x11-packages/xwayland, plus glamor.
# termux-wayland-x11 runs it instead of Termux's Xwayland. A Mesa outside the default paths has to
# be found at run time too: TERMUX_WAYLAND_XWAYLAND="<wrapper> $PREFIX/opt/xwayland-glamor/bin/Xwayland".
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

# Present paces copied frames (always, for a rootful Xwayland) with a fixed 17 ms timer, capping
# vsynced X11 programs and compositing window managers at ~60 fps: follow the output's refresh.
P=$S/hw/xwayland/xwayland-present.c
grep -q xwl_present_copy_timer_len "$P" || {
	sed -i 's/^#include "xwayland-present.h"$/#include "xwayland-output.h"\n&/' "$P"
	sed -i 's/^            timeout = TIMER_LEN_COPY;$/            timeout = xwl_present_copy_timer_len(xwl_window);/' "$P"
	sed -i '/^void$/{N;/\nxwl_present_reset_timer(/i\
static CARD32\
xwl_present_copy_timer_len(struct xwl_window *xwl_window)\
{\
    struct xwl_output *xwl_output;\
    int32_t refresh = 0;\
\
    if (xwl_window)\
        xorg_list_for_each_entry(xwl_output, &xwl_window->xwl_screen->output_list, link)\
            refresh = max(refresh, xwl_output->refresh);\
    return refresh > 0 ? max(1000000 / refresh, 1) : TIMER_LEN_COPY;\
}\

}' "$P"
	grep -q "timeout = xwl_present_copy_timer_len" "$P" || { echo "present timer patch failed"; exit 1; }
}
# Same for the modes of the rootful screen, which RandR reports at a fixed 60 Hz.
OUT=$S/hw/xwayland/xwayland-output.c
grep -q xwl_fixed_refresh "$OUT" || {
	sed -i 's/^                            60, 0, 0);$/                            xwl_fixed_refresh(xwl_output), 0, 0);/' "$OUT"
	sed -i 's/xwayland_cvt(current_width, current_height, 60, 0, 0)/xwayland_cvt(current_width, current_height, xwl_fixed_refresh(xwl_output), 0, 0)/' "$OUT"
	sed -i '/^Bool$/{N;/\nxwl_randr_add_modes_fixed(/i\
static double\
xwl_fixed_refresh(struct xwl_output *fixed)\
{\
    struct xwl_output *xwl_output;\
    int32_t refresh = 0;\
\
    if (!fixed->xwl_screen)\
        return 60;\
    xorg_list_for_each_entry(xwl_output, &fixed->xwl_screen->output_list, link)\
        refresh = max(refresh, xwl_output->refresh);\
    return refresh > 0 ? refresh / 1000.0 : 60;\
}\

}' "$OUT"
	[ "$(grep -c "xwl_fixed_refresh(xwl_output)" "$OUT")" = 2 ] || { echo "mode refresh patch failed"; exit 1; }
}
# Only the window's size as mode: the fake ones (640x480...) resize the X screen away from the
# window, which Termux:Wayland does not follow.
grep -q "only the window's size" "$OUT" || {
	sed -i '/^xwl_randr_add_modes_fixed(/,/^}/ s|^    /\* Add fake modes \*/$|    /* Add fake modes: none, only the window'"'"'s size (Termux:Wayland) */|; /^xwl_randr_add_modes_fixed(/,/^}/ s|i < ARRAY_SIZE(xwl_output_fake_modes); i++|i < 0; i++|' "$OUT"
	grep -q "only the window's size" "$OUT" || { echo "fake modes patch failed"; exit 1; }
}

# libxshmfence with futexes (static), like glibc distributions build it: Termux's uses process-shared
# pthread mutexes, a different layout in the same shared memory, so the first DRI3 present from
# a proot/chroot program (glibc libxshmfence) left Xwayland waiting in xshmfence_trigger forever.
XSF=1.3.3
XSF_SHA=d4a4df096aba96fea02c029ee3a44e11a47eb7f7213c1a729be83e85ec3fde10
if [ ! -f "$O/lib/libxshmfence.a" ]; then
	XS=$HOME/src/libxshmfence-$XSF
	if [ ! -d "$XS" ]; then
		curl -sSLf -o "$XS.tar.xz" "https://xorg.freedesktop.org/releases/individual/lib/libxshmfence-$XSF.tar.xz"
		echo "$XSF_SHA  $XS.tar.xz" | sha256sum -c - >/dev/null
		tar -xf "$XS.tar.xz" -C "$HOME/src" && rm "$XS.tar.xz"
	fi
	# glibc's values.h, only for MAXINT
	sed -i 's/^#include <values.h>$/#include <limits.h>\n#define MAXINT INT_MAX/' "$XS/src/xshmfence_futex.h"
	(cd "$XS" && CFLAGS="-O2 -D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__" ./configure -q --prefix="$O" --enable-futex --disable-shared --with-pic && make -s install) >/dev/null
	nm "$O/lib/libxshmfence.a" | grep -q pthread_mutex_lock && { echo "libxshmfence: no futexes"; exit 1; }
	rm -rf "$S/build"
fi

# gbm.pc and dri.pc from GBM_PREFIX, xshmfence.pc from $O, the rest from Termux.
GBM_PREFIX=${GBM_PREFIX:-$PREFIX}
export PKG_CONFIG_PATH=$O/lib/pkgconfig:$GBM_PREFIX/lib/pkgconfig:$PREFIX/lib/pkgconfig
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
