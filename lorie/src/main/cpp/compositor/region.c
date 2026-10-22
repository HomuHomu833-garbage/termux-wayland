#include <stdlib.h>
#include <string.h>
#include "twl.h"
#include "wayland-server-protocol.h"

bool twl_box_empty(const struct twl_box *b) {
	return b->w <= 0 || b->h <= 0;
}

void twl_box_union(struct twl_box *dst, const struct twl_box *src) {
	if (twl_box_empty(src))
		return;
	if (twl_box_empty(dst)) {
		*dst = *src;
		return;
	}
	int32_t x1 = TWL_MIN(dst->x, src->x), y1 = TWL_MIN(dst->y, src->y);
	int32_t x2 = TWL_MAX(dst->x + dst->w, src->x + src->w), y2 = TWL_MAX(dst->y + dst->h, src->y + src->h);
	*dst = (struct twl_box){x1, y1, x2 - x1, y2 - y1};
}

bool twl_box_intersect(struct twl_box *dst, const struct twl_box *a, const struct twl_box *b) {
	int32_t x1 = TWL_MAX(a->x, b->x), y1 = TWL_MAX(a->y, b->y);
	int32_t x2 = TWL_MIN(a->x + a->w, b->x + b->w), y2 = TWL_MIN(a->y + a->h, b->y + b->h);
	*dst = (struct twl_box){x1, y1, x2 - x1, y2 - y1};
	if (twl_box_empty(dst)) {
		*dst = (struct twl_box){0, 0, 0, 0};
		return false;
	}
	return true;
}

bool twl_box_contains(const struct twl_box *b, double x, double y) {
	return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

void twl_region_init(struct twl_region *r) {
	memset(r, 0, sizeof(*r));
}

void twl_region_fini(struct twl_region *r) {
	free(r->ops);
	memset(r, 0, sizeof(*r));
}

void twl_region_copy(struct twl_region *dst, const struct twl_region *src) {
	twl_region_fini(dst);
	if (!src->n)
		return;
	dst->ops = malloc(sizeof(*dst->ops) * src->n);
	if (!dst->ops)
		return;
	memcpy(dst->ops, src->ops, sizeof(*dst->ops) * src->n);
	dst->n = dst->cap = src->n;
}

void twl_region_op(struct twl_region *r, int32_t x, int32_t y, int32_t w, int32_t h, bool add) {
	if (w <= 0 || h <= 0)
		return;
	if (r->n == r->cap) {
		int cap = r->cap ? r->cap * 2 : 4;
		void *ops = realloc(r->ops, sizeof(*r->ops) * cap);
		if (!ops)
			return;
		r->ops = ops;
		r->cap = cap;
	}
	r->ops[r->n].box = (struct twl_box){x, y, w, h};
	r->ops[r->n].add = add;
	r->n++;
}

bool twl_region_contains(const struct twl_region *r, double x, double y) {
	bool inside = false;
	for (int i = 0; i < r->n; i++)
		if (twl_box_contains(&r->ops[i].box, x, y))
			inside = r->ops[i].add;
	return inside;
}

/* wl_region */

static void region_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t w,
		int32_t h) {
	twl_region_op(wl_resource_get_user_data(resource), x, y, w, h, true);
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t w,
		int32_t h) {
	twl_region_op(wl_resource_get_user_data(resource), x, y, w, h, false);
}

static const struct wl_region_interface region_impl = {
	.destroy = region_destroy,
	.add = region_add,
	.subtract = region_subtract,
};

static void region_resource_destroy(struct wl_resource *resource) {
	struct twl_region *r = wl_resource_get_user_data(resource);
	twl_region_fini(r);
	free(r);
}

struct twl_region *twl_region_from_resource(struct wl_resource *resource) {
	return resource ? wl_resource_get_user_data(resource) : NULL;
}

void twl_region_create(struct wl_client *client, struct wl_resource *compositor, uint32_t id) {
	struct twl_region *r = calloc(1, sizeof(*r));
	struct wl_resource *resource = wl_resource_create(client, &wl_region_interface,
			wl_resource_get_version(compositor), id);
	if (!r || !resource) {
		free(r);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &region_impl, r, region_resource_destroy);
}
