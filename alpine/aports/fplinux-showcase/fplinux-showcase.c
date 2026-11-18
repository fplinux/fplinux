// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "showcase-app.h"
#include "fplinux-cli.h"
#include "fplinux-system-info.h"

#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-ui-vibration.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOAD_TIMER_MS 1U
#define PREPARATION_TIMEOUT_NS 5000000000ULL

static void show_finished(void *context);
static void finish(struct showcase_app *app, enum showcase_status status,
		   const char *reason);

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static bool is_attempt(const struct showcase_app *app)
{
	return app->state == SHOWCASE_PREPARING ||
	       app->state == SHOWCASE_RUNNING;
}

static bool is_eligible(const struct showcase_app *app)
{
	return !app->closing && app->canvas_active &&
	       fplinux_ui_is_visible(app->ui) &&
	       fplinux_ui_lifecycle(app->ui) == FPLINUX_UI_FOREGROUND;
}

static uint64_t active_ms(const struct showcase_app *app)
{
	uint64_t now_ms = fplinux_ui_now_ms();
	return app->running && now_ms >= app->run.started_ms ?
		       now_ms - app->run.started_ms :
		       0;
}

static void clear_effects(struct showcase_app *app)
{
	if (app->lights_sent &&
	    fplinux_ui_light_effects_clear(app->ui, &app->light_serial))
		app->lights = (struct fplinux_ui_light_effect){ 0 };
	app->lights_sent = false;
	app->lights_applied = false;
	app->light_retry_ms = 0;
	if (app->vibration_sent)
		fplinux_ui_vibration_stop(app->ui);
	app->vibration_sent = false;
}

static void update_running(struct showcase_app *app)
{
	bool running = app->state == SHOWCASE_RUNNING && is_eligible(app);
	bool preparing = app->state == SHOWCASE_PREPARING && is_eligible(app);

	if (running != app->running) {
		app->running = running;
		if (!running)
			clear_effects(app);
	}
	if (app->frame && app->timer)
		fplinux_ui_frame_set_timer(app->frame, app->timer,
					   running || preparing);
	if (running != app->kept_on &&
	    fplinux_ui_keep_screen_on(app->ui, running))
		app->kept_on = running;
}

void showcase_show_alert(struct showcase_app *app,
			 enum fplinux_ui_alert_kind kind, const char *title,
			 const char *text)
{
	struct fplinux_ui_screen *alert =
		fplinux_ui_alert_new(app->ui, kind, title, text, 0);

	if (alert)
		fplinux_ui_push(app->ui, alert);
}

static bool release_drawing(struct showcase_app *app)
{
	bool released;

	clear_effects(app);
	if (app->timer) {
		if (app->frame)
			fplinux_ui_frame_set_timer(app->frame, NULL, false);
		lv_timer_delete(app->timer);
		app->timer = NULL;
	}
	released = armada_scene_destroy(app->scene);
	if (!released)
		fputs("fplinux-showcase: renderer cleanup was incomplete\n",
		      stderr);
	app->scene = NULL;
	fplinux_ui_frame_delete(app->frame);
	app->frame = NULL;
	app->message = NULL;
	return released;
}

static void reset_drawing(struct showcase_app *app)
{
	app->state = SHOWCASE_SETUP;
	lv_async_call_cancel(show_finished, app);
	update_running(app);
	release_drawing(app);
	app->canvas = NULL;
	app->results = NULL;
	app->canvas_active = false;
	fplinux_ui_pop_to(app->ui, app->setup);
}

void showcase_show_setup(struct showcase_app *app)
{
	reset_drawing(app);
	fplinux_ui_set_fullscreen(app->ui, false);
	showcase_setup_refresh(app, false);
}

void showcase_restart(struct showcase_app *app,
		      const struct showcase_effective *effective)
{
	struct showcase_config config = effective->config;

	config.renderer = effective->renderer;
	reset_drawing(app);
	showcase_start(app, &config);
}

static void preparation_error(struct showcase_app *app, const char *reason)
{
	char text[768];

	snprintf(
		text, sizeof(text),
		_("The run could not start. Review the settings and try again.\n\n%s"),
		reason);
	fprintf(stderr, "fplinux-showcase: %s\n", reason);
	showcase_show_setup(app);
	showcase_show_alert(app, FPLINUX_UI_ALERT_ERROR,
			    _("Preparation failed"), text);
}

static bool same_lights(const struct fplinux_ui_light_effect *first,
			const struct fplinux_ui_light_effect *second)
{
	return first->resources == second->resources &&
	       first->display_level == second->display_level &&
	       first->keypad_on == second->keypad_on;
}

static void effect_unavailable(struct showcase_app *app, unsigned int effects)
{
	if (!is_attempt(app))
		return;
	if (effects & (SHOWCASE_EFFECT_SCREEN | SHOWCASE_EFFECT_KEYPAD))
		app->light_resources = 0;
	if (effects & SHOWCASE_EFFECT_VIBRATION)
		app->vibration_available = false;
	finish(app, SHOWCASE_STATUS_ERROR,
	       _("A selected physical effect is unavailable."));
}

static void apply_effects(struct showcase_app *app,
			  const struct armada_outputs *outputs,
			  uint64_t elapsed_ms, uint32_t frame)
{
	struct fplinux_ui_light_effect lights = {
		.resources = app->light_resources
	};
	uint64_t cycle = elapsed_ms / SHOWCASE_CYCLE_MS;
	uint64_t now_ms = active_ms(app);
	uint64_t pulse_elapsed_ms = elapsed_ms % SHOWCASE_CYCLE_MS -
				    frame * 1000U / ARMADA_FRAMES_PER_SECOND;
	unsigned int remaining_ms = outputs->rumble_remaining_ms;

	if (now_ms > elapsed_ms)
		pulse_elapsed_ms += now_ms - elapsed_ms;
	if (lights.resources & FPLINUX_UI_LIGHT_SCREEN)
		lights.display_level = (unsigned int)outputs->lcd_level;
	if (lights.resources & FPLINUX_UI_LIGHT_KEYPAD)
		lights.keypad_on = outputs->keypad;
	if (lights.resources && !app->light_serial &&
	    fplinux_ui_now_ms() >= app->light_retry_ms &&
	    (!app->lights_applied || !same_lights(&lights, &app->lights))) {
		app->lights = lights;
		if (fplinux_ui_light_effects_apply(app->ui, &lights,
						   &app->light_serial)) {
			app->lights_sent = true;
		} else if (errno == EAGAIN) {
			app->light_retry_ms = fplinux_ui_now_ms() + 250U;
		} else {
			effect_unavailable(app, SHOWCASE_EFFECT_SCREEN |
							SHOWCASE_EFFECT_KEYPAD);
			return;
		}
	}
	if (!outputs->rumble || !app->vibration_available ||
	    (app->vibration_sent && app->vibration_cycle == cycle &&
	     app->vibration_cue == outputs->cue_id))
		return;
	remaining_ms = remaining_ms > pulse_elapsed_ms ?
			       remaining_ms - (unsigned int)pulse_elapsed_ms :
			       0;
	if (remaining_ms >= 50U) {
		unsigned int pattern[] = { 0, remaining_ms };

		if (fplinux_ui_vibration_play(app->ui, pattern, 2, false)) {
			app->vibration_cycle = cycle;
			app->vibration_cue = outputs->cue_id;
			app->vibration_sent = true;
		} else if (errno != EAGAIN) {
			effect_unavailable(app, SHOWCASE_EFFECT_VIBRATION);
		}
	}
}

static void update_status(struct showcase_app *app, bool force)
{
	uint64_t now_ms = fplinux_ui_now_ms();
	char text[128];

	if (!app->message || (!force && now_ms - app->run.status_ms < 250U))
		return;
	app->run.status_ms = now_ms;
	snprintf(text, sizeof(text), _("Running · %llu s"),
		 (unsigned long long)(active_ms(app) / 1000U));
	lv_label_set_text(app->message, text);
}

static bool present(struct showcase_app *app)
{
	uint16_t *pixels = fplinux_ui_frame_pixels(app->frame, NULL, NULL);

	memcpy(pixels, armada_scene_pixels(app->scene),
	       (size_t)app->effective.width * (size_t)app->effective.height *
		       sizeof(*pixels));
	fplinux_ui_frame_damage(app->frame, NULL);
	if (fplinux_ui_frame_present(app->frame))
		return true;
	finish(app,
	       errno == EAGAIN ? SHOWCASE_STATUS_INTERRUPTED :
				 SHOWCASE_STATUS_ERROR,
	       errno == EAGAIN ? _("The drawing area became unavailable.") :
				 _("The frame could not be presented."));
	return false;
}

static bool draw_frame(struct showcase_app *app, uint32_t frame,
		       uint64_t elapsed_ms)
{
	struct armada_outputs outputs;
	uint64_t started_ns = monotonic_ns();
	bool rendered = armada_scene_render(app->scene, frame, &outputs);
	uint64_t ended_ns = monotonic_ns();
	uint64_t render_ns = ended_ns >= started_ns ? ended_ns - started_ns : 0;

	if (!rendered) {
		finish(app, SHOWCASE_STATUS_ERROR,
		       armada_scene_error(app->scene));
		return false;
	}
	++app->run.frames;
	app->run.render_ns_sum += render_ns;
	if (render_ns > app->run.render_ns_max)
		app->run.render_ns_max = render_ns;
	if (!present(app))
		return false;
	if (app->running)
		apply_effects(app, &outputs, elapsed_ms, frame);
	return is_attempt(app);
}

static bool prepare(struct showcase_app *app)
{
	struct showcase_effective *effective = &app->effective;
	char error[512] = { 0 };
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(app->ui);

	if (fplinux_ui_is_fullscreen(app->ui) != effective->config.fullscreen) {
		if (monotonic_ns() - app->run.preparation_ns >=
		    PREPARATION_TIMEOUT_NS)
			preparation_error(
				app,
				_("The screen mode could not be changed. Try again."));
		return false;
	}
	if (!fplinux_ui_screen_get_content_size(app->canvas, &effective->width,
						&effective->height)) {
		preparation_error(app,
				  _("The drawing area could not be prepared."));
		return false;
	}
	if (!fplinux_ui_frame_resize(app->frame, effective->width,
				     effective->height)) {
		preparation_error(app,
				  _("The drawing area could not be prepared."));
		return false;
	}
	lv_obj_align(fplinux_ui_frame_object(app->frame), LV_ALIGN_CENTER, 0,
		     0);
	lv_obj_set_width(app->message, effective->width - 2 * metrics->margin);
	lv_obj_align(app->message, LV_ALIGN_TOP_MID, 0, metrics->margin);
	app->scene = armada_scene_create_with_renderer(
		(unsigned int)effective->width, (unsigned int)effective->height,
		effective->config.renderer, error, sizeof(error));
	if (!app->scene) {
		preparation_error(app, error);
		return false;
	}
	effective->renderer = armada_scene_render_mode(app->scene);
	snprintf(effective->renderer_name, sizeof(effective->renderer_name),
		 "%s", armada_scene_renderer(app->scene));
	if (armada_scene_fallback_reason(app->scene)[0])
		fprintf(stderr,
			"fplinux-showcase: GPU unavailable: %s; using software\n",
			armada_scene_fallback_reason(app->scene));
	effective->effects = effective->config.effects & app->capabilities;
	app->light_resources = effective->effects & (SHOWCASE_EFFECT_SCREEN |
						     SHOWCASE_EFFECT_KEYPAD);
	app->vibration_available =
		(effective->effects & SHOWCASE_EFFECT_VIBRATION) != 0;
	app->run.started_ms = fplinux_ui_now_ms();
	app->state = SHOWCASE_RUNNING;
	lv_obj_set_hidden(fplinux_ui_frame_object(app->frame), false);
	if (effective->config.fullscreen) {
		lv_obj_delete(app->message);
		app->message = NULL;
	}
	lv_timer_set_period(app->timer, LOAD_TIMER_MS);
	update_running(app);
	update_status(app, true);
	return true;
}

static void timed_tick(struct showcase_app *app)
{
	uint64_t elapsed_ms = active_ms(app);
	uint64_t now_ns = monotonic_ns();
	uint64_t duration_ns = (uint64_t)app->effective.config.cycles *
			       SHOWCASE_CYCLE_MS * 1000000ULL;
	uint32_t frame = (uint32_t)((elapsed_ms % SHOWCASE_CYCLE_MS) *
				    ARMADA_FRAMES_PER_SECOND / 1000U);

	if (!app->run.measured_started_ns)
		app->run.measured_started_ns = now_ns;
	if (duration_ns && app->run.frames &&
	    now_ns - app->run.measured_started_ns >= duration_ns) {
		finish(app, SHOWCASE_STATUS_COMPLETED,
		       _("The selected cycles completed."));
		return;
	}
	update_status(app, false);
	if (!draw_frame(app, frame, elapsed_ms))
		return;
	if (duration_ns &&
	    monotonic_ns() - app->run.measured_started_ns >= duration_ns) {
		finish(app, SHOWCASE_STATUS_COMPLETED,
		       _("The selected cycles completed."));
	}
}

static void tick(lv_timer_t *timer)
{
	struct showcase_app *app = lv_timer_get_user_data(timer);

	if (app->state == SHOWCASE_PREPARING) {
		if (is_eligible(app))
			prepare(app);
		return;
	}
	if (!app->running)
		return;
	timed_tick(app);
}

static void finish(struct showcase_app *app, enum showcase_status status,
		   const char *reason)
{
	uint64_t now_ns = monotonic_ns();

	if (!is_attempt(app))
		return;
	app->result = (struct showcase_result){
		.effective = app->effective,
		.status = status,
		.frames = app->run.frames,
		.duration_ns = app->run.measured_started_ns ?
				       now_ns - app->run.measured_started_ns :
				       0,
		.render_ns_sum = app->run.render_ns_sum,
		.render_ns_max = app->run.render_ns_max,
		.available = true,
	};
	snprintf(app->result.reason, sizeof(app->result.reason), "%s", reason);
	app->state = SHOWCASE_RESULT;
	update_running(app);
	clear_effects(app);
	if (!fplinux_system_info_read_line("/proc/device-tree/model",
					   app->result.model,
					   sizeof(app->result.model)))
		snprintf(app->result.model, sizeof(app->result.model), "%s",
			 _("Unknown"));
	if (!fplinux_system_info_read_os_field("/etc/os-release", "BUILD_ID",
					       app->result.build,
					       sizeof(app->result.build)))
		snprintf(app->result.build, sizeof(app->result.build), "%s",
			 _("Unknown"));
	if (!app->closing)
		lv_async_call(show_finished, app);
}

static void show_finished(void *context)
{
	struct showcase_app *app = context;

	if (app->closing || app->state != SHOWCASE_RESULT)
		return;
	showcase_show_setup(app);
	showcase_show_result(app);
}

static void active(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		   bool enabled, void *context)
{
	struct showcase_app *app = context;

	(void)ui;
	(void)canvas;
	app->canvas_active = enabled;
	if (!enabled && is_attempt(app))
		finish(app, SHOWCASE_STATUS_INTERRUPTED,
		       _("The run lost the foreground drawing area."));
	update_running(app);
}

static void command(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		    int command_id, void *context)
{
	struct showcase_app *app = context;

	(void)ui;
	(void)screen;
	if (command_id == SHOWCASE_STOP)
		finish(app, SHOWCASE_STATUS_STOPPED, _("Stopped by user."));
}

void showcase_start(struct showcase_app *app,
		    const struct showcase_config *config)
{
	const struct fplinux_ui_theme *theme = fplinux_ui_theme(app->ui);
	const struct fplinux_ui_command stop = {
		.id = SHOWCASE_STOP,
		.type = FPLINUX_UI_COMMAND_STOP,
		.keep_visible = true,
	};

	if (app->state != SHOWCASE_SETUP || app->closing)
		return;
	app->effective = (struct showcase_effective){
		.config = *config,
		.renderer = config->renderer,
	};
	app->run = (struct showcase_run){ .preparation_ns = monotonic_ns() };
	app->state = SHOWCASE_PREPARING;
	app->canvas = fplinux_ui_canvas_new(
		app->ui, config->fullscreen ? NULL : "ARMADA");
	if (!app->canvas)
		goto failed;
	app->frame =
		fplinux_ui_frame_new(app->ui, 1, 1, FPLINUX_UI_FRAME_DIRECT);
	if (!app->frame)
		goto failed;
	lv_obj_set_parent(fplinux_ui_frame_object(app->frame),
			  fplinux_ui_canvas_object(app->canvas));
	lv_obj_set_hidden(fplinux_ui_frame_object(app->frame), true);
	app->message = lv_label_create(fplinux_ui_canvas_object(app->canvas));
	if (!app->message)
		goto failed;
	lv_obj_set_style_text_font(app->message, theme->font, 0);
	lv_obj_set_style_text_color(app->message, theme->foreground, 0);
	lv_obj_set_style_bg_color(app->message, theme->background, 0);
	lv_obj_set_style_bg_opa(app->message, LV_OPA_COVER, 0);
	lv_obj_set_style_text_align(app->message, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(app->message, LV_LABEL_LONG_WRAP);
	lv_label_set_text(app->message, _("Preparing…"));
	app->timer = lv_timer_create(tick, LOAD_TIMER_MS, app);
	if (!app->timer)
		goto failed;
	fplinux_ui_frame_set_timer(app->frame, app->timer, false);
	fplinux_ui_screen_add_command(app->canvas, &stop);
	fplinux_ui_screen_set_listener(app->canvas, command, NULL, app);
	fplinux_ui_canvas_set_active_listener(app->canvas, active, app);
	if (!fplinux_ui_set_fullscreen(app->ui, config->fullscreen) ||
	    !fplinux_ui_push(app->ui, app->canvas))
		goto failed;
	update_running(app);
	return;

failed:
	preparation_error(app, _("The drawing area could not be prepared."));
}

static void session_event(struct showcase_app *app,
			  const struct fplinux_ui_session_message *reply)
{
	bool light_error = reply->type == FPLINUX_UI_SESSION_ERROR &&
			   !strcmp(reply->name, "light_effect");

	if (reply->type == FPLINUX_UI_SESSION_LIGHT_EFFECT || light_error) {
		if (!app->light_serial || reply->serial != app->light_serial)
			return;
		app->light_serial = 0;
		app->lights_applied =
			!light_error &&
			same_lights(&reply->light_effect, &app->lights) &&
			app->lights.resources && app->running;
		if (light_error) {
			if (!strcmp(reply->reason, "not-visible"))
				app->light_retry_ms =
					fplinux_ui_now_ms() + 250U;
			else
				effect_unavailable(
					app, SHOWCASE_EFFECT_SCREEN |
						     SHOWCASE_EFFECT_KEYPAD);
		}
	} else if (reply->type == FPLINUX_UI_SESSION_ERROR &&
		   !strcmp(reply->name, "vibration") &&
		   strcmp(reply->reason, "not-visible") &&
		   app->vibration_available && app->vibration_sent) {
		effect_unavailable(app, SHOWCASE_EFFECT_VIBRATION);
	}
}

static void event(struct fplinux_ui *ui, const struct fplinux_ui_event *event,
		  void *context)
{
	struct showcase_app *app = context;

	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		app->closing = true;
		lv_async_call_cancel(show_finished, app);
		finish(app, SHOWCASE_STATUS_INTERRUPTED,
		       _("The application was closed."));
		update_running(app);
		clear_effects(app);
	} else if (event->type == FPLINUX_UI_EVENT_ACTIVATION &&
		   event->activation == FPLINUX_UI_ACTIVATION_OPEN) {
		fplinux_ui_menu_close(ui);
		if (is_attempt(app))
			finish(app, SHOWCASE_STATUS_INTERRUPTED,
			       _("Opened from the application catalogue."));
		if (app->setup)
			showcase_show_setup(app);
	} else if (event->type == FPLINUX_UI_EVENT_VISIBILITY ||
		   event->type == FPLINUX_UI_EVENT_LIFECYCLE) {
		if (is_attempt(app) && !is_eligible(app))
			finish(app, SHOWCASE_STATUS_INTERRUPTED,
			       _("The application became hidden or inactive."));
		update_running(app);
	} else if (event->type == FPLINUX_UI_EVENT_RESIZE && app->canvas) {
		if (app->state == SHOWCASE_RUNNING)
			finish(app, SHOWCASE_STATUS_INTERRUPTED,
			       _("The drawing area changed size."));
	} else if (event->type == FPLINUX_UI_EVENT_SESSION && event->session) {
		session_event(app, event->session);
	}
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-showcase",
		.description = "Display the ARMADA showcase.",
	};
	struct showcase_app app = { 0 };
	int result = fplinux_cli_parse(&cli, argc, argv);

	if (result != FPLINUX_CLI_READY)
		return result;
	app.ui = fplinux_ui_open(SHOWCASE_APP_ID, event, &app);
	if (!app.ui) {
		perror("fplinux-showcase: fplinux_ui_open");
		return EXIT_FAILURE;
	}
	app.capabilities = fplinux_ui_light_effects_supported(app.ui);
	if (fplinux_ui_vibration_supported(app.ui))
		app.capabilities |= SHOWCASE_EFFECT_VIBRATION;
	showcase_setup_load(&app);
	if (!showcase_setup_create(&app)) {
		perror("fplinux-showcase: create view");
		result = -1;
	} else {
		result = fplinux_ui_run(app.ui);
	}
	app.closing = true;
	lv_async_call_cancel(show_finished, &app);
	update_running(&app);
	if (!release_drawing(&app))
		result = -1;
	fplinux_ui_prefs_close(app.prefs);
	fplinux_ui_close(app.ui);
	return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
