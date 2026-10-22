/*
 * Termux:Wayland server side: runs in Termux's process (app_process, CmdEntryPoint) like the X
 * server of Termux:X11 did, and talks to the activity over the same lorie protocol.
 *
 * The Wayland compositor (../compositor) composites every window into one of two
 * AHardwareBuffers (the "root window" the activity shows), paced by the display's vsync
 * (AChoreographer). The activity draws that buffer and the cursor, and sends input back.
 */
#define _GNU_SOURCE
#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <android/choreographer.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "lorie.h"
#include "../compositor/twl.h"

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "TermuxWayland", __VA_ARGS__)

__attribute__((weak)) EGLClientBuffer eglGetNativeClientBufferANDROID(const struct AHardwareBuffer *buffer);

/* XI2 touch event types the activity sends */
#define XI_TouchBegin 18
#define XI_TouchUpdate 19
#define XI_TouchEnd 20

struct root_buffer {
	LorieBuffer *buffer;
	EGLImageKHR image;
	GLuint tex, fbo;
};

static struct {
	JavaVM *vm;
	JNIEnv *env; /* compositor thread */
	jobject thiz;
	jmethodID sendBroadcast, sendBroadcastDelayed;

	struct twl_config config;
	int cli_scale;
	char **xstartup_argv;
	char *xstartup;

	struct twl_server server;
	pthread_mutex_t ready_lock;
	pthread_cond_t ready_cond;
	int ready; /* 1 running, -1 failed */

	EGLDisplay dpy;
	EGLContext ctx;
	EGLSurface pbuffer;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

	int wake_fd;
	atomic_int pending_conn;
	atomic_bool vsync;

	volatile int conn_fd;
	struct wl_event_source *conn_source;

	int state_fd;
	struct lorie_shared_server_state *state;
	pthread_cond_t *renderer_cond;
	pthread_cond_t renderer_cond_placeholder;

	struct root_buffer root[2];
	int front;
	int32_t root_w, root_h;

	int32_t width, height, framerate, pref_scale, dpi;
	bool surface_available;
	bool clipboard_sync;
	bool text_input;
	bool pointer_captured;
} g = {
	.pending_conn = -1,
	.conn_fd = -1,
	.state_fd = -1,
	.width = 1280,
	.height = 720,
	.framerate = 60,
	.dpi = 160,
	.config = {.dmabuf = true, .dmabuf_import = true, .ahb = true},
	.ready_lock = PTHREAD_MUTEX_INITIALIZER,
	.ready_cond = PTHREAD_COND_INITIALIZER,
	.renderer_cond_placeholder = PTHREAD_COND_INITIALIZER,
};

bool lorieConnectionAlive(void) {
	int fd = g.conn_fd;
	if (fd == -1)
		return false;
	struct pollfd p = {.fd = fd, .events = POLLIN | POLLHUP | POLLERR | POLLRDHUP};
	return !(poll(&p, 1, 0) == 1 && (p.revents & (POLLERR | POLLNVAL | POLLRDHUP | POLLHUP)));
}

static void send_event(const lorieEvent *e) {
	if (g.conn_fd != -1)
		(void) !write(g.conn_fd, e, sizeof(*e));
}

static void wake(void) {
	eventfd_write(g.wake_fd, 1);
}

static void signal_renderer(void) {
	pthread_cond_signal(g.renderer_cond ? g.renderer_cond : &g.renderer_cond_placeholder);
}

/* ---- root window buffers ---- */

static void register_buffer(LorieBuffer *b) {
	if (g.conn_fd == -1 || !b)
		return;
	lorieEvent e = {.type = EVENT_ADD_BUFFER};
	send_event(&e);
	LorieBuffer_sendHandleToUnixSocket(b, g.conn_fd);
}

static void root_free(void) {
	for (int i = 0; i < 2; i++) {
		struct root_buffer *r = &g.root[i];
		if (!r->buffer)
			continue;
		if (g.conn_fd != -1) {
			lorieEvent e = {.removeBuffer = {.t = EVENT_REMOVE_BUFFER, .id = LorieBuffer_description(r->buffer)->id}};
			send_event(&e);
		}
		glDeleteFramebuffers(1, &r->fbo);
		glDeleteTextures(1, &r->tex);
		g.destroy_image(g.dpy, r->image);
		LorieBuffer_release(r->buffer);
		memset(r, 0, sizeof(*r));
	}
	g.root_w = g.root_h = 0;
}

static bool root_alloc(int32_t w, int32_t h) {
	root_free();
	const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
	for (int i = 0; i < 2; i++) {
		struct root_buffer *r = &g.root[i];
		AHardwareBuffer *ahb = NULL;
		AHardwareBuffer_Desc desc = {
			.width = (uint32_t) w,
			.height = (uint32_t) h,
			.layers = 1,
			.format = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM,
			.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER,
		};
		if (AHardwareBuffer_allocate(&desc, &ahb) != 0) {
			log(ERROR, "AHardwareBuffer_allocate %dx%d failed", w, h);
			root_free();
			return false;
		}
		r->buffer = LorieBuffer_wrapAHardwareBuffer(ahb); /* takes our reference */
		EGLClientBuffer cb = eglGetNativeClientBufferANDROID ? eglGetNativeClientBufferANDROID(ahb) : NULL;
		r->image = cb ? g.create_image(g.dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attribs) : NULL;
		if (!r->buffer || !r->image) {
			log(ERROR, "can't render into an AHardwareBuffer (EGL 0x%x)", eglGetError());
			root_free();
			return false;
		}
		glGenTextures(1, &r->tex);
		glBindTexture(GL_TEXTURE_2D, r->tex);
		g.image_target_texture(GL_TEXTURE_2D, r->image);
		glGenFramebuffers(1, &r->fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			log(ERROR, "root framebuffer incomplete");
			root_free();
			return false;
		}
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		register_buffer(r->buffer);
	}
	glFinish();
	g.root_w = w;
	g.root_h = h;
	g.front = 0;
	if (g.state) {
		g.state->rootWindowTextureID = LorieBuffer_description(g.root[0].buffer)->id;
		g.state->drawRequested = 1;
		signal_renderer();
	}
	return true;
}

static void update_output(void) {
	if ((g.width != g.root_w || g.height != g.root_h) && g.width > 0 && g.height > 0)
		root_alloc(g.width, g.height);
	g.server.config.scale = g.cli_scale ? g.cli_scale : g.pref_scale;
	bool have = g.conn_fd != -1 && g.surface_available && g.root_w > 0;
	twl_server_set_output(&g.server, have, g.root_w, g.root_h, g.dpi, g.framerate * 1000);
}

/* ---- backend ---- */

static bool begin_frame(void *data, int32_t *w, int32_t *h) {
	if (g.conn_fd == -1 || !g.root_w)
		return false;
	glBindFramebuffer(GL_FRAMEBUFFER, g.root[!g.front].fbo);
	*w = g.root_w;
	*h = g.root_h;
	return true;
}

static void end_frame(void *data) {
	/* The activity samples this buffer from another process: it must be complete first. */
	glFinish();
	g.front = !g.front;
	lorie_mutex_lock(&g.state->lock, &g.state->lockingPid);
	g.state->rootWindowTextureID = LorieBuffer_description(g.root[g.front].buffer)->id;
	g.state->drawRequested = 1;
	lorie_mutex_unlock(&g.state->lock, &g.state->lockingPid);
	signal_renderer();
}

static void send_to_frontend(void *data, const struct twl_event *e) {
	if (e->type == TWL_EV_SHOW_KEYBOARD) {
		g.text_input = e->arg != 0;
		lorieEvent le = {.textInput = {.t = EVENT_TEXT_INPUT_STATE, .enabled = (uint8_t) g.text_input}};
		send_event(&le);
	} else if (e->type == TWL_EV_POINTER_CAPTURE) {
		g.pointer_captured = e->arg != 0;
		lorieEvent le = {.textInput = {.t = EVENT_POINTER_CAPTURE, .enabled = (uint8_t) g.pointer_captured}};
		send_event(&le);
	}
}

static void set_cursor(void *data, const uint8_t *rgba, int32_t w, int32_t h, int32_t hx, int32_t hy) {
	struct lorie_shared_server_state *s = g.state;
	lorie_mutex_lock(&s->cursor.lock, &s->cursor.lockingPid);
	if (w > 0 && h > 0 && w < 512 && h < 512) {
		memcpy(s->cursor.bits, rgba, (size_t) w * h * 4);
		s->cursor.width = (uint32_t) w;
		s->cursor.height = (uint32_t) h;
		s->cursor.xhot = (uint32_t) TWL_MAX(hx, 0);
		s->cursor.yhot = (uint32_t) TWL_MAX(hy, 0);
	} else {
		s->cursor.width = s->cursor.height = 0;
	}
	s->cursor.updated = 1;
	lorie_mutex_unlock(&s->cursor.lock, &s->cursor.lockingPid);
	signal_renderer();
}

static void move_cursor(void *data, int32_t x, int32_t y, bool visible) {
	struct lorie_shared_server_state *s = g.state;
	s->cursor.x = (uint32_t) TWL_MAX(x, 0);
	s->cursor.y = (uint32_t) TWL_MAX(y, 0);
	s->cursor.visible = visible;
	s->cursor.moved = 1;
	signal_renderer();
}

static void clipboard_text(void *data, const char *text) {
	if (!g.clipboard_sync || g.conn_fd == -1)
		return;
	size_t len = strlen(text);
	lorieEvent e = {.clipboardSend = {.t = EVENT_CLIPBOARD_SEND, .count = (uint32_t) len}};
	send_event(&e);
	(void) !write(g.conn_fd, text, len);
}

/* ---- activity connection ---- */

static void handle_event(const lorieEvent *e);

static void disconnect(void) {
	if (g.conn_source)
		wl_event_source_remove(g.conn_source);
	g.conn_source = NULL;
	if (g.conn_fd != -1)
		close(g.conn_fd);
	g.conn_fd = -1;
	twl_server_handle_event(&g.server, &(struct twl_event){.type = TWL_EV_KEYS_RELEASE}, NULL);
	twl_server_handle_event(&g.server, &(struct twl_event){.type = TWL_EV_TOUCH_CANCEL}, NULL);
	g.surface_available = false;
	update_output();
	log(INFO, "activity disconnected");
	if (g.env)
		(*g.env)->CallVoidMethod(g.env, g.thiz, g.sendBroadcastDelayed);
}

static int conn_readable(int fd, uint32_t mask, void *data) {
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		disconnect();
		return 0;
	}
	lorieEvent e;
	int n;
	do {
		ssize_t r = read(fd, &e, sizeof(e));
		if (r != sizeof(e)) {
			if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR))
				disconnect();
			return 0;
		}
		handle_event(&e);
		if (g.conn_fd != fd)
			return 0;
	} while (ioctl(fd, FIONREAD, &n) >= 0 && n >= (int) sizeof(e));
	wl_display_flush_clients(g.server.display);
	return 0;
}

static void connected(int fd) {
	if (g.conn_fd != -1) {
		wl_event_source_remove(g.conn_source);
		close(g.conn_fd);
	}
	g.conn_fd = fd;
	g.conn_source = wl_event_loop_add_fd(g.server.loop, fd, WL_EVENT_READABLE, conn_readable, NULL);
	log(INFO, "activity connected");

	g.state->drawRequested = g.state->cursor.updated = 1;
	lorieEvent e = {.type = EVENT_SHARED_SERVER_STATE};
	send_event(&e);
	ancil_send_fd(g.conn_fd, g.state_fd);
	for (int i = 0; i < 2; i++)
		register_buffer(g.root[i].buffer);
	if (g.text_input) {
		lorieEvent t = {.textInput = {.t = EVENT_TEXT_INPUT_STATE, .enabled = 1}};
		send_event(&t);
	}
	update_output();
}

static void utf8_encode(uint32_t c, char out[5]) {
	if (c < 0x80) {
		out[0] = (char) c;
		out[1] = 0;
	} else if (c < 0x800) {
		out[0] = (char) (0xc0 | (c >> 6));
		out[1] = (char) (0x80 | (c & 0x3f));
		out[2] = 0;
	} else if (c < 0x10000) {
		out[0] = (char) (0xe0 | (c >> 12));
		out[1] = (char) (0x80 | ((c >> 6) & 0x3f));
		out[2] = (char) (0x80 | (c & 0x3f));
		out[3] = 0;
	} else {
		out[0] = (char) (0xf0 | (c >> 18));
		out[1] = (char) (0x80 | ((c >> 12) & 0x3f));
		out[2] = (char) (0x80 | ((c >> 6) & 0x3f));
		out[3] = (char) (0x80 | (c & 0x3f));
		out[4] = 0;
	}
}

static void read_payload(char *buf, size_t n) {
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(g.conn_fd, buf + got, n - got);
		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			break;
		}
		got += (size_t) r;
	}
	buf[got] = 0;
}

static void handle_event(const lorieEvent *e) {
	struct twl_server *server = &g.server;
	struct twl_event te = {.time = twl_now_ms()};
	switch (e->type) {
	case EVENT_SCREEN_SIZE: {
		if (e->screenSize.name_size) {
			char *name = malloc(e->screenSize.name_size + 1);
			if (name)
				read_payload(name, e->screenSize.name_size);
			free(name);
		}
		if (e->screenSize.width && e->screenSize.height) {
			g.width = e->screenSize.width;
			g.height = e->screenSize.height;
		}
		if (e->screenSize.framerate)
			g.framerate = e->screenSize.framerate;
		update_output();
		break;
	}
	case EVENT_OUTPUT_CONFIG:
		g.pref_scale = e->outputConfig.scale;
		if (e->outputConfig.dpi)
			g.dpi = e->outputConfig.dpi;
		update_output();
		break;
	case EVENT_TOUCH:
		te.type = e->touch.type == XI_TouchBegin ? TWL_EV_TOUCH_DOWN
				: e->touch.type == XI_TouchUpdate ? TWL_EV_TOUCH_MOTION : TWL_EV_TOUCH_UP;
		te.id = e->touch.id;
		te.x = e->touch.x;
		te.y = e->touch.y;
		twl_server_handle_event(server, &te, NULL);
		twl_server_handle_event(server, &(struct twl_event){.type = TWL_EV_TOUCH_FRAME}, NULL);
		break;
	case EVENT_MOUSE:
		switch (e->mouse.detail) {
		case 0:
			te.type = e->mouse.relative ? TWL_EV_POINTER_RELATIVE : TWL_EV_POINTER_MOTION;
			te.x = e->mouse.x;
			te.y = e->mouse.y;
			break;
		case 1:
		case 2:
		case 3: {
			static const int32_t buttons[] = {0, 0x110 /* left */, 0x112 /* middle */, 0x111 /* right */};
			te.type = TWL_EV_POINTER_BUTTON;
			te.id = buttons[e->mouse.detail];
			te.arg = e->mouse.down;
			break;
		}
		case 4:
			te.type = TWL_EV_POINTER_AXIS;
			te.x = e->mouse.x / 120.f;
			te.y = e->mouse.y / 120.f;
			break;
		default:
			return;
		}
		twl_server_handle_event(server, &te, NULL);
		break;
	case EVENT_STYLUS: {
		static uint8_t prev;
		te.type = TWL_EV_POINTER_MOTION;
		te.x = e->stylus.x;
		te.y = e->stylus.y;
		twl_server_handle_event(server, &te, NULL);
		uint8_t diff = prev ^ e->stylus.buttons;
		static const int32_t buttons[] = {0x110, 0x112, 0x111};
		for (int i = 0; i < 3; i++)
			if (diff & (1 << i))
				twl_server_handle_event(server, &(struct twl_event){.type = TWL_EV_POINTER_BUTTON, .id = buttons[i],
						.arg = (e->stylus.buttons >> i) & 1}, NULL);
		prev = e->stylus.buttons;
		break;
	}
	case EVENT_KEY:
		/* X keycodes: evdev + 8 */
		if (e->key.key < 8)
			break;
		te.type = TWL_EV_KEY;
		te.id = e->key.key - 8;
		te.arg = e->key.state;
		twl_server_handle_event(server, &te, NULL);
		break;
	case EVENT_UNICODE: {
		char text[5];
		utf8_encode(e->unicode.code, text);
		te.type = TWL_EV_TEXT;
		twl_server_handle_event(server, &te, text);
		break;
	}
	case EVENT_LOCK_KEYS_STATE:
		twl_seat_set_locks(server, e->lockKeysState.state & 1, (e->lockKeysState.state >> 1) & 1);
		break;
	case EVENT_CLIPBOARD_ENABLE:
		g.clipboard_sync = e->clipboardEnable.enable;
		break;
	case EVENT_CLIPBOARD_ANNOUNCE:
		if (g.clipboard_sync) {
			lorieEvent r = {.type = EVENT_CLIPBOARD_REQUEST};
			send_event(&r);
		}
		break;
	case EVENT_CLIPBOARD_SEND: {
		char *text = malloc((size_t) e->clipboardSend.count + 1);
		if (!text)
			break;
		read_payload(text, e->clipboardSend.count);
		if (g.clipboard_sync)
			twl_seat_set_text_selection(server, text);
		free(text);
		break;
	}
	case EVENT_RENDERER_WAKEUP_COND: {
		int fd = ancil_recv_fd(g.conn_fd);
		if (fd < 0)
			break;
		pthread_cond_t *cond = mmap(NULL, sizeof(pthread_cond_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (cond == MAP_FAILED)
			break;
		pthread_cond_t *old = g.renderer_cond;
		g.renderer_cond = cond;
		signal_renderer();
		if (old)
			munmap(old, sizeof(pthread_cond_t));
		break;
	}
	case EVENT_SYNC: {
		lorieEvent r = {.sync = {.t = EVENT_SYNC_REPLY, .serial = e->sync.serial}};
		send_event(&r);
		break;
	}
	default:
		break;
	}
}

/* ---- vsync and wakeups ---- */

static void choreographer_callback(long frame_time, void *data) {
	AChoreographer_postFrameCallback(AChoreographer_getInstance(), choreographer_callback, NULL);
	atomic_store(&g.vsync, true);
	wake();
}

static int wake_readable(int fd, uint32_t mask, void *data) {
	eventfd_t v;
	eventfd_read(fd, &v);
	int conn = atomic_exchange(&g.pending_conn, -1);
	if (conn >= 0)
		connected(conn);
	if (atomic_exchange(&g.vsync, false) && g.state) {
		g.state->waitForNextFrame = 0;
		bool available = g.state->surfaceAvailable && g.conn_fd != -1;
		if (available != g.surface_available) {
			g.surface_available = available;
			update_output();
		}
		twl_server_vsync(&g.server);
	}
	return 0;
}

/* ---- knocks: the activity asks for a broadcast with our binder ---- */

static int knock_readable(int fd, uint32_t mask, void *data) {
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (c < 0)
		return 0;
	char buf[64] = {0};
	struct pollfd p = {.fd = c, .events = POLLIN};
	if (poll(&p, 1, 200) > 0 && recv(c, buf, sizeof(buf), MSG_DONTWAIT) > 0 && !memcmp(buf, MAGIC, sizeof(MAGIC) - 1))
		(*g.env)->CallVoidMethod(g.env, g.thiz, g.sendBroadcast);
	close(c);
	return 0;
}

static void listen_for_knocks(void) {
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr = {.s_addr = htonl(INADDR_ANY)}};
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
	if (fd < 0)
		return;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, 5) < 0) {
		log(ERROR, "knock port %d: %s", PORT, strerror(errno));
		close(fd);
		return;
	}
	wl_event_loop_add_fd(g.server.loop, fd, WL_EVENT_READABLE, knock_readable, NULL);
}

/* ---- startup command ---- */

static void *child_waiter(void *arg) {
	pid_t pid = (pid_t) (intptr_t) arg;
	int status;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	log(INFO, "startup command exited (%d), stopping", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	_exit(0);
	return NULL;
}

static void start_xstartup(const char *socket) {
	if (!g.xstartup && !g.xstartup_argv)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		setenv("WAYLAND_DISPLAY", socket, 1);
		unsetenv("DISPLAY");
#define INHERIT_VAR(v) { char *val = getenv("XSTARTUP_" #v); if (val && *val) setenv(#v, val, 1); unsetenv("XSTARTUP_" #v); }
		INHERIT_VAR(CLASSPATH)
		INHERIT_VAR(LD_LIBRARY_PATH)
		INHERIT_VAR(LD_PRELOAD)
#undef INHERIT_VAR
		if (g.xstartup_argv) {
			execvp(g.xstartup_argv[0], g.xstartup_argv);
			dprintf(2, "Failed to start `%s`: %s\n", g.xstartup_argv[0], strerror(errno));
		} else {
			execlp("sh", "sh", "-c", g.xstartup, NULL);
			dprintf(2, "Failed to start `sh -c \"%s\"`: %s\n", g.xstartup, strerror(errno));
		}
		_exit(127);
	}
	if (pid > 0) {
		pthread_t t;
		pthread_create(&t, NULL, child_waiter, (void *) (intptr_t) pid);
	}
}

/* ---- compositor thread ---- */

static bool egl_init(void) {
	g.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(g.dpy, NULL, NULL))
		return false;
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLConfig config;
	EGLint n = 0;
	if (!eglChooseConfig(g.dpy, config_attribs, &config, 1, &n) || n < 1)
		return false;
	const EGLint es3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}, es2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	g.ctx = eglCreateContext(g.dpy, config, EGL_NO_CONTEXT, es3);
	if (g.ctx == EGL_NO_CONTEXT)
		g.ctx = eglCreateContext(g.dpy, config, EGL_NO_CONTEXT, es2);
	if (g.ctx == EGL_NO_CONTEXT)
		return false;
	const char *ext = eglQueryString(g.dpy, EGL_EXTENSIONS);
	if (!ext || !strstr(ext, "EGL_KHR_surfaceless_context")) {
		const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
		g.pbuffer = eglCreatePbufferSurface(g.dpy, config, pb);
	}
	if (!eglMakeCurrent(g.dpy, g.pbuffer, g.pbuffer, g.ctx))
		return false;
	g.create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
	g.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
	g.image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	return g.create_image && g.destroy_image && g.image_target_texture;
}

static void set_ready(int ready) {
	pthread_mutex_lock(&g.ready_lock);
	g.ready = ready;
	pthread_cond_broadcast(&g.ready_cond);
	pthread_mutex_unlock(&g.ready_lock);
}

static void *compositor_thread(void *arg) {
	if ((*g.vm)->AttachCurrentThread(g.vm, &g.env, NULL) != JNI_OK) {
		set_ready(-1);
		return NULL;
	}
	if (!egl_init()) {
		dprintf(2, "Termux:Wayland: EGL setup failed (0x%x)\n", eglGetError());
		set_ready(-1);
		return NULL;
	}

	g.state_fd = LorieBuffer_createRegion("termux-wayland", sizeof(*g.state));
	g.state = g.state_fd < 0 ? MAP_FAILED : mmap(NULL, sizeof(*g.state), PROT_READ | PROT_WRITE, MAP_SHARED, g.state_fd, 0);
	if (g.state == MAP_FAILED) {
		dprintf(2, "Termux:Wayland: can't allocate the shared state\n");
		set_ready(-1);
		return NULL;
	}
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&g.state->lock, &attr);
	pthread_mutex_init(&g.state->cursor.lock, &attr);

	struct twl_backend backend = {
		.begin_frame = begin_frame,
		.end_frame = end_frame,
		.send_to_frontend = send_to_frontend,
		.paced = true,
		.flip_y = true,
		.set_cursor = set_cursor,
		.move_cursor = move_cursor,
		.clipboard_text = clipboard_text,
	};
	if (twl_server_init(&g.server, &g.config, &backend, g.dpy) < 0) {
		set_ready(-1);
		return NULL;
	}
	wl_event_loop_add_fd(g.server.loop, g.wake_fd, WL_EVENT_READABLE, wake_readable, NULL);
	listen_for_knocks();
	root_alloc(g.width, g.height);
	update_output();

	const char *socket = twl_server_socket_name(&g.server);
	printf("Termux:Wayland: WAYLAND_DISPLAY=%s (XDG_RUNTIME_DIR=%s)\n", socket, getenv("XDG_RUNTIME_DIR"));
	fflush(stdout);
	start_xstartup(socket);
	(*g.env)->CallVoidMethod(g.env, g.thiz, g.sendBroadcastDelayed);
	set_ready(1);

	wl_display_run(g.server.display);
	twl_server_finish(&g.server);
	_exit(0);
	return NULL;
}

/* ---- JNI ---- */

static void usage(void) {
	printf("termux-wayland [options] [-- command args...]\n"
	       "  -scale N          output scale (default: the app's preference, else from the screen density)\n"
	       "  -socket NAME      Wayland socket in $XDG_RUNTIME_DIR (default: first free wayland-N)\n"
	       "  -keymap FILE      XKB keymap for clients (xkbcli compile-keymap --layout ...); default us\n"
	       "  -no-dmabuf        no linux-dmabuf: GPU clients fall back to wl_shm\n"
	       "  -no-dmabuf-import copy dma-bufs instead of importing them into EGL\n"
	       "  -no-ahb           upload textures with glTexSubImage2D instead of AHardwareBuffers\n"
	       "  -xstartup \"cmd\"   run cmd with WAYLAND_DISPLAY set; the server exits with it\n"
	       "  -- command args   same, without a shell\n");
}

static jboolean start(JNIEnv *env, jobject self, jobjectArray args) {
	int argc = (*env)->GetArrayLength(env, args);
	char **argv = calloc((size_t) argc + 1, sizeof(char *));
	for (int i = 0; i < argc; i++) {
		jstring js = (*env)->GetObjectArrayElement(env, args, i);
		const char *s = (*env)->GetStringUTFChars(env, js, NULL);
		argv[i] = strdup(s);
		(*env)->ReleaseStringUTFChars(env, js, s);
	}
	for (int i = 0; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--")) {
			if (i + 1 < argc)
				g.xstartup_argv = argv + i + 1;
			break;
		} else if (!strcmp(a, "-scale") && v) {
			g.cli_scale = atoi(argv[++i]);
		} else if (!strcmp(a, "-socket") && v) {
			g.config.socket = argv[++i];
		} else if (!strcmp(a, "-keymap") && v) {
			g.config.keymap_file = argv[++i];
		} else if (!strcmp(a, "-xstartup") && v) {
			g.xstartup = argv[++i];
		} else if (!strcmp(a, "-no-dmabuf")) {
			g.config.dmabuf = false;
		} else if (!strcmp(a, "-no-dmabuf-import")) {
			g.config.dmabuf_import = false;
		} else if (!strcmp(a, "-no-ahb")) {
			g.config.ahb = false;
		} else if (!strcmp(a, "-help") || !strcmp(a, "--help") || !strcmp(a, "-h")) {
			usage();
			return JNI_FALSE;
		} else {
			printf("Unknown option %s\n", a);
			usage();
			return JNI_FALSE;
		}
	}
	if (!g.xstartup && !g.xstartup_argv) {
		const char *x = getenv("TERMUX_WAYLAND_XSTARTUP");
		if (x && *x)
			g.xstartup = strdup(x);
	}

	/* adb sets TMPDIR to /data/local/tmp; Termux's own tmp is what clients see */
	if (!strcmp(getenv("TMPDIR") ? getenv("TMPDIR") : "", "/data/local/tmp"))
		unsetenv("TMPDIR");
	if (!getenv("TMPDIR") && access("/data/data/com.termux/files/usr/tmp", F_OK) == 0)
		setenv("TMPDIR", "/data/data/com.termux/files/usr/tmp", 1);
	if (!getenv("XDG_RUNTIME_DIR") && getenv("TMPDIR"))
		setenv("XDG_RUNTIME_DIR", getenv("TMPDIR"), 1);
	if (!getenv("XDG_RUNTIME_DIR")) {
		dprintf(2, "Termux:Wayland: neither XDG_RUNTIME_DIR nor TMPDIR is set\n");
		return JNI_FALSE;
	}
	/* children of the server (the startup command) get termux-exec like any Termux program */
	if (access("/data/data/com.termux/files/usr/lib/libtermux-exec.so", F_OK) == 0 && !getenv("XSTARTUP_LD_PRELOAD"))
		setenv("LD_PRELOAD", "/data/data/com.termux/files/usr/lib/libtermux-exec.so", 1);
	signal(SIGPIPE, SIG_IGN);

	(*env)->GetJavaVM(env, &g.vm);
	g.thiz = (*env)->NewGlobalRef(env, self);
	jclass cls = (*env)->GetObjectClass(env, self);
	g.sendBroadcast = (*env)->GetMethodID(env, cls, "sendBroadcast", "()V");
	g.sendBroadcastDelayed = (*env)->GetMethodID(env, cls, "sendBroadcastDelayed", "()V");
	g.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	/* vsync on the main looper (Looper.loop() runs once we return) */
	AChoreographer_postFrameCallback(AChoreographer_getInstance(), choreographer_callback, NULL);

	pthread_t t;
	pthread_create(&t, NULL, compositor_thread, NULL);
	pthread_mutex_lock(&g.ready_lock);
	while (!g.ready)
		pthread_cond_wait(&g.ready_cond, &g.ready_lock);
	pthread_mutex_unlock(&g.ready_lock);
	return g.ready > 0;
}

static jobject adopt_fd(JNIEnv *env, int fd) {
	jclass pfd = (*env)->FindClass(env, "android/os/ParcelFileDescriptor");
	jmethodID adopt = (*env)->GetStaticMethodID(env, pfd, "adoptFd", "(I)Landroid/os/ParcelFileDescriptor;");
	return (*env)->CallStaticObjectMethod(env, pfd, adopt, fd);
}

static jobject get_connection(JNIEnv *env, jobject self) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
		return NULL;
	int old = atomic_exchange(&g.pending_conn, sv[1]);
	if (old >= 0)
		close(old);
	wake();
	return adopt_fd(env, sv[0]);
}

/* The activity's logcat, shown on our stderr (TERMUX_WAYLAND_DEBUG=1). */
static void *logcat_forwarder(void *arg) {
	char buf[4096];
	ssize_t n;
	while ((n = read((int) (intptr_t) arg, buf, sizeof(buf))) > 0)
		(void) !write(2, buf, (size_t) n);
	close((int) (intptr_t) arg);
	return NULL;
}

static jobject get_logcat_output(JNIEnv *env, jobject self) {
	const char *debug = getenv("TERMUX_WAYLAND_DEBUG");
	if (!debug || strcmp(debug, "1"))
		return NULL;
	int p[2];
	if (pipe(p) < 0)
		return NULL;
	fchmod(p[1], 0777);
	pthread_t t;
	pthread_create(&t, NULL, logcat_forwarder, (void *) (intptr_t) p[0]);
	return adopt_fd(env, p[1]);
}

static void report_fatal_error(JNIEnv *env, jobject self, jstring message) {
	if (g.conn_fd != -1)
		return;
	const char *m = (*env)->GetStringUTFChars(env, message, NULL);
	dprintf(2, "Termux:Wayland: %s\n", m);
	log(ERROR, "%s", m);
	(*env)->ReleaseStringUTFChars(env, message, m);
	_exit(1);
}

static jboolean is_connected(void) {
	return g.conn_fd != -1;
}

void registerCmdEntryPointNatives(JNIEnv *env) {
	static JNINativeMethod methods[] = {
		{"start", "([Ljava/lang/String;)Z", (void *) start},
		{"getXConnection", "()Landroid/os/ParcelFileDescriptor;", (void *) get_connection},
		{"getLogcatOutput", "()Landroid/os/ParcelFileDescriptor;", (void *) get_logcat_output},
		{"reportFatalError", "(Ljava/lang/String;)V", (void *) report_fatal_error},
		{"connected", "()Z", (void *) is_connected}, /* @CriticalNative */
	};
	jclass cls = (*env)->FindClass(env, "com/termux/wayland/CmdEntryPoint");
	if (cls)
		(*env)->RegisterNatives(env, cls, methods, sizeof(methods) / sizeof(methods[0]));
	(*env)->ExceptionClear(env);
}
