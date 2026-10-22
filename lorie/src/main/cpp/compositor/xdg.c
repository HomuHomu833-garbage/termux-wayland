/*
 * xdg-shell and the window policy of a phone screen: top-level windows fill the output
 * (maximized, or fullscreen when asked), dialogs and fixed-size windows float centered,
 * the focused window is on top. Popups (menus, tooltips) are placed by their positioner and
 * kept inside the output.
 */
#include <stdlib.h>
#include <string.h>
#include "twl.h"
#include "xdg-shell-protocol.h"
#include "xdg-decoration-unstable-v1-protocol.h"

struct positioner {
	int32_t w, h;
	struct twl_box anchor_rect;
	uint32_t anchor, gravity, constraint;
	int32_t ox, oy;
};

/* ---- geometry helpers ---- */

static struct twl_box window_geometry(struct twl_xdg_surface *xdg) {
	if (xdg->has_geometry)
		return xdg->geometry;
	if (xdg->surface)
		return (struct twl_box){0, 0, xdg->surface->width, xdg->surface->height};
	return (struct twl_box){0, 0, 0, 0};
}

static int32_t output_w(struct twl_server *server) {
	return server->width_px / server->scale;
}

static int32_t output_h(struct twl_server *server) {
	return server->height_px / server->scale;
}

bool twl_xdg_window_position(struct twl_xdg_surface *xdg, int32_t *x, int32_t *y) {
	if (!xdg || !xdg->mapped)
		return false;
	if (xdg->toplevel) {
		*x = xdg->toplevel->x;
		*y = xdg->toplevel->y;
		return true;
	}
	if (xdg->popup && xdg->popup->parent) {
		int32_t px, py;
		if (!twl_xdg_window_position(xdg->popup->parent, &px, &py))
			return false;
		*x = px + xdg->popup->geometry.x;
		*y = py + xdg->popup->geometry.y;
		return true;
	}
	return false;
}

bool twl_xdg_surface_position(struct twl_xdg_surface *xdg, int32_t *x, int32_t *y) {
	if (!twl_xdg_window_position(xdg, x, y))
		return false;
	struct twl_box g = window_geometry(xdg);
	*x -= g.x;
	*y -= g.y;
	return true;
}

/* ---- configure ---- */

static bool toplevel_floating(struct twl_toplevel *t) {
	if (t->fullscreen_requested)
		return false;
	if (t->parent)
		return true;
	return t->min_w > 0 && t->min_w == t->max_w && t->min_h > 0 && t->min_h == t->max_h;
}

static void add_state(struct wl_array *states, uint32_t state) {
	uint32_t *p = wl_array_add(states, sizeof(*p));
	if (p)
		*p = state;
}

static void send_configure(struct twl_xdg_surface *xdg) {
	struct twl_server *server = xdg->server;
	if (xdg->toplevel) {
		struct twl_toplevel *t = xdg->toplevel;
		struct wl_array states;
		wl_array_init(&states);
		int32_t w = 0, h = 0;
		uint32_t version = wl_resource_get_version(t->resource);
		t->floating = toplevel_floating(t);
		if (!t->floating) {
			w = output_w(server);
			h = output_h(server);
			if (t->fullscreen_requested) {
				add_state(&states, XDG_TOPLEVEL_STATE_FULLSCREEN);
			} else {
				add_state(&states, XDG_TOPLEVEL_STATE_MAXIMIZED);
				if (version >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION) {
					add_state(&states, XDG_TOPLEVEL_STATE_TILED_LEFT);
					add_state(&states, XDG_TOPLEVEL_STATE_TILED_RIGHT);
					add_state(&states, XDG_TOPLEVEL_STATE_TILED_TOP);
					add_state(&states, XDG_TOPLEVEL_STATE_TILED_BOTTOM);
				}
			}
		}
		if (server->focused == t)
			add_state(&states, XDG_TOPLEVEL_STATE_ACTIVATED);
		if (version >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
			xdg_toplevel_send_configure_bounds(t->resource, output_w(server), output_h(server));
		if (version >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION && !xdg->initial_commit_done) {
			struct wl_array caps;
			wl_array_init(&caps);
			add_state(&caps, XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
			add_state(&caps, XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
			xdg_toplevel_send_wm_capabilities(t->resource, &caps);
			wl_array_release(&caps);
		}
		xdg_toplevel_send_configure(t->resource, w, h, &states);
		t->configured_w = w;
		t->configured_h = h;
		wl_array_release(&states);
		if (t->decoration)
			zxdg_toplevel_decoration_v1_send_configure(t->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	} else if (xdg->popup) {
		struct twl_box *g = &xdg->popup->geometry;
		xdg_popup_send_configure(xdg->popup->resource, g->x, g->y, g->w, g->h);
	} else {
		return;
	}
	xdg->configure_serial = wl_display_next_serial(server->display);
	xdg_surface_send_configure(xdg->resource, xdg->configure_serial);
}

static void configure_idle(void *data) {
	struct twl_xdg_surface *xdg = data;
	xdg->configure_idle = NULL;
	send_configure(xdg);
}

static void schedule_configure(struct twl_xdg_surface *xdg) {
	if (!xdg->initial_commit_done || xdg->configure_idle)
		return;
	xdg->configure_idle = wl_event_loop_add_idle(xdg->server->loop, configure_idle, xdg);
}

void twl_xdg_reconfigure_all(struct twl_server *server) {
	struct twl_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		t->placed = false;
		schedule_configure(t->xdg);
	}
}

/* ---- focus and stacking ---- */

static struct twl_toplevel *top_mapped(struct twl_server *server) {
	struct twl_toplevel *t;
	wl_list_for_each_reverse(t, &server->toplevels, link)
		if (t->xdg->mapped)
			return t;
	return NULL;
}

struct twl_toplevel *twl_xdg_focused_toplevel(struct twl_server *server) {
	return server->focused;
}

void twl_xdg_update_focus(struct twl_server *server) {
	struct twl_toplevel *top = top_mapped(server), *old = server->focused;
	if (top != old) {
		server->focused = top;
		if (old)
			schedule_configure(old->xdg);
		if (top)
			schedule_configure(top->xdg);
	}
	/* Keyboard: the topmost grabbing popup, else the focused window. */
	struct twl_surface *kb = top ? top->xdg->surface : NULL;
	struct twl_popup *p;
	wl_list_for_each_reverse(p, &server->popups, link)
		if (p->grab && p->xdg->mapped) {
			kb = p->xdg->surface;
			break;
		}
	twl_seat_set_keyboard_focus(server, kb);
	twl_server_schedule_repaint(server);
}

void twl_xdg_raise(struct twl_toplevel *t) {
	struct twl_server *server = t->xdg->server;
	wl_list_remove(&t->link);
	wl_list_insert(server->toplevels.prev, &t->link);
	twl_xdg_update_focus(server);
}

void twl_xdg_cycle(struct twl_server *server, int direction) {
	int mapped = 0;
	struct twl_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link)
		mapped += t->xdg->mapped;
	if (mapped < 2)
		return;
	twl_xdg_close_popups(server);
	if (direction == 0) {
		/* top to the bottom */
		struct twl_toplevel *top = top_mapped(server);
		wl_list_remove(&top->link);
		wl_list_insert(&server->toplevels, &top->link);
	} else {
		/* lowest mapped to the top */
		wl_list_for_each(t, &server->toplevels, link)
			if (t->xdg->mapped)
				break;
		wl_list_remove(&t->link);
		wl_list_insert(server->toplevels.prev, &t->link);
	}
	twl_xdg_update_focus(server);
	twl_seat_refocus_pointer(server);
}

static void place_toplevel(struct twl_toplevel *t) {
	struct twl_server *server = t->xdg->server;
	struct twl_box g = window_geometry(t->xdg);
	if (!t->floating) {
		t->x = 0;
		t->y = 0;
	} else if (!t->placed) {
		int32_t cx = output_w(server) / 2, cy = output_h(server) / 2;
		if (t->parent && t->parent->xdg->mapped) {
			struct twl_box pg = window_geometry(t->parent->xdg);
			cx = t->parent->x + pg.w / 2;
			cy = t->parent->y + pg.h / 2;
		}
		t->x = cx - g.w / 2;
		t->y = cy - g.h / 2;
	}
	/* Keep the title bar reachable. */
	if (t->x + g.w > output_w(server))
		t->x = output_w(server) - g.w;
	if (t->y + g.h > output_h(server))
		t->y = output_h(server) - g.h;
	if (t->x < 0)
		t->x = 0;
	if (t->y < 0)
		t->y = 0;
	t->placed = true;
}

/* ---- popups ---- */

static void popup_unmap(struct twl_popup *p);

static void popup_dismiss(struct twl_popup *p) {
	if (!p->resource)
		return;
	xdg_popup_send_popup_done(p->resource);
	popup_unmap(p);
}

/* Dismisses every popup whose parent chain contains xdg. */
static void dismiss_children(struct twl_server *server, struct twl_xdg_surface *xdg) {
	struct twl_popup *p, *tmp;
	wl_list_for_each_reverse_safe(p, tmp, &server->popups, link) {
		struct twl_xdg_surface *a = p->parent;
		while (a && a != xdg)
			a = a->popup ? a->popup->parent : NULL;
		if (a == xdg && p->xdg->mapped)
			popup_dismiss(p);
	}
}

void twl_xdg_close_popups(struct twl_server *server) {
	struct twl_popup *p, *tmp;
	wl_list_for_each_reverse_safe(p, tmp, &server->popups, link)
		if (p->xdg->mapped && p->grab)
			popup_dismiss(p);
}

static void popup_unmap(struct twl_popup *p) {
	struct twl_xdg_surface *xdg = p->xdg;
	if (!xdg->mapped)
		return;
	dismiss_children(xdg->server, xdg);
	xdg->mapped = false;
	if (xdg->surface)
		twl_seat_surface_unmapped(xdg->server, xdg->surface);
	twl_xdg_update_focus(xdg->server);
}

static int32_t anchor_x(const struct positioner *pos, uint32_t anchor) {
	const struct twl_box *r = &pos->anchor_rect;
	switch (anchor) {
	case XDG_POSITIONER_ANCHOR_LEFT:
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
		return r->x;
	case XDG_POSITIONER_ANCHOR_RIGHT:
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		return r->x + r->w;
	default:
		return r->x + r->w / 2;
	}
}

static int32_t anchor_y(const struct positioner *pos, uint32_t anchor) {
	const struct twl_box *r = &pos->anchor_rect;
	switch (anchor) {
	case XDG_POSITIONER_ANCHOR_TOP:
	case XDG_POSITIONER_ANCHOR_TOP_LEFT:
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
		return r->y;
	case XDG_POSITIONER_ANCHOR_BOTTOM:
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		return r->y + r->h;
	default:
		return r->y + r->h / 2;
	}
}

/* Popup x for a gravity (the direction the popup extends from the anchor point). */
static int32_t gravity_x(int32_t ax, int32_t w, uint32_t gravity) {
	switch (gravity) {
	case XDG_POSITIONER_GRAVITY_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
		return ax - w;
	case XDG_POSITIONER_GRAVITY_RIGHT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		return ax;
	default:
		return ax - w / 2;
	}
}

static int32_t gravity_y(int32_t ay, int32_t h, uint32_t gravity) {
	switch (gravity) {
	case XDG_POSITIONER_GRAVITY_TOP:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		return ay - h;
	case XDG_POSITIONER_GRAVITY_BOTTOM:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		return ay;
	default:
		return ay - h / 2;
	}
}

static uint32_t flip_anchor_x(uint32_t a) {
	switch (a) {
	case XDG_POSITIONER_ANCHOR_LEFT: return XDG_POSITIONER_ANCHOR_RIGHT;
	case XDG_POSITIONER_ANCHOR_RIGHT: return XDG_POSITIONER_ANCHOR_LEFT;
	case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
	default: return a;
	}
}

static uint32_t flip_anchor_y(uint32_t a) {
	switch (a) {
	case XDG_POSITIONER_ANCHOR_TOP: return XDG_POSITIONER_ANCHOR_BOTTOM;
	case XDG_POSITIONER_ANCHOR_BOTTOM: return XDG_POSITIONER_ANCHOR_TOP;
	case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
	default: return a;
	}
}

/* Anchors and gravities share their numbering. */
#define flip_gravity_x flip_anchor_x
#define flip_gravity_y flip_anchor_y

static void popup_place(struct twl_popup *p, const struct positioner *pos) {
	struct twl_server *server = p->xdg->server;
	int32_t px = 0, py = 0;
	if (p->parent)
		twl_xdg_window_position(p->parent, &px, &py);
	int32_t ow = output_w(server), oh = output_h(server);
	int32_t w = pos->w, h = pos->h;

	int32_t x = gravity_x(anchor_x(pos, pos->anchor), w, pos->gravity) + pos->ox;
	int32_t y = gravity_y(anchor_y(pos, pos->anchor), h, pos->gravity) + pos->oy;

	/* x: flip, then slide, then resize */
	if (px + x < 0 || px + x + w > ow) {
		if (pos->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X) {
			int32_t fx = gravity_x(anchor_x(pos, flip_anchor_x(pos->anchor)), w, flip_gravity_x(pos->gravity)) - pos->ox;
			if (px + fx >= 0 && px + fx + w <= ow)
				x = fx;
		}
		if ((px + x < 0 || px + x + w > ow) && (pos->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X)) {
			if (px + x + w > ow)
				x = ow - w - px;
			if (px + x < 0)
				x = -px;
		}
		if ((px + x < 0 || px + x + w > ow) && (pos->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X)) {
			int32_t x1 = TWL_MAX(px + x, 0), x2 = TWL_MIN(px + x + w, ow);
			if (x2 > x1) {
				x = x1 - px;
				w = x2 - x1;
			}
		}
	}
	if (py + y < 0 || py + y + h > oh) {
		if (pos->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y) {
			int32_t fy = gravity_y(anchor_y(pos, flip_anchor_y(pos->anchor)), h, flip_gravity_y(pos->gravity)) - pos->oy;
			if (py + fy >= 0 && py + fy + h <= oh)
				y = fy;
		}
		if ((py + y < 0 || py + y + h > oh) && (pos->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y)) {
			if (py + y + h > oh)
				y = oh - h - py;
			if (py + y < 0)
				y = -py;
		}
		if ((py + y < 0 || py + y + h > oh) && (pos->constraint & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y)) {
			int32_t y1 = TWL_MAX(py + y, 0), y2 = TWL_MIN(py + y + h, oh);
			if (y2 > y1) {
				y = y1 - py;
				h = y2 - y1;
			}
		}
	}
	p->geometry = (struct twl_box){x, y, w, h};
}

/* ---- xdg_positioner ---- */

static void positioner_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void positioner_set_size(struct wl_client *client, struct wl_resource *resource, int32_t w, int32_t h) {
	struct positioner *p = wl_resource_get_user_data(resource);
	if (w <= 0 || h <= 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid size");
		return;
	}
	p->w = w;
	p->h = h;
}

static void positioner_set_anchor_rect(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
		int32_t w, int32_t h) {
	struct positioner *p = wl_resource_get_user_data(resource);
	if (w < 0 || h < 0) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor rect");
		return;
	}
	p->anchor_rect = (struct twl_box){x, y, w, h};
}

static void positioner_set_anchor(struct wl_client *client, struct wl_resource *resource, uint32_t anchor) {
	struct positioner *p = wl_resource_get_user_data(resource);
	if (anchor > XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor");
		return;
	}
	p->anchor = anchor;
}

static void positioner_set_gravity(struct wl_client *client, struct wl_resource *resource, uint32_t gravity) {
	struct positioner *p = wl_resource_get_user_data(resource);
	if (gravity > XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT) {
		wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid gravity");
		return;
	}
	p->gravity = gravity;
}

static void positioner_set_constraint_adjustment(struct wl_client *client, struct wl_resource *resource,
		uint32_t constraint) {
	((struct positioner *) wl_resource_get_user_data(resource))->constraint = constraint;
}

static void positioner_set_offset(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
	struct positioner *p = wl_resource_get_user_data(resource);
	p->ox = x;
	p->oy = y;
}

static void positioner_set_reactive(struct wl_client *client, struct wl_resource *resource) {
}

static void positioner_set_parent_size(struct wl_client *client, struct wl_resource *resource, int32_t w, int32_t h) {
}

static void positioner_set_parent_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
}

static const struct xdg_positioner_interface positioner_impl = {
	.destroy = positioner_destroy,
	.set_size = positioner_set_size,
	.set_anchor_rect = positioner_set_anchor_rect,
	.set_anchor = positioner_set_anchor,
	.set_gravity = positioner_set_gravity,
	.set_constraint_adjustment = positioner_set_constraint_adjustment,
	.set_offset = positioner_set_offset,
	.set_reactive = positioner_set_reactive,
	.set_parent_size = positioner_set_parent_size,
	.set_parent_configure = positioner_set_parent_configure,
};

static void positioner_resource_destroy(struct wl_resource *resource) {
	free(wl_resource_get_user_data(resource));
}

/* ---- xdg_popup ---- */

static void popup_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void popup_grab(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat,
		uint32_t serial) {
	struct twl_popup *p = wl_resource_get_user_data(resource);
	if (!p)
		return;
	if (p->committed_once) {
		wl_resource_post_error(resource, XDG_POPUP_ERROR_INVALID_GRAB, "grab after the popup was mapped");
		return;
	}
	p->grab = true;
}

static void popup_reposition(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *positioner_resource, uint32_t token) {
	struct twl_popup *p = wl_resource_get_user_data(resource);
	if (!p)
		return;
	popup_place(p, wl_resource_get_user_data(positioner_resource));
	xdg_popup_send_repositioned(resource, token);
	send_configure(p->xdg);
	twl_server_schedule_repaint(p->xdg->server);
}

static const struct xdg_popup_interface popup_impl = {
	.destroy = popup_destroy,
	.grab = popup_grab,
	.reposition = popup_reposition,
};

static void popup_free(struct twl_popup *p) {
	popup_unmap(p);
	wl_list_remove(&p->link);
	p->xdg->popup = NULL;
	free(p);
}

static void popup_resource_destroy(struct wl_resource *resource) {
	struct twl_popup *p = wl_resource_get_user_data(resource);
	if (p)
		popup_free(p);
}

/* ---- xdg_toplevel ---- */

static void toplevel_unmap(struct twl_toplevel *t) {
	struct twl_xdg_surface *xdg = t->xdg;
	struct twl_server *server = xdg->server;
	if (!xdg->mapped)
		return;
	dismiss_children(server, xdg);
	xdg->mapped = false;
	if (server->seat.moving == t)
		server->seat.moving = NULL;
	if (xdg->surface)
		twl_seat_surface_unmapped(server, xdg->surface);
	twl_xdg_update_focus(server);
	twl_seat_refocus_pointer(server);
}

static void toplevel_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void toplevel_set_parent(struct wl_client *client, struct wl_resource *resource, struct wl_resource *parent) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (!t)
		return;
	t->parent = parent ? wl_resource_get_user_data(parent) : NULL;
	if (t->parent == t)
		t->parent = NULL;
	schedule_configure(t->xdg);
}

static void toplevel_set_title(struct wl_client *client, struct wl_resource *resource, const char *title) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (!t)
		return;
	free(t->title);
	t->title = strdup(title);
}

static void toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource, const char *app_id) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (!t)
		return;
	free(t->app_id);
	t->app_id = strdup(app_id);
}

static void toplevel_show_window_menu(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
}

static void toplevel_move(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat,
		uint32_t serial) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (t && t->floating && t->xdg->mapped)
		twl_xdg_begin_move(t);
}

static void toplevel_resize(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat,
		uint32_t serial, uint32_t edges) {
}

static void toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource, int32_t w, int32_t h) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (!t)
		return;
	t->max_w = w;
	t->max_h = h;
}

static void toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource, int32_t w, int32_t h) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (!t)
		return;
	t->min_w = w;
	t->min_h = h;
}

static void toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (t)
		schedule_configure(t->xdg); /* always maximized */
}

static void toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *output) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (!t)
		return;
	t->fullscreen_requested = true;
	schedule_configure(t->xdg);
}

static void toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (!t)
		return;
	t->fullscreen_requested = false;
	schedule_configure(t->xdg);
}

static void toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (!t || !t->xdg->mapped)
		return;
	struct twl_server *server = t->xdg->server;
	wl_list_remove(&t->link);
	wl_list_insert(&server->toplevels, &t->link);
	twl_xdg_update_focus(server);
	twl_seat_refocus_pointer(server);
}

static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = toplevel_destroy,
	.set_parent = toplevel_set_parent,
	.set_title = toplevel_set_title,
	.set_app_id = toplevel_set_app_id,
	.show_window_menu = toplevel_show_window_menu,
	.move = toplevel_move,
	.resize = toplevel_resize,
	.set_max_size = toplevel_set_max_size,
	.set_min_size = toplevel_set_min_size,
	.set_maximized = toplevel_set_maximized,
	.unset_maximized = toplevel_set_maximized,
	.set_fullscreen = toplevel_set_fullscreen,
	.unset_fullscreen = toplevel_unset_fullscreen,
	.set_minimized = toplevel_set_minimized,
};

static void toplevel_free(struct twl_toplevel *t) {
	struct twl_server *server = t->xdg->server;
	toplevel_unmap(t);
	struct twl_toplevel *other;
	wl_list_for_each(other, &server->toplevels, link)
		if (other->parent == t)
			other->parent = NULL;
	if (t->decoration)
		wl_resource_set_user_data(t->decoration, NULL);
	wl_list_remove(&t->link);
	if (server->focused == t)
		server->focused = NULL;
	t->xdg->toplevel = NULL;
	free(t->title);
	free(t->app_id);
	free(t);
	twl_xdg_update_focus(server);
}

static void toplevel_resource_destroy(struct wl_resource *resource) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (t)
		toplevel_free(t);
}

/* ---- xdg_surface ---- */

static void xdg_unmap(struct twl_xdg_surface *xdg) {
	if (xdg->toplevel)
		toplevel_unmap(xdg->toplevel);
	else if (xdg->popup)
		popup_unmap(xdg->popup);
}

void twl_xdg_surface_commit(struct twl_surface *s) {
	struct twl_xdg_surface *xdg = s->role_data;
	if (!xdg)
		return;
	if (xdg->pending_has_geometry) {
		xdg->geometry = xdg->pending_geometry;
		xdg->has_geometry = true;
	}
	if (!xdg->toplevel && !xdg->popup)
		return;
	if (!xdg->initial_commit_done) {
		xdg->initial_commit_done = true;
		if (xdg->popup)
			xdg->popup->committed_once = true;
		send_configure(xdg);
		return;
	}
	if (!s->has_buffer) {
		if (xdg->mapped) {
			xdg_unmap(xdg);
			/* Unmapped: the next commit starts over with an initial configure. */
			xdg->initial_commit_done = false;
			xdg->configured = false;
		}
		return;
	}
	if (!xdg->configured)
		return;
	struct twl_server *server = xdg->server;
	if (xdg->toplevel) {
		struct twl_toplevel *t = xdg->toplevel;
		place_toplevel(t);
		if (!xdg->mapped) {
			xdg->mapped = true;
			wl_list_remove(&t->link);
			wl_list_insert(server->toplevels.prev, &t->link);
			twl_xdg_close_popups(server);
			twl_xdg_update_focus(server);
			twl_seat_refocus_pointer(server);
		}
	} else if (!xdg->mapped) {
		xdg->mapped = true;
		struct twl_popup *p = xdg->popup;
		wl_list_remove(&p->link);
		wl_list_insert(server->popups.prev, &p->link);
		if (p->grab)
			twl_xdg_update_focus(server);
		twl_seat_refocus_pointer(server);
	}
}

static void xdg_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct twl_xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (!xdg->surface || xdg->toplevel || xdg->popup) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "already constructed");
		return;
	}
	struct twl_toplevel *t = calloc(1, sizeof(*t));
	if (!t) {
		wl_client_post_no_memory(client);
		return;
	}
	t->resource = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
	if (!t->resource) {
		free(t);
		wl_client_post_no_memory(client);
		return;
	}
	t->xdg = xdg;
	xdg->toplevel = t;
	wl_list_insert(&xdg->server->toplevels, &t->link);
	wl_resource_set_implementation(t->resource, &toplevel_impl, t, toplevel_resource_destroy);
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *parent, struct wl_resource *positioner_resource) {
	struct twl_xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (!xdg->surface || xdg->toplevel || xdg->popup) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "already constructed");
		return;
	}
	struct positioner *pos = wl_resource_get_user_data(positioner_resource);
	if (pos->w <= 0 || pos->anchor_rect.w < 0) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER, "incomplete positioner");
		return;
	}
	struct twl_popup *p = calloc(1, sizeof(*p));
	if (!p) {
		wl_client_post_no_memory(client);
		return;
	}
	p->resource = wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(resource), id);
	if (!p->resource) {
		free(p);
		wl_client_post_no_memory(client);
		return;
	}
	p->xdg = xdg;
	p->parent = parent ? wl_resource_get_user_data(parent) : NULL;
	xdg->popup = p;
	wl_list_insert(xdg->server->popups.prev, &p->link);
	popup_place(p, pos);
	wl_resource_set_implementation(p->resource, &popup_impl, p, popup_resource_destroy);
}

static void xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource, int32_t x,
		int32_t y, int32_t w, int32_t h) {
	struct twl_xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (w <= 0 || h <= 0) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE, "invalid window geometry");
		return;
	}
	xdg->pending_geometry = (struct twl_box){x, y, w, h};
	xdg->pending_has_geometry = true;
}

static void xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
	struct twl_xdg_surface *xdg = wl_resource_get_user_data(resource);
	xdg->configured = true;
}

static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = xdg_surface_destroy,
	.get_toplevel = xdg_surface_get_toplevel,
	.get_popup = xdg_surface_get_popup,
	.set_window_geometry = xdg_surface_set_window_geometry,
	.ack_configure = xdg_surface_ack_configure,
};

static void xdg_surface_free(struct twl_xdg_surface *xdg) {
	if (xdg->toplevel) {
		wl_resource_set_user_data(xdg->toplevel->resource, NULL);
		toplevel_free(xdg->toplevel);
	}
	if (xdg->popup) {
		wl_resource_set_user_data(xdg->popup->resource, NULL);
		popup_free(xdg->popup);
	}
	/* Popups pointing at this surface lose their parent. */
	struct twl_popup *p;
	wl_list_for_each(p, &xdg->server->popups, link)
		if (p->parent == xdg)
			p->parent = NULL;
	if (xdg->configure_idle)
		wl_event_source_remove(xdg->configure_idle);
	if (xdg->surface) {
		xdg->surface->role_data = NULL;
		wl_list_remove(&xdg->surface_destroy.link);
	}
	twl_xdg_update_focus(xdg->server);
	free(xdg);
}

static void xdg_surface_resource_destroy(struct wl_resource *resource) {
	struct twl_xdg_surface *xdg = wl_resource_get_user_data(resource);
	if (xdg)
		xdg_surface_free(xdg);
}

static void xdg_surface_surface_destroyed(struct wl_listener *listener, void *data) {
	struct twl_xdg_surface *xdg = wl_container_of(listener, xdg, surface_destroy);
	xdg_unmap(xdg);
	wl_list_remove(&xdg->surface_destroy.link);
	wl_list_init(&xdg->surface_destroy.link);
	xdg->surface = NULL;
}

/* ---- xdg_wm_base ---- */

static void wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct positioner *p = calloc(1, sizeof(*p));
	struct wl_resource *r = wl_resource_create(client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
	if (!p || !r) {
		free(p);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &positioner_impl, p, positioner_resource_destroy);
}

static void wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct twl_surface *s = twl_surface_from_resource(surface_resource);
	if (s->role == TWL_ROLE_XDG && s->role_data) {
		wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE, "surface already has an xdg_surface");
		return;
	}
	if (!twl_surface_set_role(s, TWL_ROLE_XDG, resource, XDG_WM_BASE_ERROR_ROLE))
		return;
	if (s->has_buffer) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "surface has a buffer");
		return;
	}
	struct twl_xdg_surface *xdg = calloc(1, sizeof(*xdg));
	if (!xdg) {
		wl_client_post_no_memory(client);
		return;
	}
	xdg->resource = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
	if (!xdg->resource) {
		free(xdg);
		wl_client_post_no_memory(client);
		return;
	}
	xdg->server = server;
	xdg->surface = s;
	xdg->surface_destroy.notify = xdg_surface_surface_destroyed;
	wl_signal_add(&s->destroy_signal, &xdg->surface_destroy);
	s->role_data = xdg;
	wl_resource_set_implementation(xdg->resource, &xdg_surface_impl, xdg, xdg_surface_resource_destroy);
}

static void wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
}

static const struct xdg_wm_base_interface wm_base_impl = {
	.destroy = wm_base_destroy,
	.create_positioner = wm_base_create_positioner,
	.get_xdg_surface = wm_base_get_xdg_surface,
	.pong = wm_base_pong,
};

static void wm_base_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wm_base_impl, data, NULL);
}

/* ---- zxdg_decoration_manager_v1: always client-side ---- */

static void decoration_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void decoration_set_mode(struct wl_client *client, struct wl_resource *resource, uint32_t mode) {
	if (wl_resource_get_user_data(resource))
		zxdg_toplevel_decoration_v1_send_configure(resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

static void decoration_unset_mode(struct wl_client *client, struct wl_resource *resource) {
	decoration_set_mode(client, resource, 0);
}

static const struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
	.destroy = decoration_destroy,
	.set_mode = decoration_set_mode,
	.unset_mode = decoration_unset_mode,
};

static void decoration_resource_destroy(struct wl_resource *resource) {
	struct twl_toplevel *t = wl_resource_get_user_data(resource);
	if (t)
		t->decoration = NULL;
}

static void decoration_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void decoration_manager_get(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *toplevel) {
	struct twl_toplevel *t = wl_resource_get_user_data(toplevel);
	struct wl_resource *r = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface,
			wl_resource_get_version(resource), id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &decoration_impl, t, decoration_resource_destroy);
	if (t)
		t->decoration = r;
}

static const struct zxdg_decoration_manager_v1_interface decoration_manager_impl = {
	.destroy = decoration_manager_destroy,
	.get_toplevel_decoration = decoration_manager_get,
};

static void decoration_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &decoration_manager_impl, data, NULL);
}

void twl_xdg_global_init(struct twl_server *server) {
	wl_global_create(server->display, &xdg_wm_base_interface, 6, server, wm_base_bind);
	wl_global_create(server->display, &zxdg_decoration_manager_v1_interface, 1, server, decoration_manager_bind);
}
