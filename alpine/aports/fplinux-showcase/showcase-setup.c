// SPDX-License-Identifier: GPL-2.0-only
#include "showcase-app.h"

#include <errno.h>
#include <stdio.h>

static const unsigned int cycle_values[] = { 1, 2, 3, 5, 10, 0 };

static struct showcase_config defaults(void)
{
	return (struct showcase_config){
		.renderer = ARMADA_RENDER_AUTO,
		.cycles = 1,
		.effects = SHOWCASE_EFFECT_SCREEN | SHOWCASE_EFFECT_KEYPAD |
			   SHOWCASE_EFFECT_VIBRATION,
		.fullscreen = true,
	};
}

static const char *renderer_name(enum armada_render_mode renderer)
{
	const char *names[] = { _("Auto"), _("Software"), _("GPU") };

	return names[renderer];
}

const char *showcase_effect_name(unsigned int flag)
{
	switch (flag) {
	case SHOWCASE_EFFECT_SCREEN:
		return _("Scene brightness");
	case SHOWCASE_EFFECT_KEYPAD:
		return _("Keypad light");
	default:
		return _("Vibration");
	}
}

static long long preference(struct showcase_app *app, const char *key,
			    long long fallback, long long minimum,
			    long long maximum)
{
	long long value;

	value = fplinux_ui_prefs_get_int(app->prefs, key, fallback);
	return value >= minimum && value <= maximum ? value : fallback;
}

static unsigned int cycles_preference(struct showcase_app *app,
				      unsigned int fallback)
{
	long long value = preference(app, "cycles", fallback, 0, 10);

	for (size_t index = 0;
	     index < sizeof(cycle_values) / sizeof(cycle_values[0]); ++index)
		if (value == cycle_values[index])
			return cycle_values[index];
	return fallback;
}

void showcase_setup_load(struct showcase_app *app)
{
	struct showcase_config *config = &app->draft;

	app->prefs = fplinux_ui_prefs_open(SHOWCASE_APP_ID);
	if (!app->prefs)
		app->prefs_error = errno;
	*config = defaults();
	config->renderer = (enum armada_render_mode)preference(
		app, "renderer", config->renderer, ARMADA_RENDER_AUTO,
		ARMADA_RENDER_GPU);
	config->fullscreen = preference(app, "fullscreen", 1, 0, 1);
	config->effects =
		(unsigned int)preference(app, "effects", config->effects, 0, 7);
	config->cycles = cycles_preference(app, config->cycles);
}

static void save(struct showcase_app *app)
{
	const struct showcase_config *config = &app->draft;
	bool updated = app->prefs &&
		       fplinux_ui_prefs_set_int(app->prefs, "renderer",
						config->renderer) &&
		       fplinux_ui_prefs_set_int(app->prefs, "fullscreen",
						config->fullscreen) &&
		       fplinux_ui_prefs_set_int(app->prefs, "cycles",
						config->cycles) &&
		       fplinux_ui_prefs_set_int(app->prefs, "effects",
						config->effects);
	struct fplinux_storage_result result;

	result = updated ?
			 fplinux_ui_prefs_save(app->prefs) :
			 (struct fplinux_storage_result){
				 .state = FPLINUX_STORAGE_UNPUBLISHED,
				 .error = app->prefs ? errno : app->prefs_error,
			 };
	if (result.state == FPLINUX_STORAGE_DURABLE) {
		app->save_warning = false;
	} else if (!app->save_warning) {
		char text[768];

		app->save_warning = true;
		snprintf(text, sizeof(text),
			 _("These settings apply for this session only.\n\n%s"),
			 fplinux_ui_storage_error(result));
		showcase_show_alert(app, FPLINUX_UI_ALERT_WARNING,
				    _("Settings not saved"), text);
	}
}

static void child_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			  int command_id, void *context)
{
	(void)list;
	(void)context;
	if (command_id == SHOWCASE_BACK)
		fplinux_ui_go_back(ui);
}

static void effects_changed(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *list, size_t index,
			    void *context)
{
	struct showcase_app *app = context;
	struct showcase_config *config = &app->draft;
	unsigned int flag;

	(void)ui;
	if (index >= app->items.effect_count)
		return;
	flag = app->items.effect_flags[index];
	if (fplinux_ui_list_is_selected(list, index))
		config->effects |= flag;
	else
		config->effects &= ~flag;
	showcase_setup_refresh(app, false);
	save(app);
}

static void set_glyph(struct showcase_app *app, struct fplinux_ui_screen *list,
		      size_t row, const char *name)
{
	const void *glyph =
		fplinux_ui_glyph(app->ui, name, FPLINUX_UI_GLYPH_ROW);

	if (glyph)
		fplinux_ui_list_set_icon(list, row, glyph,
					 FPLINUX_UI_LIST_ICON_IMAGE);
}

static const char *effect_glyph(unsigned int flag)
{
	switch (flag) {
	case SHOWCASE_EFFECT_SCREEN:
		return "brightness";
	case SHOWCASE_EFFECT_KEYPAD:
		return "keyboard";
	default:
		return "settings";
	}
}

static void open_effects(struct showcase_app *app)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		app->ui, _("Effects"), FPLINUX_UI_LIST_MULTIPLE);
	const struct fplinux_ui_command back = {
		.id = SHOWCASE_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
	};

	if (!list)
		return;
	for (size_t index = 0; index < app->items.effect_count; ++index) {
		unsigned int flag = app->items.effect_flags[index];

		fplinux_ui_list_append(list, showcase_effect_name(flag), NULL);
		set_glyph(app, list, index, effect_glyph(flag));
		fplinux_ui_list_set_selected(list, index,
					     (app->draft.effects & flag) != 0);
	}
	fplinux_ui_screen_add_command(list, &back);
	fplinux_ui_screen_set_listener(list, child_command, effects_changed,
				       app);
	fplinux_ui_push(app->ui, list);
}

static void choice_changed(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, size_t index,
			   void *context)
{
	struct showcase_app *app = context;
	struct showcase_config *config = &app->draft;
	size_t item = app->choice_item;

	(void)list;
	if (item == app->items.renderer) {
		if (index > ARMADA_RENDER_GPU)
			return;
		config->renderer = (enum armada_render_mode)index;
	} else if (item == app->items.screen_mode) {
		if (index >= 2)
			return;
		config->fullscreen = index == 1;
	} else if (item == app->items.cycles) {
		if (index >= sizeof(cycle_values) / sizeof(cycle_values[0]))
			return;
		config->cycles = cycle_values[index];
	} else {
		return;
	}
	showcase_setup_refresh(app, false);
	fplinux_ui_go_back(ui);
	save(app);
}

static void open_choice(struct showcase_app *app, size_t item)
{
	const struct showcase_config *config = &app->draft;
	const char *renderers[] = { _("Auto"), _("Software"), _("GPU") };
	const char *screen_modes[] = { _("Standard"), _("Full screen") };
	const char *cycles[] = { "1", "2", "3", "5", "10", _("Until stopped") };
	const char **names;
	const char *title;
	size_t count;
	size_t current;
	struct fplinux_ui_screen *list;
	const struct fplinux_ui_command back = {
		.id = SHOWCASE_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
	};

	if (item == app->items.renderer) {
		title = _("Renderer");
		names = renderers;
		count = sizeof(renderers) / sizeof(renderers[0]);
		current = config->renderer;
	} else if (item == app->items.screen_mode) {
		title = _("Screen mode");
		names = screen_modes;
		count = sizeof(screen_modes) / sizeof(screen_modes[0]);
		current = config->fullscreen;
	} else if (item == app->items.cycles) {
		title = _("Cycles");
		names = cycles;
		count = sizeof(cycle_values) / sizeof(cycle_values[0]);
		current = 0;
		for (size_t index = 0; index < count; ++index)
			if (config->cycles == cycle_values[index])
				current = index;
	} else {
		return;
	}
	list = fplinux_ui_list_new(app->ui, title, FPLINUX_UI_LIST_EXCLUSIVE);
	if (!list)
		return;
	for (size_t index = 0; index < count; ++index)
		fplinux_ui_list_append(list, names[index], NULL);
	fplinux_ui_list_set_selected(list, current, true);
	fplinux_ui_list_set_focused(list, current);
	fplinux_ui_screen_add_command(list, &back);
	fplinux_ui_screen_set_listener(list, child_command, choice_changed,
				       app);
	app->choice_item = item;
	fplinux_ui_push(app->ui, list);
}

static void command(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		    int command_id, void *context)
{
	struct showcase_app *app = context;
	struct showcase_config *config = &app->draft;

	if (command_id == SHOWCASE_BACK) {
		fplinux_ui_go_back(ui);
	} else if (command_id == SHOWCASE_START) {
		showcase_start(app, config);
	} else if (command_id == SHOWCASE_RESET) {
		*config = defaults();
		showcase_setup_refresh(app, true);
		save(app);
	} else if (command_id == SHOWCASE_LAST_RESULT) {
		showcase_show_result(app);
	} else if (command_id == SHOWCASE_EFFECTS) {
		open_effects(app);
	} else if (command_id == SHOWCASE_EDIT) {
		open_choice(app, fplinux_ui_screen_get_command_item(screen));
	}
}

static size_t append_row(struct showcase_app *app, const char *label,
			 const char *value, int command_id,
			 const char *command_label, const char *glyph)
{
	size_t item = fplinux_ui_list_append(app->setup, label, value);
	const struct fplinux_ui_command command = {
		.id = command_id,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = command_label,
	};

	fplinux_ui_list_add_item_command(app->setup, item, &command);
	set_glyph(app, app->setup, item, glyph);
	return item;
}

static void append_effects(struct showcase_app *app)
{
	struct showcase_setup_items *items = &app->items;
	const unsigned int flags[] = { SHOWCASE_EFFECT_SCREEN,
				       SHOWCASE_EFFECT_KEYPAD,
				       SHOWCASE_EFFECT_VIBRATION };
	unsigned int selected = 0;
	const char *value = _("Off");
	char count[16];

	for (size_t index = 0; index < 3; ++index) {
		if (app->capabilities & flags[index]) {
			items->effect_flags[items->effect_count] = flags[index];
			++items->effect_count;
			if (app->draft.effects & flags[index]) {
				++selected;
				value = showcase_effect_name(flags[index]);
			}
		}
	}
	if (!items->effect_count)
		return;
	if (selected == items->effect_count)
		value = _("On");
	else if (selected > 1) {
		snprintf(count, sizeof(count), "%u/%zu", selected,
			 items->effect_count);
		value = count;
	}
	items->effects = append_row(app, _("Effects"), value, SHOWCASE_EFFECTS,
				    _("Edit"), "settings");
}

void showcase_setup_refresh(struct showcase_app *app, bool focus_start)
{
	const struct showcase_config *config = &app->draft;
	size_t focus = fplinux_ui_list_get_focused(app->setup);
	size_t first_visible = fplinux_ui_list_get_first_visible(app->setup);
	char cycles[16];

	fplinux_ui_list_delete_all(app->setup);
	app->items = (struct showcase_setup_items){
		.effects = SIZE_MAX,
	};
	app->items.start = append_row(app, _("Start"), NULL, SHOWCASE_START,
				      _("Start"), "play");
	app->items.renderer = append_row(app, _("Renderer"),
					 renderer_name(config->renderer),
					 SHOWCASE_EDIT, _("Edit"), "display");
	app->items.screen_mode = append_row(
		app, _("Screen mode"),
		config->fullscreen ? _("Full screen") : _("Standard"),
		SHOWCASE_EDIT, _("Edit"), "fullscreen");
	snprintf(cycles, sizeof(cycles), "%u", config->cycles);
	app->items.cycles = append_row(
		app, _("Cycles"), config->cycles ? cycles : _("Until stopped"),
		SHOWCASE_EDIT, _("Edit"), "refresh");
	append_effects(app);
	if (app->result.available)
		append_row(app, _("Last result"), NULL, SHOWCASE_LAST_RESULT,
			   _("Open"), "history");
	fplinux_ui_list_set_focused(
		app->setup,
		focus_start || focus == SIZE_MAX ? app->items.start : focus);
	if (!focus_start && first_visible != SIZE_MAX)
		fplinux_ui_list_set_first_visible(app->setup, first_visible);
}

bool showcase_setup_create(struct showcase_app *app)
{
	const struct fplinux_ui_command back = {
		.id = SHOWCASE_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
	};
	const struct fplinux_ui_command reset = {
		.id = SHOWCASE_RESET,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Reset parameters"),
	};

	app->setup = fplinux_ui_list_new(app->ui, "ARMADA",
					 FPLINUX_UI_LIST_IMPLICIT);
	if (!app->setup || !fplinux_ui_list_set_value_details(app->setup))
		return false;
	fplinux_ui_screen_add_command(app->setup, &back);
	fplinux_ui_screen_add_command(app->setup, &reset);
	fplinux_ui_screen_set_listener(app->setup, command, NULL, app);
	showcase_setup_refresh(app, true);
	if (!fplinux_ui_push(app->ui, app->setup))
		return false;
	if (app->prefs_error)
		save(app);
	return true;
}
