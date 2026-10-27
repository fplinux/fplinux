/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ui-frame-peer.h"
#include "xdg-shell-client-protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

struct protocol_client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct wl_surface *surface;
	struct xdg_surface *shell;
	struct xdg_toplevel *toplevel;
	uint32_t serial;
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
	(void)data;
	(void)shm;
	(void)format;
}

static const struct wl_shm_listener shm_listener = { .format = shm_format };

static void registry_global(void *data, struct wl_registry *registry,
			    uint32_t name, const char *interface,
			    uint32_t version)
{
	struct protocol_client *client = data;

	(void)version;
	if (!strcmp(interface, "wl_compositor"))
		client->compositor = wl_registry_bind(
			registry, name, &wl_compositor_interface, 1);
	else if (!strcmp(interface, "wl_shm")) {
		client->shm =
			wl_registry_bind(registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(client->shm, &shm_listener, NULL);
	} else if (!strcmp(interface, "xdg_wm_base"))
		client->wm = wl_registry_bind(registry, name,
					      &xdg_wm_base_interface, 1);
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
};

static void surface_configure(void *data, struct xdg_surface *surface,
			      uint32_t serial)
{
	struct protocol_client *client = data;

	(void)surface;
	client->serial = serial;
}

static const struct xdg_surface_listener surface_listener = {
	.configure = surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
			       int32_t width, int32_t height,
			       struct wl_array *states)
{
	(void)data;
	(void)toplevel;
	(void)width;
	(void)height;
	(void)states;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
};

static void send_requests(struct protocol_client *client,
			  struct fplinux_ui_frame_peer *peer)
{
	assert(wl_display_flush(client->display) >= 0);
	fplinux_ui_frame_pump(peer);
}

static void receive_configure(struct protocol_client *client, uint32_t previous)
{
	while (client->serial == previous)
		assert(wl_display_dispatch(client->display) >= 0);
}

static void send_frame(struct protocol_client *client,
		       struct fplinux_ui_frame_peer *peer, int width,
		       int height, uint16_t colour)
{
	size_t bytes = (size_t)width * height * sizeof(uint16_t);
	int fd = memfd_create("frame-peer", MFD_CLOEXEC);
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint16_t *pixels;

	assert(fd >= 0 && ftruncate(fd, (off_t)bytes) == 0);
	pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(pixels != MAP_FAILED);
	for (size_t i = 0; i < bytes / sizeof(*pixels); ++i)
		pixels[i] = colour;
	pool = wl_shm_create_pool(client->shm, fd, (int)bytes);
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
					   width * sizeof(*pixels),
					   WL_SHM_FORMAT_RGB565);
	wl_shm_pool_destroy(pool);
	assert(munmap(pixels, bytes) == 0 && close(fd) == 0);
	wl_surface_attach(client->surface, buffer, 0, 0);
	wl_surface_damage(client->surface, 0, 0, width, height);
	wl_surface_commit(client->surface);
	wl_buffer_destroy(buffer);
	send_requests(client, peer);
}

static void assert_frame(struct fplinux_ui_frame_peer *peer, int width,
			 int height, uint16_t colour)
{
	assert(peer->width == width && peer->height == height);
	for (size_t i = 0; i < (size_t)width * height; ++i)
		assert(peer->pixels[i] == colour);
}

static void invalid_scenario(const char *scenario)
{
	printf("invalid: %s\n", scenario);
	assert(fflush(stdout) == 0);
}

int main(int argc, char **argv)
{
	struct fplinux_ui_frame_peer peer;
	struct protocol_client client = { 0 };
	uint32_t serial;
	int sockets[2];
	const char *scenario;

	assert(argc == 2);
	scenario = argv[1];
	fplinux_ui_frame_socketpair(sockets);
	fplinux_ui_frame_init(&peer, sockets[0], 128, 128, NULL);
	client.display = wl_display_connect_to_fd(sockets[1]);
	assert(client.display);
	client.registry = wl_display_get_registry(client.display);
	wl_registry_add_listener(client.registry, &registry_listener, &client);
	send_requests(&client, &peer);
	assert(wl_display_dispatch(client.display) >= 0);
	assert(client.compositor && client.shm && client.wm);
	client.surface = wl_compositor_create_surface(client.compositor);
	client.shell = xdg_wm_base_get_xdg_surface(client.wm, client.surface);
	xdg_surface_add_listener(client.shell, &surface_listener, &client);
	client.toplevel = xdg_surface_get_toplevel(client.shell);
	xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, NULL);
	wl_surface_commit(client.surface);
	send_requests(&client, &peer);
	receive_configure(&client, 0);
	if (!strcmp(scenario, "initial-ack")) {
		invalid_scenario(scenario);
		send_frame(&client, &peer, 128, 128, 0xf800);
		return 1;
	}
	if (!strcmp(scenario, "serial")) {
		invalid_scenario(scenario);
		xdg_surface_ack_configure(client.shell, client.serial + 1);
		send_requests(&client, &peer);
		return 1;
	}
	xdg_surface_ack_configure(client.shell, client.serial);
	if (!strcmp(scenario, "duplicate-ack")) {
		invalid_scenario(scenario);
		xdg_surface_ack_configure(client.shell, client.serial);
		send_requests(&client, &peer);
		return 1;
	}
	send_frame(&client, &peer, 128, 128, 0xf800);
	assert(peer.commits == 1);
	assert_frame(&peer, 128, 128, 0xf800);
	serial = client.serial;
	fplinux_ui_frame_configure(&peer, 176, 178, true);
	if (!strcmp(scenario, "pre-ack-dimensions")) {
		invalid_scenario(scenario);
		send_frame(&client, &peer, 176, 178, 0x07e0);
		return 1;
	}
	if (!strcmp(scenario, "resize")) {
		/* The client has not dispatched or acknowledged the new configure. */
		send_frame(&client, &peer, 128, 128, 0x001f);
		assert(peer.commits == 2 && peer.configure_pending);
		assert_frame(&peer, 128, 128, 0x001f);
	}
	fplinux_ui_frame_pump(&peer);
	receive_configure(&client, serial);
	xdg_surface_ack_configure(client.shell, client.serial);
	if (!strcmp(scenario, "dimensions")) {
		invalid_scenario(scenario);
		send_frame(&client, &peer, 128, 128, 0x07e0);
		return 1;
	}
	assert(!strcmp(scenario, "resize"));
	send_requests(&client, &peer);
	/* ACK alone must not publish a snapshot at the proposed dimensions. */
	assert(peer.commits == 2 && peer.configure_pending);
	assert_frame(&peer, 128, 128, 0x001f);
	send_frame(&client, &peer, 176, 178, 0x07e0);
	assert(peer.commits == 3 && !peer.configure_pending);
	assert_frame(&peer, 176, 178, 0x07e0);
	xdg_toplevel_destroy(client.toplevel);
	xdg_surface_destroy(client.shell);
	wl_surface_destroy(client.surface);
	xdg_wm_base_destroy(client.wm);
	wl_shm_destroy(client.shm);
	wl_compositor_destroy(client.compositor);
	wl_registry_destroy(client.registry);
	send_requests(&client, &peer);
	wl_display_disconnect(client.display);
	fplinux_ui_frame_cleanup(&peer);
	puts("resize: old frame, resize ACK and new frame checked");
	return 0;
}
