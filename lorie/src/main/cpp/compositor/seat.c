#define _GNU_SOURCE
/*
 * wl_seat: pointer, keyboard, touch; the clipboard (wl_data_device), zwp_text_input_v3 for
 * Android's soft keyboard, relative pointer and pointer locks for games.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include "twl.h"
#include "wayland-server-protocol.h"
#include "text-input-unstable-v3-protocol.h"
#include "relative-pointer-unstable-v1-protocol.h"
#include "pointer-constraints-unstable-v1-protocol.h"

#define BTN_LEFT 0x110
#define KEY_TAB 15
#define KEY_F4 62
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTCTRL 29
#define KEY_RIGHTCTRL 97
#define KEY_LEFTALT 56
#define KEY_RIGHTALT 100
#define KEY_LEFTMETA 125
#define KEY_RIGHTMETA 126
#define KEY_CAPSLOCK 58
#define KEY_NUMLOCK 69

#define MOD_SHIFT (1u << 0)
#define MOD_LOCK (1u << 1)
#define MOD_CTRL (1u << 2)
#define MOD_ALT (1u << 3)
#define MOD_NUM (1u << 4)
#define MOD_SUPER (1u << 6)

struct text_input {
	struct wl_resource *resource;
	struct twl_server *server;
	struct wl_list link;
	bool pending_enabled, enabled;
	uint32_t commits;
	struct twl_surface *focus;
};

struct constraint {
	struct wl_resource *resource;
	struct twl_server *server;
	struct twl_surface *surface;
	struct wl_listener surface_destroy;
	struct wl_list link;
	bool locked; /* lock, else confine */
	bool oneshot, active;
};

struct data_source {
	struct wl_resource *resource;
	struct wl_array mimes; /* char* */
};

/* the compositor-owned selection (Android's clipboard), for offers made from it */
static const char *own_selection_text;

static struct twl_seat *seat_of(struct twl_server *server) {
	return &server->seat;
}

static bool same_client(struct wl_resource *a, struct twl_surface *s) {
	return s && wl_resource_get_client(a) == wl_resource_get_client(s->resource);
}

/* ---- surface positions and hit testing ---- */

static bool surface_origin(struct twl_surface *s, int32_t *x, int32_t *y) {
	int32_t ox = 0, oy = 0;
	while (s->role == TWL_ROLE_SUBSURFACE) {
		struct twl_subsurface *sub = s->role_data;
		if (!sub || !sub->parent)
			return false;
		ox += sub->x;
		oy += sub->y;
		s = sub->parent;
	}
	if (s->role != TWL_ROLE_XDG || !s->role_data)
		return false;
	int32_t rx, ry;
	if (!twl_xdg_surface_position(s->role_data, &rx, &ry))
		return false;
	*x = rx + ox;
	*y = ry + oy;
	return true;
}

static struct twl_surface *surface_at(struct twl_server *server, double x, double y, double *lx, double *ly) {
	struct twl_popup *p;
	wl_list_for_each_reverse(p, &server->popups, link) {
		int32_t sx, sy;
		if (!p->xdg->mapped || !p->xdg->surface || !twl_xdg_surface_position(p->xdg, &sx, &sy))
			continue;
		struct twl_surface *hit = twl_surface_at(p->xdg->surface, sx, sy, x, y, lx, ly);
		if (hit)
			return hit;
	}
	struct twl_toplevel *t;
	wl_list_for_each_reverse(t, &server->toplevels, link) {
		int32_t sx, sy;
		if (!t->xdg->mapped || !t->xdg->surface || !twl_xdg_surface_position(t->xdg, &sx, &sy))
			continue;
		struct twl_surface *hit = twl_surface_at(t->xdg->surface, sx, sy, x, y, lx, ly);
		if (hit)
			return hit;
	}
	return NULL;
}

static struct twl_xdg_surface *xdg_of(struct twl_surface *s) {
	if (!s)
		return NULL;
	s = twl_surface_root(s);
	return s->role == TWL_ROLE_XDG ? s->role_data : NULL;
}

/* A press: raise the window under it, dismiss popups pressed outside of. */
static void press_at(struct twl_server *server, struct twl_surface *hit) {
	struct twl_xdg_surface *xdg = xdg_of(hit);
	if (!xdg || !xdg->popup)
		twl_xdg_close_popups(server);
	if (xdg && xdg->toplevel && xdg->toplevel != server->focused)
		twl_xdg_raise(xdg->toplevel);
}

/* ---- pointer ---- */

static void constraints_update(struct twl_server *server);

static void pointer_send_frame(struct twl_seat *seat, struct twl_surface *s) {
	struct wl_resource *r;
	wl_resource_for_each(r, &seat->pointers)
		if (same_client(r, s) && wl_resource_get_version(r) >= WL_POINTER_FRAME_SINCE_VERSION)
			wl_pointer_send_frame(r);
}

static void pointer_set_focus(struct twl_server *server, struct twl_surface *s, double lx, double ly) {
	struct twl_seat *seat = seat_of(server);
	if (seat->pointer_focus == s)
		return;
	struct wl_resource *r;
	uint32_t serial = wl_display_next_serial(server->display);
	if (seat->pointer_focus) {
		wl_resource_for_each(r, &seat->pointers)
			if (same_client(r, seat->pointer_focus))
				wl_pointer_send_leave(r, serial, seat->pointer_focus->resource);
		pointer_send_frame(seat, seat->pointer_focus);
	}
	/* The client sets its cursor on enter; until then, none. */
	if (!s || !seat->pointer_focus || !same_client(s->resource, seat->pointer_focus))
		seat->cursor = NULL;
	seat->pointer_focus = s;
	if (s) {
		wl_resource_for_each(r, &seat->pointers)
			if (same_client(r, s))
				wl_pointer_send_enter(r, serial, s->resource, wl_fixed_from_double(lx), wl_fixed_from_double(ly));
		pointer_send_frame(seat, s);
	}
	constraints_update(server);
	twl_seat_cursor_changed(server);
	twl_server_schedule_repaint(server);
}

static struct constraint *active_lock(struct twl_server *server) {
	struct constraint *c;
	wl_list_for_each(c, &seat_of(server)->constraints, link)
		if (c->active && c->locked)
			return c;
	return NULL;
}

void twl_seat_pointer_motion(struct twl_server *server, double x, double y, uint32_t time) {
	struct twl_seat *seat = seat_of(server);
	int32_t ow = server->width_px / server->scale, oh = server->height_px / server->scale;
	x = TWL_MAX(0, TWL_MIN(x, ow - 1));
	y = TWL_MAX(0, TWL_MIN(y, oh - 1));
	double dx = x - seat->px, dy = y - seat->py;
	seat->px = x;
	seat->py = y;

	if (seat->moving) {
		seat->moving->x = (int32_t) (x - seat->move_dx);
		seat->moving->y = (int32_t) (y - seat->move_dy);
		twl_server_schedule_repaint(server);
		return;
	}

	struct twl_surface *focus;
	double lx, ly;
	if (seat->buttons_down && seat->pointer_focus) {
		/* implicit grab: stay with the surface the button went down on */
		int32_t ox, oy;
		focus = seat->pointer_focus;
		if (!surface_origin(focus, &ox, &oy))
			return;
		lx = x - ox;
		ly = y - oy;
	} else {
		focus = surface_at(server, x, y, &lx, &ly);
		pointer_set_focus(server, focus, lx, ly);
	}
	if (seat->cursor)
		twl_server_schedule_repaint(server);
	if (!focus)
		return;

	struct wl_resource *r;
	wl_resource_for_each(r, &seat->relative_pointers)
		if (same_client(r, focus))
			zwp_relative_pointer_v1_send_relative_motion(r, 0, time, wl_fixed_from_double(dx),
					wl_fixed_from_double(dy), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	if (active_lock(server)) {
		pointer_send_frame(seat, focus);
		return;
	}
	wl_resource_for_each(r, &seat->pointers)
		if (same_client(r, focus))
			wl_pointer_send_motion(r, time, wl_fixed_from_double(lx), wl_fixed_from_double(ly));
	pointer_send_frame(seat, focus);
}

void twl_seat_pointer_relative(struct twl_server *server, double dx, double dy, uint32_t time) {
	struct twl_seat *seat = seat_of(server);
	if (!active_lock(server)) {
		twl_seat_pointer_motion(server, seat->px + dx, seat->py + dy, time);
		return;
	}
	struct wl_resource *r;
	wl_resource_for_each(r, &seat->relative_pointers)
		if (same_client(r, seat->pointer_focus))
			zwp_relative_pointer_v1_send_relative_motion(r, 0, time, wl_fixed_from_double(dx),
					wl_fixed_from_double(dy), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	pointer_send_frame(seat, seat->pointer_focus);
}

void twl_seat_refocus_pointer(struct twl_server *server) {
	struct twl_seat *seat = seat_of(server);
	if (seat->buttons_down || seat->moving)
		return;
	double lx, ly;
	struct twl_surface *s = surface_at(server, seat->px, seat->py, &lx, &ly);
	pointer_set_focus(server, s, lx, ly);
}

void twl_seat_pointer_button(struct twl_server *server, uint32_t button, bool pressed, uint32_t time) {
	struct twl_seat *seat = seat_of(server);
	uint32_t bit = button >= BTN_LEFT && button < BTN_LEFT + 32 ? 1u << (button - BTN_LEFT) : 0;
	if (pressed) {
		if (bit & seat->buttons_down)
			return;
		if (!seat->buttons_down) {
			double lx, ly;
			struct twl_surface *hit = surface_at(server, seat->px, seat->py, &lx, &ly);
			press_at(server, hit);
			hit = surface_at(server, seat->px, seat->py, &lx, &ly); /* popups may be gone */
			pointer_set_focus(server, hit, lx, ly);
		}
		seat->buttons_down |= bit;
	} else {
		if (!(bit & seat->buttons_down))
			return;
		seat->buttons_down &= ~bit;
		if (seat->moving && !seat->buttons_down) {
			seat->moving = NULL;
			return;
		}
	}
	struct twl_surface *focus = seat->pointer_focus;
	if (focus) {
		uint32_t serial = wl_display_next_serial(server->display);
		struct wl_resource *r;
		wl_resource_for_each(r, &seat->pointers)
			if (same_client(r, focus))
				wl_pointer_send_button(r, serial, time, button,
						pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
		pointer_send_frame(seat, focus);
	}
	if (!seat->buttons_down)
		twl_seat_refocus_pointer(server);
}

void twl_seat_pointer_axis(struct twl_server *server, double dx, double dy, uint32_t time) {
	struct twl_seat *seat = seat_of(server);
	struct twl_surface *focus = seat->pointer_focus;
	if (!focus)
		return;
	struct wl_resource *r;
	double v[2] = {dy, dx};
	uint32_t axis[2] = {WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_HORIZONTAL_SCROLL};
	wl_resource_for_each(r, &seat->pointers) {
		if (!same_client(r, focus))
			continue;
		uint32_t version = wl_resource_get_version(r);
		if (version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
			wl_pointer_send_axis_source(r, WL_POINTER_AXIS_SOURCE_WHEEL);
		for (int i = 0; i < 2; i++) {
			if (v[i] == 0)
				continue;
			if (version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
				wl_pointer_send_axis_value120(r, axis[i], (int32_t) (v[i] * 120));
			else if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
				wl_pointer_send_axis_discrete(r, axis[i], v[i] > 0 ? 1 : -1);
			wl_pointer_send_axis(r, time, axis[i], wl_fixed_from_double(v[i] * 15));
		}
	}
	pointer_send_frame(seat, focus);
}

void twl_xdg_begin_move(struct twl_toplevel *t) {
	struct twl_seat *seat = &t->xdg->server->seat;
	seat->moving = t;
	seat->move_dx = seat->px - t->x;
	seat->move_dy = seat->py - t->y;
}

/* Cursor images come in buffer pixels and the frontend draws them in output pixels: scale them by
 * the output scale over the buffer's, or a scale 1 cursor (Xwayland's) is half size at scale 2. */
static void send_cursor(struct twl_server *server, struct twl_surface *s, int32_t hx, int32_t hy) {
	int32_t bs = s->scale > 0 ? s->scale : 1;
	int32_t k = server->scale > bs ? server->scale / bs : 1;
	int32_t w = s->cursor_w, h = s->cursor_h;
	uint8_t *scaled = k > 1 ? malloc((size_t) w * k * h * k * 4) : NULL;
	if (!scaled) {
		server->backend.set_cursor(server->backend.data, s->cursor_rgba, w, h, hx * bs, hy * bs);
		return;
	}
	for (int32_t y = 0; y < h * k; y++) {
		const uint32_t *src = (const uint32_t *) s->cursor_rgba + (size_t) (y / k) * w;
		uint32_t *dst = (uint32_t *) scaled + (size_t) y * w * k;
		for (int32_t x = 0; x < w * k; x++)
			dst[x] = src[x / k];
	}
	server->backend.set_cursor(server->backend.data, scaled, w * k, h * k, hx * bs * k, hy * bs * k);
	free(scaled);
}

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource, uint32_t serial,
		struct wl_resource *surface_resource, int32_t hx, int32_t hy) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct twl_seat *seat = seat_of(server);
	if (!seat->pointer_focus || !same_client(resource, seat->pointer_focus))
		return;
	struct twl_surface *s = twl_surface_from_resource(surface_resource);
	if (s && !twl_surface_set_role(s, TWL_ROLE_CURSOR, resource, WL_POINTER_ERROR_ROLE))
		return;
	seat->cursor = s;
	seat->cursor_hx = hx;
	seat->cursor_hy = hy;
	if (s && s->cursor_rgba && server->backend.set_cursor)
		send_cursor(server, s, hx, hy);
	twl_seat_cursor_changed(server);
	twl_server_schedule_repaint(server);
}

static void resource_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = pointer_set_cursor,
	.release = resource_release,
};

static void unlink_resource(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static bool cursor_shown(struct twl_server *server) {
	struct twl_seat *seat = seat_of(server);
	return seat->cursor && seat->cursor->has_buffer && seat->cursor_visible && seat->pointer_focus &&
			!active_lock(server);
}

void twl_seat_cursor_changed(struct twl_server *server) {
	struct twl_seat *seat = seat_of(server);
	if (!server->backend.move_cursor)
		return;
	int32_t S = server->scale;
	server->backend.move_cursor(server->backend.data, (int32_t) (seat->px * S), (int32_t) (seat->py * S),
			cursor_shown(server));
}

void twl_seat_cursor_committed(struct twl_server *server, struct twl_surface *s, const void *pixels, int32_t stride,
		int32_t width, int32_t height, uint32_t drm_format) {
	struct twl_seat *seat = seat_of(server);
	if (!server->backend.set_cursor)
		return;
	uint8_t *rgba = realloc(s->cursor_rgba, (size_t) width * height * 4);
	if (!rgba)
		return;
	s->cursor_rgba = rgba;
	s->cursor_w = width;
	s->cursor_h = height;
	bool bgra = drm_format == 0x34325241 || drm_format == 0x34325258; /* ARGB8888/XRGB8888: B,G,R,A bytes */
	bool opaque = drm_format == 0x34325258 || drm_format == 0x34324258;
	for (int32_t y = 0; y < height; y++) {
		const uint8_t *src = (const uint8_t *) pixels + (size_t) y * stride;
		uint8_t *dst = rgba + (size_t) y * width * 4;
		for (int32_t x = 0; x < width; x++, src += 4, dst += 4) {
			dst[0] = bgra ? src[2] : src[0];
			dst[1] = src[1];
			dst[2] = bgra ? src[0] : src[2];
			dst[3] = opaque ? 255 : src[3];
		}
	}
	if (s != seat->cursor)
		return;
	send_cursor(server, s, seat->cursor_hx, seat->cursor_hy);
	twl_seat_cursor_changed(server);
}

void twl_seat_draw_cursor(struct twl_server *server, void (*draw)(struct twl_surface *, int32_t, int32_t, void *),
		void *data) {
	struct twl_seat *seat = seat_of(server);
	if (server->backend.set_cursor || !cursor_shown(server))
		return;
	twl_surface_for_each(seat->cursor, (int32_t) seat->px - seat->cursor_hx, (int32_t) seat->py - seat->cursor_hy,
			draw, data);
}

/* ---- keyboard ---- */

static void text_inputs_focus(struct twl_server *server, struct twl_surface *s);
static void send_selection(struct twl_server *server, struct wl_client *client);

static void send_modifiers(struct twl_seat *seat, struct wl_resource *r, uint32_t serial) {
	wl_keyboard_send_modifiers(r, serial, seat->mods_depressed, 0, seat->mods_locked, 0);
}

void twl_seat_set_keyboard_focus(struct twl_server *server, struct twl_surface *s) {
	struct twl_seat *seat = seat_of(server);
	if (seat->keyboard_focus == s)
		return;
	struct wl_resource *r;
	uint32_t serial = wl_display_next_serial(server->display);
	if (seat->keyboard_focus)
		wl_resource_for_each(r, &seat->keyboards)
			if (same_client(r, seat->keyboard_focus))
				wl_keyboard_send_leave(r, serial, seat->keyboard_focus->resource);
	struct wl_client *old_client = seat->keyboard_focus ? wl_resource_get_client(seat->keyboard_focus->resource) : NULL;
	seat->keyboard_focus = s;
	if (s) {
		wl_resource_for_each(r, &seat->keyboards)
			if (same_client(r, s)) {
				wl_keyboard_send_enter(r, serial, s->resource, &seat->keys);
				send_modifiers(seat, r, serial);
			}
		if (wl_resource_get_client(s->resource) != old_client)
			send_selection(server, wl_resource_get_client(s->resource));
	}
	text_inputs_focus(server, s);
}

static uint32_t modifier_bit(uint32_t key) {
	switch (key) {
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		return MOD_SHIFT;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		return MOD_CTRL;
	case KEY_LEFTALT:
	case KEY_RIGHTALT:
		return MOD_ALT;
	case KEY_LEFTMETA:
	case KEY_RIGHTMETA:
		return MOD_SUPER;
	default:
		return 0;
	}
}

static void send_key(struct twl_server *server, uint32_t key, bool pressed, uint32_t time, bool mods_changed) {
	struct twl_seat *seat = seat_of(server);
	struct twl_surface *focus = seat->keyboard_focus;
	if (!focus)
		return;
	uint32_t serial = wl_display_next_serial(server->display);
	struct wl_resource *r;
	wl_resource_for_each(r, &seat->keyboards) {
		if (!same_client(r, focus))
			continue;
		wl_keyboard_send_key(r, serial, time, key,
				pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
		if (mods_changed)
			send_modifiers(seat, r, serial);
	}
}

void twl_seat_key(struct twl_server *server, uint32_t key, bool pressed, uint32_t time) {
	struct twl_seat *seat = seat_of(server);
	uint32_t *k;
	bool held = false;
	wl_array_for_each(k, &seat->keys)
		if (*k == key)
			held = true;
	if (held == pressed)
		return;

	/* Compositor shortcuts: Alt+Tab, Alt+Shift+Tab, Alt+F4. */
	if (pressed && (seat->mods_depressed & MOD_ALT) && (key == KEY_TAB || key == KEY_F4)) {
		if (key == KEY_TAB)
			twl_xdg_cycle(server, (seat->mods_depressed & MOD_SHIFT) ? 1 : 0);
		else
			twl_server_handle_event(server, &(struct twl_event){.type = TWL_EV_CLOSE_WINDOW}, NULL);
		return;
	}

	if (pressed) {
		k = wl_array_add(&seat->keys, sizeof(*k));
		if (k)
			*k = key;
	} else {
		uint32_t *end = (uint32_t *) ((char *) seat->keys.data + seat->keys.size);
		for (k = seat->keys.data; k < end; k++)
			if (*k == key) {
				*k = end[-1];
				seat->keys.size -= sizeof(*k);
				break;
			}
	}
	uint32_t old_depressed = seat->mods_depressed, old_locked = seat->mods_locked;
	uint32_t bit = modifier_bit(key);
	if (bit) {
		/* the bit stays while the other side's key is held */
		seat->mods_depressed &= ~bit;
		wl_array_for_each(k, &seat->keys)
			seat->mods_depressed |= modifier_bit(*k);
	}
	if (pressed && key == KEY_CAPSLOCK)
		seat->mods_locked ^= MOD_LOCK;
	if (pressed && key == KEY_NUMLOCK)
		seat->mods_locked ^= MOD_NUM;
	send_key(server, key, pressed, time, old_depressed != seat->mods_depressed || old_locked != seat->mods_locked);
}

void twl_seat_set_locks(struct twl_server *server, bool caps, bool num) {
	struct twl_seat *seat = seat_of(server);
	uint32_t locked = (caps ? MOD_LOCK : 0) | (num ? MOD_NUM : 0);
	if (locked == seat->mods_locked)
		return;
	seat->mods_locked = locked;
	struct twl_surface *focus = seat->keyboard_focus;
	if (!focus)
		return;
	uint32_t serial = wl_display_next_serial(server->display);
	struct wl_resource *r;
	wl_resource_for_each(r, &seat->keyboards)
		if (same_client(r, focus))
			send_modifiers(seat, r, serial);
}

void twl_seat_release_keys(struct twl_server *server, uint32_t time) {
	struct twl_seat *seat = seat_of(server);
	while (seat->keys.size)
		twl_seat_key(server, ((uint32_t *) seat->keys.data)[0], false, time);
	while (seat->buttons_down) {
		int b = __builtin_ctz(seat->buttons_down);
		twl_seat_pointer_button(server, BTN_LEFT + b, false, time);
	}
}

static const struct wl_keyboard_interface keyboard_impl = {
	.release = resource_release,
};

/* ---- text input: Android's soft keyboard ---- */

static struct text_input *focused_text_input(struct twl_server *server) {
	struct twl_seat *seat = seat_of(server);
	struct wl_resource *r;
	wl_resource_for_each(r, &seat->text_inputs) {
		struct text_input *ti = wl_resource_get_user_data(r);
		if (ti->enabled && ti->focus && ti->focus == seat->keyboard_focus)
			return ti;
	}
	return NULL;
}

static void update_soft_keyboard(struct twl_server *server) {
	struct twl_seat *seat = seat_of(server);
	bool enabled = focused_text_input(server) != NULL;
	if (enabled != seat->text_input_enabled) {
		seat->text_input_enabled = enabled;
		twl_server_send_to_frontend(server, TWL_EV_SHOW_KEYBOARD, enabled);
	}
}

void twl_seat_text(struct twl_server *server, const char *utf8, uint32_t time) {
	struct text_input *ti = focused_text_input(server);
	if (ti) {
		zwp_text_input_v3_send_commit_string(ti->resource, utf8);
		zwp_text_input_v3_send_done(ti->resource, ti->commits);
		return;
	}
	/* No text-input client: type it on the US keymap. */
	struct twl_seat *seat = seat_of(server);
	for (const unsigned char *c = (const unsigned char *) utf8; *c; c++) {
		uint32_t key;
		bool shift;
		if (!twl_ascii_to_key(*c, &key, &shift)) {
			if (*c >= 0x80 && (*c & 0xc0) == 0xc0)
				twl_info("can't type U+%02x... without a text-input client", *c);
			continue;
		}
		bool had_shift = seat->mods_depressed & MOD_SHIFT;
		if (shift && !had_shift)
			twl_seat_key(server, KEY_LEFTSHIFT, true, time);
		twl_seat_key(server, key, true, time);
		twl_seat_key(server, key, false, time);
		if (shift && !had_shift)
			twl_seat_key(server, KEY_LEFTSHIFT, false, time);
	}
}

static void text_inputs_focus(struct twl_server *server, struct twl_surface *s) {
	struct wl_resource *r;
	wl_resource_for_each(r, &seat_of(server)->text_inputs) {
		struct text_input *ti = wl_resource_get_user_data(r);
		if (ti->focus && ti->focus != s) {
			zwp_text_input_v3_send_leave(r, ti->focus->resource);
			ti->focus = NULL;
		}
		if (s && !ti->focus && same_client(r, s)) {
			ti->focus = s;
			zwp_text_input_v3_send_enter(r, s->resource);
		}
	}
	update_soft_keyboard(server);
}

static void ti_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void ti_enable(struct wl_client *client, struct wl_resource *resource) {
	((struct text_input *) wl_resource_get_user_data(resource))->pending_enabled = true;
}

static void ti_disable(struct wl_client *client, struct wl_resource *resource) {
	((struct text_input *) wl_resource_get_user_data(resource))->pending_enabled = false;
}

static void ti_set_surrounding_text(struct wl_client *client, struct wl_resource *resource, const char *text,
		int32_t cursor, int32_t anchor) {
}

static void ti_set_text_change_cause(struct wl_client *client, struct wl_resource *resource, uint32_t cause) {
}

static void ti_set_content_type(struct wl_client *client, struct wl_resource *resource, uint32_t hint,
		uint32_t purpose) {
}

static void ti_set_cursor_rectangle(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
		int32_t w, int32_t h) {
}

static void ti_commit(struct wl_client *client, struct wl_resource *resource) {
	struct text_input *ti = wl_resource_get_user_data(resource);
	ti->commits++;
	ti->enabled = ti->pending_enabled;
	update_soft_keyboard(ti->server);
}

static const struct zwp_text_input_v3_interface text_input_impl = {
	.destroy = ti_destroy,
	.enable = ti_enable,
	.disable = ti_disable,
	.set_surrounding_text = ti_set_surrounding_text,
	.set_text_change_cause = ti_set_text_change_cause,
	.set_content_type = ti_set_content_type,
	.set_cursor_rectangle = ti_set_cursor_rectangle,
	.commit = ti_commit,
};

static void ti_resource_destroy(struct wl_resource *resource) {
	struct text_input *ti = wl_resource_get_user_data(resource);
	wl_list_remove(wl_resource_get_link(resource));
	struct twl_server *server = ti->server;
	free(ti);
	update_soft_keyboard(server);
}

static void ti_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void ti_manager_get(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *seat_resource) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct text_input *ti = calloc(1, sizeof(*ti));
	struct wl_resource *r = wl_resource_create(client, &zwp_text_input_v3_interface, 1, id);
	if (!ti || !r) {
		free(ti);
		wl_client_post_no_memory(client);
		return;
	}
	ti->resource = r;
	ti->server = server;
	wl_resource_set_implementation(r, &text_input_impl, ti, ti_resource_destroy);
	wl_list_insert(&server->seat.text_inputs, wl_resource_get_link(r));
	struct twl_surface *focus = server->seat.keyboard_focus;
	if (focus && same_client(r, focus)) {
		ti->focus = focus;
		zwp_text_input_v3_send_enter(r, focus->resource);
	}
}

static const struct zwp_text_input_manager_v3_interface ti_manager_impl = {
	.destroy = ti_manager_destroy,
	.get_text_input = ti_manager_get,
};

static void ti_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *r = wl_resource_create(client, &zwp_text_input_manager_v3_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &ti_manager_impl, data, NULL);
}

/* ---- touch ---- */

static int touch_slot(struct twl_seat *seat, int32_t id, bool create) {
	for (int i = 0; i < TWL_MAX_TOUCH; i++)
		if (seat->touch[i].active && seat->touch[i].id == id)
			return i;
	if (!create)
		return -1;
	for (int i = 0; i < TWL_MAX_TOUCH; i++)
		if (!seat->touch[i].active)
			return i;
	return -1;
}

void twl_seat_touch(struct twl_server *server, uint32_t type, int32_t id, double x, double y, uint32_t time) {
	struct twl_seat *seat = seat_of(server);
	struct wl_resource *r;

	if (server->config.touch_pointer) {
		/* The first finger is a mouse with its left button held. */
		if (type == TWL_EV_TOUCH_DOWN && seat->emulating_touch < 0) {
			seat->emulating_touch = id;
			seat->cursor_visible = false;
			twl_seat_pointer_motion(server, x, y, time);
			twl_seat_pointer_button(server, BTN_LEFT, true, time);
		} else if (id == seat->emulating_touch) {
			twl_seat_pointer_motion(server, x, y, time);
			if (type == TWL_EV_TOUCH_UP || type == TWL_EV_TOUCH_CANCEL) {
				twl_seat_pointer_button(server, BTN_LEFT, false, time);
				seat->emulating_touch = -1;
			}
		}
		return;
	}

	if (type == TWL_EV_TOUCH_CANCEL) {
		for (int i = 0; i < TWL_MAX_TOUCH; i++) {
			if (!seat->touch[i].active)
				continue;
			if (seat->touch[i].surface)
				wl_resource_for_each(r, &seat->touches)
					if (same_client(r, seat->touch[i].surface))
						wl_touch_send_cancel(r);
			seat->touch[i].active = false;
			seat->touch[i].surface = NULL;
		}
		seat->moving = NULL;
		return;
	}

	int slot = touch_slot(seat, id, type == TWL_EV_TOUCH_DOWN);
	if (slot < 0)
		return;
	if (type == TWL_EV_TOUCH_DOWN) {
		double lx, ly;
		struct twl_surface *hit = surface_at(server, x, y, &lx, &ly);
		bool first = true;
		for (int i = 0; i < TWL_MAX_TOUCH; i++)
			first &= !seat->touch[i].active;
		if (first) {
			press_at(server, hit);
			hit = surface_at(server, x, y, &lx, &ly);
		}
		seat->touch[slot].active = true;
		seat->touch[slot].id = id;
		seat->touch[slot].surface = hit;
		seat->touch[slot].ox = x - lx;
		seat->touch[slot].oy = y - ly;
		/* Remember where the finger is for xdg_toplevel.move from a touch on a title bar. */
		seat->px = x;
		seat->py = y;
		seat->cursor_visible = false;
		twl_seat_cursor_changed(server);
		if (!hit)
			return;
		uint32_t serial = wl_display_next_serial(server->display);
		wl_resource_for_each(r, &seat->touches)
			if (same_client(r, hit))
				wl_touch_send_down(r, serial, time, hit->resource, id, wl_fixed_from_double(lx),
						wl_fixed_from_double(ly));
		return;
	}

	struct twl_surface *s = seat->touch[slot].surface;
	if (type == TWL_EV_TOUCH_MOTION) {
		if (seat->moving) {
			seat->moving->x = (int32_t) (x - seat->move_dx);
			seat->moving->y = (int32_t) (y - seat->move_dy);
			seat->px = x;
			seat->py = y;
			twl_server_schedule_repaint(server);
			return;
		}
		if (!s)
			return;
		int32_t ox, oy;
		double lx = x - seat->touch[slot].ox, ly = y - seat->touch[slot].oy;
		if (surface_origin(s, &ox, &oy)) {
			lx = x - ox;
			ly = y - oy;
		}
		wl_resource_for_each(r, &seat->touches)
			if (same_client(r, s))
				wl_touch_send_motion(r, time, id, wl_fixed_from_double(lx), wl_fixed_from_double(ly));
		return;
	}
	/* up */
	seat->touch[slot].active = false;
	seat->touch[slot].surface = NULL;
	seat->moving = NULL;
	if (!s)
		return;
	uint32_t serial = wl_display_next_serial(server->display);
	wl_resource_for_each(r, &seat->touches)
		if (same_client(r, s))
			wl_touch_send_up(r, serial, time, id);
	/* The frame for this up has to go to this surface's client even though the slot is free. */
	wl_resource_for_each(r, &seat->touches)
		if (same_client(r, s) && wl_resource_get_version(r) >= WL_TOUCH_FRAME_SINCE_VERSION)
			wl_touch_send_frame(r);
}

void twl_seat_touch_frame(struct twl_server *server) {
	struct twl_seat *seat = seat_of(server);
	struct wl_resource *r;
	wl_resource_for_each(r, &seat->touches) {
		bool has = false;
		for (int i = 0; i < TWL_MAX_TOUCH; i++)
			has |= seat->touch[i].active && same_client(r, seat->touch[i].surface);
		if (has)
			wl_touch_send_frame(r);
	}
}

static const struct wl_touch_interface touch_impl = {
	.release = resource_release,
};

/* ---- clipboard ---- */

static void offer_accept(struct wl_client *client, struct wl_resource *resource, uint32_t serial, const char *mime) {
}

struct own_write {
	int fd;
	char *text;
};

static void *own_writer(void *arg) {
	struct own_write *w = arg;
	size_t len = strlen(w->text), off = 0;
	while (off < len) {
		ssize_t n = write(w->fd, w->text + off, len - off);
		if (n <= 0)
			break;
		off += (size_t) n;
	}
	close(w->fd);
	free(w->text);
	free(w);
	return NULL;
}

static void offer_receive(struct wl_client *client, struct wl_resource *resource, const char *mime, int32_t fd) {
	struct wl_resource *source = wl_resource_get_user_data(resource);
	if (source) {
		wl_data_source_send_send(source, mime, fd);
		close(fd);
		return;
	}
	/* The Android clipboard: written from a thread, the client may read it slowly. */
	struct own_write *w = calloc(1, sizeof(*w));
	const char *text = own_selection_text;
	if (!w || !text) {
		free(w);
		close(fd);
		return;
	}
	w->fd = fd;
	w->text = strdup(text);
	pthread_t t;
	if (!w->text || pthread_create(&t, NULL, own_writer, w) != 0) {
		free(w->text);
		free(w);
		close(fd);
		return;
	}
	pthread_detach(t);
}

static void offer_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void offer_finish(struct wl_client *client, struct wl_resource *resource) {
}

static void offer_set_actions(struct wl_client *client, struct wl_resource *resource, uint32_t actions,
		uint32_t preferred) {
}

static const struct wl_data_offer_interface offer_impl = {
	.accept = offer_accept,
	.receive = offer_receive,
	.destroy = offer_destroy,
	.finish = offer_finish,
	.set_actions = offer_set_actions,
};

static const char *own_mimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"};

static void send_selection(struct twl_server *server, struct wl_client *client) {
	struct twl_seat *seat = seat_of(server);
	struct wl_resource *dev;
	wl_resource_for_each(dev, &seat->data_devices) {
		if (wl_resource_get_client(dev) != client)
			continue;
		if (!seat->selection && seat->own_text) {
			struct wl_resource *offer = wl_resource_create(client, &wl_data_offer_interface,
					wl_resource_get_version(dev), 0);
			if (!offer) {
				wl_client_post_no_memory(client);
				continue;
			}
			wl_resource_set_implementation(offer, &offer_impl, NULL, NULL);
			wl_data_device_send_data_offer(dev, offer);
			for (size_t i = 0; i < sizeof(own_mimes) / sizeof(own_mimes[0]); i++)
				wl_data_offer_send_offer(offer, own_mimes[i]);
			wl_data_device_send_selection(dev, offer);
			continue;
		}
		if (!seat->selection) {
			wl_data_device_send_selection(dev, NULL);
			continue;
		}
		struct wl_resource *offer = wl_resource_create(client, &wl_data_offer_interface,
				wl_resource_get_version(dev), 0);
		if (!offer) {
			wl_client_post_no_memory(client);
			continue;
		}
		wl_resource_set_implementation(offer, &offer_impl, seat->selection, NULL);
		wl_data_device_send_data_offer(dev, offer);
		struct data_source *src = wl_resource_get_user_data(seat->selection);
		char **mime;
		wl_array_for_each(mime, &src->mimes)
			wl_data_offer_send_offer(offer, *mime);
		wl_data_device_send_selection(dev, offer);
	}
}

static void selection_destroyed(struct wl_listener *listener, void *data) {
	struct twl_seat *seat = wl_container_of(listener, seat, selection_destroy);
	seat->selection = NULL;
	wl_list_remove(&seat->selection_destroy.link);
	wl_list_init(&seat->selection_destroy.link);
	if (seat->keyboard_focus)
		send_selection(seat->server, wl_resource_get_client(seat->keyboard_focus->resource));
}

static void source_offer(struct wl_client *client, struct wl_resource *resource, const char *mime) {
	struct data_source *src = wl_resource_get_user_data(resource);
	char **p = wl_array_add(&src->mimes, sizeof(*p));
	if (p)
		*p = strdup(mime);
}

static void source_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void source_set_actions(struct wl_client *client, struct wl_resource *resource, uint32_t actions) {
}

static const struct wl_data_source_interface source_impl = {
	.offer = source_offer,
	.destroy = source_destroy,
	.set_actions = source_set_actions,
};

static void source_resource_destroy(struct wl_resource *resource) {
	struct data_source *src = wl_resource_get_user_data(resource);
	char **mime;
	wl_array_for_each(mime, &src->mimes)
		free(*mime);
	wl_array_release(&src->mimes);
	free(src);
}

static void device_start_drag(struct wl_client *client, struct wl_resource *resource, struct wl_resource *source,
		struct wl_resource *origin, struct wl_resource *icon, uint32_t serial) {
	/* No drag and drop yet: the drag ends at once. */
	if (source)
		wl_data_source_send_cancelled(source);
}

/* ---- a client's text selection, read for Android's clipboard ---- */

static void clip_read_done(struct twl_server *server) {
	struct twl_seat *seat = seat_of(server);
	if (seat->clip_source)
		wl_event_source_remove(seat->clip_source);
	seat->clip_source = NULL;
	free(seat->clip_buf);
	seat->clip_buf = NULL;
	seat->clip_len = 0;
}

static int clip_readable(int fd, uint32_t mask, void *data) {
	struct twl_server *server = data;
	struct twl_seat *seat = seat_of(server);
	char buf[65536];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n > 0 && seat->clip_len + (size_t) n < 16 * 1024 * 1024) {
		char *p = realloc(seat->clip_buf, seat->clip_len + (size_t) n + 1);
		if (p) {
			memcpy(p + seat->clip_len, buf, (size_t) n);
			seat->clip_len += (size_t) n;
			p[seat->clip_len] = 0;
			seat->clip_buf = p;
			return 0;
		}
	}
	if (n < 0 && errno == EAGAIN)
		return 0;
	if (n == 0 && seat->clip_buf && server->backend.clipboard_text)
		server->backend.clipboard_text(server->backend.data, seat->clip_buf);
	close(fd);
	clip_read_done(server);
	return 0;
}

static void read_selection_for_frontend(struct twl_server *server, struct wl_resource *source) {
	struct twl_seat *seat = seat_of(server);
	clip_read_done(server);
	if (!server->backend.clipboard_text || !source)
		return;
	struct data_source *src = wl_resource_get_user_data(source);
	const char *mime = NULL;
	for (size_t i = 0; i < sizeof(own_mimes) / sizeof(own_mimes[0]) && !mime; i++) {
		char **m;
		wl_array_for_each(m, &src->mimes)
			if (!strcmp(*m, own_mimes[i])) {
				mime = *m;
				break;
			}
	}
	int p[2];
	if (!mime || pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	wl_data_source_send_send(source, mime, p[1]);
	close(p[1]);
	seat->clip_source = wl_event_loop_add_fd(server->loop, p[0], WL_EVENT_READABLE, clip_readable, server);
	if (!seat->clip_source)
		close(p[0]);
}

void twl_seat_set_text_selection(struct twl_server *server, const char *utf8) {
	struct twl_seat *seat = seat_of(server);
	if (seat->selection) {
		wl_data_source_send_cancelled(seat->selection);
		wl_list_remove(&seat->selection_destroy.link);
		wl_list_init(&seat->selection_destroy.link);
		seat->selection = NULL;
	}
	free(seat->own_text);
	seat->own_text = strdup(utf8);
	own_selection_text = seat->own_text;
	if (seat->keyboard_focus)
		send_selection(server, wl_resource_get_client(seat->keyboard_focus->resource));
}

static void device_set_selection(struct wl_client *client, struct wl_resource *resource, struct wl_resource *source,
		uint32_t serial) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct twl_seat *seat = seat_of(server);
	if (seat->selection == source)
		return;
	free(seat->own_text);
	seat->own_text = NULL;
	own_selection_text = NULL;
	read_selection_for_frontend(server, source);
	if (seat->selection) {
		wl_data_source_send_cancelled(seat->selection);
		wl_list_remove(&seat->selection_destroy.link);
		wl_list_init(&seat->selection_destroy.link);
	}
	seat->selection = source;
	if (source)
		wl_resource_add_destroy_listener(source, &seat->selection_destroy);
	if (seat->keyboard_focus)
		send_selection(server, wl_resource_get_client(seat->keyboard_focus->resource));
}

static const struct wl_data_device_interface device_impl = {
	.start_drag = device_start_drag,
	.set_selection = device_set_selection,
	.release = resource_release,
};

static void manager_create_source(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct data_source *src = calloc(1, sizeof(*src));
	struct wl_resource *r = wl_resource_create(client, &wl_data_source_interface, wl_resource_get_version(resource), id);
	if (!src || !r) {
		free(src);
		wl_client_post_no_memory(client);
		return;
	}
	src->resource = r;
	wl_array_init(&src->mimes);
	wl_resource_set_implementation(r, &source_impl, src, source_resource_destroy);
}

static void manager_get_device(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *seat_resource) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *r = wl_resource_create(client, &wl_data_device_interface, wl_resource_get_version(resource), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &device_impl, server, unlink_resource);
	wl_list_insert(&server->seat.data_devices, wl_resource_get_link(r));
	if (server->seat.keyboard_focus && same_client(r, server->seat.keyboard_focus))
		send_selection(server, client);
}

static const struct wl_data_device_manager_interface manager_impl = {
	.create_data_source = manager_create_source,
	.get_data_device = manager_get_device,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *r = wl_resource_create(client, &wl_data_device_manager_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

/* ---- relative pointer ---- */

static const struct zwp_relative_pointer_v1_interface relative_pointer_impl = {
	.destroy = resource_release,
};

static void relative_manager_get(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *pointer) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *r = wl_resource_create(client, &zwp_relative_pointer_v1_interface, 1, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &relative_pointer_impl, server, unlink_resource);
	wl_list_insert(&server->seat.relative_pointers, wl_resource_get_link(r));
}

static const struct zwp_relative_pointer_manager_v1_interface relative_manager_impl = {
	.destroy = resource_release,
	.get_relative_pointer = relative_manager_get,
};

static void relative_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *r = wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &relative_manager_impl, data, NULL);
}

/* ---- pointer constraints ---- */

static void constraints_update(struct twl_server *server) {
	struct twl_seat *seat = seat_of(server);
	bool was_locked = active_lock(server) != NULL;
	struct constraint *c, *tmp;
	wl_list_for_each_safe(c, tmp, &seat->constraints, link) {
		bool want = c->surface && c->surface == seat->pointer_focus;
		if (want == c->active)
			continue;
		c->active = want;
		if (c->locked) {
			if (want)
				zwp_locked_pointer_v1_send_locked(c->resource);
			else
				zwp_locked_pointer_v1_send_unlocked(c->resource);
		} else {
			if (want)
				zwp_confined_pointer_v1_send_confined(c->resource);
			else
				zwp_confined_pointer_v1_send_unconfined(c->resource);
		}
		if (!want && c->oneshot)
			c->surface = NULL; /* defunct */
	}
	bool locked = active_lock(server) != NULL;
	if (locked != was_locked)
		twl_server_send_to_frontend(server, TWL_EV_POINTER_CAPTURE, locked);
}

static void constraint_surface_destroyed(struct wl_listener *listener, void *data) {
	struct constraint *c = wl_container_of(listener, c, surface_destroy);
	wl_list_remove(&c->surface_destroy.link);
	wl_list_init(&c->surface_destroy.link);
	c->surface = NULL;
	c->active = false;
}

static void constraint_resource_destroy(struct wl_resource *resource) {
	struct constraint *c = wl_resource_get_user_data(resource);
	struct twl_server *server = c->server;
	bool was = c->active && c->locked;
	wl_list_remove(&c->link);
	wl_list_remove(&c->surface_destroy.link);
	free(c);
	if (was && !active_lock(server))
		twl_server_send_to_frontend(server, TWL_EV_POINTER_CAPTURE, 0);
}

static void constraint_noop_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
}

static void locked_set_hint(struct wl_client *client, struct wl_resource *resource, wl_fixed_t x, wl_fixed_t y) {
}

static const struct zwp_locked_pointer_v1_interface locked_impl = {
	.destroy = resource_release,
	.set_cursor_position_hint = locked_set_hint,
	.set_region = constraint_noop_region,
};

static const struct zwp_confined_pointer_v1_interface confined_impl = {
	.destroy = resource_release,
	.set_region = constraint_noop_region,
};

static void constraints_create(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface, uint32_t lifetime, bool locked) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct twl_surface *s = twl_surface_from_resource(surface);
	struct constraint *c;
	wl_list_for_each(c, &server->seat.constraints, link)
		if (c->surface == s) {
			wl_resource_post_error(resource, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
					"surface already constrained");
			return;
		}
	c = calloc(1, sizeof(*c));
	struct wl_resource *r = wl_resource_create(client,
			locked ? &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface, 1, id);
	if (!c || !r) {
		free(c);
		wl_client_post_no_memory(client);
		return;
	}
	c->resource = r;
	c->server = server;
	c->surface = s;
	c->locked = locked;
	c->oneshot = lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT;
	c->surface_destroy.notify = constraint_surface_destroyed;
	wl_signal_add(&s->destroy_signal, &c->surface_destroy);
	wl_list_insert(&server->seat.constraints, &c->link);
	wl_resource_set_implementation(r, locked ? (const void *) &locked_impl : (const void *) &confined_impl, c,
			constraint_resource_destroy);
	constraints_update(server);
}

static void constraints_lock(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface, struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime) {
	constraints_create(client, resource, id, surface, lifetime, true);
}

static void constraints_confine(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface, struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime) {
	constraints_create(client, resource, id, surface, lifetime, false);
}

static const struct zwp_pointer_constraints_v1_interface constraints_impl = {
	.destroy = resource_release,
	.lock_pointer = constraints_lock,
	.confine_pointer = constraints_confine,
};

static void constraints_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *r = wl_resource_create(client, &zwp_pointer_constraints_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &constraints_impl, data, NULL);
}

/* ---- wl_seat ---- */

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *r = wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &pointer_impl, server, unlink_resource);
	wl_list_insert(&server->seat.pointers, wl_resource_get_link(r));
	/* The surface can have the focus already (Xwayland maps its window before it gets its
	 * pointer): without an enter the client drops every motion and button. */
	struct twl_surface *focus = server->seat.pointer_focus;
	int32_t ox, oy;
	if (focus && same_client(r, focus) && surface_origin(focus, &ox, &oy)) {
		wl_pointer_send_enter(r, wl_display_next_serial(server->display), focus->resource,
				wl_fixed_from_double(server->seat.px - ox), wl_fixed_from_double(server->seat.py - oy));
		if (wl_resource_get_version(r) >= WL_POINTER_FRAME_SINCE_VERSION)
			wl_pointer_send_frame(r);
	}
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct twl_seat *seat = &server->seat;
	struct wl_resource *r = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &keyboard_impl, server, unlink_resource);
	wl_list_insert(&seat->keyboards, wl_resource_get_link(r));
	wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, seat->keymap_fd, seat->keymap_size);
	if (wl_resource_get_version(r) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
		wl_keyboard_send_repeat_info(r, 25, 600);
	if (seat->keyboard_focus && same_client(r, seat->keyboard_focus)) {
		uint32_t serial = wl_display_next_serial(server->display);
		wl_keyboard_send_enter(r, serial, seat->keyboard_focus->resource, &seat->keys);
		send_modifiers(seat, r, serial);
	}
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *r = wl_resource_create(client, &wl_touch_interface, wl_resource_get_version(resource), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &touch_impl, server, unlink_resource);
	wl_list_insert(&server->seat.touches, wl_resource_get_link(r));
}

static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = resource_release,
};

static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *r = wl_resource_create(client, &wl_seat_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &seat_impl, data, NULL);
	wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
	if (version >= WL_SEAT_NAME_SINCE_VERSION)
		wl_seat_send_name(r, "seat0");
}

void twl_seat_surface_unmapped(struct twl_server *server, struct twl_surface *s) {
	struct twl_seat *seat = seat_of(server);
	/* s or anything below it in its tree */
#define IN_TREE(f) ((f) && ((f) == s || twl_surface_root(f) == s))
	if (IN_TREE(seat->pointer_focus)) {
		seat->buttons_down = 0;
		pointer_set_focus(server, NULL, 0, 0);
	}
	if (seat->cursor == s) {
		seat->cursor = NULL;
		twl_seat_cursor_changed(server);
	}
	if (IN_TREE(seat->keyboard_focus))
		twl_seat_set_keyboard_focus(server, NULL);
	for (int i = 0; i < TWL_MAX_TOUCH; i++)
		if (IN_TREE(seat->touch[i].surface))
			seat->touch[i].surface = NULL;
#undef IN_TREE
}

void twl_seat_init(struct twl_server *server) {
	struct twl_seat *seat = &server->seat;
	memset(seat, 0, sizeof(*seat));
	seat->server = server;
	seat->emulating_touch = -1;
	wl_list_init(&seat->pointers);
	wl_list_init(&seat->keyboards);
	wl_list_init(&seat->touches);
	wl_list_init(&seat->data_devices);
	wl_list_init(&seat->text_inputs);
	wl_list_init(&seat->relative_pointers);
	wl_list_init(&seat->constraints);
	wl_list_init(&seat->selection_destroy.link);
	seat->selection_destroy.notify = selection_destroyed;
	wl_array_init(&seat->keys);

	/* The keymap, from --keymap or built in (us). */
	const char *keymap = twl_keymap_string();
	char *loaded = NULL;
	if (server->config.keymap_file) {
		FILE *f = fopen(server->config.keymap_file, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long size = ftell(f);
			fseek(f, 0, SEEK_SET);
			loaded = calloc(1, size + 1);
			if (loaded && fread(loaded, 1, size, f) == (size_t) size)
				keymap = loaded;
			fclose(f);
		} else {
			twl_error("keymap %s: can't open it, using us", server->config.keymap_file);
		}
	}
	seat->keymap_size = strlen(keymap) + 1;
	seat->keymap_fd = twl_create_anon_file(seat->keymap_size);
	if (seat->keymap_fd >= 0)
		pwrite(seat->keymap_fd, keymap, seat->keymap_size, 0);
	free(loaded);

	wl_global_create(server->display, &wl_seat_interface, 8, server, seat_bind);
	wl_global_create(server->display, &wl_data_device_manager_interface, 3, server, manager_bind);
	wl_global_create(server->display, &zwp_text_input_manager_v3_interface, 1, server, ti_manager_bind);
	wl_global_create(server->display, &zwp_relative_pointer_manager_v1_interface, 1, server, relative_manager_bind);
	wl_global_create(server->display, &zwp_pointer_constraints_v1_interface, 1, server, constraints_bind);
}
