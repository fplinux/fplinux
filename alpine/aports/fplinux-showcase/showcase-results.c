// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "showcase-app.h"

#include <stdio.h>
#include <stdlib.h>

static const char *status_name(enum showcase_status status)
{
	switch (status) {
	case SHOWCASE_STATUS_COMPLETED:
		return _("Completed");
	case SHOWCASE_STATUS_STOPPED:
		return _("Stopped");
	case SHOWCASE_STATUS_INTERRUPTED:
		return _("Interrupted");
	case SHOWCASE_STATUS_ERROR:
		return _("Error");
	}
	return _("Unknown");
}

static bool append_value(struct fplinux_ui_screen *form, const char *label,
			 const char *text)
{
	return fplinux_ui_form_append_value(form, label, text) != SIZE_MAX;
}

static bool measurement(struct fplinux_ui_screen *form, const char *label,
			const char *format, double value)
{
	char text[80];

	snprintf(text, sizeof(text), format, value);
	return append_value(form, label, text);
}

static bool append_measurements(struct fplinux_ui_screen *form,
				const struct showcase_result *result)
{
	char frames[32];
	double fps = result->duration_ns ? (double)result->frames * 1e9 /
						   (double)result->duration_ns :
					   0.0;
	uint64_t average_ns =
		result->frames ? result->render_ns_sum / result->frames : 0;

	snprintf(frames, sizeof(frames), "%llu",
		 (unsigned long long)result->frames);
	return append_value(form, _("Status"), status_name(result->status)) &&
	       measurement(form, _("Generated FPS"), "%.2f", fps) &&
	       measurement(form, _("Active time"), _("%.3f s"),
			   (double)result->duration_ns / 1e9) &&
	       append_value(form, _("Frames"), frames) &&
	       measurement(form, _("Average render"), _("%.3f ms"),
			   (double)average_ns / 1e6) &&
	       measurement(form, _("Longest render"), _("%.3f ms"),
			   (double)result->render_ns_max / 1e6);
}

static bool append_effects(struct fplinux_ui_screen *form,
			   unsigned int selected)
{
	const unsigned int flags[] = { SHOWCASE_EFFECT_SCREEN,
				       SHOWCASE_EFFECT_KEYPAD,
				       SHOWCASE_EFFECT_VIBRATION };
	const char *names[3];
	size_t count = 0;
	char *text;
	bool added;

	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
		if (selected & flags[i])
			names[count++] = showcase_effect_name(flags[i]);
	if (count < 2)
		return append_value(form, _("Effects"),
				    count ? names[0] : _("Off"));
	if (asprintf(&text, "%s, %s%s%s", names[0], names[1],
		     count == 3 ? ", " : "", count == 3 ? names[2] : "") < 0)
		return false;
	added = append_value(form, _("Effects"), text);
	free(text);
	return added;
}

static bool append_parameters(struct fplinux_ui_screen *form,
			      const struct showcase_result *result)
{
	const struct showcase_effective *effective = &result->effective;
	char text[64], cycles[16];

	if (fplinux_ui_form_append_string(form, _("Run settings"), NULL) ==
		    SIZE_MAX ||
	    !append_value(form, _("Renderer"),
			  effective->renderer_name[0] ?
				  effective->renderer_name :
				  _("Not started")))
		return false;
	if (effective->width > 0 && effective->height > 0) {
		snprintf(text, sizeof(text), "%d×%d", effective->width,
			 effective->height);
		if (!append_value(form, _("Resolution"), text))
			return false;
	}
	snprintf(cycles, sizeof(cycles), "%u", effective->config.cycles);
	return append_value(form, _("Screen mode"),
			    effective->config.fullscreen ? _("Full screen") :
							   _("Standard")) &&
	       append_value(form, _("Cycles"),
			    effective->config.cycles ? cycles :
						       _("Until stopped")) &&
	       append_effects(form, effective->effects) &&
	       append_value(form, _("Model"), result->model) &&
	       append_value(form, _("Build"), result->build) &&
	       (!result->reason[0] ||
		fplinux_ui_form_append_string(form, _("Reason"),
					      result->reason) != SIZE_MAX);
}

static void command(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		    int command_id, void *context)
{
	struct showcase_app *app = context;

	(void)ui;
	(void)screen;
	if (command_id == SHOWCASE_SETTINGS || command_id == SHOWCASE_BACK)
		showcase_show_setup(app);
	else if (command_id == SHOWCASE_REPEAT)
		showcase_restart(app, &app->result.effective);
}

void showcase_show_result(struct showcase_app *app)
{
	struct fplinux_ui_screen *viewer;
	const struct fplinux_ui_command repeat = {
		.id = SHOWCASE_REPEAT,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Repeat"),
		.icon = fplinux_ui_glyph(app->ui, "refresh",
					 FPLINUX_UI_GLYPH_ROW),
	};
	const struct fplinux_ui_command settings = {
		.id = SHOWCASE_SETTINGS,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Settings"),
		.keep_visible = true,
	};

	if (!app->result.available || app->closing || app->results)
		return;
	viewer = fplinux_ui_form_new(app->ui, _("Result"));
	if (!viewer || !append_measurements(viewer, &app->result) ||
	    !append_parameters(viewer, &app->result))
		return;
	fplinux_ui_screen_add_command(viewer, &repeat);
	fplinux_ui_screen_add_command(viewer, &settings);
	fplinux_ui_screen_set_listener(viewer, command, NULL, app);
	app->state = SHOWCASE_RESULT;
	app->results = viewer;
	if (!fplinux_ui_push(app->ui, viewer))
		app->results = NULL;
}
