#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include "twl.h"
#include "wayland-server-protocol.h"
#include "linux-dmabuf-v1-protocol.h"

#define DRM_FORMAT_ARGB8888 0x34325241
#define DRM_FORMAT_XRGB8888 0x34325258
#define DRM_FORMAT_ABGR8888 0x34324241
#define DRM_FORMAT_XBGR8888 0x34324258
#define DRM_FORMAT_MOD_LINEAR 0ull
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull

static const uint32_t dmabuf_formats[] = {
	DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
};
#define N_FORMATS (sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]))

static int format_table_fd = -1;
static uint32_t format_table_size;

/* ---- buffers ---- */

static const struct wl_buffer_interface dmabuf_buffer_impl;

struct twl_buffer *twl_buffer_from_resource(struct wl_resource *resource) {
	if (resource && wl_resource_instance_of(resource, &wl_buffer_interface, &dmabuf_buffer_impl))
		return wl_resource_get_user_data(resource);
	return NULL;
}

struct twl_buffer *twl_buffer_ref(struct twl_buffer *b) {
	b->refs++;
	return b;
}

void twl_buffer_unref(struct twl_buffer *b) {
	if (!b || --b->refs > 0)
		return;
	twl_renderer_forget_buffer(b->server->renderer, b);
	twl_vkcopy_free(b->vk);
	if (b->map)
		munmap(b->map, b->map_size);
	for (int i = 0; i < b->attribs.n_planes; i++)
		if (b->attribs.fd[i] >= 0)
			close(b->attribs.fd[i]);
	free(b);
}

void twl_buffer_send_release(struct wl_resource *buffer_resource) {
	if (buffer_resource)
		wl_buffer_send_release(buffer_resource);
}

bool twl_dmabuf_idle(struct twl_buffer *b) {
	struct pollfd p = {.fd = b->attribs.fd[0], .events = POLLIN};
	int r = poll(&p, 1, 0);
	return r != 0; /* 1: readable; -1: can't tell, don't wait forever */
}

bool twl_dmabuf_read(struct twl_buffer *b, void (*fn)(void *data, const void *pixels, int32_t stride), void *data) {
	const struct twl_dmabuf_attribs *a = &b->attribs;
	if (!b->map) {
		size_t size = (size_t) a->offset[0] + (size_t) a->stride[0] * a->height;
		void *map = mmap(NULL, size, PROT_READ, MAP_SHARED, a->fd[0], 0);
		if (map == MAP_FAILED) {
			twl_error("mmap of a dma-buf failed: %s", strerror(errno));
			return false;
		}
		b->map = map;
		b->map_size = size;
	}
	struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
	ioctl(a->fd[0], DMA_BUF_IOCTL_SYNC, &sync);
	fn(data, (const uint8_t *) b->map + a->offset[0], (int32_t) a->stride[0]);
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(a->fd[0], DMA_BUF_IOCTL_SYNC, &sync);
	return true;
}

static void buffer_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface dmabuf_buffer_impl = {
	.destroy = buffer_destroy,
};

static void buffer_resource_destroy(struct wl_resource *resource) {
	struct twl_buffer *b = wl_resource_get_user_data(resource);
	b->resource = NULL;
	twl_buffer_unref(b);
}

/* ---- zwp_linux_buffer_params_v1 ---- */

struct params {
	struct twl_server *server;
	struct twl_dmabuf_attribs attribs;
	bool used;
};

static void params_resource_destroy(struct wl_resource *resource) {
	struct params *p = wl_resource_get_user_data(resource);
	for (int i = 0; i < 4; i++)
		if (p->attribs.fd[i] >= 0)
			close(p->attribs.fd[i]);
	free(p);
}

static void params_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client, struct wl_resource *resource, int32_t fd, uint32_t plane_idx,
		uint32_t offset, uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) {
	struct params *p = wl_resource_get_user_data(resource);
	if (p->used) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
		return;
	}
	if (plane_idx >= 4) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "plane index %u", plane_idx);
		return;
	}
	if (p->attribs.fd[plane_idx] >= 0) {
		close(fd);
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane %u already set",
				plane_idx);
		return;
	}
	p->attribs.fd[plane_idx] = fd;
	p->attribs.offset[plane_idx] = offset;
	p->attribs.stride[plane_idx] = stride;
	p->attribs.modifier = ((uint64_t) modifier_hi << 32) | modifier_lo;
}

/* 1: valid, 0: unsupported ("failed"), -1: protocol error posted */
static int params_check(struct params *p, struct wl_resource *resource, int32_t width, int32_t height,
		uint32_t format) {
	struct twl_dmabuf_attribs *a = &p->attribs;
	int n = 0;
	while (n < 4 && a->fd[n] >= 0)
		n++;
	for (int i = n; i < 4; i++)
		if (a->fd[i] >= 0) {
			wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "plane %d missing", n);
			return -1;
		}
	if (n == 0) {
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "no planes");
		return -1;
	}
	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS, "bad size");
		return -1;
	}
	a->n_planes = n;
	a->width = width;
	a->height = height;
	a->format = format;
	bool known = false;
	for (size_t i = 0; i < N_FORMATS; i++)
		known |= dmabuf_formats[i] == format;
	/* Unsupported: "failed", not a protocol error. */
	if (!known || n != 1)
		return 0;
	if (a->modifier != DRM_FORMAT_MOD_LINEAR && a->modifier != DRM_FORMAT_MOD_INVALID)
		return 0;
	off_t size = lseek(a->fd[0], 0, SEEK_END);
	if (size >= 0 && (uint64_t) a->offset[0] + (uint64_t) a->stride[0] * height > (uint64_t) size) {
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS, "plane out of bounds");
		return -1;
	}
	if (a->stride[0] < (uint32_t) width * 4) {
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS, "stride too small");
		return -1;
	}
	return 1;
}

static struct twl_buffer *params_create_buffer(struct wl_client *client, struct wl_resource *resource,
		uint32_t buffer_id, int32_t width, int32_t height, uint32_t format, uint32_t flags, bool *error) {
	struct params *p = wl_resource_get_user_data(resource);
	*error = false;
	if (p->used) {
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
		*error = true;
		return NULL;
	}
	p->used = true;
	int valid = params_check(p, resource, width, height, format);
	if (valid <= 0) {
		*error = valid < 0;
		return NULL;
	}
	struct twl_buffer *b = calloc(1, sizeof(*b));
	if (!b) {
		wl_client_post_no_memory(client);
		*error = true;
		return NULL;
	}
	b->server = p->server;
	b->dmabuf = true;
	b->attribs = p->attribs;
	b->attribs.flags = flags;
	for (int i = 0; i < 4; i++)
		p->attribs.fd[i] = -1; /* now the buffer's */
	b->resource = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
	if (!b->resource) {
		for (int i = 0; i < b->attribs.n_planes; i++)
			close(b->attribs.fd[i]);
		free(b);
		wl_client_post_no_memory(client);
		*error = true;
		return NULL;
	}
	b->refs = 1;
	wl_resource_set_implementation(b->resource, &dmabuf_buffer_impl, b, buffer_resource_destroy);
	return b;
}

static void params_create(struct wl_client *client, struct wl_resource *resource, int32_t width, int32_t height,
		uint32_t format, uint32_t flags) {
	bool error;
	struct twl_buffer *b = params_create_buffer(client, resource, 0, width, height, format, flags, &error);
	if (b)
		zwp_linux_buffer_params_v1_send_created(resource, b->resource);
	else if (!error)
		zwp_linux_buffer_params_v1_send_failed(resource);
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource, uint32_t buffer_id,
		int32_t width, int32_t height, uint32_t format, uint32_t flags) {
	bool error;
	struct twl_buffer *b = params_create_buffer(client, resource, buffer_id, width, height, format, flags, &error);
	if (!b && !error)
		wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
				"unsupported dma-buf (format %08x, modifier or planes)", format);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
	.destroy = params_destroy,
	.add = params_add,
	.create = params_create,
	.create_immed = params_create_immed,
};

/* ---- feedback ---- */

static void feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
	.destroy = feedback_destroy,
};

static void send_feedback(struct twl_server *server, struct wl_client *client, struct wl_resource *dmabuf,
		uint32_t id) {
	struct wl_resource *fb = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
			wl_resource_get_version(dmabuf), id);
	if (!fb) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(fb, &feedback_impl, server, NULL);

	struct wl_array dev;
	wl_array_init(&dev);
	dev_t *d = wl_array_add(&dev, sizeof(dev_t));
	if (d)
		*d = server->dmabuf_dev;
	zwp_linux_dmabuf_feedback_v1_send_format_table(fb, format_table_fd, format_table_size);
	zwp_linux_dmabuf_feedback_v1_send_main_device(fb, &dev);
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb, &dev);
	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(fb, 0);
	struct wl_array idx;
	wl_array_init(&idx);
	for (uint16_t i = 0; i < N_FORMATS; i++) {
		uint16_t *p = wl_array_add(&idx, sizeof(*p));
		if (p)
			*p = i;
	}
	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb, &idx);
	zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb);
	zwp_linux_dmabuf_feedback_v1_send_done(fb);
	wl_array_release(&idx);
	wl_array_release(&dev);
}

/* ---- zwp_linux_dmabuf_v1 ---- */

static void dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void dmabuf_create_params(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct params *p = calloc(1, sizeof(*p));
	struct wl_resource *r = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
			wl_resource_get_version(resource), id);
	if (!p || !r) {
		free(p);
		wl_client_post_no_memory(client);
		return;
	}
	p->server = wl_resource_get_user_data(resource);
	for (int i = 0; i < 4; i++)
		p->attribs.fd[i] = -1;
	p->attribs.modifier = DRM_FORMAT_MOD_INVALID;
	wl_resource_set_implementation(r, &params_impl, p, params_resource_destroy);
}

static void dmabuf_get_default_feedback(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	send_feedback(wl_resource_get_user_data(resource), client, resource, id);
}

static void dmabuf_get_surface_feedback(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface) {
	send_feedback(wl_resource_get_user_data(resource), client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
	.destroy = dmabuf_destroy,
	.create_params = dmabuf_create_params,
	.get_default_feedback = dmabuf_get_default_feedback,
	.get_surface_feedback = dmabuf_get_surface_feedback,
};

static void dmabuf_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &dmabuf_impl, data, NULL);
	if (version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION)
		return;
	for (size_t i = 0; i < N_FORMATS; i++) {
		if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION)
			zwp_linux_dmabuf_v1_send_modifier(resource, dmabuf_formats[i], 0, 0);
		else
			zwp_linux_dmabuf_v1_send_format(resource, dmabuf_formats[i]);
	}
}

void twl_buffer_global_init(struct twl_server *server) {
	wl_display_init_shm(server->display);
	wl_display_add_shm_format(server->display, WL_SHM_FORMAT_ABGR8888);
	wl_display_add_shm_format(server->display, WL_SHM_FORMAT_XBGR8888);

	if (!server->dmabuf_enabled)
		return;
	struct { uint32_t format, pad; uint64_t modifier; } table[N_FORMATS];
	for (size_t i = 0; i < N_FORMATS; i++)
		table[i] = (typeof(table[0])){dmabuf_formats[i], 0, DRM_FORMAT_MOD_LINEAR};
	format_table_size = sizeof(table);
	format_table_fd = twl_create_anon_file(format_table_size);
	if (format_table_fd < 0 || pwrite(format_table_fd, table, sizeof(table), 0) != (ssize_t) sizeof(table)) {
		twl_error("linux-dmabuf disabled: no format table");
		server->dmabuf_enabled = false;
		return;
	}
	wl_global_create(server->display, &zwp_linux_dmabuf_v1_interface, 4, server, dmabuf_bind);
}
