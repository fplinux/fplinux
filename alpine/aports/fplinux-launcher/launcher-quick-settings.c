/* SPDX-License-Identifier: GPL-2.0-only */
#include "launcher-quick-settings.h"
#include "fplinux-bluez-adapter.h"
#include "fplinux-settings-values.h"
#include "fplinux-ui-system.h"

#include <libintl.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#define _(text) gettext(text)
#define BLUETOOTH_TIMEOUT_MS 10000

enum quick_command {
	QUICK_COMMAND_BACK = 300,
	QUICK_COMMAND_SAVE,
	QUICK_COMMAND_CANCEL,
	QUICK_COMMAND_CLOSE,
};

struct row_descriptor {
	const char *title;
	const char *glyph;
	enum fplinux_ui_setting_kind setting;
	enum fplinux_ui_setting_kind preview;
	int minimum;
	int maximum;
};

static const struct row_descriptor descriptors[] = {
	[FPLINUX_LAUNCHER_QUICK_SETTING_BRIGHTNESS] = {
		"Brightness", "brightness", FPLINUX_UI_SETTING_BRIGHTNESS,
		FPLINUX_UI_SETTING_BRIGHTNESS_PREVIEW,
		FPLINUX_SETTINGS_BRIGHTNESS_MIN, FPLINUX_SETTINGS_BRIGHTNESS_MAX,
	},
	[FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME] = {
		"Volume", "volume", FPLINUX_UI_SETTING_VOLUME,
		FPLINUX_UI_SETTING_VOLUME_PREVIEW, 0, 10,
	},
	[FPLINUX_LAUNCHER_QUICK_SETTING_WIFI] = {
		"Wi-Fi", "wifi", FPLINUX_UI_SETTING_WIFI,
		FPLINUX_UI_SETTING_NONE, 0, 0,
	},
	[FPLINUX_LAUNCHER_QUICK_SETTING_BLUETOOTH] = {
		"Bluetooth", "bluetooth", FPLINUX_UI_SETTING_NONE,
		FPLINUX_UI_SETTING_NONE, 0, 0,
	},
};

static void view_changed(struct fplinux_launcher_quick_settings *quick)
{
	if (quick->listener.view_changed)
		quick->listener.view_changed(quick->listener.context);
}

static bool send_setting(struct fplinux_launcher_quick_settings *quick,
			 enum fplinux_ui_setting_kind kind,
			 enum fplinux_ui_setting_operation operation,
			 union fplinux_ui_setting_value value)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.setting = { .kind = kind,
			     .operation = operation,
			     .value = value },
	};

	if (!++quick->setting_serial)
		++quick->setting_serial;
	message.serial = quick->setting_serial;
	return fplinux_ui_send(quick->ui, &message);
}

static bool
bluetooth_present(const struct fplinux_launcher_quick_settings *quick)
{
	return quick->bluetooth &&
	       fplinux_bluez_adapter_present(quick->bluetooth);
}

static enum fplinux_launcher_quick_setting
focused_row(const struct fplinux_launcher_quick_settings *quick)
{
	size_t focus = quick->list ? fplinux_ui_list_get_focused(quick->list) :
				     SIZE_MAX;

	return focus < quick->row_count ? quick->rows[focus] :
					  FPLINUX_LAUNCHER_QUICK_SETTING_COUNT;
}

static void show_action(struct fplinux_launcher_quick_settings *quick)
{
	enum fplinux_launcher_quick_setting row = focused_row(quick);
	const char *action = "";

	if (!quick->list)
		return;
	if (!quick->editing && row == FPLINUX_LAUNCHER_QUICK_SETTING_WIFI &&
	    quick->wifi_status.power != FPLINUX_UI_WIFI_UNAVAILABLE &&
	    !quick->wifi_pending)
		action = quick->wifi_status.power == FPLINUX_UI_WIFI_ON ?
				 _("Turn off") :
				 _("Turn on");
	if (!quick->editing &&
	    row == FPLINUX_LAUNCHER_QUICK_SETTING_BLUETOOTH &&
	    bluetooth_present(quick) &&
	    !fplinux_bluez_adapter_pending(quick->bluetooth))
		action = fplinux_bluez_adapter_powered(quick->bluetooth) ?
				 _("Turn off") :
				 _("Turn on");
	fplinux_ui_list_set_select_label(quick->list, action);
}

static const char *
wifi_detail(const struct fplinux_launcher_quick_settings *quick)
{
	if (quick->wifi_pending)
		return quick->wifi_turning_on ? _("Turning on") :
						_("Turning off");
	if (quick->wifi_status.power != FPLINUX_UI_WIFI_ON)
		return NULL;
	switch (quick->wifi_status.link) {
	case FPLINUX_UI_WIFI_DISCONNECTED:
		return _("No network");
	case FPLINUX_UI_WIFI_CONNECTING:
		return _("Connecting…");
	case FPLINUX_UI_WIFI_ADDRESSING:
		return _("Getting IP address…");
	case FPLINUX_UI_WIFI_CONNECTED:
		return quick->wifi_status.name;
	case FPLINUX_UI_WIFI_NO_ADDRESS:
		return _("No IP address");
	}
	return NULL;
}

static void show_rows(struct fplinux_launcher_quick_settings *quick)
{
	enum fplinux_launcher_quick_setting
		rows[FPLINUX_LAUNCHER_QUICK_SETTING_COUNT];
	enum fplinux_launcher_quick_setting focus = focused_row(quick);
	size_t count = 0;

	if (!quick->list)
		return;
	rows[count++] = FPLINUX_LAUNCHER_QUICK_SETTING_BRIGHTNESS;
	rows[count++] = FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME;
	if (quick->wifi_status.power != FPLINUX_UI_WIFI_UNAVAILABLE)
		rows[count++] = FPLINUX_LAUNCHER_QUICK_SETTING_WIFI;
	if (bluetooth_present(quick))
		rows[count++] = FPLINUX_LAUNCHER_QUICK_SETTING_BLUETOOTH;
	if (count != quick->row_count ||
	    memcmp(rows, quick->rows, count * sizeof(*rows))) {
		fplinux_ui_list_delete_all(quick->list);
		memcpy(quick->rows, rows, count * sizeof(*rows));
		quick->row_count = count;
		for (size_t i = 0; i < count; ++i) {
			const struct row_descriptor *descriptor =
				&descriptors[rows[i]];
			const void *glyph =
				fplinux_ui_glyph(quick->ui, descriptor->glyph,
						 FPLINUX_UI_GLYPH_ROW);

			fplinux_ui_list_append(quick->list,
					       _(descriptor->title), NULL);
			if (glyph)
				fplinux_ui_list_set_icon(
					quick->list, i, glyph,
					FPLINUX_UI_LIST_ICON_IMAGE);
			if (rows[i] == focus)
				fplinux_ui_list_set_focused(quick->list, i);
		}
	}
	for (size_t i = 0; i < count; ++i) {
		enum fplinux_launcher_quick_setting row = rows[i];
		const struct row_descriptor *descriptor = &descriptors[row];
		const char *detail = NULL;

		if (row == FPLINUX_LAUNCHER_QUICK_SETTING_WIFI)
			detail = wifi_detail(quick);
		if (row == FPLINUX_LAUNCHER_QUICK_SETTING_BLUETOOTH &&
		    fplinux_bluez_adapter_pending(quick->bluetooth))
			detail = quick->bluetooth_turning_on ? _("Turning on") :
							       _("Turning off");
		fplinux_ui_list_set(quick->list, i, _(descriptor->title),
				    detail);
		if (row <= FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME) {
			struct fplinux_launcher_quick_level *level =
				&quick->levels[row];

			if (level->known)
				fplinux_ui_list_set_level(
					quick->list, i, descriptor->minimum,
					descriptor->maximum,
					quick->editing &&
							row == quick->editing_row ?
						quick->draft :
						level->value);
			else
				fplinux_ui_list_set_trailing_value(
					quick->list, i,
					level->unavailable ? _("Unavailable") :
							     _("Loading…"));
		} else {
			bool on = row == FPLINUX_LAUNCHER_QUICK_SETTING_WIFI ?
					  quick->wifi_status.power ==
						  FPLINUX_UI_WIFI_ON :
					  fplinux_bluez_adapter_powered(
						  quick->bluetooth);

			fplinux_ui_list_set_trailing_value(
				quick->list, i, on ? _("On") : _("Off"));
		}
	}
	show_action(quick);
}

static bool key_filter(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		       const struct fplinux_ui_key *key, void *context);

static void alert_closed(struct fplinux_ui *ui, struct fplinux_ui_screen *alert,
			 int command_id, void *context)
{
	struct fplinux_launcher_quick_settings *quick = context;

	(void)ui;
	(void)command_id;
	if (alert == quick->alert)
		quick->alert = NULL;
}

static void show_alert(struct fplinux_launcher_quick_settings *quick,
		       const char *title, const char *text)
{
	const struct fplinux_ui_command close = {
		.id = QUICK_COMMAND_CLOSE,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Close"),
	};

	if (!quick->list || quick->alert)
		return;
	quick->alert = fplinux_ui_alert_new(quick->ui, FPLINUX_UI_ALERT_ERROR,
					    title, text, 0);
	if (!quick->alert)
		return;
	fplinux_ui_screen_add_command(quick->alert, &close);
	fplinux_ui_screen_set_listener(quick->alert, alert_closed, NULL, quick);
	fplinux_ui_screen_set_key_filter(quick->alert, key_filter, quick);
	fplinux_ui_push(quick->ui, quick->alert);
}

static void editing_commands(struct fplinux_launcher_quick_settings *quick)
{
	const struct fplinux_ui_command back = {
		.id = QUICK_COMMAND_BACK,
		.type = FPLINUX_UI_COMMAND_BACK,
	};
	const struct fplinux_ui_command save = {
		.id = QUICK_COMMAND_SAVE,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Save"),
		.direct = true,
	};
	const struct fplinux_ui_command cancel = {
		.id = QUICK_COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
	};

	if (!quick->list)
		return;
	fplinux_ui_screen_remove_command(quick->list, QUICK_COMMAND_BACK);
	fplinux_ui_screen_remove_command(quick->list, QUICK_COMMAND_SAVE);
	fplinux_ui_screen_remove_command(quick->list, QUICK_COMMAND_CANCEL);
	if (quick->editing) {
		if (!quick->save_requested) {
			fplinux_ui_screen_add_command(quick->list, &save);
			fplinux_ui_screen_add_command(quick->list, &cancel);
		}
	} else {
		fplinux_ui_screen_add_command(quick->list, &back);
	}
	show_action(quick);
}

static void end_edit(struct fplinux_launcher_quick_settings *quick,
		     bool rollback)
{
	if (!quick->editing)
		return;
	if (rollback)
		send_setting(quick, descriptors[quick->editing_row].preview,
			     FPLINUX_UI_SETTING_CANCEL,
			     (union fplinux_ui_setting_value){ 0 });
	quick->editing = false;
	quick->preview_pending = false;
	quick->save_requested = false;
	quick->save_pending = false;
	editing_commands(quick);
	show_rows(quick);
}

static void send_draft(struct fplinux_launcher_quick_settings *quick)
{
	const struct row_descriptor *descriptor =
		&descriptors[quick->editing_row];
	bool volume = quick->editing_row ==
		      FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME;

	if (quick->save_pending || (!volume && quick->preview_pending) ||
	    (volume && !quick->save_requested))
		return;
	if (quick->save_requested) {
		if (volume) {
			quick->preview_pending = false;
			quick->preview_serial = 0;
		}
		quick->save_pending =
			send_setting(quick,
				     volume ? FPLINUX_UI_SETTING_VOLUME_COMMIT :
					      descriptor->setting,
				     volume ? FPLINUX_UI_SETTING_COMMIT :
					      FPLINUX_UI_SETTING_SET,
				     (union fplinux_ui_setting_value){
					     .level = quick->draft });
		quick->save_serial = quick->setting_serial;
		if (!quick->save_pending) {
			quick->save_requested = false;
			editing_commands(quick);
			show_alert(
				quick, _(descriptor->title),
				_("The setting was not changed. Try again."));
		}
	} else {
		quick->preview_sent = quick->draft;
		quick->preview_pending = send_setting(
			quick, descriptor->preview, FPLINUX_UI_SETTING_PREVIEW,
			(union fplinux_ui_setting_value){
				.level = quick->draft });
		quick->preview_serial = quick->setting_serial;
		if (!quick->preview_pending) {
			end_edit(quick, true);
			show_alert(
				quick, _(descriptor->title),
				_("The setting was not changed. Try again."));
		}
	}
}

static void send_volume_step(struct fplinux_launcher_quick_settings *quick,
			     int step)
{
	quick->draft += step;
	if (quick->draft < 0)
		quick->draft = 0;
	if (quick->draft > 10)
		quick->draft = 10;
	quick->preview_pending = send_setting(
		quick, FPLINUX_UI_SETTING_VOLUME_STEP, FPLINUX_UI_SETTING_STEP,
		(union fplinux_ui_setting_value){ .step = step });
	quick->preview_serial = quick->setting_serial;
	if (!quick->preview_pending) {
		end_edit(quick, true);
		show_alert(quick, _("Volume"),
			   _("The setting was not changed. Try again."));
	}
}

static void setting_error(struct fplinux_launcher_quick_settings *quick,
			  const struct row_descriptor *descriptor,
			  const struct fplinux_ui_session_message *message)
{
	char text[384];

	if (!strcmp(message->reason, "save-failed") && message->applied) {
		snprintf(
			text, sizeof(text), "%s\n%s",
			message->storage_state ==
					FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
				_("The setting is applied. Saving is not confirmed.") :
				_("The setting is applied for this session. It was not saved."),
			fplinux_ui_storage_error(
				(struct fplinux_storage_result){
					.state = message->storage_state,
					.error = message->storage_error,
				}));
		show_alert(quick, _(descriptor->title), text);
	} else {
		show_alert(
			quick, _(descriptor->title),
			!strcmp(message->reason, "unavailable") ?
				_("This setting is unavailable now. Try again later.") :
				_("The setting was not changed. Try again."));
	}
}

bool fplinux_launcher_quick_settings_receive(
	struct fplinux_launcher_quick_settings *quick,
	const struct fplinux_ui_session_message *message)
{
	enum fplinux_launcher_quick_setting row;
	const struct row_descriptor *descriptor;
	bool preview;
	bool valid;
	const struct fplinux_ui_setting *setting = &message->setting;
	int value;
	enum fplinux_ui_setting_kind kind = setting->kind;

	if (message->type == FPLINUX_UI_SESSION_WIFI_STATUS) {
		quick->wifi_status = message->wifi_status;
		show_rows(quick);
		return true;
	}
	if (message->type != FPLINUX_UI_SESSION_SETTING &&
	    message->type != FPLINUX_UI_SESSION_ERROR)
		return false;
	if (kind == FPLINUX_UI_SETTING_NONE)
		return false;
	if (kind == FPLINUX_UI_SETTING_VOLUME_STEP)
		kind = FPLINUX_UI_SETTING_VOLUME_PREVIEW;
	else if (kind == FPLINUX_UI_SETTING_VOLUME_COMMIT)
		kind = FPLINUX_UI_SETTING_VOLUME;
	for (row = 0; row <= FPLINUX_LAUNCHER_QUICK_SETTING_WIFI; ++row) {
		descriptor = &descriptors[row];
		if (kind == descriptor->setting || kind == descriptor->preview)
			break;
	}
	if (row > FPLINUX_LAUNCHER_QUICK_SETTING_WIFI)
		return false;
	if (row == FPLINUX_LAUNCHER_QUICK_SETTING_WIFI) {
		if (message->type == FPLINUX_UI_SESSION_SETTING &&
		    setting->operation == FPLINUX_UI_SETTING_RESULT) {
			quick->wifi_pending = false;
		} else if (message->type == FPLINUX_UI_SESSION_ERROR) {
			if (quick->wifi_pending) {
				quick->wifi_pending = false;
				setting_error(quick, descriptor, message);
			}
		}
		show_rows(quick);
		return true;
	}
	preview = kind == descriptor->preview;
	value = setting->value.level;
	valid = (setting->operation == FPLINUX_UI_SETTING_RESULT ||
		 setting->operation == FPLINUX_UI_SETTING_PREVIEW ||
		 setting->operation == FPLINUX_UI_SETTING_SET) &&
		value >= descriptor->minimum && value <= descriptor->maximum;
	if (quick->editing && row == quick->editing_row) {
		bool volume = row == FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME;
		bool current_preview =
			volume ? message->serial == quick->preview_serial :
				 (!valid || value == quick->preview_sent);

		if (preview && current_preview &&
		    (quick->preview_pending || volume)) {
			quick->preview_pending = false;
			if (message->type == FPLINUX_UI_SESSION_ERROR) {
				end_edit(quick, true);
				setting_error(quick, descriptor, message);
			} else {
				if (volume && valid) {
					quick->draft = value;
				} else if (quick->save_requested ||
					   quick->draft !=
						   quick->preview_sent) {
					send_draft(quick);
				}
			}
		} else if (!preview && quick->save_pending &&
			   (!volume || message->serial == quick->save_serial)) {
			quick->save_pending = false;
			if ((message->type == FPLINUX_UI_SESSION_SETTING ||
			     message->applied) &&
			    valid)
				end_edit(quick, false);
			else if (volume)
				end_edit(quick, true);
			else {
				quick->save_requested = false;
				editing_commands(quick);
			}
			if (message->type == FPLINUX_UI_SESSION_ERROR)
				setting_error(quick, descriptor, message);
		}
	}
	show_rows(quick);
	return true;
}

void fplinux_launcher_quick_settings_levels_changed(
	struct fplinux_launcher_quick_settings *quick)
{
	for (enum fplinux_launcher_quick_setting row = 0;
	     row <= FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME; ++row) {
		struct fplinux_launcher_quick_level *level =
			&quick->levels[row];
		struct fplinux_system_level_state state;
		enum fplinux_system_level_kind kind =
			row == FPLINUX_LAUNCHER_QUICK_SETTING_BRIGHTNESS ?
				FPLINUX_SYSTEM_LEVEL_BRIGHTNESS :
				FPLINUX_SYSTEM_LEVEL_MEDIA;

		if (!fplinux_ui_system_level_get(quick->ui, kind, &state))
			continue;
		level->known = state.availability == FPLINUX_SYSTEM_LEVEL_READY;
		level->unavailable = state.availability ==
				     FPLINUX_SYSTEM_LEVEL_UNAVAILABLE;
		if (level->known) {
			level->value = (int)state.value;
			if (row == FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME &&
			    quick->editing && quick->editing_row == row &&
			    !state.pending && !quick->preview_pending &&
			    !quick->save_requested)
				quick->draft = level->value;
		}
	}
	show_rows(quick);
}

void fplinux_launcher_quick_settings_bluetooth_changed(
	struct fplinux_launcher_quick_settings *quick)
{
	show_rows(quick);
}

void fplinux_launcher_quick_settings_bluetooth_finished(
	struct fplinux_launcher_quick_settings *quick, bool applied)
{
	bool owned = quick->bluetooth_owned;

	quick->bluetooth_owned = false;
	show_rows(quick);
	if (!applied && owned)
		show_alert(quick, _("Bluetooth"),
			   quick->bluetooth_turning_on ?
				   _("Cannot turn Bluetooth on. Try again.") :
				   _("Cannot turn Bluetooth off. Try again."));
}

static void toggle(struct fplinux_launcher_quick_settings *quick,
		   enum fplinux_launcher_quick_setting row)
{
	if (row == FPLINUX_LAUNCHER_QUICK_SETTING_WIFI &&
	    quick->wifi_status.power != FPLINUX_UI_WIFI_UNAVAILABLE &&
	    !quick->wifi_pending) {
		bool value = quick->wifi_status.power == FPLINUX_UI_WIFI_OFF;

		quick->wifi_turning_on = value;
		quick->wifi_pending = send_setting(
			quick, FPLINUX_UI_SETTING_WIFI, FPLINUX_UI_SETTING_SET,
			(union fplinux_ui_setting_value){ .enabled = value });
		if (!quick->wifi_pending)
			show_alert(
				quick, _("Wi-Fi"),
				_("The setting was not changed. Try again."));
	} else if (row == FPLINUX_LAUNCHER_QUICK_SETTING_BLUETOOTH &&
		   bluetooth_present(quick) &&
		   !fplinux_bluez_adapter_pending(quick->bluetooth)) {
		quick->bluetooth_turning_on =
			!fplinux_bluez_adapter_powered(quick->bluetooth);
		quick->bluetooth_owned = true;
		if (!fplinux_bluez_adapter_set_powered(
			    quick->bluetooth, quick->bluetooth_turning_on,
			    BLUETOOTH_TIMEOUT_MS))
			fplinux_launcher_quick_settings_bluetooth_finished(
				quick, false);
	}
	show_rows(quick);
}

static void list_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			 int command_id, void *context)
{
	struct fplinux_launcher_quick_settings *quick = context;

	(void)ui;
	if (list != quick->list)
		return;
	if (command_id == QUICK_COMMAND_BACK)
		fplinux_launcher_quick_settings_close(quick);
	else if (command_id == QUICK_COMMAND_CANCEL && !quick->save_requested)
		end_edit(quick, true);
	else if (command_id == QUICK_COMMAND_SAVE && quick->editing &&
		 !quick->save_requested) {
		quick->save_requested = true;
		editing_commands(quick);
		send_draft(quick);
	} else if (command_id == FPLINUX_UI_SELECT_COMMAND && !quick->editing)
		toggle(quick, focused_row(quick));
}

static void row_focused(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			size_t index, void *context)
{
	(void)ui;
	(void)list;
	(void)index;
	show_action(context);
}

static bool key_filter(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		       const struct fplinux_ui_key *key, void *context)
{
	struct fplinux_launcher_quick_settings *quick = context;
	enum fplinux_launcher_quick_setting row;
	int next;

	(void)ui;
	if (key->source == FPLINUX_UI_KEY_PHONE &&
	    key->code == KEY_PICKUP_PHONE) {
		if (key->pressed)
			fplinux_launcher_quick_settings_close(quick);
		return true;
	}
	if (screen != quick->list)
		return false;
	if (quick->editing && (key->code == KEY_UP || key->code == KEY_DOWN))
		return true;
	if (key->code != KEY_LEFT && key->code != KEY_RIGHT)
		return false;
	row = focused_row(quick);
	if (row > FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME ||
	    !quick->levels[row].known)
		return false;
	if (!key->pressed || quick->save_requested)
		return true;
	if (row == FPLINUX_LAUNCHER_QUICK_SETTING_VOLUME) {
		if (!quick->editing) {
			quick->editing = true;
			quick->editing_row = row;
			quick->draft = quick->levels[row].value;
			editing_commands(quick);
		}
		send_volume_step(quick, key->code == KEY_LEFT ? -1 : 1);
		show_rows(quick);
		return true;
	}
	next = (quick->editing ? quick->draft : quick->levels[row].value) +
	       (key->code == KEY_LEFT ? -1 : 1);
	if (next < descriptors[row].minimum || next > descriptors[row].maximum)
		return true;
	if (!quick->editing) {
		quick->editing = true;
		quick->editing_row = row;
		editing_commands(quick);
	}
	quick->draft = next;
	send_draft(quick);
	show_rows(quick);
	return true;
}

void fplinux_launcher_quick_settings_init(
	struct fplinux_launcher_quick_settings *quick, struct fplinux_ui *ui,
	struct fplinux_bluez_adapter *bluetooth,
	const struct fplinux_launcher_quick_settings_listener *listener)
{
	memset(quick, 0, sizeof(*quick));
	quick->ui = ui;
	quick->bluetooth = bluetooth;
	if (listener)
		quick->listener = *listener;
}

bool fplinux_launcher_quick_settings_is_open(
	const struct fplinux_launcher_quick_settings *quick)
{
	return quick->list != NULL;
}

void fplinux_launcher_quick_settings_open(
	struct fplinux_launcher_quick_settings *quick)
{
	if (quick->list || quick->blocked)
		return;
	quick->list = fplinux_ui_list_new(quick->ui, _("Quick settings"),
					  FPLINUX_UI_LIST_IMPLICIT);
	if (!quick->list)
		return;
	fplinux_ui_screen_set_listener(quick->list, list_command, NULL, quick);
	fplinux_ui_list_set_focus_listener(quick->list, row_focused, quick);
	fplinux_ui_screen_set_key_filter(quick->list, key_filter, quick);
	quick->row_count = 0;
	fplinux_launcher_quick_settings_levels_changed(quick);
	editing_commands(quick);
	show_rows(quick);
	fplinux_ui_push(quick->ui, quick->list);
	view_changed(quick);
	if (quick->bluetooth)
		fplinux_bluez_adapter_connect(quick->bluetooth);
}

void fplinux_launcher_quick_settings_close(
	struct fplinux_launcher_quick_settings *quick)
{
	struct fplinux_ui_screen *list = quick->list;

	if (!list)
		return;
	end_edit(quick, !quick->save_pending);
	if (quick->alert && fplinux_ui_current(quick->ui) == quick->alert)
		fplinux_ui_pop(quick->ui);
	quick->alert = NULL;
	quick->wifi_pending = false;
	quick->bluetooth_owned = false;
	if (fplinux_ui_current(quick->ui) == list)
		fplinux_ui_pop(quick->ui);
	quick->list = NULL;
	quick->row_count = 0;
	view_changed(quick);
}

void fplinux_launcher_quick_settings_set_blocked(
	struct fplinux_launcher_quick_settings *quick, bool blocked)
{
	quick->blocked = blocked;
	if (blocked)
		fplinux_launcher_quick_settings_close(quick);
}
