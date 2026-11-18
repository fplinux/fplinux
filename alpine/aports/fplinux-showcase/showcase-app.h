// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_SHOWCASE_APP_H
#define FPLINUX_SHOWCASE_APP_H

#include "armada-scene.h"

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-frame.h>
#include <fplinux/fplinux-ui-lights.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <libintl.h>

#define SHOWCASE_APP_ID "org.fplinux.showcase"
#define _(text) gettext(text)
#define SHOWCASE_CYCLE_MS (ARMADA_DURATION_SECONDS * 1000U)

enum showcase_command {
	SHOWCASE_BACK,
	SHOWCASE_START,
	SHOWCASE_STOP,
	SHOWCASE_SETTINGS,
	SHOWCASE_RESET,
	SHOWCASE_LAST_RESULT,
	SHOWCASE_REPEAT,
	SHOWCASE_EFFECTS,
	SHOWCASE_EDIT,
};

enum showcase_state {
	SHOWCASE_SETUP,
	SHOWCASE_PREPARING,
	SHOWCASE_RUNNING,
	SHOWCASE_RESULT,
};

enum showcase_status {
	SHOWCASE_STATUS_COMPLETED,
	SHOWCASE_STATUS_STOPPED,
	SHOWCASE_STATUS_INTERRUPTED,
	SHOWCASE_STATUS_ERROR,
};

enum showcase_effect {
	SHOWCASE_EFFECT_SCREEN = FPLINUX_UI_LIGHT_SCREEN,
	SHOWCASE_EFFECT_KEYPAD = FPLINUX_UI_LIGHT_KEYPAD,
	SHOWCASE_EFFECT_VIBRATION = 4U,
};

struct showcase_config {
	enum armada_render_mode renderer;
	unsigned int cycles;
	unsigned int effects;
	bool fullscreen;
};

struct showcase_effective {
	struct showcase_config config;
	enum armada_render_mode renderer;
	unsigned int effects;
	int width;
	int height;
	char renderer_name[128];
};

struct showcase_result {
	struct showcase_effective effective;
	enum showcase_status status;
	uint64_t frames;
	uint64_t duration_ns;
	uint64_t render_ns_sum;
	uint64_t render_ns_max;
	char reason[512];
	char model[256];
	char build[256];
	bool available;
};

struct showcase_setup_items {
	size_t start;
	size_t renderer;
	size_t screen_mode;
	size_t cycles;
	size_t effects;
	unsigned int effect_flags[3];
	size_t effect_count;
};

struct showcase_run {
	uint64_t preparation_ns;
	uint64_t measured_started_ns;
	uint64_t frames;
	uint64_t render_ns_sum;
	uint64_t render_ns_max;
	uint64_t started_ms;
	uint64_t status_ms;
};

struct showcase_app {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *setup;
	struct fplinux_ui_screen *canvas;
	struct fplinux_ui_screen *results;
	struct fplinux_ui_frame *frame;
	struct armada_scene *scene;
	struct fplinux_ui_prefs *prefs;
	lv_obj_t *message;
	lv_timer_t *timer;
	struct showcase_config draft;
	enum showcase_state state;
	struct showcase_setup_items items;
	size_t choice_item;
	struct showcase_effective effective;
	struct showcase_run run;
	struct showcase_result result;
	unsigned int capabilities;
	struct fplinux_ui_light_effect lights;
	unsigned int light_resources;
	uint32_t light_serial;
	uint64_t light_retry_ms;
	uint64_t vibration_cycle;
	uint16_t vibration_cue;
	bool canvas_active;
	bool running;
	bool closing;
	bool lights_sent;
	bool lights_applied;
	bool vibration_available;
	bool vibration_sent;
	bool kept_on;
	bool save_warning;
	int prefs_error;
};

void showcase_setup_load(struct showcase_app *app);
const char *showcase_effect_name(unsigned int flag);
bool showcase_setup_create(struct showcase_app *app);
void showcase_setup_refresh(struct showcase_app *app, bool focus_start);
void showcase_show_setup(struct showcase_app *app);
void showcase_start(struct showcase_app *app,
		    const struct showcase_config *config);
void showcase_restart(struct showcase_app *app,
		      const struct showcase_effective *effective);
void showcase_show_result(struct showcase_app *app);
void showcase_show_alert(struct showcase_app *app,
			 enum fplinux_ui_alert_kind kind, const char *title,
			 const char *text);

#endif
