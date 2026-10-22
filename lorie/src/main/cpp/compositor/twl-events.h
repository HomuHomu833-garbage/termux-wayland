/*
 * Event stream between the Termux:Wayland activity (or a test driver) and the compositor:
 * fixed 24-byte little-endian records over a stream socket. Mirrored in
 * app/src/main/java/com/termux/wayland/Events.java; keep both in sync.
 *
 * Coordinates are window pixels; the compositor divides by its output scale.
 */
#pragma once
#include <stdint.h>

struct twl_event {
	uint32_t type;
	int32_t id;    /* touch id, linux key/button code, setting */
	float x, y;
	uint32_t arg;  /* pressed, payload length, value */
	uint32_t time; /* ms, any monotonic base */
};

_Static_assert(sizeof(struct twl_event) == 24, "twl_event must be 24 bytes");

enum {
	/* frontend -> compositor */
	TWL_EV_TOUCH_DOWN = 1,
	TWL_EV_TOUCH_MOTION = 2,
	TWL_EV_TOUCH_UP = 3,
	TWL_EV_TOUCH_CANCEL = 4,
	TWL_EV_TOUCH_FRAME = 5,
	TWL_EV_POINTER_MOTION = 10, /* absolute x,y */
	TWL_EV_POINTER_RELATIVE = 11, /* x,y = dx,dy (captured mouse) */
	TWL_EV_POINTER_BUTTON = 12, /* id = BTN_LEFT..., arg = pressed */
	TWL_EV_POINTER_AXIS = 13,   /* x,y = horizontal/vertical scroll, in wheel notches */
	TWL_EV_KEY = 20,            /* id = linux KEY_* code, arg = pressed */
	TWL_EV_TEXT = 21,           /* arg = length of the UTF-8 bytes that follow */
	TWL_EV_KEYS_RELEASE = 22,   /* window lost focus: release everything held */
	TWL_EV_CYCLE_WINDOW = 30,   /* arg = 0 next, 1 previous */
	TWL_EV_CLOSE_WINDOW = 31,   /* ask the focused toplevel to close */
	TWL_EV_SET = 40,            /* id = setting, arg = value */

	/* compositor -> frontend */
	TWL_EV_SHOW_KEYBOARD = 100, /* arg = 1 show, 0 hide (text-input enabled/disabled) */
	TWL_EV_POINTER_CAPTURE = 101, /* arg = 1: a client locked the pointer */
};

enum {
	TWL_SET_TOUCH_POINTER = 1, /* touch emulates the mouse */
	TWL_SET_SCALE = 2,         /* output scale, 0 = from dpi */
};
