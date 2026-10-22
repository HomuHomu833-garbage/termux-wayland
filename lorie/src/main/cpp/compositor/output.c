#include "twl.h"
#include "wayland-server-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"

static void send_output(struct twl_server *server, struct wl_resource *r) {
	uint32_t version = wl_resource_get_version(r);
	int32_t dpi = server->dpi > 0 ? server->dpi : 160;
	wl_output_send_geometry(r, 0, 0, server->width_px * 254 / dpi / 10, server->height_px * 254 / dpi / 10,
			WL_OUTPUT_SUBPIXEL_UNKNOWN, "Android", "Termux:Wayland", WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, server->width_px, server->height_px,
			server->refresh_mhz);
	if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
		wl_output_send_scale(r, server->scale);
}

static void send_xdg_output(struct twl_server *server, struct wl_resource *r) {
	zxdg_output_v1_send_logical_position(r, 0, 0);
	zxdg_output_v1_send_logical_size(r, server->width_px / server->scale, server->height_px / server->scale);
	if (wl_resource_get_version(r) < 3)
		zxdg_output_v1_send_done(r);
}

void twl_output_send_all(struct twl_server *server) {
	struct wl_resource *r;
	wl_resource_for_each(r, &server->xdg_output_resources)
		send_xdg_output(server, r);
	wl_resource_for_each(r, &server->output_resources) {
		send_output(server, r);
		if (wl_resource_get_version(r) >= WL_OUTPUT_DONE_SINCE_VERSION)
			wl_output_send_done(r);
	}
	twl_surface_send_enter_all(server);
}

static void output_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_output_interface output_impl = {
	.release = output_release,
};

static void unlink_resource(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct twl_server *server = data;
	struct wl_resource *r = wl_resource_create(client, &wl_output_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &output_impl, server, unlink_resource);
	wl_list_insert(&server->output_resources, wl_resource_get_link(r));
	send_output(server, r);
	if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
		wl_output_send_name(r, "ANDROID-1");
		wl_output_send_description(r, "Termux:Wayland");
	}
	if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
		wl_output_send_done(r);
	/* surfaces this client made before binding the output */
	struct twl_surface *s;
	wl_list_for_each(s, &server->surfaces, link)
		if (wl_resource_get_client(s->resource) == client)
			wl_surface_send_enter(s->resource, r);
}

/* zxdg_output_manager_v1 (Xwayland and toolkits read the logical size from it) */

static void xdg_output_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface xdg_output_impl = {
	.destroy = xdg_output_destroy,
};

static void xdg_output_manager_get(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *output) {
	struct twl_server *server = wl_resource_get_user_data(resource);
	uint32_t version = wl_resource_get_version(resource);
	struct wl_resource *r = wl_resource_create(client, &zxdg_output_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &xdg_output_impl, server, unlink_resource);
	wl_list_insert(&server->xdg_output_resources, wl_resource_get_link(r));
	if (version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
		zxdg_output_v1_send_name(r, "ANDROID-1");
		zxdg_output_v1_send_description(r, "Termux:Wayland");
	}
	send_xdg_output(server, r);
	if (version >= 3 && wl_resource_get_version(output) >= WL_OUTPUT_DONE_SINCE_VERSION)
		wl_output_send_done(output);
}

static void xdg_output_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_manager_v1_interface xdg_output_manager_impl = {
	.destroy = xdg_output_manager_destroy,
	.get_xdg_output = xdg_output_manager_get,
};

static void xdg_output_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *r = wl_resource_create(client, &zxdg_output_manager_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &xdg_output_manager_impl, data, NULL);
}

void twl_output_global_init(struct twl_server *server) {
	wl_list_init(&server->output_resources);
	wl_list_init(&server->xdg_output_resources);
	wl_global_create(server->display, &wl_output_interface, 4, server, output_bind);
	wl_global_create(server->display, &zxdg_output_manager_v1_interface, 3, server, xdg_output_manager_bind);
}
