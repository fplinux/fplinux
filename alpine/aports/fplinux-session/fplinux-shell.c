/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libweston/desktop.h>
#include <libweston/libweston.h>
#include <libweston/shell-utils.h>
#include <weston/weston.h>

#include "fplinux-brightness-client.h"
#include "fplinux-audio-protocol.h"
#include "fplinux-language.h"
#include "fplinux-pixman-buffer.h"
#include "fplinux-ui-profile.h"
#include "fplinux-weston-presentation.h"
#include "session-core.h"
#include "session-launch-surface.h"

struct phone_shell;

/* Filters each logical keyboard seat through the display and keyguard. */
struct shell_seat {
	struct wl_list link;
	struct phone_shell *shell;
	struct weston_seat *seat;
	struct weston_keyboard *keyboard;
	const struct weston_keyboard_grab_interface *default_interface;
	struct wl_listener caps_listener;
	struct wl_listener destroy_listener;
};

struct app_surface {
	struct wl_list link;
	struct phone_shell *shell;
	struct weston_desktop_surface *desktop;
	struct weston_view *view;
	pid_t pid;
	/* The mode last configured; see configure_toplevel(). */
	enum fplinux_ui_session_mode configured_mode;
	/* The placement last applied to the view; see place_surface(). */
	bool placed;
	struct weston_coord_global position;
	int geometry_x;
	int geometry_y;
	int width;
	int height;
};

/* A renderer image references shell-owned pixels, without a client buffer. */
struct launch_surface {
	struct weston_surface *surface;
	struct weston_view *view;
	struct weston_buffer_reference *buffer;
};

struct phone_shell {
	struct weston_compositor *compositor;
	struct weston_desktop *desktop;
	struct fplinux_session *manager;
	struct wl_event_source *manager_source;
	struct weston_layer background_layer;
	struct weston_layer launcher_layer;
	struct weston_layer application_layer;
	struct weston_layer progress_layer;
	struct weston_layer lock_layer;
	struct weston_layer startup_layer;
	struct weston_layer hidden_layer;
	struct weston_curtain *background;
	struct launch_surface held_surface;
	struct fplinux_launch_frame held_frame;
	struct launch_surface startup_mark;
	struct fplinux_launch_frame startup_frame;
	struct weston_curtain *launch_background;
	struct launch_surface launch_card;
	struct launch_surface launch_spinner;
	struct fplinux_launch_frame launch_frame;
	uint32_t spinner_pixels[FPLINUX_LAUNCH_SPINNER_PIXELS];
	uint32_t launch_generation;
	struct wl_event_source *spinner_timer;
	unsigned int spinner_phase;
	int spinner_size;
	int spinner_width;
	uint32_t spinner_rgb;
	int launch_width;
	int launch_height;
	struct weston_coord_global launch_position;
	enum fplinux_ui_session_theme launch_theme;
	bool launch_visible;
	/* The theme whose background colour the curtain shows. */
	enum fplinux_ui_session_theme background_theme;
	struct wl_list surfaces;
	struct wl_list seats;
	struct wl_listener destroy_listener;
	struct wl_listener seat_listener;
	struct wl_listener session_listener;
	struct wl_listener output_listener;
	struct wl_listener output_created_listener;
	struct wl_listener output_destroy_listener;
	struct wl_listener frame_listener;
	struct wl_listener latch_listener;
	struct wl_listener presentation_listener;
	struct weston_output *output;
	uint64_t presented_serial;
	struct wl_event_loop *loop;
	/* The frame handling deferred out of the repaint; see output_frame(). */
	struct wl_event_source *frame_idle;
	bool ready_written;
	pid_t focused_pid;
};

/*
 * The keyboard's default grab interface carries no context; the compositor
 * loads one shell, which owns every wrapped keyboard.
 */
static struct phone_shell *keyboard_shell;

static struct weston_output *get_output(struct phone_shell *shell)
{
	return weston_shell_utils_get_default_output(shell->compositor);
}

static void output_frame(struct wl_listener *listener, void *data);
static void output_latched(struct wl_listener *listener, void *data);
static void output_presented(struct wl_listener *listener, void *data);
static void hold_frame(void *context, bool enabled);

static void detach_output(struct phone_shell *shell)
{
	if (!shell->output)
		return;
	hold_frame(shell, false);
	wl_list_remove(&shell->frame_listener.link);
	wl_list_remove(&shell->latch_listener.link);
	wl_list_remove(&shell->presentation_listener.link);
	wl_list_remove(&shell->output_destroy_listener.link);
	shell->output = NULL;
	shell->presented_serial = 0;
	if (shell->frame_idle) {
		wl_event_source_remove(shell->frame_idle);
		shell->frame_idle = NULL;
	}
	fplinux_session_output_reset(shell->manager);
}

static void output_destroyed(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, output_destroy_listener);

	(void)data;
	detach_output(shell);
}

static void watch_output(struct phone_shell *shell,
			 struct weston_output *output)
{
	if (shell->output == output)
		return;
	detach_output(shell);
	shell->output = output;
	shell->frame_listener.notify = output_frame;
	wl_signal_add(&output->frame_signal, &shell->frame_listener);
	shell->latch_listener.notify = output_latched;
	wl_signal_add(&output->post_latch_signal, &shell->latch_listener);
	shell->presentation_listener.notify = output_presented;
	weston_output_add_presentation_listener(output,
						&shell->presentation_listener);
	shell->output_destroy_listener.notify = output_destroyed;
	wl_signal_add(&output->destroy_signal, &shell->output_destroy_listener);
}

static void destroy_launch_surface(struct launch_surface *surface)
{
	if (surface->view)
		weston_view_destroy(surface->view);
	if (surface->surface)
		weston_surface_unref(surface->surface);
	if (surface->buffer)
		weston_buffer_destroy_internal_image(surface->buffer);
	memset(surface, 0, sizeof(*surface));
}

static bool create_launch_surface(struct phone_shell *shell,
				  struct launch_surface *surface,
				  pixman_format_code_t format, void *pixels,
				  int width, int height, int stride,
				  struct weston_coord_global position,
				  const char *label)
{
	pixman_image_t *image =
		pixman_image_create_bits(format, width, height, pixels, stride);

	if (!image)
		return false;
	surface->buffer =
		weston_buffer_create_internal_image(shell->compositor, image);
	pixman_image_unref(image);
	if (!surface->buffer)
		return false;
	surface->surface = weston_surface_create(shell->compositor, NULL);
	if (!surface->surface)
		goto fail;
	surface->view = weston_view_create(surface->surface);
	if (!surface->view)
		goto fail;
	weston_surface_set_label_static(surface->surface, label);
	weston_surface_attach_internal_image(surface->surface, surface->buffer);
	pixman_region32_clear(&surface->surface->input);
	weston_surface_map(surface->surface);
	weston_view_set_position(surface->view, position);
	weston_view_move_to_layer(surface->view,
				  &shell->progress_layer.view_list);
	return true;

fail:
	destroy_launch_surface(surface);
	return false;
}

static bool capture_surface(struct phone_shell *shell,
			    struct app_surface *surface,
			    struct fplinux_launch_frame *frame)
{
	struct weston_surface *source = surface->view->surface;
	const struct fplinux_session_process *process =
		fplinux_session_find_process(shell->manager, surface->pid);
	struct fplinux_launch_frame_rect clip = {
		.x = surface->position.c.x - shell->output->pos.c.x,
		.y = surface->position.c.y - shell->output->pos.c.y,
		.width = surface->width,
		.height = surface->height,
	};
	uint32_t *pixels;
	size_t bytes;
	int width, height;
	bool copied;

	if (!process || !process->framed || !source->renderer_state)
		return false;
	weston_surface_get_content_size(source, &width, &height);
	if (width <= 0 || height <= 0 ||
	    width > FPLINUX_UI_SESSION_FRAME_MAX_WIDTH ||
	    height > FPLINUX_UI_SESSION_FRAME_MAX_HEIGHT ||
	    (size_t)width * (size_t)height >
		    FPLINUX_UI_SESSION_FRAME_MAX_PIXELS)
		return false;
	bytes = (size_t)width * (size_t)height * sizeof(*pixels);
	pixels = malloc(bytes);
	if (!pixels)
		return false;
	/* Both renderers expose the most recently rendered surface contents. */
	copied = weston_surface_copy_content(source, pixels, bytes, 0, 0, width,
					     height) == 0;
	if (copied)
		fplinux_launch_frame_blend(frame, pixels, width, height,
					   clip.x - surface->geometry_x,
					   clip.y - surface->geometry_y, clip);
	free(pixels);
	return copied;
}

/* Current clients use unscaled buffers in the boxes applied by place_surface(). */
static bool capture_scene(struct phone_shell *shell,
			  struct fplinux_launch_frame *frame)
{
	struct weston_output *output = shell->output;
	struct weston_layer *layers[] = {
		&shell->launcher_layer,
		&shell->application_layer,
		&shell->lock_layer,
	};
	struct app_surface *surface;
	uint32_t rgb;
	bool copied = false;

	if (!output || !shell->compositor->session_active)
		return false;
	rgb = fplinux_ui_theme_spec(fplinux_session_theme(shell->manager))
		      ->rgb[FPLINUX_UI_COLOUR_BACKGROUND];
	if (!fplinux_launch_frame_fill(frame, output->width, output->height,
				       rgb))
		return false;
	if (shell->held_frame.pixels) {
		if (shell->held_frame.width != frame->width ||
		    shell->held_frame.height != frame->height) {
			fplinux_launch_frame_clear(frame);
			return false;
		}
		memcpy(frame->pixels, shell->held_frame.pixels, frame->bytes);
		copied = true;
	}
	for (size_t i = 0; i < sizeof(layers) / sizeof(*layers); ++i) {
		/* A system modal can cover a scene still waiting for replacement. */
		if (shell->held_frame.pixels && layers[i] != &shell->lock_layer)
			continue;
		wl_list_for_each(surface, &shell->surfaces, link)
		{
			if (!surface->placed ||
			    !weston_view_is_mapped(surface->view) ||
			    surface->view->layer_link.layer != layers[i])
				continue;
			if (!capture_surface(shell, surface, frame)) {
				fplinux_launch_frame_clear(frame);
				return false;
			}
			copied = true;
		}
	}
	if (!copied)
		fplinux_launch_frame_clear(frame);
	return copied;
}

static void hold_frame(void *context, bool enabled)
{
	struct phone_shell *shell = context;
	struct fplinux_launch_frame frame = { 0 };
	struct launch_surface surface = { 0 };

	if (enabled && capture_scene(shell, &frame)) {
		if (create_launch_surface(
			    shell, &surface, PIXMAN_r5g6b5, frame.pixels,
			    frame.width, frame.height, frame.width * 2,
			    shell->output->pos, "FPLinux retained frame")) {
			destroy_launch_surface(&shell->held_surface);
			fplinux_launch_frame_clear(&shell->held_frame);
			shell->held_frame = frame;
			shell->held_surface = surface;
			return;
		}
	}
	fplinux_launch_frame_clear(&frame);
	if (enabled)
		return;
	destroy_launch_surface(&shell->held_surface);
	fplinux_launch_frame_clear(&shell->held_frame);
}

static int capture_frame(void *context, int *width, int *height)
{
	struct phone_shell *shell = context;
	struct fplinux_launch_frame frame = { 0 };
	int fd;

	if (!capture_scene(shell, &frame))
		return -1;
	fd = fplinux_launch_frame_fd(&frame);
	if (fd >= 0) {
		*width = frame.width;
		*height = frame.height;
	}
	fplinux_launch_frame_clear(&frame);
	return fd;
}

static void hide_startup_surface(struct phone_shell *shell)
{
	destroy_launch_surface(&shell->startup_mark);
	fplinux_launch_frame_clear(&shell->startup_frame);
	if (shell->background && shell->background->view->layer_link.layer ==
					 &shell->startup_layer) {
		weston_view_move_to_layer(shell->background->view,
					  &shell->background_layer.view_list);
		/* Repaint the launcher pixels previously occluded by the curtain. */
		weston_compositor_damage_all(shell->compositor);
	}
}

/* The first launcher buffer stays mapped behind this cover until it frames. */
static void show_startup_surface(struct phone_shell *shell,
				 struct weston_output *output)
{
	enum fplinux_ui_session_theme theme =
		fplinux_session_theme(shell->manager);
	const struct fplinux_ui_profile_spec *profile;
	struct weston_coord_global position = output->pos;
	char path[PATH_MAX];
	int size;

	hide_startup_surface(shell);
	if (fplinux_session_is_ready(shell->manager))
		return;
	if (shell->background)
		weston_view_move_to_layer(shell->background->view,
					  &shell->startup_layer.view_list);
	profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(output->width, output->height));
	if (!profile)
		return;
	size = profile->splash_mark;
	snprintf(path, sizeof(path),
		 "/usr/share/fplinux/brand/splash-%s-%d.rgb565",
		 theme == FPLINUX_UI_SESSION_THEME_DARK ? "dark" : "light",
		 size);
	if (!fplinux_launch_frame_load(&shell->startup_frame, path, size,
				       size)) {
		weston_log("fplinux-session: cannot load startup mark %s\n",
			   path);
		return;
	}
	position.c.x += (output->width - size) / 2;
	position.c.y += (output->height - size) / 2;
	if (!create_launch_surface(shell, &shell->startup_mark, PIXMAN_r5g6b5,
				   shell->startup_frame.pixels, size, size,
				   size * 2, position,
				   "FPLinux startup mark")) {
		fplinux_launch_frame_clear(&shell->startup_frame);
		return;
	}
	weston_view_move_to_layer(shell->startup_mark.view,
				  &shell->startup_layer.view_list);
}

/* Hidden cards keep their one RGB565 copy, but have no animation wakeups. */
static void hide_launch_surface(struct phone_shell *shell)
{
	if (shell->spinner_timer) {
		wl_event_source_remove(shell->spinner_timer);
		shell->spinner_timer = NULL;
	}
	destroy_launch_surface(&shell->launch_spinner);
	destroy_launch_surface(&shell->launch_card);
	if (shell->launch_background) {
		weston_shell_utils_curtain_destroy(shell->launch_background);
		shell->launch_background = NULL;
	}
	shell->launch_visible = false;
}

static int animate_launch_spinner(void *context)
{
	struct phone_shell *shell = context;
	struct fplinux_session_launch launch;

	fplinux_session_get_launch(shell->manager, &launch);
	if (!launch.active || !launch.shown ||
	    !shell->compositor->session_active ||
	    launch.generation != shell->launch_generation) {
		hide_launch_surface(shell);
		if (!launch.active ||
		    launch.generation != shell->launch_generation)
			fplinux_launch_frame_clear(&shell->launch_frame);
		return 0;
	}
	++shell->spinner_phase;
	fplinux_launch_spinner_draw(shell->spinner_pixels, shell->spinner_size,
				    shell->spinner_width, shell->spinner_phase,
				    shell->spinner_rgb);
	/* Content damage covers only this tiny surface, never the retained card. */
	pixman_region32_union_rect(&shell->launch_spinner.surface->damage,
				   &shell->launch_spinner.surface->damage, 0, 0,
				   shell->spinner_size, shell->spinner_size);
	weston_view_schedule_repaint(shell->launch_spinner.view);
	wl_event_source_timer_update(shell->spinner_timer,
				     FPLINUX_LAUNCH_SPINNER_MS);
	return 0;
}

/* The manager calls this with a borrowed descriptor before launcher CLOSE. */
static void receive_launch_frame(void *context, int fd, int width, int height)
{
	struct phone_shell *shell = context;
	struct weston_output *output = get_output(shell);
	struct fplinux_session_launch launch;

	hide_launch_surface(shell);
	fplinux_launch_frame_clear(&shell->launch_frame);
	fplinux_session_get_launch(shell->manager, &launch);
	shell->launch_generation = launch.generation;
	if (output && width == output->width && height == output->height)
		fplinux_launch_frame_copy(&shell->launch_frame, fd, width,
					  height);
}

static void update_launch_surface(struct phone_shell *shell,
				  struct weston_output *output)
{
	struct fplinux_session_launch launch;
	const struct fplinux_ui_profile_spec *profile;
	enum fplinux_ui_session_theme theme;
	struct weston_coord_global position;
	uint32_t rgb;
	int body_height;

	fplinux_session_get_launch(shell->manager, &launch);
	if (!launch.active || launch.generation != shell->launch_generation) {
		hide_launch_surface(shell);
		fplinux_launch_frame_clear(&shell->launch_frame);
		shell->launch_generation = launch.generation;
	}
	if (!launch.active)
		return;
	if (shell->launch_frame.pixels &&
	    (shell->launch_frame.width != output->width ||
	     shell->launch_frame.height != output->height)) {
		hide_launch_surface(shell);
		fplinux_launch_frame_clear(&shell->launch_frame);
	}
	if (!launch.shown || !shell->compositor->session_active) {
		hide_launch_surface(shell);
		return;
	}
	theme = fplinux_session_theme(shell->manager);
	if (shell->launch_visible && shell->launch_theme == theme &&
	    shell->launch_width == output->width &&
	    shell->launch_height == output->height &&
	    shell->launch_position.c.x == output->pos.c.x &&
	    shell->launch_position.c.y == output->pos.c.y)
		return;
	hide_launch_surface(shell);
	profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(output->width, output->height));
	if (!profile)
		return;
	if (shell->launch_frame.pixels &&
	    !create_launch_surface(shell, &shell->launch_card, PIXMAN_r5g6b5,
				   shell->launch_frame.pixels, output->width,
				   output->height, output->width * 2,
				   output->pos, "FPLinux launch card"))
		fplinux_launch_frame_clear(&shell->launch_frame);
	if (!shell->launch_card.view) {
		struct weston_curtain_params params;

		rgb = fplinux_ui_theme_spec(theme)
			      ->rgb[FPLINUX_UI_COLOUR_BACKGROUND];
		params = (struct weston_curtain_params){
			.r = (float)((rgb >> 16) & 255) / 255.0F,
			.g = (float)((rgb >> 8) & 255) / 255.0F,
			.b = (float)(rgb & 255) / 255.0F,
			.a = 1.0F,
			.pos = output->pos,
			.width = output->width,
			.height = output->height,
		};
		shell->launch_background = weston_shell_utils_curtain_create(
			shell->compositor, &params);
		if (shell->launch_background) {
			weston_surface_set_label_static(
				shell->launch_background->view->surface,
				"FPLinux launch background");
			weston_view_move_to_layer(
				shell->launch_background->view,
				&shell->progress_layer.view_list);
		}
	}
	shell->spinner_phase = 0;
	shell->spinner_size = profile->spinner_size;
	shell->spinner_width = profile->spinner_width;
	shell->spinner_rgb =
		fplinux_ui_theme_spec(theme)->rgb[FPLINUX_UI_COLOUR_FOCUS];
	body_height = output->height - profile->status_height -
		      profile->softkey_height;
	position = output->pos;
	position.c.x += (output->width - shell->spinner_size) / 2;
	position.c.y += profile->status_height +
			(body_height - shell->spinner_size) / 2;
	if (fplinux_launch_spinner_draw(
		    shell->spinner_pixels, shell->spinner_size,
		    shell->spinner_width, shell->spinner_phase,
		    shell->spinner_rgb) &&
	    create_launch_surface(shell, &shell->launch_spinner,
				  PIXMAN_a8r8g8b8, shell->spinner_pixels,
				  shell->spinner_size, shell->spinner_size,
				  shell->spinner_size * 4, position,
				  "FPLinux launch spinner")) {
		shell->spinner_timer = wl_event_loop_add_timer(
			shell->loop, animate_launch_spinner, shell);
		if (shell->spinner_timer)
			wl_event_source_timer_update(shell->spinner_timer,
						     FPLINUX_LAUNCH_SPINNER_MS);
	}
	shell->launch_visible = shell->launch_card.view ||
				shell->launch_background ||
				shell->launch_spinner.view;
	shell->launch_theme = theme;
	shell->launch_width = output->width;
	shell->launch_height = output->height;
	shell->launch_position = output->pos;
}

static void activate(struct phone_shell *shell, struct app_surface *surface)
{
	struct weston_seat *seat;
	struct app_surface *other;

	wl_list_for_each(other, &shell->surfaces, link)
		weston_desktop_surface_set_activated(other->desktop,
						     other == surface);
	wl_list_for_each(seat, &shell->compositor->seat_list, link)
		weston_view_activate_input(surface->view, seat,
					   WESTON_ACTIVATE_FLAG_NONE);
}

/*
 * Positions and clips an application's view. Every view change damages the
 * output and schedules a repaint, and update_surfaces() runs on each manager
 * wakeup (keys, the 2 s charger poll, commits), so an unchanged placement is
 * not applied again: an idle screen, also while the display is off, repaints
 * nothing.
 */
static void place_surface(struct app_surface *surface,
			  struct weston_coord_global position, int width,
			  int height)
{
	struct weston_geometry geometry =
		weston_desktop_surface_get_geometry(surface->desktop);

	if (surface->placed && surface->position.c.x == position.c.x &&
	    surface->position.c.y == position.c.y &&
	    surface->geometry_x == geometry.x &&
	    surface->geometry_y == geometry.y && surface->width == width &&
	    surface->height == height)
		return;
	weston_view_set_position_with_offset(
		surface->view, position,
		weston_coord_surface(-geometry.x, -geometry.y,
				     surface->view->surface));
	weston_view_set_mask(surface->view, geometry.x, geometry.y, width,
			     height);
	weston_view_update_transform(surface->view);
	surface->placed = true;
	surface->position = position;
	surface->geometry_x = geometry.x;
	surface->geometry_y = geometry.y;
	surface->width = width;
	surface->height = height;
}

/*
 * Configures the toplevel with the box size and state of a mode. libweston
 * sends the three changes in one configure; the client answers with a buffer
 * of that size, which the manager places in that mode's box.
 */
static void configure_toplevel(struct app_surface *surface,
			       enum fplinux_ui_session_mode mode)
{
	int x, y, width, height;

	fplinux_session_surface_box(surface->shell->manager, mode, &x, &y,
				    &width, &height);
	weston_desktop_surface_set_size(surface->desktop, width, height);
	weston_desktop_surface_set_fullscreen(
		surface->desktop, mode == FPLINUX_UI_SESSION_FULLSCREEN);
	weston_desktop_surface_set_maximized(
		surface->desktop, mode != FPLINUX_UI_SESSION_FULLSCREEN);
	surface->configured_mode = mode;
}

/* The launcher's view is placed above the applications. */
static bool launcher_shown(struct phone_shell *shell)
{
	struct app_surface *surface;

	wl_list_for_each(surface, &shell->surfaces, link)
	{
		const struct fplinux_session_process *process =
			fplinux_session_find_process(shell->manager,
						     surface->pid);

		if (process && !process->closing &&
		    process->mode == FPLINUX_UI_SESSION_LAUNCHER &&
		    surface->view->layer_link.layer == &shell->lock_layer)
			return true;
	}
	return false;
}

static void apply_scene(struct phone_shell *shell, struct weston_output *output)
{
	struct app_surface *surface;
	struct app_surface *focused = NULL;
	pid_t focused_pid;
	bool launcher_on_top;

	launcher_on_top = fplinux_session_launcher_on_top(shell->manager);
	wl_list_for_each(surface, &shell->surfaces, link)
	{
		const struct fplinux_session_process *process =
			fplinux_session_find_process(shell->manager,
						     surface->pid);
		struct weston_layer *layer = &shell->hidden_layer;
		struct weston_coord_global position = output->pos;
		int x, y, width, height;

		/*
		 * The keyguard and battery notices raise the launcher and hide
		 * applications. An application appears and goes when the
		 * manager swaps it with the launcher; see session-core.h.
		 */
		if (process && !process->closing) {
			/* MODE changed its mode; the next buffer places it. */
			if (process->configured_mode !=
			    surface->configured_mode)
				configure_toplevel(surface,
						   process->configured_mode);
			if (process->mode == FPLINUX_UI_SESSION_LAUNCHER)
				layer = launcher_on_top ?
						&shell->lock_layer :
						&shell->launcher_layer;
			else if (!launcher_on_top && process->shown)
				layer = &shell->application_layer;
		}
		/* A closing surface keeps its box, so hiding does not move it. */
		fplinux_session_surface_box(
			shell->manager,
			process ? process->mode : FPLINUX_UI_SESSION_FULLSCREEN,
			&x, &y, &width, &height);
		position.c.x += x;
		position.c.y += y;
		place_surface(surface, position, width, height);
		if (surface->view->surface->width <= 0)
			continue;
		/* Moving a view, even within its layer, damages the output. */
		if (surface->view->layer_link.layer != layer) {
			/* Launcher damage may have been clipped by the outgoing view. */
			if (layer == &shell->hidden_layer &&
			    surface->view->layer_link.layer ==
				    &shell->application_layer)
				weston_compositor_damage_all(shell->compositor);
			weston_view_move_to_layer(surface->view,
						  &layer->view_list);
			weston_desktop_surface_propagate_layer(
				surface->desktop);
		}
	}
	/*
	 * Only a launcher that is really on top may hide the application and
	 * take its keys; the manager then names the focus.
	 */
	fplinux_session_set_launcher_shown(shell->manager,
					   launcher_shown(shell));
	if (!shell->compositor->session_active)
		return;
	focused_pid = fplinux_session_focused_pid(shell->manager);
	wl_list_for_each(surface, &shell->surfaces, link)
	{
		if (surface->pid == focused_pid &&
		    surface->view->layer_link.layer &&
		    surface->view->layer_link.layer != &shell->hidden_layer)
			focused = surface;
	}
	if (focused && focused->pid != shell->focused_pid) {
		activate(shell, focused);
		shell->focused_pid = focused->pid;
	} else if (!focused && (launcher_on_top || !focused_pid)) {
		struct weston_seat *seat;

		/*
		 * No hidden application keeps keyboard focus, and no surface
		 * while the manager names none, as during a launch. Until an
		 * application is swapped in, the launcher keeps the keys.
		 */
		wl_list_for_each(seat, &shell->compositor->seat_list,
				 link) if (seat->keyboard_state)
			weston_keyboard_set_focus(seat->keyboard_state, NULL);
		shell->focused_pid = 0;
	}
}

/* The curtain behind every surface, in the theme's background colour. */
static void show_background(struct phone_shell *shell,
			    struct weston_output *output)
{
	enum fplinux_ui_session_theme theme =
		fplinux_session_theme(shell->manager);
	uint32_t rgb =
		fplinux_ui_theme_spec(theme)->rgb[FPLINUX_UI_COLOUR_BACKGROUND];
	/*
	 * The curtain owns the label in its parameters. Assign a static label
	 * after construction so it does not free a string literal.
	 */
	struct weston_curtain_params background = {
		.r = (float)((rgb >> 16) & 0xff) / 255.0F,
		.g = (float)((rgb >> 8) & 0xff) / 255.0F,
		.b = (float)(rgb & 0xff) / 255.0F,
		.a = 1.0F,
		.pos = output->pos,
		.width = output->width,
		.height = output->height,
	};

	if (shell->background)
		weston_shell_utils_curtain_destroy(shell->background);
	shell->background = weston_shell_utils_curtain_create(shell->compositor,
							      &background);
	if (shell->background) {
		weston_surface_set_label_static(
			shell->background->view->surface, "FPLinux background");
		weston_view_move_to_layer(shell->background->view,
					  &shell->background_layer.view_list);
	}
	shell->background_theme = theme;
	show_startup_surface(shell, output);
}

/* Runs on every manager wakeup and client commit, and after output frames. */
static void update_surfaces(void *context)
{
	struct phone_shell *shell = context;
	struct weston_output *output = get_output(shell);

	if (!shell->manager || !output)
		return;
	watch_output(shell, output);
	/* A new theme restarts the clients; the curtain follows at once. */
	if (shell->background_theme != fplinux_session_theme(shell->manager))
		show_background(shell, output);
	apply_scene(shell, output);
	update_launch_surface(shell, output);
	if (fplinux_session_is_ready(shell->manager))
		hide_startup_surface(shell);
	if (!shell->ready_written && fplinux_session_is_ready(shell->manager)) {
		int fd = open("/run/fplinux-gui/ready",
			      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);

		if (fd >= 0) {
			write(fd, "ready\n", 6);
			close(fd);
			shell->ready_written = true;
			weston_log(
				"fplinux-session: input and first launcher frame ready\n");
		}
	}
}

static void surface_added(struct weston_desktop_surface *desktop, void *context)
{
	struct phone_shell *shell = context;
	pid_t pid = weston_desktop_surface_get_pid(desktop);
	const struct fplinux_session_process *process =
		fplinux_session_find_process(shell->manager, pid);
	struct weston_output *output = get_output(shell);
	struct app_surface *surface;
	struct app_surface *existing;

	if (!process || process->closing || !output) {
		weston_desktop_surface_close(desktop);
		return;
	}
	wl_list_for_each(existing, &shell->surfaces,
			 link) if (existing->pid == pid)
	{
		weston_desktop_surface_close(desktop);
		return;
	}
	surface = calloc(1, sizeof(*surface));
	if (!surface)
		return;
	surface->view = weston_desktop_surface_create_view(desktop);
	if (!surface->view) {
		free(surface);
		return;
	}
	surface->desktop = desktop;
	surface->shell = shell;
	surface->pid = pid;
	wl_list_insert(&shell->surfaces, &surface->link);
	weston_desktop_surface_set_user_data(desktop, surface);
	configure_toplevel(surface, process->configured_mode);
}

static void surface_removed(struct weston_desktop_surface *desktop,
			    void *context)
{
	struct app_surface *surface =
		weston_desktop_surface_get_user_data(desktop);

	(void)context;
	if (!surface)
		return;
	if (surface->shell->focused_pid == surface->pid)
		surface->shell->focused_pid = 0;
	weston_desktop_surface_set_user_data(desktop, NULL);
	weston_desktop_surface_unlink_view(surface->view);
	weston_view_destroy(surface->view);
	wl_list_remove(&surface->link);
	free(surface);
}

static void committed(struct weston_desktop_surface *desktop,
		      struct weston_coord_surface offset, void *context)
{
	struct phone_shell *shell = context;
	struct app_surface *surface =
		weston_desktop_surface_get_user_data(desktop);
	struct weston_surface *weston_surface =
		weston_desktop_surface_get_surface(desktop);

	(void)offset;
	if (!surface || !weston_surface->width)
		return;
	if (!weston_surface_is_mapped(weston_surface))
		weston_surface_map(weston_surface);
	/* A swap or a new box for this buffer lands in the same repaint. */
	fplinux_session_surface_committed(shell->manager, surface->pid,
					  weston_surface->width,
					  weston_surface->height);
	update_surfaces(shell);
}

/*
 * Scene changes stay outside repaint and presentation callbacks. A first
 * launcher frame can raise it for a notice; only a presented wake snapshot
 * may restore the backlight.
 */
static void frame_done(void *context)
{
	struct phone_shell *shell = context;
	struct app_surface *surface;

	shell->frame_idle = NULL;
	wl_list_for_each(surface, &shell->surfaces,
			 link) if (weston_view_is_mapped(surface->view))
		fplinux_session_frame(shell->manager, surface->pid);
	if (shell->presented_serial) {
		uint64_t serial = shell->presented_serial;

		shell->presented_serial = 0;
		fplinux_session_output_presented(shell->manager, serial);
	}
	update_surfaces(shell);
}

/*
 * Weston emits frame_signal from inside the output repaint and clears the
 * output's repaint request afterwards, so a view moved here would be painted
 * only by some later, unrelated repaint: a launcher raised above a fullscreen
 * application for a battery notice stayed invisible. The frame is handled
 * once the repaint has returned, from an idle callback. This renderer signal
 * does not establish presentation and never authorizes the backlight.
 */
static void output_frame(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, frame_listener);

	(void)data;
	if (!shell->frame_idle)
		shell->frame_idle =
			wl_event_loop_add_idle(shell->loop, frame_done, shell);
}

static void output_latched(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, latch_listener);
	struct weston_output *output = data;
	struct app_surface *surface;
	pid_t pids[FPLINUX_UI_SESSION_TASK_MAX + 1];
	size_t count = 0;

	wl_list_for_each(surface, &shell->surfaces, link)
	{
		if (!weston_view_is_mapped(surface->view) ||
		    surface->view->layer_link.layer == &shell->hidden_layer)
			continue;
		if (count < sizeof(pids) / sizeof(pids[0]))
			pids[count++] = surface->pid;
	}
	fplinux_session_output_latched(
		shell->manager, weston_output_get_submission_serial(output),
		pids, count);
}

static void output_presented(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, presentation_listener);
	const struct fplinux_weston_presentation *presentation = data;

	if (presentation->output != shell->output)
		return;
	shell->presented_serial = presentation->serial;
	if (!shell->frame_idle)
		shell->frame_idle =
			wl_event_loop_add_idle(shell->loop, frame_done, shell);
}

static const struct weston_desktop_api desktop_api = {
	.struct_size = sizeof(desktop_api),
	.surface_added = surface_added,
	.surface_removed = surface_removed,
	.committed = committed,
};

static int manager_event(int fd, uint32_t mask, void *context)
{
	struct phone_shell *shell = context;

	(void)fd;
	(void)mask;
	fplinux_session_dispatch(shell->manager);
	update_surfaces(shell);
	/* An ACK may arrive after its buffer was latched or presented already. */
	if (shell->output && fplinux_session_wake_pending(shell->manager))
		weston_compositor_damage_all(shell->compositor);
	return 0;
}

static void home_binding(struct weston_keyboard *keyboard,
			 const struct timespec *time, uint32_t key,
			 void *context)
{
	struct phone_shell *shell = context;

	(void)keyboard;
	(void)time;
	(void)key;
	fplinux_session_keyboard_home(shell->manager);
	fplinux_session_dispatch(shell->manager);
	update_surfaces(shell);
}

static struct shell_seat *seat_for_keyboard(struct phone_shell *shell,
					    struct weston_keyboard *keyboard)
{
	struct shell_seat *seat;

	wl_list_for_each(seat, &shell->seats,
			 link) if (seat->keyboard == keyboard) return seat;
	return NULL;
}

static enum fplinux_session_key_source key_source(const struct shell_seat *seat)
{
	return !strcmp(seat->seat->seat_name, FPLINUX_UI_KEYPAD_SEAT) ?
		       FPLINUX_SESSION_KEY_PHONE :
		       FPLINUX_SESSION_KEY_KEYBOARD;
}

/* Bindings such as Ctrl+Escape run before the default grab sees a key. */
static void filter_key(struct weston_keyboard_grab *grab,
		       const struct weston_key_event *event)
{
	struct phone_shell *shell = keyboard_shell;
	struct shell_seat *seat =
		shell ? seat_for_keyboard(shell, grab->keyboard) : NULL;
	bool pressed = event->key_state == WL_KEYBOARD_KEY_STATE_PRESSED;
	bool deliver;

	if (!seat)
		return;
	if (key_source(seat) == FPLINUX_SESSION_KEY_PHONE) {
		uint64_t time_ms = (uint64_t)event->base.ts.tv_sec * 1000 +
				   event->base.ts.tv_nsec / 1000000;

		deliver = fplinux_session_key(shell->manager, event->key,
					      pressed, time_ms);
	} else {
		deliver = fplinux_session_keyboard_key(shell->manager,
						       event->key, pressed);
	}
	update_surfaces(shell);
	if (deliver)
		seat->default_interface->key(grab, event);
}

static void filter_modifiers(struct weston_keyboard_grab *grab, uint32_t serial,
			     uint32_t depressed, uint32_t latched,
			     uint32_t locked, uint32_t group)
{
	struct shell_seat *seat =
		keyboard_shell ?
			seat_for_keyboard(keyboard_shell, grab->keyboard) :
			NULL;

	if (seat)
		seat->default_interface->modifiers(grab, serial, depressed,
						   latched, locked, group);
}

static void filter_cancel(struct weston_keyboard_grab *grab)
{
	struct shell_seat *seat =
		keyboard_shell ?
			seat_for_keyboard(keyboard_shell, grab->keyboard) :
			NULL;

	if (seat)
		seat->default_interface->cancel(grab);
}

static const struct weston_keyboard_grab_interface filter_interface = {
	.key = filter_key,
	.modifiers = filter_modifiers,
	.cancel = filter_cancel,
};

/* The keyboard object persists once created, also after unplugging. */
static void wrap_keyboard(struct shell_seat *seat)
{
	struct weston_keyboard *keyboard = seat->seat->keyboard_state;

	if (!keyboard || seat->keyboard == keyboard)
		return;
	seat->keyboard = keyboard;
	seat->default_interface = keyboard->default_grab.interface;
	keyboard->default_grab.interface = &filter_interface;
}

static void seat_caps_changed(struct wl_listener *listener, void *data)
{
	struct shell_seat *seat =
		wl_container_of(listener, seat, caps_listener);
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat->seat);

	(void)data;
	if (!keyboard)
		fplinux_session_cancel_input(seat->shell->manager,
					     key_source(seat));
	wrap_keyboard(seat);
	if (keyboard && !keyboard->focus) {
		/* A seat gains its keyboard after creation, also on reconnect. */
		seat->shell->focused_pid = 0;
		update_surfaces(seat->shell);
	}
}

static void remove_seat(struct shell_seat *seat)
{
	wl_list_remove(&seat->caps_listener.link);
	wl_list_remove(&seat->destroy_listener.link);
	wl_list_remove(&seat->link);
	free(seat);
}

static void seat_destroyed(struct wl_listener *listener, void *data)
{
	struct shell_seat *seat =
		wl_container_of(listener, seat, destroy_listener);

	(void)data;
	fplinux_session_cancel_input(seat->shell->manager, key_source(seat));
	remove_seat(seat);
}

static void add_seat(struct phone_shell *shell, struct weston_seat *weston_seat)
{
	struct shell_seat *seat;

	wl_list_for_each(seat, &shell->seats,
			 link) if (seat->seat == weston_seat) return;
	seat = calloc(1, sizeof(*seat));
	if (!seat) {
		weston_log("fplinux-session: cannot filter keyboard input\n");
		return;
	}
	seat->shell = shell;
	seat->seat = weston_seat;
	seat->caps_listener.notify = seat_caps_changed;
	wl_signal_add(&weston_seat->updated_caps_signal, &seat->caps_listener);
	seat->destroy_listener.notify = seat_destroyed;
	wl_signal_add(&weston_seat->destroy_signal, &seat->destroy_listener);
	wl_list_insert(&shell->seats, &seat->link);
	wrap_keyboard(seat);
}

static void seat_created(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, seat_listener);

	add_seat(shell, data);
	shell->focused_pid = 0;
	update_surfaces(shell);
}

static void session_changed(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, session_listener);

	(void)data;
	if (shell->compositor->session_active) {
		shell->focused_pid = 0;
		update_surfaces(shell);
	} else {
		fplinux_session_cancel_input(shell->manager,
					     FPLINUX_SESSION_KEY_PHONE);
		fplinux_session_cancel_input(shell->manager,
					     FPLINUX_SESSION_KEY_KEYBOARD);
		hide_launch_surface(shell);
	}
}

static void configure_output(struct phone_shell *shell,
			     struct weston_output *output)
{
	if (output != get_output(shell))
		return;
	watch_output(shell, output);
	fplinux_session_output_reset(shell->manager);
	/* Views are placed again for the new size; see session-core.h. */
	fplinux_session_output_resized(shell->manager, output->width,
				       output->height);
	show_background(shell, output);
	update_surfaces(shell);
	weston_output_set_ready(output);
}

static void output_changed(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, output_listener);
	struct weston_output *output = data;

	configure_output(shell, output);
}

static void output_created(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, output_created_listener);

	configure_output(shell, data);
}

static void shell_destroy(struct wl_listener *listener, void *data)
{
	struct phone_shell *shell =
		wl_container_of(listener, shell, destroy_listener);
	struct app_surface *surface;
	struct app_surface *next;
	struct shell_seat *seat;
	struct shell_seat *next_seat;

	(void)data;
	keyboard_shell = NULL;
	wl_list_for_each_safe(seat, next_seat, &shell->seats, link)
	{
		if (seat->keyboard &&
		    seat->keyboard->default_grab.interface == &filter_interface)
			seat->keyboard->default_grab.interface =
				seat->default_interface;
		remove_seat(seat);
	}
	wl_list_remove(&shell->destroy_listener.link);
	wl_list_remove(&shell->seat_listener.link);
	wl_list_remove(&shell->session_listener.link);
	wl_list_remove(&shell->output_listener.link);
	wl_list_remove(&shell->output_created_listener.link);
	detach_output(shell);
	if (shell->frame_idle)
		wl_event_source_remove(shell->frame_idle);
	if (shell->manager_source)
		wl_event_source_remove(shell->manager_source);
	hide_launch_surface(shell);
	fplinux_launch_frame_clear(&shell->launch_frame);
	hide_startup_surface(shell);
	fplinux_session_destroy(shell->manager);
	wl_list_for_each_safe(surface, next, &shell->surfaces, link)
		surface_removed(surface->desktop, shell);
	weston_desktop_destroy(shell->desktop);
	if (shell->background)
		weston_shell_utils_curtain_destroy(shell->background);
	weston_layer_fini(&shell->background_layer);
	weston_layer_fini(&shell->launcher_layer);
	weston_layer_fini(&shell->application_layer);
	weston_layer_fini(&shell->progress_layer);
	weston_layer_fini(&shell->lock_layer);
	weston_layer_fini(&shell->startup_layer);
	weston_layer_fini(&shell->hidden_layer);
	unlink("/run/fplinux-gui/ready");
	free(shell);
}

WL_EXPORT int wet_shell_init(struct weston_compositor *compositor, int *argc,
			     char *argv[])
{
	struct phone_shell *shell;
	struct passwd *user = getpwnam("fplinux");
	struct weston_output *output;
	struct weston_seat *seat;
	struct wl_event_loop *loop =
		wl_display_get_event_loop(compositor->wl_display);
	struct fplinux_session_options options;
	const char *socket_path = getenv("WAYLAND_DISPLAY");

	(void)argc;
	(void)argv;
	if (!user || !socket_path || socket_path[0] != '/' ||
	    chown(socket_path, 0, user->pw_gid) < 0 ||
	    chmod(socket_path, 0660) < 0) {
		weston_log(
			"fplinux-session: user or shared Wayland socket is unavailable\n");
		return -1;
	}
	shell = calloc(1, sizeof(*shell));
	if (!shell)
		return -1;
	shell->compositor = compositor;
	shell->loop = loop;
	wl_list_init(&shell->surfaces);
	wl_list_init(&shell->seats);
	weston_layer_init(&shell->background_layer, compositor);
	weston_layer_init(&shell->launcher_layer, compositor);
	weston_layer_init(&shell->application_layer, compositor);
	weston_layer_init(&shell->progress_layer, compositor);
	weston_layer_init(&shell->lock_layer, compositor);
	weston_layer_init(&shell->startup_layer, compositor);
	weston_layer_init(&shell->hidden_layer, compositor);
	weston_layer_set_position(&shell->background_layer,
				  WESTON_LAYER_POSITION_BACKGROUND);
	weston_layer_set_position(&shell->launcher_layer,
				  WESTON_LAYER_POSITION_NORMAL);
	weston_layer_set_position(&shell->application_layer,
				  WESTON_LAYER_POSITION_NORMAL + 1);
	weston_layer_set_position(&shell->progress_layer,
				  WESTON_LAYER_POSITION_NORMAL + 2);
	weston_layer_set_position(&shell->lock_layer,
				  WESTON_LAYER_POSITION_LOCK);
	weston_layer_set_position(&shell->startup_layer,
				  WESTON_LAYER_POSITION_LOCK + 1);
	weston_layer_set_position(&shell->hidden_layer,
				  WESTON_LAYER_POSITION_HIDDEN);
	shell->destroy_listener.notify = shell_destroy;
	wl_signal_add(&compositor->destroy_signal, &shell->destroy_listener);
	shell->seat_listener.notify = seat_created;
	wl_signal_add(&compositor->seat_created_signal, &shell->seat_listener);
	shell->session_listener.notify = session_changed;
	wl_signal_add(&compositor->session_signal, &shell->session_listener);
	shell->output_listener.notify = output_changed;
	wl_signal_add(&compositor->output_resized_signal,
		      &shell->output_listener);
	shell->output_created_listener.notify = output_created;
	wl_signal_add(&compositor->output_created_signal,
		      &shell->output_created_listener);
	shell->desktop = weston_desktop_create(compositor, &desktop_api, shell);
	if (!shell->desktop) {
		weston_log(
			"fplinux-session: cannot initialize desktop and phone input\n");
		return -1;
	}
	output = get_output(shell);
	if (!output)
		return -1;
	options = (struct fplinux_session_options){
		.applications = "/usr/share/applications",
		.socket_path = FPLINUX_UI_SESSION_SOCKET,
		.user = "fplinux",
		.home = "/home/fplinux",
		.app_uid = user->pw_uid,
		.app_gid = user->pw_gid,
		.metadata_uid = 0,
		.width = output->width,
		.height = output->height,
		.settings_path = "/etc/fplinux/session.conf",
		.languages_dir = FPLINUX_LANGUAGE_DIRECTORY,
		.brightness_socket = FPLINUX_BRIGHTNESS_SOCKET,
		.leds_dir = "/sys/class/leds",
		.power_supply_dir = "/sys/class/power_supply",
		.rtc_path = "/dev/rtc0",
		.audio_socket = FPLINUX_AUDIO_CONTROL_SOCKET,
		.audio_service = "/usr/libexec/fplinux/audiod",
		.audio_device = "/dev/snd/pcmC0D0p",
		.input_dir = "/dev/input",
		.clock_signal_path = "/usr/libexec/fplinux/clock-signal",
		.clock_preferences_directory = "/var/lib/fplinux/clocks",
		.changed = update_surfaces,
		.launch_frame = receive_launch_frame,
		.hold_frame = hold_frame,
		.capture_frame = capture_frame,
		.context = shell,
	};
	shell->manager = fplinux_session_create(&options);
	if (!shell->manager) {
		weston_log(
			"fplinux-session: cannot create application manager: %s\n",
			strerror(errno));
		return -1;
	}
	shell->manager_source = wl_event_loop_add_fd(
		loop, fplinux_session_get_fd(shell->manager), WL_EVENT_READABLE,
		manager_event, shell);
	if (!shell->manager_source)
		return -1;
	configure_output(shell, output);
	weston_compositor_add_key_binding(compositor, KEY_ESC, MODIFIER_CTRL,
					  home_binding, shell);
	keyboard_shell = shell;
	wl_list_for_each(seat, &compositor->seat_list, link)
		add_seat(shell, seat);
	return 0;
}
