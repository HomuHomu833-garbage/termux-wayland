#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "twl.h"
#include "xdg-shell-protocol.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

void twl_log(int level, const char *fmt, ...) {
	static int debug = -1;
	if (debug < 0)
		debug = getenv("TWL_DEBUG") != NULL;
	if (level == TWL_LOG_DEBUG && !debug)
		return;
	va_list ap;
	va_start(ap, fmt);
#ifdef __ANDROID__
	va_list ap2;
	va_copy(ap2, ap);
	__android_log_vprint(level == TWL_LOG_ERROR ? ANDROID_LOG_ERROR : level == TWL_LOG_INFO ? ANDROID_LOG_INFO
			: ANDROID_LOG_DEBUG, "TermuxWayland", fmt, ap2);
	va_end(ap2);
#endif
	fprintf(stderr, "termux-wayland: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

uint32_t twl_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int twl_create_anon_file(size_t size) {
	int fd = (int) syscall(SYS_memfd_create, "termux-wayland", 1u /* MFD_CLOEXEC */);
	if (fd < 0) {
		char path[512];
		const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
		snprintf(path, sizeof(path), "%s/termux-wayland-XXXXXX", dir);
		fd = mkostemp(path, O_CLOEXEC);
		if (fd < 0)
			return -1;
		unlink(path);
	}
	if (ftruncate(fd, (off_t) size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* ---- repaint ---- */

static void release_after_frame(struct twl_server *server) {
	struct twl_buffer **b;
	wl_array_for_each(b, &server->release_after_frame) {
		if ((*b)->resource)
			twl_buffer_send_release((*b)->resource);
		twl_buffer_unref(*b);
	}
	server->release_after_frame.size = 0;
}

static void repaint(void *data) {
	struct twl_server *server = data;
	server->repaint_idle = NULL;
	if (!server->have_output)
		return;
	int32_t w, h;
	if (!server->backend.begin_frame(server->backend.data, &w, &h))
		return;
	twl_render_frame(server, w, h);
	server->backend.end_frame(server->backend.data);
	release_after_frame(server);
	uint32_t now = twl_now_ms();
	struct twl_surface *s;
	wl_list_for_each(s, &server->surfaces, link)
		twl_surface_send_frame_done(s, now);
	wl_display_flush_clients(server->display);
}

void twl_server_schedule_repaint(struct twl_server *server) {
	if (server->backend.paced) {
		server->repaint_needed = true;
		return;
	}
	if (server->repaint_idle || !server->have_output)
		return;
	server->repaint_idle = wl_event_loop_add_idle(server->loop, repaint, server);
}

void twl_server_vsync(struct twl_server *server) {
	if (!server->repaint_needed || !server->have_output)
		return;
	server->repaint_needed = false;
	repaint(server);
}

/* ---- output ---- */

/* Auto: about 200 dpi per scale step, but never so much that the shorter side of the output
 * drops below 480 logical pixels (a 1680x1050 resolution on a phone is ~700 dpi, and scale 4
 * would leave apps 262 pixels of height). */
static int32_t output_scale_for(struct twl_server *server, int32_t width, int32_t height) {
	if (server->config.scale > 0)
		return server->config.scale;
	int32_t s = server->dpi / 200, shorter = TWL_MIN(width, height);
	if (s > 4)
		s = 4;
	while (s > 1 && shorter / s < 480)
		s--;
	return s < 1 ? 1 : s;
}

static int32_t output_scale(struct twl_server *server) {
	return output_scale_for(server, server->width_px, server->height_px);
}

void twl_server_set_output(struct twl_server *server, bool have_output, int32_t width_px, int32_t height_px,
		int32_t dpi, int32_t refresh_mhz) {
	bool had = server->have_output;
	server->have_output = have_output;
	if (!have_output) {
		release_after_frame(server);
		if (server->repaint_idle) {
			wl_event_source_remove(server->repaint_idle);
			server->repaint_idle = NULL;
		}
		return;
	}
	if (dpi > 0)
		server->dpi = dpi;
	if (refresh_mhz > 0)
		server->refresh_mhz = refresh_mhz;
	int32_t scale = output_scale_for(server, width_px > 0 ? width_px : server->width_px,
			height_px > 0 ? height_px : server->height_px);
	if (width_px > 0 && height_px > 0 &&
			(width_px != server->width_px || height_px != server->height_px || scale != server->scale)) {
		server->width_px = width_px;
		server->height_px = height_px;
		server->scale = scale;
		twl_info("output %dx%d, scale %d (%dx%d logical), %d dpi, %d.%03d Hz", width_px, height_px, scale,
				width_px / scale, height_px / scale, server->dpi, server->refresh_mhz / 1000, server->refresh_mhz % 1000);
		twl_output_send_all(server);
		twl_xdg_reconfigure_all(server);
		twl_seat_cursor_changed(server);
	}
	if (!had || have_output)
		twl_server_schedule_repaint(server);
}

/* ---- frontend events ---- */

void twl_server_send_to_frontend(struct twl_server *server, uint32_t type, uint32_t arg) {
	if (!server->backend.send_to_frontend)
		return;
	struct twl_event e = {.type = type, .arg = arg, .time = twl_now_ms()};
	server->backend.send_to_frontend(server->backend.data, &e);
}

void twl_server_handle_event(struct twl_server *server, const struct twl_event *e, const char *payload) {
	double S = server->scale > 0 ? server->scale : 1;
	double x = e->x / S, y = e->y / S;
	uint32_t time = e->time ? e->time : twl_now_ms();
	switch (e->type) {
	case TWL_EV_TOUCH_DOWN:
	case TWL_EV_TOUCH_MOTION:
	case TWL_EV_TOUCH_UP:
	case TWL_EV_TOUCH_CANCEL:
		twl_seat_touch(server, e->type, e->id, x, y, time);
		break;
	case TWL_EV_TOUCH_FRAME:
		if (!server->config.touch_pointer)
			twl_seat_touch_frame(server);
		break;
	case TWL_EV_POINTER_MOTION:
		server->seat.cursor_visible = true;
		twl_seat_pointer_motion(server, x, y, time);
		twl_seat_cursor_changed(server);
		break;
	case TWL_EV_POINTER_RELATIVE:
		server->seat.cursor_visible = true;
		twl_seat_pointer_relative(server, x, y, time);
		twl_seat_cursor_changed(server);
		break;
	case TWL_EV_POINTER_BUTTON:
		twl_seat_pointer_button(server, (uint32_t) e->id, e->arg != 0, time);
		break;
	case TWL_EV_POINTER_AXIS:
		twl_seat_pointer_axis(server, e->x, e->y, time);
		break;
	case TWL_EV_KEY:
		twl_seat_key(server, (uint32_t) e->id, e->arg != 0, time);
		break;
	case TWL_EV_TEXT:
		if (payload)
			twl_seat_text(server, payload, time);
		break;
	case TWL_EV_KEYS_RELEASE:
		twl_seat_release_keys(server, time);
		break;
	case TWL_EV_CYCLE_WINDOW:
		twl_xdg_cycle(server, (int) e->arg);
		break;
	case TWL_EV_CLOSE_WINDOW:
		if (server->focused)
			xdg_toplevel_send_close(server->focused->resource);
		break;
	case TWL_EV_SET:
		if (e->id == TWL_SET_TOUCH_POINTER) {
			server->config.touch_pointer = e->arg != 0;
		} else if (e->id == TWL_SET_SCALE) {
			server->config.scale = (int32_t) e->arg;
			if (server->have_output)
				twl_server_set_output(server, true, server->width_px, server->height_px, 0, 0);
			else
				server->scale = output_scale(server);
		}
		break;
	default:
		twl_debug("unknown event %u", e->type);
	}
	wl_display_flush_clients(server->display);
}

static void close_event_fd(struct twl_server *server) {
	if (server->event_source)
		wl_event_source_remove(server->event_source);
	server->event_source = NULL;
	if (server->event_fd >= 0)
		close(server->event_fd);
	server->event_fd = -1;
	server->event_len = 0;
	twl_seat_release_keys(server, twl_now_ms());
	twl_seat_touch(server, TWL_EV_TOUCH_CANCEL, 0, 0, 0, twl_now_ms());
}

#define TEXT_MAX 4096

static int event_readable(int fd, uint32_t mask, void *data) {
	struct twl_server *server = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		twl_info("frontend disconnected");
		close_event_fd(server);
		return 0;
	}
	for (;;) {
		ssize_t n = read(fd, server->event_buf + server->event_len, sizeof(server->event_buf) - server->event_len);
		if (n == 0) {
			twl_info("frontend disconnected");
			close_event_fd(server);
			return 0;
		}
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN)
				close_event_fd(server);
			return 0;
		}
		server->event_len += (size_t) n;
		size_t off = 0;
		while (server->event_len - off >= sizeof(struct twl_event)) {
			struct twl_event e;
			memcpy(&e, server->event_buf + off, sizeof(e));
			if (e.type == TWL_EV_TEXT) {
				if (e.arg > TEXT_MAX) {
					twl_error("text event too long (%u), frontend out of sync", e.arg);
					close_event_fd(server);
					return 0;
				}
				if (server->event_len - off < sizeof(e) + e.arg)
					break;
				char text[TEXT_MAX + 1];
				memcpy(text, server->event_buf + off + sizeof(e), e.arg);
				text[e.arg] = 0;
				twl_server_handle_event(server, &e, text);
				off += sizeof(e) + e.arg;
			} else {
				twl_server_handle_event(server, &e, NULL);
				off += sizeof(e);
			}
			if (server->event_fd < 0)
				return 0;
		}
		memmove(server->event_buf, server->event_buf + off, server->event_len - off);
		server->event_len -= off;
	}
}

void twl_server_set_event_fd(struct twl_server *server, int fd) {
	close_event_fd(server);
	if (fd < 0)
		return;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	server->event_fd = fd;
	server->event_source = wl_event_loop_add_fd(server->loop, fd, WL_EVENT_READABLE, event_readable, server);
	/* the frontend wants to know whether to show its keyboard */
	if (server->seat.text_input_enabled)
		twl_server_send_to_frontend(server, TWL_EV_SHOW_KEYBOARD, 1);
}

/* ---- setup ---- */

int twl_server_init(struct twl_server *server, const struct twl_config *config, const struct twl_backend *backend,
		EGLDisplay egl_display) {
	server->config = *config;
	server->backend = *backend;
	server->egl_display = egl_display;
	server->event_fd = -1;
	server->dpi = 160;
	server->refresh_mhz = 60000;
	server->width_px = 1280;
	server->height_px = 720;
	server->scale = output_scale(server);
	wl_list_init(&server->surfaces);
	wl_list_init(&server->toplevels);
	wl_list_init(&server->popups);
	wl_array_init(&server->release_after_frame);

	server->display = wl_display_create();
	if (!server->display)
		return -1;
	server->loop = wl_display_get_event_loop(server->display);

	server->renderer = twl_renderer_create(server, egl_display);
	if (!server->renderer) {
		twl_error("no renderer");
		return -1;
	}

	if (config->dmabuf) {
		const char *node = config->render_node ? config->render_node : "/dev/dri/renderD128";
		struct stat st;
		if (stat(node, &st) == 0 && S_ISCHR(st.st_mode)) {
			server->dmabuf_dev = st.st_rdev;
			server->dmabuf_enabled = true;
			twl_info("linux-dmabuf on %s", node);
		} else {
			twl_info("linux-dmabuf off: %s: %s (clients use wl_shm, e.g. llvmpipe)", node, strerror(errno));
		}
	}

	twl_compositor_global_init(server);
	twl_viewporter_global_init(server);
	twl_buffer_global_init(server);
	twl_output_global_init(server);
	twl_xdg_global_init(server);
	twl_seat_init(server);

	const char *socket = config->socket;
	if (socket) {
		if (wl_display_add_socket(server->display, socket) < 0) {
			twl_error("can't listen on %s (XDG_RUNTIME_DIR=%s): %s", socket, getenv("XDG_RUNTIME_DIR"),
					strerror(errno));
			return -1;
		}
		snprintf(server->socket_name, sizeof(server->socket_name), "%s", socket);
	} else {
		if (!(socket = wl_display_add_socket_auto(server->display))) {
			twl_error("can't create a socket in XDG_RUNTIME_DIR=%s", getenv("XDG_RUNTIME_DIR"));
			return -1;
		}
		snprintf(server->socket_name, sizeof(server->socket_name), "%s", socket);
	}
	return 0;
}

const char *twl_server_socket_name(struct twl_server *server) {
	return server->socket_name;
}

void twl_server_finish(struct twl_server *server) {
	close_event_fd(server);
	wl_display_destroy_clients(server->display);
	release_after_frame(server);
	wl_array_release(&server->release_after_frame);
	twl_renderer_destroy(server->renderer);
	wl_display_destroy(server->display);
}
