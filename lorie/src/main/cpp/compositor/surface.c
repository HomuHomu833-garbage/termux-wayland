#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "twl.h"
#include "wayland-server-protocol.h"
#include "viewporter-protocol.h"

static const struct wl_surface_interface surface_impl;

struct twl_surface *twl_surface_from_resource(struct wl_resource *resource) {
	return resource ? wl_resource_get_user_data(resource) : NULL;
}

/* ---- double-buffered state ---- */

static void state_buffer_destroyed(struct wl_listener *listener, void *data) {
	struct twl_surface_state *st = wl_container_of(listener, st, buffer_destroy);
	st->buffer = NULL;
	wl_list_remove(&st->buffer_destroy.link);
	wl_list_init(&st->buffer_destroy.link);
}

static void state_init(struct twl_surface_state *st) {
	memset(st, 0, sizeof(*st));
	st->scale = 1;
	st->input_infinite = true;
	st->src_w = -1;
	wl_list_init(&st->frame_callbacks);
	wl_list_init(&st->buffer_destroy.link);
	st->buffer_destroy.notify = state_buffer_destroyed;
}

static void state_set_buffer(struct twl_surface_state *st, struct wl_resource *buffer) {
	wl_list_remove(&st->buffer_destroy.link);
	wl_list_init(&st->buffer_destroy.link);
	st->buffer = buffer;
	if (buffer)
		wl_resource_add_destroy_listener(buffer, &st->buffer_destroy);
}

static void state_fini(struct twl_surface_state *st) {
	struct wl_resource *cb, *tmp;
	wl_resource_for_each_safe(cb, tmp, &st->frame_callbacks)
		wl_resource_destroy(cb);
	state_set_buffer(st, NULL);
	twl_region_fini(&st->input);
}

/* Moves src into dst (dst is applied later as a whole), leaving src empty. */
static void state_merge(struct twl_surface_state *dst, struct twl_surface_state *src) {
	if (src->committed & TWL_STATE_BUFFER) {
		/* A buffer replaced before it was ever shown is free again. */
		if (dst->committed & TWL_STATE_BUFFER && dst->buffer && dst->buffer != src->buffer)
			twl_buffer_send_release(dst->buffer);
		state_set_buffer(dst, src->buffer);
		state_set_buffer(src, NULL);
		dst->dx += src->dx;
		dst->dy += src->dy;
	}
	if (src->committed & TWL_STATE_OFFSET) {
		dst->dx += src->dx;
		dst->dy += src->dy;
	}
	if (src->committed & TWL_STATE_SCALE)
		dst->scale = src->scale;
	if (src->committed & TWL_STATE_TRANSFORM)
		dst->transform = src->transform;
	if (src->committed & TWL_STATE_INPUT) {
		dst->input_infinite = src->input_infinite;
		twl_region_copy(&dst->input, &src->input);
	}
	if (src->committed & TWL_STATE_VIEWPORT) {
		dst->src_x = src->src_x;
		dst->src_y = src->src_y;
		dst->src_w = src->src_w;
		dst->src_h = src->src_h;
		dst->dst_w = src->dst_w;
		dst->dst_h = src->dst_h;
	}
	twl_box_union(&dst->damage, &src->damage);
	twl_box_union(&dst->buffer_damage, &src->buffer_damage);
	wl_list_insert_list(dst->frame_callbacks.prev, &src->frame_callbacks);
	wl_list_init(&src->frame_callbacks);
	dst->committed |= src->committed;
	src->committed = 0;
	src->dx = src->dy = 0;
	src->damage = src->buffer_damage = (struct twl_box){0, 0, 0, 0};
}

/* ---- showing buffers ---- */

static void drop_shown(struct twl_surface *s) {
	if (!s->shown)
		return;
	struct twl_server *server = s->server;
	/* The GPU may still sample it for the frame in flight: release after the next frame. */
	if (server->have_output && s->shown->resource) {
		struct twl_buffer **slot = wl_array_add(&server->release_after_frame, sizeof(*slot));
		if (slot) {
			*slot = s->shown;
			s->shown = NULL;
			return;
		}
	}
	if (s->shown->resource)
		twl_buffer_send_release(s->shown->resource);
	twl_buffer_unref(s->shown);
	s->shown = NULL;
}

static void cancel_waiting(struct twl_surface *s) {
	if (s->wait_source) {
		wl_event_source_remove(s->wait_source);
		s->wait_source = NULL;
	}
	if (s->waiting) {
		if (s->waiting->resource)
			twl_buffer_send_release(s->waiting->resource);
		twl_buffer_unref(s->waiting);
		s->waiting = NULL;
	}
}

struct upload_ctx {
	struct twl_surface *s;
	struct twl_buffer *b;
	const struct twl_box *damage;
};

static void upload_dmabuf_pixels(void *data, const void *pixels, int32_t stride) {
	struct upload_ctx *c = data;
	twl_texture_upload(c->s->server->renderer, &c->s->texture, pixels, stride, c->b->attribs.width,
			c->b->attribs.height, c->b->attribs.format, c->damage);
}

/* The client finished rendering s->waiting: show it. */
static void show_waiting(struct twl_surface *s) {
	struct twl_buffer *b = s->waiting;
	struct twl_server *server = s->server;
	s->waiting = NULL;
	if (s->wait_source) {
		wl_event_source_remove(s->wait_source);
		s->wait_source = NULL;
	}
	if (!b->resource) { /* destroyed meanwhile */
		twl_buffer_unref(b);
		return;
	}
	if (twl_renderer_import_dmabuf(server->renderer, b)) {
		drop_shown(s);
		s->shown = b; /* keeps our reference; released when replaced */
	} else {
		struct upload_ctx c = {s, b, &s->waiting_damage};
		if (!twl_dmabuf_read(b, upload_dmabuf_pixels, &c))
			twl_error("dma-buf %dx%d: could not read it", b->attribs.width, b->attribs.height);
		drop_shown(s);
		twl_buffer_send_release(b->resource);
		twl_buffer_unref(b);
	}
	s->waiting_damage = (struct twl_box){0, 0, 0, 0};
	twl_server_schedule_repaint(server);
}

static int waiting_ready(int fd, uint32_t mask, void *data) {
	struct twl_surface *s = data;
	if (s->wait_source) {
		wl_event_source_remove(s->wait_source);
		s->wait_source = NULL;
	}
	close(fd);
	if (s->waiting)
		show_waiting(s);
	return 0;
}

static void attach_dmabuf(struct twl_surface *s, struct twl_buffer *b, const struct twl_box *damage) {
	if (s->waiting) {
		/* Superseded before it was ready: never shown, so the copy needs its damage too. */
		twl_box_union(&s->waiting_damage, damage);
		cancel_waiting(s);
	} else {
		s->waiting_damage = *damage;
	}
	s->waiting = twl_buffer_ref(b);
	if (twl_dmabuf_idle(b)) {
		show_waiting(s);
		return;
	}
	/* Implicit sync: the dma-buf polls readable once its write fences signal. Android's GLES
	 * ignores those fences, so without this we would show half-rendered frames. */
	int fd = dup(b->attribs.fd[0]);
	if (fd >= 0)
		s->wait_source = wl_event_loop_add_fd(s->server->loop, fd, WL_EVENT_READABLE, waiting_ready, s);
	if (!s->wait_source) {
		if (fd >= 0)
			close(fd);
		show_waiting(s);
	}
}

static void attach_buffer(struct twl_surface *s, struct wl_resource *buffer, const struct twl_surface_state *st) {
	struct twl_server *server = s->server;
	struct twl_box damage = st->buffer_damage;
	struct twl_box sd = st->damage;
	if (!twl_box_empty(&sd)) {
		/* Surface damage in buffer coordinates (viewports: the whole buffer). */
		if (s->src_w >= 0 || s->dst_w > 0)
			sd = (struct twl_box){0, 0, INT32_MAX / 2, INT32_MAX / 2};
		else
			sd = (struct twl_box){sd.x * s->scale, sd.y * s->scale, sd.w * s->scale, sd.h * s->scale};
		twl_box_union(&damage, &sd);
	}

	struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
	if (shm) {
		int32_t w = wl_shm_buffer_get_width(shm), h = wl_shm_buffer_get_height(shm);
		uint32_t format = wl_shm_buffer_get_format(shm);
		/* wl_shm formats are DRM fourccs except the two legacy codes. */
		if (format == WL_SHM_FORMAT_ARGB8888)
			format = 0x34325241; /* AR24 */
		else if (format == WL_SHM_FORMAT_XRGB8888)
			format = 0x34325258; /* XR24 */
		cancel_waiting(s);
		wl_shm_buffer_begin_access(shm);
		if (s->role == TWL_ROLE_CURSOR && server->backend.set_cursor)
			twl_seat_cursor_committed(server, s, wl_shm_buffer_get_data(shm), wl_shm_buffer_get_stride(shm), w, h,
					format);
		else
			twl_texture_upload(server->renderer, &s->texture, wl_shm_buffer_get_data(shm),
					wl_shm_buffer_get_stride(shm), w, h, format, &damage);
		wl_shm_buffer_end_access(shm);
		drop_shown(s);
		twl_buffer_send_release(buffer);
		s->buffer_width = w;
		s->buffer_height = h;
		s->has_buffer = true;
		return;
	}

	struct twl_buffer *b = twl_buffer_from_resource(buffer);
	if (b && b->dmabuf) {
		s->buffer_width = b->attribs.width;
		s->buffer_height = b->attribs.height;
		s->has_buffer = true;
		attach_dmabuf(s, b, &damage);
		return;
	}
	wl_resource_post_error(s->resource, WL_SURFACE_ERROR_INVALID_SIZE, "unsupported buffer type");
}

static void update_size(struct twl_surface *s) {
	if (!s->has_buffer) {
		s->width = s->height = 0;
		return;
	}
	if (s->dst_w > 0) {
		s->width = s->dst_w;
		s->height = s->dst_h;
	} else if (s->src_w >= 0) {
		s->width = (int32_t) s->src_w;
		s->height = (int32_t) s->src_h;
	} else {
		int32_t scale = s->scale > 0 ? s->scale : 1;
		s->width = s->buffer_width / scale;
		s->height = s->buffer_height / scale;
	}
}

static void apply_state(struct twl_surface *s, struct twl_surface_state *st) {
	if (st->committed & TWL_STATE_SCALE)
		s->scale = st->scale;
	if (st->committed & TWL_STATE_VIEWPORT) {
		s->src_x = st->src_x;
		s->src_y = st->src_y;
		s->src_w = st->src_w;
		s->src_h = st->src_h;
		s->dst_w = st->dst_w;
		s->dst_h = st->dst_h;
	}
	if (st->committed & TWL_STATE_INPUT) {
		s->input_infinite = st->input_infinite;
		twl_region_copy(&s->input, &st->input);
	}
	if (s->role == TWL_ROLE_CURSOR && (st->committed & (TWL_STATE_BUFFER | TWL_STATE_OFFSET))) {
		s->server->seat.cursor_hx -= st->dx;
		s->server->seat.cursor_hy -= st->dy;
	}
	if (st->committed & TWL_STATE_BUFFER) {
		struct wl_resource *buffer = st->buffer;
		state_set_buffer(st, NULL);
		if (buffer) {
			attach_buffer(s, buffer, st);
		} else {
			cancel_waiting(s);
			drop_shown(s);
			twl_texture_release(s->server->renderer, &s->texture);
			s->has_buffer = false;
		}
	}
	update_size(s);
	wl_list_insert_list(s->frame_callbacks.prev, &st->frame_callbacks);
	wl_list_init(&st->frame_callbacks);
	st->committed = 0;
	st->dx = st->dy = 0;
	st->damage = st->buffer_damage = (struct twl_box){0, 0, 0, 0};
}

/* ---- subsurfaces ---- */

static bool subsurface_synchronized(struct twl_subsurface *sub) {
	for (; sub; sub = sub->parent && sub->parent->role == TWL_ROLE_SUBSURFACE ? sub->parent->role_data : NULL)
		if (sub->synchronized)
			return true;
	return false;
}

static void apply_children(struct twl_surface *s);

static void commit_tree(struct twl_surface *s, struct twl_surface_state *st) {
	apply_state(s, st);
	apply_children(s);
	if (s->role == TWL_ROLE_XDG)
		twl_xdg_surface_commit(s);
}

static void apply_children(struct twl_surface *s) {
	struct wl_list *lists[2] = {&s->below, &s->above};
	for (int i = 0; i < 2; i++) {
		struct twl_subsurface *sub;
		wl_list_for_each(sub, lists[i], link) {
			if (sub->position_pending) {
				sub->x = sub->pending_x;
				sub->y = sub->pending_y;
				sub->position_pending = false;
			}
			if (sub->has_cache && subsurface_synchronized(sub)) {
				sub->has_cache = false;
				commit_tree(sub->surface, &sub->cached);
			}
		}
	}
}

/* ---- wl_surface ---- */

static void surface_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer,
		int32_t x, int32_t y) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	if (wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION && (x || y)) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET, "attach offset must be 0 since v5");
		return;
	}
	state_set_buffer(&s->pending, buffer);
	s->pending.committed |= TWL_STATE_BUFFER;
	s->pending.dx = x;
	s->pending.dy = y;
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t w,
		int32_t h) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	struct twl_box b = {x, y, TWL_MIN(w, INT32_MAX / 8), TWL_MIN(h, INT32_MAX / 8)};
	twl_box_union(&s->pending.damage, &b);
}

static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
		int32_t w, int32_t h) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	struct twl_box b = {x, y, TWL_MIN(w, INT32_MAX / 8), TWL_MIN(h, INT32_MAX / 8)};
	twl_box_union(&s->pending.buffer_damage, &b);
}

static void callback_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	struct wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, id);
	if (!cb) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(cb, NULL, NULL, callback_resource_destroy);
	wl_list_insert(s->pending.frame_callbacks.prev, wl_resource_get_link(cb));
	twl_server_schedule_repaint(s->server);
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *region) {
}

static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *region) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	s->pending.committed |= TWL_STATE_INPUT;
	if (region) {
		s->pending.input_infinite = false;
		twl_region_copy(&s->pending.input, twl_region_from_resource(region));
	} else {
		s->pending.input_infinite = true;
		twl_region_fini(&s->pending.input);
	}
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	if (s->role == TWL_ROLE_SUBSURFACE && s->role_data) {
		struct twl_subsurface *sub = s->role_data;
		if (subsurface_synchronized(sub)) {
			state_merge(&sub->cached, &s->pending);
			sub->has_cache = true;
			return;
		}
		if (sub->has_cache) {
			state_merge(&sub->cached, &s->pending);
			sub->has_cache = false;
			commit_tree(s, &sub->cached);
			twl_server_schedule_repaint(s->server);
			return;
		}
	}
	commit_tree(s, &s->pending);
	twl_server_schedule_repaint(s->server);
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource, int32_t transform) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	if (transform < 0 || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM, "invalid transform %d", transform);
		return;
	}
	s->pending.transform = transform;
	s->pending.committed |= TWL_STATE_TRANSFORM;
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource, int32_t scale) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	if (scale < 1) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE, "invalid scale %d", scale);
		return;
	}
	s->pending.scale = scale;
	s->pending.committed |= TWL_STATE_SCALE;
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	s->pending.dx = x;
	s->pending.dy = y;
	s->pending.committed |= TWL_STATE_OFFSET;
}

static const struct wl_surface_interface surface_impl = {
	.destroy = surface_destroy,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = surface_frame,
	.set_opaque_region = surface_set_opaque_region,
	.set_input_region = surface_set_input_region,
	.commit = surface_commit,
	.set_buffer_transform = surface_set_buffer_transform,
	.set_buffer_scale = surface_set_buffer_scale,
	.damage_buffer = surface_damage_buffer,
	.offset = surface_offset,
};

static void subsurface_unlink(struct twl_subsurface *sub);

static void surface_resource_destroy(struct wl_resource *resource) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	struct twl_server *server = s->server;
	wl_signal_emit(&s->destroy_signal, s);
	twl_seat_surface_unmapped(server, s);

	/* Children lose their parent (they stay alive, unmapped). */
	struct twl_subsurface *sub, *tmp;
	wl_list_for_each_safe(sub, tmp, &s->below, link)
		subsurface_unlink(sub);
	wl_list_for_each_safe(sub, tmp, &s->above, link)
		subsurface_unlink(sub);

	cancel_waiting(s);
	drop_shown(s);
	twl_texture_release(server->renderer, &s->texture);
	free(s->cursor_rgba);
	state_fini(&s->pending);
	struct wl_resource *cb, *ctmp;
	wl_resource_for_each_safe(cb, ctmp, &s->frame_callbacks)
		wl_resource_destroy(cb);
	twl_region_fini(&s->input);
	wl_list_remove(&s->link);
	free(s);
	twl_server_schedule_repaint(server);
}

static void send_enter(struct twl_surface *s) {
	struct wl_client *client = wl_resource_get_client(s->resource);
	struct wl_resource *output;
	wl_resource_for_each(output, &s->server->output_resources)
		if (wl_resource_get_client(output) == client)
			wl_surface_send_enter(s->resource, output);
	if (wl_resource_get_version(s->resource) >= WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION &&
			s->preferred_scale_sent != s->server->scale) {
		wl_surface_send_preferred_buffer_scale(s->resource, s->server->scale);
		s->preferred_scale_sent = s->server->scale;
	}
}

void twl_surface_send_enter_all(struct twl_server *server) {
	struct twl_surface *s;
	wl_list_for_each(s, &server->surfaces, link)
		send_enter(s);
}

static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	struct twl_surface *s = calloc(1, sizeof(*s));
	if (!s) {
		wl_client_post_no_memory(client);
		return;
	}
	s->resource = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
	if (!s->resource) {
		free(s);
		wl_client_post_no_memory(client);
		return;
	}
	s->server = server;
	s->scale = 1;
	s->src_w = -1;
	s->input_infinite = true;
	state_init(&s->pending);
	wl_list_init(&s->frame_callbacks);
	wl_list_init(&s->below);
	wl_list_init(&s->above);
	wl_signal_init(&s->destroy_signal);
	wl_list_insert(server->surfaces.prev, &s->link);
	wl_resource_set_implementation(s->resource, &surface_impl, s, surface_resource_destroy);
	send_enter(s);
}

static void compositor_create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	twl_region_create(client, resource, id);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface,
	.create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_impl, data, NULL);
}

bool twl_surface_set_role(struct twl_surface *s, enum twl_surface_role role, struct wl_resource *error_resource,
		uint32_t error_code) {
	if (s->role != TWL_ROLE_NONE && s->role != role) {
		wl_resource_post_error(error_resource, error_code, "surface already has another role");
		return false;
	}
	s->role = role;
	return true;
}

struct twl_surface *twl_surface_root(struct twl_surface *s) {
	while (s->role == TWL_ROLE_SUBSURFACE && s->role_data && ((struct twl_subsurface *) s->role_data)->parent)
		s = ((struct twl_subsurface *) s->role_data)->parent;
	return s;
}

bool twl_surface_is_mapped(struct twl_surface *s) {
	if (!s->has_buffer)
		return false;
	switch (s->role) {
	case TWL_ROLE_XDG:
		return s->role_data && ((struct twl_xdg_surface *) s->role_data)->mapped;
	case TWL_ROLE_SUBSURFACE: {
		struct twl_subsurface *sub = s->role_data;
		return sub && sub->parent && twl_surface_is_mapped(sub->parent);
	}
	case TWL_ROLE_CURSOR:
		return true;
	default:
		return false;
	}
}

void twl_surface_send_frame_done(struct twl_surface *s, uint32_t time_ms) {
	struct wl_resource *cb, *tmp;
	wl_resource_for_each_safe(cb, tmp, &s->frame_callbacks) {
		wl_callback_send_done(cb, time_ms);
		wl_resource_destroy(cb);
	}
}

void twl_surface_for_each(struct twl_surface *s, int32_t x, int32_t y,
		void (*fn)(struct twl_surface *s, int32_t x, int32_t y, void *data), void *data) {
	struct twl_subsurface *sub;
	wl_list_for_each(sub, &s->below, link)
		twl_surface_for_each(sub->surface, x + sub->x, y + sub->y, fn, data);
	if (s->has_buffer)
		fn(s, x, y, data);
	wl_list_for_each(sub, &s->above, link)
		twl_surface_for_each(sub->surface, x + sub->x, y + sub->y, fn, data);
}

static bool accepts_input(struct twl_surface *s, double lx, double ly) {
	if (!s->has_buffer || lx < 0 || ly < 0 || lx >= s->width || ly >= s->height)
		return false;
	return s->input_infinite || twl_region_contains(&s->input, lx, ly);
}

struct twl_surface *twl_surface_at(struct twl_surface *s, int32_t ox, int32_t oy, double x, double y, double *lx,
		double *ly) {
	struct twl_subsurface *sub;
	struct twl_surface *hit;
	wl_list_for_each_reverse(sub, &s->above, link)
		if ((hit = twl_surface_at(sub->surface, ox + sub->x, oy + sub->y, x, y, lx, ly)))
			return hit;
	if (accepts_input(s, x - ox, y - oy)) {
		*lx = x - ox;
		*ly = y - oy;
		return s;
	}
	wl_list_for_each_reverse(sub, &s->below, link)
		if ((hit = twl_surface_at(sub->surface, ox + sub->x, oy + sub->y, x, y, lx, ly)))
			return hit;
	return NULL;
}

/* ---- wl_subcompositor ---- */

static void subsurface_unlink(struct twl_subsurface *sub) {
	if (!sub->parent)
		return;
	wl_list_remove(&sub->link);
	wl_list_init(&sub->link);
	wl_list_remove(&sub->parent_destroy.link);
	wl_list_init(&sub->parent_destroy.link);
	sub->parent = NULL;
}

static void subsurface_parent_destroyed(struct wl_listener *listener, void *data) {
	struct twl_subsurface *sub = wl_container_of(listener, sub, parent_destroy);
	subsurface_unlink(sub);
}

static void subsurface_resource_destroy(struct wl_resource *resource) {
	struct twl_subsurface *sub = wl_resource_get_user_data(resource);
	if (!sub)
		return;
	subsurface_unlink(sub);
	wl_list_remove(&sub->surface_destroy.link);
	sub->surface->role_data = NULL;
	twl_server_schedule_repaint(sub->surface->server);
	state_fini(&sub->cached);
	free(sub);
}

static void subsurface_surface_destroyed(struct wl_listener *listener, void *data) {
	struct twl_subsurface *sub = wl_container_of(listener, sub, surface_destroy);
	subsurface_unlink(sub);
	wl_list_remove(&sub->surface_destroy.link);
	wl_resource_set_user_data(sub->resource, NULL);
	state_fini(&sub->cached);
	free(sub);
}

static void subsurface_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void subsurface_set_position(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
	struct twl_subsurface *sub = wl_resource_get_user_data(resource);
	if (!sub)
		return;
	sub->pending_x = x;
	sub->pending_y = y;
	sub->position_pending = true;
}

static struct twl_subsurface *sibling_of(struct twl_subsurface *sub, struct wl_resource *sibling_resource) {
	struct twl_surface *sibling = twl_surface_from_resource(sibling_resource);
	if (sibling->role != TWL_ROLE_SUBSURFACE || !sibling->role_data)
		return NULL;
	struct twl_subsurface *other = sibling->role_data;
	return other->parent == sub->parent ? other : NULL;
}

static void subsurface_place(struct wl_resource *resource, struct wl_resource *sibling_resource, bool above) {
	struct twl_subsurface *sub = wl_resource_get_user_data(resource);
	if (!sub || !sub->parent)
		return;
	struct twl_surface *parent = sub->parent;
	if (twl_surface_from_resource(sibling_resource) == parent) {
		wl_list_remove(&sub->link);
		if (above)
			wl_list_insert(&parent->above, &sub->link); /* just above the parent */
		else
			wl_list_insert(parent->below.prev, &sub->link); /* just below the parent */
		return;
	}
	struct twl_subsurface *sibling = sibling_of(sub, sibling_resource);
	if (!sibling || sibling == sub) {
		wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE, "not a sibling or the parent");
		return;
	}
	wl_list_remove(&sub->link);
	if (above)
		wl_list_insert(&sibling->link, &sub->link);
	else
		wl_list_insert(sibling->link.prev, &sub->link);
}

static void subsurface_place_above(struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling) {
	subsurface_place(resource, sibling, true);
}

static void subsurface_place_below(struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling) {
	subsurface_place(resource, sibling, false);
}

static void subsurface_set_sync(struct wl_client *client, struct wl_resource *resource) {
	struct twl_subsurface *sub = wl_resource_get_user_data(resource);
	if (sub)
		sub->synchronized = true;
}

static void subsurface_set_desync(struct wl_client *client, struct wl_resource *resource) {
	struct twl_subsurface *sub = wl_resource_get_user_data(resource);
	if (!sub)
		return;
	sub->synchronized = false;
	if (sub->has_cache && !subsurface_synchronized(sub)) {
		sub->has_cache = false;
		commit_tree(sub->surface, &sub->cached);
		twl_server_schedule_repaint(sub->surface->server);
	}
}

static const struct wl_subsurface_interface subsurface_impl = {
	.destroy = subsurface_destroy,
	.set_position = subsurface_set_position,
	.place_above = subsurface_place_above,
	.place_below = subsurface_place_below,
	.set_sync = subsurface_set_sync,
	.set_desync = subsurface_set_desync,
};

static void subcompositor_get_subsurface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource, struct wl_resource *parent_resource) {
	struct twl_surface *s = twl_surface_from_resource(surface_resource);
	struct twl_surface *parent = twl_surface_from_resource(parent_resource);
	if (s == parent || twl_surface_root(parent) == s) {
		wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT, "bad parent");
		return;
	}
	if (s->role == TWL_ROLE_SUBSURFACE && s->role_data) {
		wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "already a subsurface");
		return;
	}
	if (!twl_surface_set_role(s, TWL_ROLE_SUBSURFACE, resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE))
		return;
	struct twl_subsurface *sub = calloc(1, sizeof(*sub));
	if (!sub) {
		wl_client_post_no_memory(client);
		return;
	}
	sub->resource = wl_resource_create(client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
	if (!sub->resource) {
		free(sub);
		wl_client_post_no_memory(client);
		return;
	}
	sub->surface = s;
	sub->parent = parent;
	sub->synchronized = true;
	state_init(&sub->cached);
	wl_list_insert(parent->above.prev, &sub->link);
	sub->parent_destroy.notify = subsurface_parent_destroyed;
	wl_signal_add(&parent->destroy_signal, &sub->parent_destroy);
	sub->surface_destroy.notify = subsurface_surface_destroyed;
	wl_signal_add(&s->destroy_signal, &sub->surface_destroy);
	s->role_data = sub;
	wl_resource_set_implementation(sub->resource, &subsurface_impl, sub, subsurface_resource_destroy);
}

static void subcompositor_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
	.destroy = subcompositor_destroy,
	.get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client, &wl_subcompositor_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &subcompositor_impl, data, NULL);
}

void twl_compositor_global_init(struct twl_server *server) {
	wl_global_create(server->display, &wl_compositor_interface, 6, server, compositor_bind);
	wl_global_create(server->display, &wl_subcompositor_interface, 1, server, subcompositor_bind);
}

/* ---- wp_viewporter ---- */

static void viewport_resource_destroy(struct wl_resource *resource) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	if (!s)
		return;
	s->viewport = NULL;
	s->pending.src_w = -1;
	s->pending.dst_w = s->pending.dst_h = 0;
	s->pending.committed |= TWL_STATE_VIEWPORT;
}

static void viewport_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void viewport_set_source(struct wl_client *client, struct wl_resource *resource, wl_fixed_t x, wl_fixed_t y,
		wl_fixed_t w, wl_fixed_t h) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	if (!s) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE, "surface destroyed");
		return;
	}
	if (x == wl_fixed_from_int(-1) && y == x && w == x && h == x) {
		s->pending.src_w = -1;
	} else if (x < 0 || y < 0 || w <= 0 || h <= 0) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid source rectangle");
		return;
	} else {
		s->pending.src_x = wl_fixed_to_double(x);
		s->pending.src_y = wl_fixed_to_double(y);
		s->pending.src_w = wl_fixed_to_double(w);
		s->pending.src_h = wl_fixed_to_double(h);
	}
	/* Keep the other half of the viewport state. */
	s->pending.dst_w = s->pending.committed & TWL_STATE_VIEWPORT ? s->pending.dst_w : s->dst_w;
	s->pending.dst_h = s->pending.committed & TWL_STATE_VIEWPORT ? s->pending.dst_h : s->dst_h;
	s->pending.committed |= TWL_STATE_VIEWPORT;
}

static void viewport_set_destination(struct wl_client *client, struct wl_resource *resource, int32_t w, int32_t h) {
	struct twl_surface *s = wl_resource_get_user_data(resource);
	if (!s) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE, "surface destroyed");
		return;
	}
	if (w == -1 && h == -1) {
		w = h = 0;
	} else if (w <= 0 || h <= 0) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE, "invalid destination size");
		return;
	}
	if (!(s->pending.committed & TWL_STATE_VIEWPORT)) {
		s->pending.src_x = s->src_x;
		s->pending.src_y = s->src_y;
		s->pending.src_w = s->src_w;
		s->pending.src_h = s->src_h;
	}
	s->pending.dst_w = w;
	s->pending.dst_h = h;
	s->pending.committed |= TWL_STATE_VIEWPORT;
}

static const struct wp_viewport_interface viewport_impl = {
	.destroy = viewport_destroy,
	.set_source = viewport_set_source,
	.set_destination = viewport_set_destination,
};

static void viewport_surface_destroyed(struct wl_listener *listener, void *data) {
	struct twl_surface *s = data;
	if (s->viewport)
		wl_resource_set_user_data(s->viewport, NULL);
	wl_list_remove(&listener->link);
	free(listener);
}

static void viewporter_get_viewport(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct twl_surface *s = twl_surface_from_resource(surface_resource);
	if (s->viewport) {
		wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, "surface already has a viewport");
		return;
	}
	struct wl_listener *l = calloc(1, sizeof(*l));
	struct wl_resource *vp = wl_resource_create(client, &wp_viewport_interface, 1, id);
	if (!l || !vp) {
		free(l);
		wl_client_post_no_memory(client);
		return;
	}
	l->notify = viewport_surface_destroyed;
	wl_signal_add(&s->destroy_signal, l);
	s->viewport = vp;
	wl_resource_set_implementation(vp, &viewport_impl, s, viewport_resource_destroy);
}

static void viewporter_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_viewporter_interface viewporter_impl = {
	.destroy = viewporter_destroy,
	.get_viewport = viewporter_get_viewport,
};

static void viewporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client, &wp_viewporter_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &viewporter_impl, data, NULL);
}

void twl_viewporter_global_init(struct twl_server *server) {
	wl_global_create(server->display, &wp_viewporter_interface, 1, server, viewporter_bind);
}
