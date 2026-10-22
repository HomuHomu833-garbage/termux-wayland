/*
 * Termux:Wayland compositor core.
 *
 * One thread runs the wl_display event loop; everything in this header is only touched from it.
 * The platform (Android: android/jni.c, Linux test build: host/headless.c) supplies a backend
 * that owns EGL and the output window, and feeds input through the twl_event socket protocol
 * (twl-events.h), the same bytes the Android activity sends.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "twl-events.h"

#define TWL_LOG_DEBUG 0
#define TWL_LOG_INFO 1
#define TWL_LOG_ERROR 2
void twl_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define twl_debug(...) twl_log(TWL_LOG_DEBUG, __VA_ARGS__)
#define twl_info(...) twl_log(TWL_LOG_INFO, __VA_ARGS__)
#define twl_error(...) twl_log(TWL_LOG_ERROR, __VA_ARGS__)

#define TWL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TWL_MAX(a, b) ((a) > (b) ? (a) : (b))

struct twl_server;
struct twl_surface;

/* ---- boxes and regions (region.c) ---- */

struct twl_box {
	int32_t x, y, w, h;
};

bool twl_box_empty(const struct twl_box *b);
void twl_box_union(struct twl_box *dst, const struct twl_box *src);
bool twl_box_intersect(struct twl_box *dst, const struct twl_box *a, const struct twl_box *b);
bool twl_box_contains(const struct twl_box *b, double x, double y);

/* A wl_region kept as its add/subtract operations: exact for hit testing, which is all the
 * compositor needs regions for (damage is tracked as a bounding box). */
struct twl_region {
	int n, cap;
	struct { struct twl_box box; bool add; } *ops;
};

void twl_region_init(struct twl_region *r);
void twl_region_fini(struct twl_region *r);
void twl_region_copy(struct twl_region *dst, const struct twl_region *src);
void twl_region_op(struct twl_region *r, int32_t x, int32_t y, int32_t w, int32_t h, bool add);
bool twl_region_contains(const struct twl_region *r, double x, double y);
struct twl_region *twl_region_from_resource(struct wl_resource *resource);
void twl_region_create(struct wl_client *client, struct wl_resource *compositor, uint32_t id);

/* ---- textures and rendering (render.c) ---- */

/* Pixel content of a surface. On Android the pixels live in AHardwareBuffers (CPU writes,
 * the GPU samples them through an EGLImage, as Termux:Wayland does); elsewhere, or if that fails,
 * in a plain GL texture. A dma-buf that EGL imports directly is shown without any copy. */
struct twl_texture {
	GLuint tex;
	GLenum target;
	int32_t width, height;
	bool swizzle; /* buffer bytes are B,G,R,A: sample .bgra */
	bool opaque;  /* X formats: ignore alpha */
	bool valid;
	void *ahb[2];       /* AHardwareBuffer*, double-buffered so the CPU never writes what the GPU reads */
	EGLImageKHR image[2];
	GLuint ahb_tex[2];
	int front;
	struct twl_box prev_damage; /* damage written to the front copy only */
};

struct twl_renderer;
struct twl_buffer;

struct twl_renderer *twl_renderer_create(struct twl_server *server, EGLDisplay display);
void twl_renderer_destroy(struct twl_renderer *r);
void twl_texture_upload(struct twl_renderer *r, struct twl_texture *t, const void *data, int32_t stride,
		int32_t width, int32_t height, uint32_t drm_format, const struct twl_box *damage);
void twl_texture_release(struct twl_renderer *r, struct twl_texture *t);
bool twl_renderer_dmabuf_import_supported(struct twl_renderer *r);
/* Zero-copy import of a dma-buf (cached on the buffer); false: copy it instead. */
bool twl_renderer_import_dmabuf(struct twl_renderer *r, struct twl_buffer *b);
void twl_renderer_forget_buffer(struct twl_renderer *r, struct twl_buffer *b);
void twl_render_frame(struct twl_server *server, int32_t width_px, int32_t height_px);
/* Reads the last rendered frame (host test build): RGBA rows, bottom-up. */
void twl_renderer_read_pixels(struct twl_renderer *r, int32_t w, int32_t h, void *out);

/* ---- buffers (buffer.c) ---- */

struct twl_dmabuf_attribs {
	int32_t width, height;
	uint32_t format, flags;
	uint64_t modifier;
	int n_planes;
	int fd[4];
	uint32_t offset[4], stride[4];
};

/* A client wl_buffer we keep state for (dma-bufs; shm buffers are read at commit time). */
struct twl_buffer {
	struct twl_server *server;
	struct wl_resource *resource; /* NULL once the client destroyed it */
	int refs;
	bool dmabuf;
	struct twl_dmabuf_attribs attribs;
	/* EGL import (zero copy) */
	EGLImageKHR image;
	GLuint tex;
	bool import_failed;
	/* CPU mapping (copy path), kept while the buffer lives */
	void *map;
	size_t map_size;
};

void twl_buffer_global_init(struct twl_server *server);
struct twl_buffer *twl_buffer_from_resource(struct wl_resource *resource); /* NULL for shm */
struct twl_buffer *twl_buffer_ref(struct twl_buffer *b);
void twl_buffer_unref(struct twl_buffer *b);
void twl_buffer_send_release(struct wl_resource *buffer_resource);
/* Waits for the dma-buf's write fences (implicit sync); true when idle. */
bool twl_dmabuf_idle(struct twl_buffer *b);
bool twl_dmabuf_read(struct twl_buffer *b, void (*fn)(void *data, const void *pixels, int32_t stride), void *data);

/* ---- surfaces (surface.c) ---- */

enum twl_surface_role {
	TWL_ROLE_NONE,
	TWL_ROLE_XDG,
	TWL_ROLE_SUBSURFACE,
	TWL_ROLE_CURSOR,
};

#define TWL_STATE_BUFFER (1u << 0)
#define TWL_STATE_SCALE (1u << 1)
#define TWL_STATE_INPUT (1u << 2)
#define TWL_STATE_VIEWPORT (1u << 3)
#define TWL_STATE_OFFSET (1u << 4)
#define TWL_STATE_TRANSFORM (1u << 5)

struct twl_surface_state {
	uint32_t committed;
	struct wl_resource *buffer; /* attached (NULL: detach) */
	struct wl_listener buffer_destroy;
	int32_t dx, dy;
	int32_t scale;
	int32_t transform;
	struct twl_box damage;        /* surface coordinates */
	struct twl_box buffer_damage; /* buffer coordinates */
	bool input_infinite;
	struct twl_region input;
	double src_x, src_y, src_w, src_h; /* viewport source, src_w < 0: unset */
	int32_t dst_w, dst_h;              /* viewport destination, <= 0: unset */
	struct wl_list frame_callbacks;
};

struct twl_subsurface {
	struct twl_surface *surface, *parent;
	struct wl_resource *resource;
	struct wl_list link; /* parent->below or parent->above */
	int32_t x, y, pending_x, pending_y;
	bool position_pending;
	bool synchronized;
	struct twl_surface_state cached;
	bool has_cache;
	struct wl_listener parent_destroy;
	struct wl_listener surface_destroy;
};

struct twl_surface {
	struct twl_server *server;
	struct wl_resource *resource;
	struct wl_list link; /* server->surfaces */
	struct wl_signal destroy_signal;

	struct twl_surface_state pending;

	/* current state */
	int32_t scale;
	int32_t buffer_width, buffer_height;
	int32_t width, height; /* surface (logical) size */
	bool input_infinite;
	struct twl_region input;
	double src_x, src_y, src_w, src_h;
	int32_t dst_w, dst_h;
	struct wl_list frame_callbacks;
	bool has_buffer;

	/* what is shown */
	struct twl_texture texture;   /* copied content (shm, mmapped dma-buf) */
	struct twl_buffer *shown;     /* imported dma-buf shown without copy, or NULL */
	struct wl_resource *shown_resource;
	struct twl_buffer *waiting;   /* dma-buf committed but still being rendered by the client */
	struct wl_event_source *wait_source;
	struct twl_box waiting_damage;

	enum twl_surface_role role;
	void *role_data;
	struct wl_resource *role_resource;

	struct wl_list below, above; /* twl_subsurface.link, bottom to top */
	struct wl_resource *viewport;
	/* cursor pixels for a frontend that draws the cursor (R,G,B,A), kept for re-use */
	uint8_t *cursor_rgba;
	int32_t cursor_w, cursor_h;
	int32_t preferred_scale_sent;
};

void twl_compositor_global_init(struct twl_server *server);
struct twl_surface *twl_surface_from_resource(struct wl_resource *resource);
bool twl_surface_set_role(struct twl_surface *s, enum twl_surface_role role, struct wl_resource *error_resource,
		uint32_t error_code);
struct twl_surface *twl_surface_root(struct twl_surface *s);
void twl_surface_send_frame_done(struct twl_surface *s, uint32_t time_ms);
void twl_surface_send_enter_all(struct twl_server *server);
/* Calls fn for the surface and its subsurfaces, bottom to top, with absolute positions. */
void twl_surface_for_each(struct twl_surface *s, int32_t x, int32_t y,
		void (*fn)(struct twl_surface *s, int32_t x, int32_t y, void *data), void *data);
/* Topmost surface of the tree at (x,y) (surface tree origin at ox,oy), with local coordinates. */
struct twl_surface *twl_surface_at(struct twl_surface *s, int32_t ox, int32_t oy, double x, double y,
		double *lx, double *ly);
bool twl_surface_is_mapped(struct twl_surface *s);
void twl_viewporter_global_init(struct twl_server *server);

/* ---- xdg shell and window management (xdg.c) ---- */

struct twl_xdg_surface;

struct twl_toplevel {
	struct twl_xdg_surface *xdg;
	struct wl_resource *resource;
	struct wl_list link; /* server->toplevels, bottom to top */
	struct twl_toplevel *parent;
	char *title, *app_id;
	int32_t min_w, min_h, max_w, max_h;
	bool fullscreen_requested, maximize_requested;
	bool floating;      /* dialogs and fixed-size windows: own size, centered */
	bool placed;
	int32_t x, y;       /* window geometry position, logical */
	int32_t configured_w, configured_h;
	struct wl_resource *decoration;
};

struct twl_popup {
	struct twl_xdg_surface *xdg;
	struct wl_resource *resource;
	struct twl_xdg_surface *parent;
	struct wl_list link; /* server->popups, bottom to top */
	struct twl_box geometry; /* relative to the parent's window geometry */
	bool grab;
	bool committed_once;
};

struct twl_xdg_surface {
	struct twl_server *server;
	struct wl_resource *resource;
	struct twl_surface *surface;
	struct wl_listener surface_destroy;
	struct twl_toplevel *toplevel;
	struct twl_popup *popup;
	struct twl_box geometry, pending_geometry;
	bool has_geometry, pending_has_geometry;
	uint32_t configure_serial;
	bool configured; /* client acked a configure */
	bool initial_commit_done;
	bool mapped;
	struct wl_event_source *configure_idle;
};

void twl_xdg_global_init(struct twl_server *server);
void twl_xdg_surface_commit(struct twl_surface *s);
void twl_xdg_reconfigure_all(struct twl_server *server);
/* Absolute position of the xdg surface's window geometry origin; false if unmapped. */
bool twl_xdg_window_position(struct twl_xdg_surface *xdg, int32_t *x, int32_t *y);
/* Absolute position of the surface (buffer origin) of a mapped xdg surface. */
bool twl_xdg_surface_position(struct twl_xdg_surface *xdg, int32_t *x, int32_t *y);
struct twl_toplevel *twl_xdg_focused_toplevel(struct twl_server *server);
void twl_xdg_raise(struct twl_toplevel *t);
void twl_xdg_cycle(struct twl_server *server, int direction);
void twl_xdg_close_popups(struct twl_server *server);
void twl_xdg_begin_move(struct twl_toplevel *t);
void twl_xdg_update_focus(struct twl_server *server);

/* ---- seat (seat.c) ---- */

#define TWL_MAX_TOUCH 10

struct twl_seat {
	struct twl_server *server;
	struct wl_list pointers, keyboards, touches, data_devices, text_inputs; /* resources */
	struct wl_list relative_pointers, primary_devices;

	/* pointer */
	double px, py; /* logical */
	struct twl_surface *pointer_focus;
	struct wl_listener pointer_focus_destroy;
	double focus_x, focus_y;
	uint32_t buttons_down;
	struct twl_surface *cursor;
	struct wl_listener cursor_destroy;
	int32_t cursor_hx, cursor_hy;
	bool cursor_visible;
	/* interactive move */
	struct twl_toplevel *moving;
	double move_dx, move_dy;
	/* pointer constraints */
	struct wl_list constraints;

	/* keyboard */
	struct twl_surface *keyboard_focus;
	struct wl_listener keyboard_focus_destroy;
	struct wl_array keys;
	uint32_t mods_depressed, mods_locked;
	int keymap_fd;
	uint32_t keymap_size;

	/* touch */
	struct {
		bool active;
		int32_t id;
		struct twl_surface *surface;
		struct wl_listener destroy;
		double ox, oy; /* surface origin when the touch started */
		bool emulated; /* touch driving the pointer */
	} touch[TWL_MAX_TOUCH];
	int emulating_touch; /* id of the touch emulating the pointer, -1 */

	/* selection */
	struct wl_resource *selection;          /* wl_data_source */
	struct wl_listener selection_destroy;
	char *own_text;                         /* compositor-owned selection (Android clipboard) */
	struct wl_event_source *clip_source;    /* reading a client's selection for Android */
	char *clip_buf;
	size_t clip_len;

	/* text input (zwp_text_input_v3) */
	bool text_input_enabled;
};

void twl_seat_init(struct twl_server *server);
void twl_seat_pointer_motion(struct twl_server *server, double x, double y, uint32_t time);
void twl_seat_pointer_relative(struct twl_server *server, double dx, double dy, uint32_t time);
void twl_seat_pointer_button(struct twl_server *server, uint32_t button, bool pressed, uint32_t time);
void twl_seat_pointer_axis(struct twl_server *server, double dx, double dy, uint32_t time);
void twl_seat_touch(struct twl_server *server, uint32_t type, int32_t id, double x, double y, uint32_t time);
void twl_seat_touch_frame(struct twl_server *server);
void twl_seat_key(struct twl_server *server, uint32_t key, bool pressed, uint32_t time);
void twl_seat_release_keys(struct twl_server *server, uint32_t time);
void twl_seat_text(struct twl_server *server, const char *utf8, uint32_t time);
void twl_seat_set_locks(struct twl_server *server, bool caps, bool num);
/* Android's clipboard: offered to clients as the selection. */
void twl_seat_set_text_selection(struct twl_server *server, const char *utf8);
void twl_seat_set_keyboard_focus(struct twl_server *server, struct twl_surface *s);
void twl_seat_refocus_pointer(struct twl_server *server);
void twl_seat_surface_unmapped(struct twl_server *server, struct twl_surface *s);
/* The cursor moved, changed or was hidden: tell a frontend that draws it. */
void twl_seat_cursor_changed(struct twl_server *server);
void twl_seat_cursor_committed(struct twl_server *server, struct twl_surface *s, const void *pixels, int32_t stride,
		int32_t width, int32_t height, uint32_t drm_format);
void twl_seat_draw_cursor(struct twl_server *server, void (*draw)(struct twl_surface *, int32_t, int32_t, void *),
		void *data);

/* keymap.c */
const char *twl_keymap_string(void);
bool twl_ascii_to_key(uint32_t c, uint32_t *key, bool *shift);

/* ---- output (output.c) ---- */

void twl_output_global_init(struct twl_server *server);
void twl_output_send_all(struct twl_server *server); /* after a mode/scale change */

/* ---- server (server.c) ---- */

struct twl_backend {
	/* Makes the output window current; false when there is none (activity in background). */
	bool (*begin_frame)(void *data, int32_t *width_px, int32_t *height_px);
	void (*end_frame)(void *data);
	/* Sends bytes to the activity (show the soft keyboard...). May be NULL. */
	void (*send_to_frontend)(void *data, const struct twl_event *e);
	/* Repaints wait for twl_server_vsync() instead of running when idle. */
	bool paced;
	/* The frame is read with row 0 at the top (an AHardwareBuffer), not GL's bottom-up. */
	bool flip_y;
	/* The frame is shown with red and blue swapped (Termux:X11's renderer samples an R8G8B8X8
	 * root window as B,G,R, the X server's byte order): write it that way. */
	bool swap_rb;
	/* A client copied text (clipboard sync to Android). May be NULL. */
	void (*clipboard_text)(void *data, const char *utf8);
	/* The frontend draws the cursor (both NULL: the compositor draws it). Pixels are R,G,B,A
	 * bytes, premultiplied; position and hotspot in output pixels. */
	void (*set_cursor)(void *data, const uint8_t *rgba, int32_t width, int32_t height, int32_t hx, int32_t hy);
	void (*move_cursor)(void *data, int32_t x, int32_t y, bool visible);
	void *data;
};

struct twl_config {
	int32_t scale;      /* 0: from dpi */
	bool touch_pointer; /* touch emulates a mouse instead of wl_touch */
	bool dmabuf;        /* advertise linux-dmabuf */
	bool dmabuf_import; /* try EGL dma-buf import before copying */
	bool ahb;           /* AHardwareBuffer textures (Android) */
	const char *socket; /* NULL: wayland-0... */
	const char *keymap_file;
	const char *render_node;
};

struct twl_server {
	struct twl_config config;
	struct wl_display *display;
	struct wl_event_loop *loop;
	char socket_name[64];
	struct twl_backend backend;
	struct twl_renderer *renderer;
	EGLDisplay egl_display;

	/* output, in pixels; logical = pixels / scale */
	int32_t width_px, height_px, dpi, refresh_mhz, scale;
	bool have_output;
	struct wl_list output_resources, xdg_output_resources;

	struct wl_list surfaces;
	struct wl_list toplevels; /* bottom to top */
	struct wl_list popups;    /* bottom to top */
	struct twl_toplevel *focused;
	struct twl_seat seat;

	bool dmabuf_enabled;
	dev_t dmabuf_dev;

	struct wl_array release_after_frame; /* twl_buffer*: sampled by the frame in flight */
	struct wl_event_source *repaint_idle;
	bool repaint_needed;

	/* frontend event stream */
	int event_fd;
	struct wl_event_source *event_source;
	uint8_t event_buf[4096 + sizeof(struct twl_event)];
	size_t event_len;
};

int twl_server_init(struct twl_server *server, const struct twl_config *config, const struct twl_backend *backend,
		EGLDisplay egl_display);
void twl_server_finish(struct twl_server *server);
const char *twl_server_socket_name(struct twl_server *server);
void twl_server_schedule_repaint(struct twl_server *server);
/* Paced backends: the display's vsync. Renders if anything changed. */
void twl_server_vsync(struct twl_server *server);
/* Output size/density changed (or the window went away: have_output false). */
void twl_server_set_output(struct twl_server *server, bool have_output, int32_t width_px, int32_t height_px,
		int32_t dpi, int32_t refresh_mhz);
/* Takes ownership of fd (the activity's event socket); replaces the previous one. */
void twl_server_set_event_fd(struct twl_server *server, int fd);
void twl_server_handle_event(struct twl_server *server, const struct twl_event *e, const char *payload);
void twl_server_send_to_frontend(struct twl_server *server, uint32_t type, uint32_t arg);
uint32_t twl_now_ms(void);
int twl_create_anon_file(size_t size);
