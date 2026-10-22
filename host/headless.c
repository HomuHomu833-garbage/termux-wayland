/*
 * Linux test build of the compositor: renders off screen (EGL surfaceless, Mesa), takes the
 * same input events as the Android activity on a unix socket, writes each frame to a PPM.
 * Lets the compositor be run against real Wayland clients (GTK, Qt, SDL, llvmpipe EGL,
 * weston-simple-*) without a phone:
 *
 *   twl-headless --socket wayland-twl --size 1280x720 --scale 1 --dump /tmp/frame.ppm \
 *       --events /tmp/twl-events
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "../lorie/src/main/cpp/compositor/twl.h"

struct headless {
	struct twl_server server;
	EGLDisplay display;
	EGLContext context;
	GLuint fbo, rb;
	int32_t w, h;
	const char *dump;
	int listen_fd;
	unsigned frames;
};

static bool begin_frame(void *data, int32_t *w, int32_t *h) {
	struct headless *hl = data;
	glBindFramebuffer(GL_FRAMEBUFFER, hl->fbo);
	*w = hl->w;
	*h = hl->h;
	return true;
}

static void end_frame(void *data) {
	struct headless *hl = data;
	hl->frames++;
	if (!hl->dump)
		return;
	size_t n = (size_t) hl->w * hl->h * 4;
	uint8_t *px = malloc(n);
	if (!px)
		return;
	twl_renderer_read_pixels(hl->server.renderer, hl->w, hl->h, px);
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.tmp", hl->dump);
	FILE *f = fopen(tmp, "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", hl->w, hl->h);
		for (int32_t y = hl->h - 1; y >= 0; y--)
			for (int32_t x = 0; x < hl->w; x++)
				fwrite(px + ((size_t) y * hl->w + x) * 4, 1, 3, f);
		fclose(f);
		rename(tmp, hl->dump);
	}
	free(px);
}

static void send_to_frontend(void *data, const struct twl_event *e) {
	struct headless *hl = data;
	if (hl->server.event_fd >= 0)
		(void) !write(hl->server.event_fd, e, sizeof(*e));
	twl_info("to frontend: type %u arg %u", e->type, e->arg);
}

static int accept_events(int fd, uint32_t mask, void *data) {
	struct headless *hl = data;
	int c = accept(fd, NULL, NULL);
	if (c >= 0)
		twl_server_set_event_fd(&hl->server, c);
	return 0;
}

static bool egl_init(struct headless *hl) {
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
			(PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
	hl->display = get_platform_display ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
			: eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(hl->display, NULL, NULL))
		return false;
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint config_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
	EGLConfig config = NULL;
	EGLint n = 0;
	eglChooseConfig(hl->display, config_attribs, &config, 1, &n);
	const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	hl->context = eglCreateContext(hl->display, n ? config : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
	if (hl->context == EGL_NO_CONTEXT)
		return false;
	if (!eglMakeCurrent(hl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, hl->context))
		return false;
	glGenRenderbuffers(1, &hl->rb);
	glBindRenderbuffer(GL_RENDERBUFFER, hl->rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, hl->w, hl->h);
	glGenFramebuffers(1, &hl->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, hl->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, hl->rb);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

int main(int argc, char **argv) {
	static struct headless hl;
	struct twl_config config = {.dmabuf = true, .dmabuf_import = true};
	const char *events = NULL;
	int32_t dpi = 160;
	hl.w = 1280;
	hl.h = 720;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--socket") && v)
			config.socket = argv[++i];
		else if (!strcmp(a, "--size") && v && sscanf(argv[++i], "%dx%d", &hl.w, &hl.h) == 2)
			;
		else if (!strcmp(a, "--scale") && v)
			config.scale = atoi(argv[++i]);
		else if (!strcmp(a, "--dpi") && v)
			dpi = atoi(argv[++i]);
		else if (!strcmp(a, "--dump") && v)
			hl.dump = argv[++i];
		else if (!strcmp(a, "--events") && v)
			events = argv[++i];
		else if (!strcmp(a, "--touch-pointer"))
			config.touch_pointer = true;
		else if (!strcmp(a, "--no-dmabuf"))
			config.dmabuf = false;
		else if (!strcmp(a, "--keymap") && v)
			config.keymap_file = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--socket name] [--size WxH] [--scale n] [--dpi n] [--dump file.ppm] "
					"[--events socket] [--touch-pointer] [--no-dmabuf] [--keymap file]\n", argv[0]);
			return 1;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	if (!egl_init(&hl)) {
		fprintf(stderr, "EGL setup failed (0x%x)\n", eglGetError());
		return 1;
	}
	struct twl_backend backend = {
		.begin_frame = begin_frame,
		.end_frame = end_frame,
		.send_to_frontend = send_to_frontend,
		.data = &hl,
	};
	if (twl_server_init(&hl.server, &config, &backend, hl.display) < 0)
		return 1;
	if (events) {
		struct sockaddr_un addr = {.sun_family = AF_UNIX};
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", events);
		unlink(events);
		hl.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (bind(hl.listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(hl.listen_fd, 4) < 0) {
			fprintf(stderr, "%s: %s\n", events, strerror(errno));
			return 1;
		}
		wl_event_loop_add_fd(hl.server.loop, hl.listen_fd, WL_EVENT_READABLE, accept_events, &hl);
	}
	twl_server_set_output(&hl.server, true, hl.w, hl.h, dpi, 60000);
	printf("WAYLAND_DISPLAY=%s\n", config.socket ? config.socket : "(auto)");
	fflush(stdout);
	wl_display_run(hl.server.display);
	twl_server_finish(&hl.server);
	return 0;
}
