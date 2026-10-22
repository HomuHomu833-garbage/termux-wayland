/*
 * GLES2 compositing. Surfaces are drawn as textured quads, bottom to top, premultiplied alpha.
 *
 * Where pixels come from:
 *  - wl_shm and dma-bufs EGL can't import: copied into the surface's texture at commit. On
 *    Android that texture is a pair of AHardwareBuffers (the CPU writes one while the GPU may
 *    still sample the other; sampled through EGL_ANDROID_image_native_buffer), else glTexSubImage2D.
 *  - dma-bufs EGL can import (EGL_EXT_image_dma_buf_import): sampled in place, no copy.
 */
#include <stdlib.h>
#include <string.h>
#include "twl.h"

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
/* libEGL exports it since API 26 */
__attribute__((weak)) EGLClientBuffer eglGetNativeClientBufferANDROID(const struct AHardwareBuffer *buffer);
#endif

#define GL_UNPACK_ROW_LENGTH_ 0x0CF2

#define DRM_FORMAT_ARGB8888 0x34325241
#define DRM_FORMAT_XRGB8888 0x34325258
#define DRM_FORMAT_XBGR8888 0x34324258

struct program {
	GLuint id;
	GLint rect, src, tex, swizzle, opaque;
};

struct twl_renderer {
	struct twl_server *server;
	EGLDisplay display;
	struct program tex2d, external;
	bool has_external;
	GLuint vbo;
	bool row_length;
	bool dmabuf_import;
	bool dmabuf_modifiers;
	bool use_ahb;
	EGLSyncKHR frame_sync;
	int32_t width, height; /* current frame, pixels */

	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
	PFNEGLCREATESYNCKHRPROC create_sync;
	PFNEGLCLIENTWAITSYNCKHRPROC client_wait_sync;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync;
};

static const char *vertex_src =
	"attribute vec2 pos;\n"
	"uniform vec4 rect;\n"
	"uniform vec4 src;\n"
	"varying vec2 v;\n"
	"void main() {\n"
	"	v = src.xy + pos * src.zw;\n"
	"	gl_Position = vec4(rect.xy + pos * rect.zw, 0.0, 1.0);\n"
	"}\n";

static const char *fragment_2d_src =
	"precision mediump float;\n"
	"varying vec2 v;\n"
	"uniform sampler2D tex;\n"
	"uniform float swizzle;\n"
	"uniform float opaque;\n"
	"void main() {\n"
	"	vec4 c = texture2D(tex, v);\n"
	"	c = mix(c, c.bgra, swizzle);\n"
	"	c.a = max(c.a, opaque);\n"
	"	gl_FragColor = c;\n"
	"}\n";

static const char *fragment_external_src =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"varying vec2 v;\n"
	"uniform samplerExternalOES tex;\n"
	"uniform float swizzle;\n"
	"uniform float opaque;\n"
	"void main() {\n"
	"	vec4 c = texture2D(tex, v);\n"
	"	c = mix(c, c.bgra, swizzle);\n"
	"	c.a = max(c.a, opaque);\n"
	"	gl_FragColor = c;\n"
	"}\n";

static bool has_extension(const char *list, const char *ext) {
	size_t n = strlen(ext);
	for (const char *p = list; p && (p = strstr(p, ext)); p += n)
		if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
			return true;
	return false;
}

static GLuint compile(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		twl_error("shader: %s", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static bool link_program(struct program *p, const char *fragment) {
	GLuint vs = compile(GL_VERTEX_SHADER, vertex_src), fs = compile(GL_FRAGMENT_SHADER, fragment);
	if (!vs || !fs)
		return false;
	p->id = glCreateProgram();
	glAttachShader(p->id, vs);
	glAttachShader(p->id, fs);
	glBindAttribLocation(p->id, 0, "pos");
	glLinkProgram(p->id);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = 0;
	glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
	if (!ok) {
		twl_error("program link failed");
		return false;
	}
	p->rect = glGetUniformLocation(p->id, "rect");
	p->src = glGetUniformLocation(p->id, "src");
	p->tex = glGetUniformLocation(p->id, "tex");
	p->swizzle = glGetUniformLocation(p->id, "swizzle");
	p->opaque = glGetUniformLocation(p->id, "opaque");
	return true;
}

struct twl_renderer *twl_renderer_create(struct twl_server *server, EGLDisplay display) {
	struct twl_renderer *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->server = server;
	r->display = display;
	const char *egl_ext = eglQueryString(display, EGL_EXTENSIONS);
	const char *gl_ext = (const char *) glGetString(GL_EXTENSIONS);
	const char *gl_version = (const char *) glGetString(GL_VERSION);
	twl_info("GL: %s / %s", (const char *) glGetString(GL_RENDERER), gl_version);

	if (!link_program(&r->tex2d, fragment_2d_src)) {
		free(r);
		return NULL;
	}
	r->has_external = has_extension(gl_ext, "GL_OES_EGL_image_external") && link_program(&r->external,
			fragment_external_src);
	r->row_length = (gl_version && strstr(gl_version, "OpenGL ES 3")) || has_extension(gl_ext, "GL_EXT_unpack_subimage");

	r->create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
	r->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
	r->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (has_extension(egl_ext, "EGL_KHR_fence_sync")) {
		r->create_sync = (PFNEGLCREATESYNCKHRPROC) eglGetProcAddress("eglCreateSyncKHR");
		r->client_wait_sync = (PFNEGLCLIENTWAITSYNCKHRPROC) eglGetProcAddress("eglClientWaitSyncKHR");
		r->destroy_sync = (PFNEGLDESTROYSYNCKHRPROC) eglGetProcAddress("eglDestroySyncKHR");
	}
	bool images = r->create_image && r->destroy_image && r->image_target_texture;
	r->dmabuf_import = server->config.dmabuf_import && images && r->has_external &&
			has_extension(egl_ext, "EGL_EXT_image_dma_buf_import");
	r->dmabuf_modifiers = has_extension(egl_ext, "EGL_EXT_image_dma_buf_import_modifiers");
#ifdef __ANDROID__
	r->use_ahb = server->config.ahb && images && eglGetNativeClientBufferANDROID &&
			has_extension(egl_ext, "EGL_ANDROID_image_native_buffer");
#endif
	twl_info("dma-buf import: %s, AHardwareBuffer textures: %s, unpack row length: %s",
			r->dmabuf_import ? "yes" : "no (copies)", r->use_ahb ? "yes" : "no", r->row_length ? "yes" : "no");

	static const GLfloat quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
	glGenBuffers(1, &r->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	return r;
}

void twl_renderer_destroy(struct twl_renderer *r) {
	if (!r)
		return;
	if (r->frame_sync)
		r->destroy_sync(r->display, r->frame_sync);
	glDeleteBuffers(1, &r->vbo);
	glDeleteProgram(r->tex2d.id);
	if (r->has_external)
		glDeleteProgram(r->external.id);
	free(r);
}

bool twl_renderer_dmabuf_import_supported(struct twl_renderer *r) {
	return r->dmabuf_import;
}

/* ---- textures ---- */

static void texture_params(GLenum target) {
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void twl_texture_release(struct twl_renderer *r, struct twl_texture *t) {
	if (t->tex)
		glDeleteTextures(1, &t->tex);
#ifdef __ANDROID__
	for (int i = 0; i < 2; i++) {
		if (t->ahb_tex[i])
			glDeleteTextures(1, &t->ahb_tex[i]);
		if (t->image[i])
			r->destroy_image(r->display, t->image[i]);
		if (t->ahb[i])
			AHardwareBuffer_release(t->ahb[i]);
	}
#endif
	memset(t, 0, sizeof(*t));
}

static inline void copy_rows(uint8_t *dst, int32_t dst_stride, const uint8_t *src, int32_t src_stride,
		const struct twl_box *b) {
	for (int32_t y = b->y; y < b->y + b->h; y++)
		memcpy(dst + (size_t) y * dst_stride + (size_t) b->x * 4, src + (size_t) y * src_stride + (size_t) b->x * 4,
				(size_t) b->w * 4);
}

#ifdef __ANDROID__
/* The GPU is done with everything drawn so far: no AHardwareBuffer we write is being sampled. */
static void wait_last_frame(struct twl_renderer *r) {
	if (!r->frame_sync)
		return;
	r->client_wait_sync(r->display, r->frame_sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
	r->destroy_sync(r->display, r->frame_sync);
	r->frame_sync = NULL;
}

static bool ahb_create(struct twl_renderer *r, struct twl_texture *t, int32_t w, int32_t h) {
	AHardwareBuffer_Desc desc = {
		.width = (uint32_t) w,
		.height = (uint32_t) h,
		.layers = 1,
		.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
		.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
	};
	const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
	for (int i = 0; i < 2; i++) {
		if (AHardwareBuffer_allocate(&desc, (AHardwareBuffer **) &t->ahb[i]) != 0)
			return false;
		EGLClientBuffer cb = eglGetNativeClientBufferANDROID(t->ahb[i]);
		t->image[i] = cb ? r->create_image(r->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attribs) : NULL;
		if (!t->image[i])
			return false;
		glGenTextures(1, &t->ahb_tex[i]);
		glBindTexture(GL_TEXTURE_2D, t->ahb_tex[i]);
		texture_params(GL_TEXTURE_2D);
		r->image_target_texture(GL_TEXTURE_2D, t->image[i]);
		if (glGetError() != GL_NO_ERROR)
			return false;
	}
	return true;
}

static bool ahb_write(struct twl_renderer *r, struct twl_texture *t, const void *data, int32_t stride,
		const struct twl_box *box) {
	int back = !t->front;
	void *dst = NULL;
	AHardwareBuffer_Desc desc;
	AHardwareBuffer_describe(t->ahb[back], &desc);
	if (AHardwareBuffer_lock(t->ahb[back], AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &dst) != 0 || !dst)
		return false;
	copy_rows(dst, (int32_t) desc.stride * 4, data, stride, box);
	AHardwareBuffer_unlock(t->ahb[back], NULL);
	t->front = back;
	return true;
}
#endif

void twl_texture_upload(struct twl_renderer *r, struct twl_texture *t, const void *data, int32_t stride,
		int32_t width, int32_t height, uint32_t format, const struct twl_box *damage) {
	struct twl_box whole = {0, 0, width, height}, box;
	bool full = !t->valid || t->width != width || t->height != height;
	if (full)
		box = whole;
	else if (!twl_box_intersect(&box, damage, &whole))
		return;
	t->swizzle = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888;
	t->opaque = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888;

#ifdef __ANDROID__
	if (r->use_ahb) {
		if (full) {
			twl_texture_release(r, t);
			if (!ahb_create(r, t, width, height)) {
				twl_error("AHardwareBuffer textures failed, using glTexSubImage2D");
				twl_texture_release(r, t);
				r->use_ahb = false;
				goto gl;
			}
			t->width = width;
			t->height = height;
			t->target = GL_TEXTURE_2D;
			t->swizzle = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888;
			t->opaque = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888;
			t->prev_damage = whole; /* the other copy is empty */
		}
		/* The copy we write missed the previous upload too. */
		struct twl_box write = box;
		twl_box_union(&write, &t->prev_damage);
		wait_last_frame(r);
		if (ahb_write(r, t, data, stride, &write)) {
			t->prev_damage = box;
			t->tex = 0;
			t->valid = true;
			return;
		}
		twl_error("AHardwareBuffer_lock failed, using glTexSubImage2D");
		twl_texture_release(r, t);
		r->use_ahb = false;
		full = true;
		box = whole;
	}
gl:
#endif
	if (full) {
		if (!t->tex)
			glGenTextures(1, &t->tex);
		glBindTexture(GL_TEXTURE_2D, t->tex);
		texture_params(GL_TEXTURE_2D);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		t->width = width;
		t->height = height;
		t->target = GL_TEXTURE_2D;
	} else {
		glBindTexture(GL_TEXTURE_2D, t->tex);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	const uint8_t *p = (const uint8_t *) data + (size_t) box.y * stride + (size_t) box.x * 4;
	if (stride == width * 4 && box.x == 0 && box.w == width) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, box.y, width, box.h, GL_RGBA, GL_UNSIGNED_BYTE, p);
	} else if (r->row_length) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH_, stride / 4);
		glTexSubImage2D(GL_TEXTURE_2D, 0, box.x, box.y, box.w, box.h, GL_RGBA, GL_UNSIGNED_BYTE, p);
		glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
	} else {
		for (int32_t y = 0; y < box.h; y++)
			glTexSubImage2D(GL_TEXTURE_2D, 0, box.x, box.y + y, box.w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
					p + (size_t) y * stride);
	}
	t->valid = true;
}

/* ---- dma-buf import ---- */

bool twl_renderer_import_dmabuf(struct twl_renderer *r, struct twl_buffer *b) {
	if (b->tex)
		return true;
	if (!r->dmabuf_import || b->import_failed)
		return false;
	const struct twl_dmabuf_attribs *a = &b->attribs;
	EGLint attribs[32], n = 0;
	attribs[n++] = EGL_WIDTH;
	attribs[n++] = a->width;
	attribs[n++] = EGL_HEIGHT;
	attribs[n++] = a->height;
	attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[n++] = (EGLint) a->format;
	attribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attribs[n++] = a->fd[0];
	attribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attribs[n++] = (EGLint) a->offset[0];
	attribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attribs[n++] = (EGLint) a->stride[0];
	if (r->dmabuf_modifiers && a->modifier != 0x00ffffffffffffffull) {
		attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attribs[n++] = (EGLint) (a->modifier & 0xffffffff);
		attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attribs[n++] = (EGLint) (a->modifier >> 32);
	}
	attribs[n++] = EGL_IMAGE_PRESERVED_KHR;
	attribs[n++] = EGL_TRUE;
	attribs[n] = EGL_NONE;
	b->image = r->create_image(r->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
	if (!b->image) {
		b->import_failed = true;
		twl_error("EGL dma-buf import failed (0x%x): copying dma-bufs from now on", eglGetError());
		r->dmabuf_import = false;
		return false;
	}
	glGenTextures(1, &b->tex);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, b->tex);
	texture_params(GL_TEXTURE_EXTERNAL_OES);
	r->image_target_texture(GL_TEXTURE_EXTERNAL_OES, b->image);
	if (glGetError() != GL_NO_ERROR) {
		twl_renderer_forget_buffer(r, b);
		b->import_failed = true;
		return false;
	}
	return true;
}

void twl_renderer_forget_buffer(struct twl_renderer *r, struct twl_buffer *b) {
	if (b->tex)
		glDeleteTextures(1, &b->tex);
	if (b->image)
		r->destroy_image(r->display, b->image);
	b->tex = 0;
	b->image = NULL;
}

/* ---- drawing ---- */

static void draw_surface(struct twl_surface *s, int32_t x, int32_t y, void *data) {
	struct twl_renderer *r = data;
	struct twl_server *server = r->server;
	struct program *p;
	GLuint tex;
	GLenum target;
	bool swizzle, opaque;
	int32_t bw, bh;
	if (s->shown && s->shown->tex) {
		p = &r->external;
		tex = s->shown->tex;
		target = GL_TEXTURE_EXTERNAL_OES;
		swizzle = false;
		opaque = s->shown->attribs.format == DRM_FORMAT_XRGB8888 || s->shown->attribs.format == DRM_FORMAT_XBGR8888;
		bw = s->shown->attribs.width;
		bh = s->shown->attribs.height;
	} else if (s->texture.valid) {
		p = &r->tex2d;
		tex = s->texture.tex ? s->texture.tex : s->texture.ahb_tex[s->texture.front];
		target = GL_TEXTURE_2D;
		swizzle = s->texture.swizzle;
		opaque = s->texture.opaque;
		bw = s->texture.width;
		bh = s->texture.height;
	} else {
		return;
	}
	if (s->width <= 0 || s->height <= 0 || bw <= 0 || bh <= 0)
		return;

	float S = (float) server->scale, W = (float) r->width, H = (float) r->height;
	float px = x * S, py = y * S, pw = s->width * S, ph = s->height * S;
	float u0 = 0, v0 = 0, du = 1, dv = 1;
	if (s->src_w >= 0) {
		float scale = (float) (s->scale > 0 ? s->scale : 1);
		u0 = (float) s->src_x * scale / bw;
		v0 = (float) s->src_y * scale / bh;
		du = (float) s->src_w * scale / bw;
		dv = (float) s->src_h * scale / bh;
	}

	glUseProgram(p->id);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, tex);
	glUniform1i(p->tex, 0);
	if (server->backend.flip_y)
		glUniform4f(p->rect, px / W * 2 - 1, py / H * 2 - 1, pw / W * 2, ph / H * 2);
	else
		glUniform4f(p->rect, px / W * 2 - 1, 1 - py / H * 2, pw / W * 2, -ph / H * 2);
	glUniform4f(p->src, u0, v0, du, dv);
	if (server->backend.swap_rb)
		swizzle = !swizzle;
	glUniform1f(p->swizzle, swizzle ? 1.0f : 0.0f);
	glUniform1f(p->opaque, opaque ? 1.0f : 0.0f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void twl_render_frame(struct twl_server *server, int32_t width_px, int32_t height_px) {
	struct twl_renderer *r = server->renderer;
	r->width = width_px;
	r->height = height_px;
	glViewport(0, 0, width_px, height_px);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

	struct twl_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		int32_t x, y;
		if (t->xdg->surface && twl_xdg_surface_position(t->xdg, &x, &y))
			twl_surface_for_each(t->xdg->surface, x, y, draw_surface, r);
	}
	struct twl_popup *p;
	wl_list_for_each(p, &server->popups, link) {
		int32_t x, y;
		if (p->xdg->surface && twl_xdg_surface_position(p->xdg, &x, &y))
			twl_surface_for_each(p->xdg->surface, x, y, draw_surface, r);
	}
	twl_seat_draw_cursor(server, draw_surface, r);

	glDisableVertexAttribArray(0);
	if (r->create_sync) {
		if (r->frame_sync)
			r->destroy_sync(r->display, r->frame_sync);
		r->frame_sync = r->create_sync(r->display, EGL_SYNC_FENCE_KHR, NULL);
	}
}

void twl_renderer_read_pixels(struct twl_renderer *r, int32_t w, int32_t h, void *out) {
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
}
