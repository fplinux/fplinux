/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "settings-bluetooth.h"

#include "fplinux-bluetooth-opp.h"
#include "fplinux-bluez-adapter.h"
#include <fplinux/fplinux-ui-screen.h>
#include <errno.h>
#include <libintl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define _(text) gettext(text)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define REQUEST_TIMEOUT_MS 15000
#define PAIR_TIMEOUT_MS 90000
#define HISTORY_COUNT 8

enum command {
	COMMAND_BACK,
	COMMAND_SAVE,
	COMMAND_CANCEL,
	COMMAND_SEARCH,
	COMMAND_ACCEPT,
	COMMAND_REJECT,
	COMMAND_FORGET,
	COMMAND_RESET_NAME,
	COMMAND_TRANSFER,
	COMMAND_HISTORY,
	COMMAND_CLOSE_TRANSFER,
	COMMAND_CONNECT_PROFILE,
};

enum root_row {
	ROOT_POWER,
	ROOT_VISIBILITY,
	ROOT_NAME,
	ROOT_NEARBY,
	ROOT_PAIRED,
	ROOT_RECEIVE_STATUS,
};

enum device_row {
	DEVICE_ADDRESS,
	DEVICE_STATUS,
	DEVICE_SIGNAL,
	DEVICE_BATTERY,
	DEVICE_SERVICES,
	DEVICE_PAIR,
	DEVICE_CONNECTION,
	DEVICE_TRUST,
	DEVICE_RENAME,
	DEVICE_FORGET,
};

enum mutation {
	MUTATION_NONE,
	MUTATION_POWER_ON,
	MUTATION_POWER_OFF,
	MUTATION_NAME,
	MUTATION_VISIBILITY,
	MUTATION_SCAN,
	MUTATION_PAIR,
	MUTATION_CONNECT,
	MUTATION_DISCONNECT,
	MUTATION_TRUST,
	MUTATION_ALIAS,
	MUTATION_FORGET,
};

enum editor_kind {
	EDITOR_NAME,
	EDITOR_ALIAS,
};

enum peer_selection {
	PEER_INSPECT,
	PEER_SEND,
};

enum incoming_pairing {
	INCOMING_PAIRING_CLOSED,
	INCOMING_PAIRING_OPENING,
	INCOMING_PAIRING_VISIBLE,
};

struct transfer_snapshot {
	enum fplinux_bluetooth_opp_state state;
	char filename[NAME_MAX + 1];
	uint64_t transferred_bytes;
	uint64_t total_bytes;
	bool total_known;
	bool done;
	struct fplinux_bluetooth_opp_result result;
};

struct transfer_history {
	char peer[FPLINUX_BLUEZ_NAME_BYTES];
	char filename[NAME_MAX + 1];
	struct fplinux_bluetooth_opp_result result;
};

struct fplinux_settings_bluetooth {
	struct fplinux_ui *ui;
	struct fplinux_bluez_adapter *adapter;
	struct fplinux_ui_screen *root;
	struct fplinux_ui_screen *devices;
	struct fplinux_ui_screen *device;
	struct fplinux_ui_screen *services;
	struct fplinux_ui_screen *visibility;
	struct fplinux_ui_screen *editor;
	struct fplinux_ui_screen *agent;
	struct fplinux_ui_screen *notice;
	struct fplinux_ui_screen *confirm;
	struct fplinux_ui_screen *history;
	struct fplinux_ui_screen *history_detail;
	struct fplinux_ui_screen *transfer;
	struct fplinux_ui_screen *wait;
	struct fplinux_ui_screen *screens[16];
	size_t screen_count;
	char device_path[FPLINUX_BLUEZ_PATH_BYTES];
	char row_paths[FPLINUX_BLUEZ_DEVICE_COUNT][FPLINUX_BLUEZ_PATH_BYTES];
	size_t row_count;
	bool devices_empty;
	bool paired_only;
	enum peer_selection peer_selection;
	enum device_row device_rows[16];
	size_t device_row_count;
	char service_uuids[FPLINUX_BLUEZ_UUID_COUNT][FPLINUX_BLUEZ_UUID_BYTES];
	size_t service_count;
	enum editor_kind editor_kind;
	enum mutation mutation;
	int mutation_error;
	bool mutation_failed;
	bool stop_discovery;
	bool visible;
	bool receive_ready;
	bool keep_screen_on;
	bool pairing_agent_enabled;
	enum incoming_pairing incoming_pairing;
	uint64_t agent_serial;
	enum fplinux_bluez_agent_kind agent_kind;
	size_t agent_code;
	bool agent_is_alert;
	bool transfer_running;
	bool transfer_options;
	bool close_pending;
	bool close_question;
	pthread_t thread;
	pthread_mutex_t mutex;
	atomic_bool cancelled;
	int event_fd;
	char transfer_peer[18];
	char transfer_peer_name[FPLINUX_BLUEZ_NAME_BYTES];
	char transfer_path[PATH_MAX];
	struct transfer_snapshot shared;
	struct transfer_history records[HISTORY_COUNT];
	size_t record_count;
};

static void root_refresh(struct fplinux_settings_bluetooth *bluetooth);
static void devices_refresh(struct fplinux_settings_bluetooth *bluetooth);
static void device_refresh(struct fplinux_settings_bluetooth *bluetooth);
static void agent_refresh(struct fplinux_settings_bluetooth *bluetooth);
static void transfer_refresh(struct fplinux_settings_bluetooth *bluetooth);
static void history_refresh(struct fplinux_settings_bluetooth *bluetooth,
			    bool added);
static void open_transfer(struct fplinux_settings_bluetooth *bluetooth);
static void start_transfer(struct fplinux_settings_bluetooth *bluetooth,
			   const char *device_path);
static void cancel_transfer(struct fplinux_settings_bluetooth *bluetooth);

static void add_command(struct fplinux_settings_bluetooth *bluetooth,
			struct fplinux_ui_screen *screen, int id,
			enum fplinux_ui_command_type type, const char *label,
			int priority)
{
	const char *glyph = NULL;
	struct fplinux_ui_command command = {
		.id = id,
		.type = type,
		.label = label,
		.priority = priority,
	};

	switch (id) {
	case COMMAND_SEARCH:
		glyph = "search";
		break;
	case COMMAND_ACCEPT:
		glyph = "check";
		break;
	case COMMAND_REJECT:
	case COMMAND_CANCEL:
		glyph = "cancel";
		break;
	case COMMAND_FORGET:
		glyph = "delete";
		break;
	case COMMAND_TRANSFER:
		glyph = "copy";
		break;
	case COMMAND_HISTORY:
		glyph = "history";
		break;
	case COMMAND_CONNECT_PROFILE:
		glyph = "bluetooth";
		break;
	case COMMAND_CLOSE_TRANSFER:
		glyph = "exit";
		break;
	default:
		break;
	}
	if (glyph)
		command.icon = fplinux_ui_glyph(bluetooth->ui, glyph,
						FPLINUX_UI_GLYPH_ROW);
	fplinux_ui_screen_add_command(screen, &command);
}

static void add_back(struct fplinux_settings_bluetooth *bluetooth,
		     struct fplinux_ui_screen *screen)
{
	add_command(bluetooth, screen, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK,
		    NULL, 1);
}

static bool add_action_row(struct fplinux_ui_screen *form, int id,
			   const char *label)
{
	const struct fplinux_ui_command command = {
		.id = id,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = label,
	};
	size_t row = fplinux_ui_form_append_value(form, label, NULL);

	return row != SIZE_MAX &&
	       fplinux_ui_form_add_item_command(form, row, &command);
}

static void set_glyph(struct fplinux_settings_bluetooth *bluetooth,
		      struct fplinux_ui_screen *list, size_t row,
		      const char *name)
{
	fplinux_ui_list_set_icon(list, row,
				 fplinux_ui_glyph(bluetooth->ui, name,
						  FPLINUX_UI_GLYPH_ROW),
				 FPLINUX_UI_LIST_ICON_IMAGE);
}

static void forget_screen(struct fplinux_settings_bluetooth *bluetooth,
			  struct fplinux_ui_screen *screen)
{
	struct fplinux_ui_screen **fields[] = {
		&bluetooth->root,	    &bluetooth->devices,
		&bluetooth->device,	    &bluetooth->services,
		&bluetooth->visibility,	    &bluetooth->editor,
		&bluetooth->agent,	    &bluetooth->notice,
		&bluetooth->confirm,	    &bluetooth->history,
		&bluetooth->history_detail, &bluetooth->transfer,
	};

	for (size_t i = 0; i < ARRAY_SIZE(fields); ++i)
		if (*fields[i] == screen)
			*fields[i] = NULL;
	for (size_t i = 0; i < bluetooth->screen_count; ++i)
		if (bluetooth->screens[i] == screen) {
			--bluetooth->screen_count;
			memmove(&bluetooth->screens[i],
				&bluetooth->screens[i + 1],
				(bluetooth->screen_count - i) *
					sizeof(bluetooth->screens[0]));
			break;
		}
}

static void push_screen(struct fplinux_settings_bluetooth *bluetooth,
			struct fplinux_ui_screen *screen)
{
	if (fplinux_ui_push(bluetooth->ui, screen))
		bluetooth->screens[bluetooth->screen_count++] = screen;
	else
		forget_screen(bluetooth, screen);
}

static void close_screen(struct fplinux_settings_bluetooth *bluetooth,
			 struct fplinux_ui_screen **field)
{
	struct fplinux_ui_screen *screen = *field;
	size_t index;

	if (!screen)
		return;
	if (bluetooth->close_question && screen == bluetooth->agent &&
	    fplinux_ui_current(bluetooth->ui) == bluetooth->confirm)
		return;
	for (index = 0; index < bluetooth->screen_count; ++index)
		if (bluetooth->screens[index] == screen)
			break;
	if (!fplinux_ui_pop_to(bluetooth->ui, screen))
		return;
	fplinux_ui_pop(bluetooth->ui);
	/* A withdrawn asynchronous prompt can have another prompt above it. */
	while (bluetooth->screen_count > index)
		forget_screen(bluetooth,
			      bluetooth->screens[bluetooth->screen_count - 1]);
}

static const char *device_name(const struct fplinux_bluez_device *device)
{
	return *device->alias ? device->alias :
	       *device->name  ? device->name :
				device->address;
}

static bool usable(const struct fplinux_settings_bluetooth *bluetooth)
{
	return bluetooth->adapter &&
	       fplinux_bluez_adapter_present(bluetooth->adapter) &&
	       fplinux_bluez_adapter_powered(bluetooth->adapter);
}

static bool can_answer(const struct fplinux_settings_bluetooth *bluetooth)
{
	return bluetooth->root && bluetooth->visible &&
	       fplinux_ui_lifecycle(bluetooth->ui) == FPLINUX_UI_FOREGROUND;
}

static void update_screen_hold(struct fplinux_settings_bluetooth *bluetooth)
{
	const struct fplinux_bluez_agent_request *request =
		fplinux_bluez_adapter_agent_request(bluetooth->adapter);
	bool enabled;

	enabled = can_answer(bluetooth) &&
		  (bluetooth->mutation == MUTATION_PAIR ||
		   request->kind != FPLINUX_BLUEZ_AGENT_NONE ||
		   (bluetooth->incoming_pairing == INCOMING_PAIRING_VISIBLE &&
		    fplinux_bluez_adapter_visibility(bluetooth->adapter) ==
			    FPLINUX_BLUEZ_VISIBLE_TIMED));
	if (enabled != bluetooth->keep_screen_on &&
	    fplinux_ui_keep_screen_on(bluetooth->ui, enabled))
		bluetooth->keep_screen_on = enabled;
}

static const char *power_value(struct fplinux_settings_bluetooth *bluetooth)
{
	if (!fplinux_bluez_adapter_present(bluetooth->adapter))
		return _("Unavailable");
	return fplinux_bluez_adapter_powered(bluetooth->adapter) ? _("On") :
								   _("Off");
}

static const char *visibility_value(enum fplinux_bluez_visibility visibility)
{
	switch (visibility) {
	case FPLINUX_BLUEZ_VISIBLE_TIMED:
		return _("2 minutes");
	case FPLINUX_BLUEZ_VISIBLE_ALWAYS:
		return _("Always visible");
	default:
		return _("Hidden");
	}
}

static void notice_closed(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int id,
			  void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;

	(void)ui;
	(void)id;
	forget_screen(bluetooth, screen);
}

static void show_notice(struct fplinux_settings_bluetooth *bluetooth,
			const char *title, const char *text, bool error)
{
	if (!can_answer(bluetooth))
		return;
	close_screen(bluetooth, &bluetooth->notice);
	bluetooth->notice = fplinux_ui_alert_new(
		bluetooth->ui,
		error ? FPLINUX_UI_ALERT_ERROR : FPLINUX_UI_ALERT_INFO, title,
		text, 0);
	if (!bluetooth->notice)
		return;
	add_command(bluetooth, bluetooth->notice, COMMAND_BACK,
		    FPLINUX_UI_COMMAND_BACK, _("Close"), 0);
	fplinux_ui_screen_set_listener(bluetooth->notice, notice_closed, NULL,
				       bluetooth);
	push_screen(bluetooth, bluetooth->notice);
}

static const char *bluetooth_error_text(int error)
{
	switch (error) {
	case ENODEV:
		return _("Bluetooth is unavailable.");
	case ENETDOWN:
		return _("Turn Bluetooth on first.");
	case EBUSY:
		return _("Another Bluetooth operation is in progress.");
	case EACCES:
	case EPERM:
		return _("The Bluetooth request was refused.");
	case EOPNOTSUPP:
		return _("This Bluetooth service is unavailable.");
	case ETIMEDOUT:
		return _("The device did not respond. Try again.");
	case ENOMEM:
		return _("Not enough memory.");
	default:
		return _("Bluetooth operation failed. Try again.");
	}
}

static void show_error(struct fplinux_settings_bluetooth *bluetooth, int error)
{
	show_notice(bluetooth, _("Bluetooth"), bluetooth_error_text(error),
		    true);
}

static const char *
transfer_problem(const struct fplinux_bluetooth_opp_result *result)
{
	if (!result->error_number ||
	    result->outcome == FPLINUX_BLUETOOTH_OPP_CANCELLED)
		return "";
	return bluetooth_error_text(result->error_number);
}

static void release_discovery(struct fplinux_settings_bluetooth *bluetooth)
{
	if (!fplinux_bluez_adapter_discovery_owned(bluetooth->adapter) &&
	    bluetooth->mutation != MUTATION_SCAN) {
		bluetooth->stop_discovery = false;
		return;
	}
	if (fplinux_bluez_adapter_pending(bluetooth->adapter)) {
		bluetooth->stop_discovery = true;
		return;
	}
	bluetooth->stop_discovery = false;
	if (fplinux_bluez_adapter_discovery_owned(bluetooth->adapter))
		fplinux_bluez_adapter_set_discovery(bluetooth->adapter, false,
						    REQUEST_TIMEOUT_MS);
}

static void update_answering(struct fplinux_settings_bluetooth *bluetooth)
{
	bool visible = fplinux_bluez_adapter_visibility(bluetooth->adapter) !=
		       FPLINUX_BLUEZ_HIDDEN;
	bool answer = can_answer(bluetooth) && usable(bluetooth);
	bool adding = bluetooth->devices && !bluetooth->paired_only;

	if (!answer || (!visible && bluetooth->incoming_pairing ==
					    INCOMING_PAIRING_VISIBLE)) {
		bluetooth->incoming_pairing = INCOMING_PAIRING_CLOSED;
	} else if (visible &&
		   bluetooth->incoming_pairing == INCOMING_PAIRING_OPENING) {
		bluetooth->incoming_pairing = INCOMING_PAIRING_VISIBLE;
	}
	answer = answer && (adding || bluetooth->incoming_pairing !=
					      INCOMING_PAIRING_CLOSED);
	if (answer != bluetooth->pairing_agent_enabled) {
		bluetooth->pairing_agent_enabled = answer;
		fplinux_bluez_adapter_set_agent_enabled(bluetooth->adapter,
							answer);
	}
	update_screen_hold(bluetooth);
}

static void mutation_done(struct fplinux_ui *ui, void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	enum mutation mutation = bluetooth->mutation;
	bool failed = bluetooth->mutation_failed;
	int error = bluetooth->mutation_error;

	(void)ui;
	bluetooth->wait = NULL;
	bluetooth->mutation = MUTATION_NONE;
	bluetooth->mutation_failed = false;
	if (mutation == MUTATION_VISIBILITY && failed)
		bluetooth->incoming_pairing = INCOMING_PAIRING_CLOSED;
	update_answering(bluetooth);
	if (!failed &&
	    (mutation == MUTATION_NAME || mutation == MUTATION_ALIAS))
		close_screen(bluetooth, &bluetooth->editor);
	root_refresh(bluetooth);
	devices_refresh(bluetooth);
	device_refresh(bluetooth);
	if (!failed || error == ECANCELED)
		return;
	if (mutation == MUTATION_POWER_ON || mutation == MUTATION_POWER_OFF)
		show_notice(bluetooth, _("Bluetooth"),
			    mutation == MUTATION_POWER_ON ?
				    _("Cannot turn Bluetooth on. Try again.") :
				    _("Cannot turn Bluetooth off. Try again."),
			    true);
	else
		show_error(bluetooth, error);
}

static void cancel_pair(struct fplinux_ui *ui, void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;

	(void)ui;
	fplinux_bluez_adapter_cancel_pairing(bluetooth->adapter);
}

static void mutation_begin(struct fplinux_settings_bluetooth *bluetooth,
			   enum mutation mutation, bool queued,
			   const char *message)
{
	bluetooth->mutation = mutation;
	if (!queued) {
		bluetooth->mutation_failed = true;
		bluetooth->mutation_error =
			fplinux_bluez_adapter_error(bluetooth->adapter);
		mutation_done(bluetooth->ui, bluetooth);
		return;
	}
	bluetooth->wait = fplinux_ui_wait_begin(
		bluetooth->ui, _("Bluetooth"), message,
		mutation == MUTATION_PAIR ? cancel_pair : NULL, bluetooth);
	update_screen_hold(bluetooth);
	root_refresh(bluetooth);
}

static void scan(struct fplinux_settings_bluetooth *bluetooth, bool start)
{
	if (!usable(bluetooth) ||
	    fplinux_bluez_adapter_pending(bluetooth->adapter))
		return;
	mutation_begin(bluetooth, MUTATION_SCAN,
		       fplinux_bluez_adapter_set_discovery(
			       bluetooth->adapter, start, REQUEST_TIMEOUT_MS),
		       start ? _("Starting search…") : _("Stopping search…"));
}

static const char *receive_status(struct fplinux_settings_bluetooth *bluetooth)
{
	if (!fplinux_bluez_adapter_present(bluetooth->adapter))
		return _("Unavailable");
	if (!usable(bluetooth))
		return _("Bluetooth is off");
	if (!fplinux_bluez_adapter_connectable(bluetooth->adapter))
		return _("Connections disabled");
	return bluetooth->receive_ready ? _("Ready") : _("Service unavailable");
}

static void root_refresh(struct fplinux_settings_bluetooth *bluetooth)
{
	char nearby[32];
	char paired[32];
	size_t nearby_count = 0;
	size_t paired_count = 0;
	bool present;
	const char *name;

	if (!bluetooth->root)
		return;
	if (!bluetooth->transfer_options &&
	    (bluetooth->transfer_running || bluetooth->record_count)) {
		add_command(bluetooth, bluetooth->root, COMMAND_TRANSFER,
			    FPLINUX_UI_COMMAND_SCREEN, _("File transfer"), 0);
		add_command(bluetooth, bluetooth->root, COMMAND_HISTORY,
			    FPLINUX_UI_COMMAND_SCREEN, _("Transfer history"),
			    0);
		bluetooth->transfer_options = true;
	}
	present = fplinux_bluez_adapter_present(bluetooth->adapter);
	for (size_t i = 0;
	     i < fplinux_bluez_adapter_device_count(bluetooth->adapter); ++i) {
		const struct fplinux_bluez_device *device =
			fplinux_bluez_adapter_device_at(bluetooth->adapter, i);

		if (device->paired)
			++paired_count;
		else
			++nearby_count;
	}
	snprintf(nearby, sizeof(nearby), "%zu", nearby_count);
	snprintf(paired, sizeof(paired), "%zu", paired_count);
	name = fplinux_bluez_adapter_name(bluetooth->adapter);
	fplinux_ui_list_set(bluetooth->root, ROOT_POWER, _("Power"),
			    power_value(bluetooth));
	fplinux_ui_list_set(
		bluetooth->root, ROOT_VISIBILITY, _("Visibility"),
		usable(bluetooth) ?
			visibility_value(fplinux_bluez_adapter_visibility(
				bluetooth->adapter)) :
			_("Unavailable"));
	fplinux_ui_list_set(bluetooth->root, ROOT_NAME, _("Phone name"),
			    present && name && *name ? name : _("Unavailable"));
	fplinux_ui_list_set(bluetooth->root, ROOT_NEARBY, _("Nearby devices"),
			    usable(bluetooth) ? nearby : _("Unavailable"));
	fplinux_ui_list_set(bluetooth->root, ROOT_PAIRED, _("Paired devices"),
			    present ? paired : _("Unavailable"));
	fplinux_ui_list_set(bluetooth->root, ROOT_RECEIVE_STATUS,
			    _("File reception"), receive_status(bluetooth));
	if (fplinux_ui_list_get_focused(bluetooth->root) == ROOT_POWER)
		fplinux_ui_list_set_select_label(
			bluetooth->root,
			!present ? "" :
			fplinux_bluez_adapter_powered(bluetooth->adapter) ?
				   _("Turn off") :
				   _("Turn on"));
	else if (fplinux_ui_list_get_focused(bluetooth->root) ==
		 ROOT_RECEIVE_STATUS)
		fplinux_ui_list_set_select_label(bluetooth->root, "");
	else
		fplinux_ui_list_set_select_label(bluetooth->root, _("Open"));
}

static void root_focused(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			 size_t row, void *context)
{
	(void)ui;
	(void)list;
	(void)row;
	root_refresh(context);
}

static bool matches_list(const struct fplinux_settings_bluetooth *bluetooth,
			 const struct fplinux_bluez_device *device)
{
	return device->paired == bluetooth->paired_only;
}

static void devices_refresh(struct fplinux_settings_bluetooth *bluetooth)
{
	char detail[384];
	bool scanning;

	if (!bluetooth->devices)
		return;
	if (bluetooth->devices_empty) {
		fplinux_ui_list_delete_all(bluetooth->devices);
		bluetooth->devices_empty = false;
	}
	for (size_t i = 0; i < bluetooth->row_count;) {
		const struct fplinux_bluez_device *device =
			fplinux_bluez_adapter_device(bluetooth->adapter,
						     bluetooth->row_paths[i]);

		if (device && matches_list(bluetooth, device)) {
			++i;
			continue;
		}
		fplinux_ui_list_delete(bluetooth->devices, i);
		--bluetooth->row_count;
		memmove(&bluetooth->row_paths[i], &bluetooth->row_paths[i + 1],
			(bluetooth->row_count - i) *
				sizeof(bluetooth->row_paths[0]));
	}
	for (size_t i = 0;
	     i < fplinux_bluez_adapter_device_count(bluetooth->adapter); ++i) {
		const struct fplinux_bluez_device *device =
			fplinux_bluez_adapter_device_at(bluetooth->adapter, i);
		size_t row;

		if (!matches_list(bluetooth, device))
			continue;
		for (row = 0; row < bluetooth->row_count; ++row)
			if (!strcmp(bluetooth->row_paths[row], device->path))
				break;
		if (row == bluetooth->row_count) {
			if (row == ARRAY_SIZE(bluetooth->row_paths))
				continue;
			snprintf(bluetooth->row_paths[row],
				 sizeof(bluetooth->row_paths[row]), "%s",
				 device->path);
			fplinux_ui_list_append(bluetooth->devices,
					       device_name(device), NULL);
			set_glyph(bluetooth, bluetooth->devices, row,
				  "bluetooth");
			++bluetooth->row_count;
		}
		snprintf(detail, sizeof(detail), "%s · %s", device->address,
			 device->connected ? _("Connected") :
			 device->paired	   ? _("Paired") :
					     _("Not paired"));
		fplinux_ui_list_set(bluetooth->devices, row,
				    device_name(device), detail);
	}
	scanning = fplinux_bluez_adapter_discovery_owned(bluetooth->adapter);
	fplinux_ui_screen_set_header_status(bluetooth->devices,
					    scanning ? _("Searching…") : NULL,
					    scanning ? _("Search") : NULL);
	fplinux_ui_screen_remove_command(bluetooth->devices, COMMAND_SEARCH);
	if (!bluetooth->paired_only)
		add_command(bluetooth, bluetooth->devices, COMMAND_SEARCH,
			    scanning ? FPLINUX_UI_COMMAND_STOP :
				       FPLINUX_UI_COMMAND_SCREEN,
			    scanning ? NULL : _("Search"), 0);
	if (!bluetooth->row_count) {
		fplinux_ui_list_append(
			bluetooth->devices,
			!usable(bluetooth)     ? _("Bluetooth is off") :
			bluetooth->paired_only ? _("No paired devices") :
						 _("No nearby devices"),
			bluetooth->paired_only ?
				NULL :
			usable(bluetooth) ?
				_("Choose Options, then Search to find devices.") :
				_("Turn Bluetooth on to search."));
		bluetooth->devices_empty = true;
	}
}

static size_t device_append(struct fplinux_settings_bluetooth *bluetooth,
			    enum device_row kind, const char *label,
			    const char *value, const char *glyph)
{
	size_t row = fplinux_ui_list_append(bluetooth->device, label, value);

	if (row != SIZE_MAX) {
		bluetooth->device_rows[bluetooth->device_row_count++] = kind;
		if (glyph)
			set_glyph(bluetooth, bluetooth->device, row, glyph);
	}
	return row;
}

static void device_refresh(struct fplinux_settings_bluetooth *bluetooth)
{
	const struct fplinux_bluez_device *device;
	size_t focus;
	size_t first;
	enum device_row focused = DEVICE_ADDRESS;
	char value[48];

	if (!bluetooth->device)
		return;
	focus = fplinux_ui_list_get_focused(bluetooth->device);
	first = fplinux_ui_list_get_first_visible(bluetooth->device);
	if (focus < bluetooth->device_row_count)
		focused = bluetooth->device_rows[focus];
	fplinux_ui_list_delete_all(bluetooth->device);
	bluetooth->device_row_count = 0;
	device = fplinux_bluez_adapter_device(bluetooth->adapter,
					      bluetooth->device_path);
	if (!device) {
		device_append(bluetooth, DEVICE_STATUS, _("Device unavailable"),
			      _("The device is no longer known to Bluetooth."),
			      "bluetooth");
		return;
	}
	fplinux_ui_screen_set_title(bluetooth->device, device_name(device));
	device_append(bluetooth, DEVICE_ADDRESS, _("Address"), device->address,
		      "bluetooth");
	device_append(bluetooth, DEVICE_STATUS, _("Status"),
		      device->connected ? _("Connected") :
		      device->paired	? _("Paired") :
					  _("Not paired"),
		      "about");
	if (device->rssi_known) {
		snprintf(value, sizeof(value), _("%d dBm"), device->rssi);
		device_append(bluetooth, DEVICE_SIGNAL, _("Signal"), value,
			      "wifi");
	}
	if (device->battery_known) {
		snprintf(value, sizeof(value), "%u%%", device->battery);
		device_append(bluetooth, DEVICE_BATTERY, _("Battery"), value,
			      "about");
	}
	snprintf(value, sizeof(value), "%zu", device->uuid_count);
	device_append(bluetooth, DEVICE_SERVICES, _("Services"),
		      device->services_resolved ? value : _("Not resolved"),
		      "list");
	if (!device->paired)
		device_append(bluetooth, DEVICE_PAIR, _("Pair"), NULL, "add");
	device_append(bluetooth, DEVICE_CONNECTION,
		      device->connected ? _("Disconnect") : _("Connect"), NULL,
		      device->connected ? "cancel" : "bluetooth");
	device_append(bluetooth, DEVICE_TRUST, _("Trusted"),
		      device->trusted ? _("Yes") : _("No"), "check");
	device_append(bluetooth, DEVICE_RENAME, _("Rename device"), NULL,
		      "edit");
	device_append(bluetooth, DEVICE_FORGET, _("Forget device"), NULL,
		      "delete");
	for (size_t i = 0; i < bluetooth->device_row_count; ++i)
		if (bluetooth->device_rows[i] == focused) {
			fplinux_ui_list_set_focused(bluetooth->device, i);
			break;
		}
	if (first != SIZE_MAX)
		fplinux_ui_list_set_first_visible(bluetooth->device, first);
}

static const char *service_name(const char *uuid)
{
	if (!strcasecmp(uuid, "00001105-0000-1000-8000-00805f9b34fb"))
		return _("Object Push");
	if (!strcasecmp(uuid, "0000110a-0000-1000-8000-00805f9b34fb"))
		return _("Audio source");
	if (!strcasecmp(uuid, "0000110b-0000-1000-8000-00805f9b34fb"))
		return _("Audio sink");
	if (!strcasecmp(uuid, "0000111e-0000-1000-8000-00805f9b34fb"))
		return _("Hands-free");
	if (!strcasecmp(uuid, "00001124-0000-1000-8000-00805f9b34fb"))
		return _("Input device");
	if (!strcasecmp(uuid, "0000180f-0000-1000-8000-00805f9b34fb"))
		return _("Battery service");
	return _("Bluetooth service");
}

static bool service_connectable(const char *uuid)
{
	static const char *const profiles[] = {
		"00001108-0000-1000-8000-00805f9b34fb",
		"0000110a-0000-1000-8000-00805f9b34fb",
		"0000110b-0000-1000-8000-00805f9b34fb",
		"00001112-0000-1000-8000-00805f9b34fb",
		"0000111e-0000-1000-8000-00805f9b34fb",
		"0000111f-0000-1000-8000-00805f9b34fb",
		"00001124-0000-1000-8000-00805f9b34fb",
	};

	for (size_t i = 0; i < ARRAY_SIZE(profiles); ++i)
		if (!strcasecmp(uuid, profiles[i]))
			return true;
	return false;
}

static void services_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *list, int id,
			     void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	size_t row = fplinux_ui_screen_get_command_item(list);

	(void)ui;
	if (id == COMMAND_BACK) {
		close_screen(bluetooth, &bluetooth->services);
		return;
	}
	if (row == SIZE_MAX)
		row = fplinux_ui_list_get_focused(list);
	if (id != COMMAND_CONNECT_PROFILE || row >= bluetooth->service_count ||
	    !service_connectable(bluetooth->service_uuids[row]) ||
	    !usable(bluetooth) ||
	    fplinux_bluez_adapter_pending(bluetooth->adapter))
		return;
	mutation_begin(bluetooth, MUTATION_CONNECT,
		       fplinux_bluez_adapter_connect_profile(
			       bluetooth->adapter, bluetooth->device_path,
			       bluetooth->service_uuids[row],
			       REQUEST_TIMEOUT_MS),
		       _("Connecting service…"));
}

static void open_services(struct fplinux_settings_bluetooth *bluetooth)
{
	const struct fplinux_bluez_device *device =
		fplinux_bluez_adapter_device(bluetooth->adapter,
					     bluetooth->device_path);

	if (!device)
		return;
	bluetooth->services = fplinux_ui_list_new(bluetooth->ui, _("Services"),
						  FPLINUX_UI_LIST_IMPLICIT);
	if (!bluetooth->services)
		return;
	bluetooth->service_count = device->uuid_count;
	for (size_t i = 0; i < device->uuid_count; ++i) {
		snprintf(bluetooth->service_uuids[i],
			 sizeof(bluetooth->service_uuids[i]), "%s",
			 device->uuids[i]);
		fplinux_ui_list_append(bluetooth->services,
				       service_name(device->uuids[i]),
				       device->uuids[i]);
		set_glyph(bluetooth, bluetooth->services, i, "bluetooth");
		if (service_connectable(device->uuids[i])) {
			const struct fplinux_ui_command connect = {
				.id = COMMAND_CONNECT_PROFILE,
				.type = FPLINUX_UI_COMMAND_OK,
				.label = _("Connect"),
				.icon = fplinux_ui_glyph(bluetooth->ui,
							 "bluetooth",
							 FPLINUX_UI_GLYPH_ROW),
			};

			fplinux_ui_list_add_item_command(bluetooth->services, i,
							 &connect);
		}
	}
	if (!device->uuid_count)
		fplinux_ui_list_append(
			bluetooth->services, _("No services reported"),
			_("Connect the device to resolve services."));
	fplinux_ui_list_set_select_label(bluetooth->services, "");
	add_back(bluetooth, bluetooth->services);
	fplinux_ui_screen_set_listener(bluetooth->services, services_command,
				       NULL, bluetooth);
	push_screen(bluetooth, bluetooth->services);
}

static void editor_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	char name[FPLINUX_BLUEZ_NAME_BYTES];
	const char *text;
	bool queued;

	(void)ui;
	if (id == COMMAND_BACK) {
		close_screen(bluetooth, &bluetooth->editor);
		return;
	}
	if (id != COMMAND_SAVE && id != COMMAND_RESET_NAME)
		return;
	text = id == COMMAND_RESET_NAME ? "" :
					  fplinux_ui_form_get_text(screen, 0);
	if (!text)
		return;
	if (strlen(text) >= sizeof(name)) {
		show_notice(bluetooth, _("Name"), _("The name is too long."),
			    true);
		return;
	}
	snprintf(name, sizeof(name), "%s", text);
	if (!*name && id != COMMAND_RESET_NAME) {
		show_notice(bluetooth, _("Name"), _("Enter a name."), true);
		return;
	}
	if (fplinux_bluez_adapter_pending(bluetooth->adapter))
		return;
	queued = bluetooth->editor_kind == EDITOR_NAME ?
			 fplinux_bluez_adapter_set_name(
				 bluetooth->adapter, name, REQUEST_TIMEOUT_MS) :
			 fplinux_bluez_adapter_set_alias(bluetooth->adapter,
							 bluetooth->device_path,
							 name,
							 REQUEST_TIMEOUT_MS);
	mutation_begin(bluetooth,
		       bluetooth->editor_kind == EDITOR_NAME ? MUTATION_NAME :
							       MUTATION_ALIAS,
		       queued, _("Saving name…"));
}

static void open_editor(struct fplinux_settings_bluetooth *bluetooth,
			enum editor_kind kind)
{
	const struct fplinux_bluez_device *device;
	const char *text;

	if (kind == EDITOR_ALIAS) {
		device = fplinux_bluez_adapter_device(bluetooth->adapter,
						      bluetooth->device_path);
		if (!device)
			return;
		text = device_name(device);
	} else {
		text = fplinux_bluez_adapter_name(bluetooth->adapter);
	}
	bluetooth->editor_kind = kind;
	bluetooth->editor = fplinux_ui_form_new(
		bluetooth->ui,
		kind == EDITOR_NAME ? _("Phone name") : _("Rename device"));
	if (!bluetooth->editor)
		return;
	fplinux_ui_form_append_text_field(bluetooth->editor, _("Name"),
					  text ? text : "",
					  FPLINUX_BLUEZ_NAME_BYTES - 1,
					  FPLINUX_UI_TEXT_ANY);
	add_back(bluetooth, bluetooth->editor);
	add_action_row(bluetooth->editor, COMMAND_SAVE, _("Save"));
	if (kind == EDITOR_NAME)
		add_command(bluetooth, bluetooth->editor, COMMAND_RESET_NAME,
			    FPLINUX_UI_COMMAND_SCREEN, _("Use model name"), 0);
	fplinux_ui_screen_set_listener(bluetooth->editor, editor_command, NULL,
				       bluetooth);
	push_screen(bluetooth, bluetooth->editor);
}

static void forget_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;

	(void)ui;
	forget_screen(bluetooth, screen);
	if (id == COMMAND_FORGET)
		mutation_begin(bluetooth, MUTATION_FORGET,
			       fplinux_bluez_adapter_remove_device(
				       bluetooth->adapter,
				       bluetooth->device_path,
				       REQUEST_TIMEOUT_MS),
			       _("Forgetting device…"));
}

static void confirm_forget(struct fplinux_settings_bluetooth *bluetooth)
{
	bluetooth->confirm = fplinux_ui_alert_new(
		bluetooth->ui, FPLINUX_UI_ALERT_CONFIRMATION,
		_("Forget device"),
		_("Remove this device and its pairing? Pair it again to reconnect."),
		0);
	if (!bluetooth->confirm)
		return;
	add_command(bluetooth, bluetooth->confirm, COMMAND_BACK,
		    FPLINUX_UI_COMMAND_CANCEL, NULL, 0);
	add_command(bluetooth, bluetooth->confirm, COMMAND_FORGET,
		    FPLINUX_UI_COMMAND_OK, _("Forget"), 0);
	fplinux_ui_screen_set_listener(bluetooth->confirm, forget_command, NULL,
				       bluetooth);
	push_screen(bluetooth, bluetooth->confirm);
}

static void device_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *list, int id,
			   void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	const struct fplinux_bluez_device *device;
	size_t row = fplinux_ui_list_get_focused(list);

	(void)ui;
	if (id == COMMAND_BACK) {
		close_screen(bluetooth, &bluetooth->device);
		return;
	}
	device = fplinux_bluez_adapter_device(bluetooth->adapter,
					      bluetooth->device_path);
	if (id != FPLINUX_UI_SELECT_COMMAND || !device ||
	    row >= bluetooth->device_row_count ||
	    fplinux_bluez_adapter_pending(bluetooth->adapter))
		return;
	switch (bluetooth->device_rows[row]) {
	case DEVICE_SERVICES:
		open_services(bluetooth);
		break;
	case DEVICE_PAIR:
		if (!fplinux_bluez_adapter_agent_ready(bluetooth->adapter)) {
			show_notice(bluetooth, _("Pair"),
				    _("Pairing is unavailable. Try again."),
				    true);
			break;
		}
		mutation_begin(bluetooth, MUTATION_PAIR,
			       fplinux_bluez_adapter_pair(
				       bluetooth->adapter,
				       bluetooth->device_path, PAIR_TIMEOUT_MS),
			       _("Pairing…"));
		break;
	case DEVICE_CONNECTION:
		mutation_begin(bluetooth,
			       device->connected ? MUTATION_DISCONNECT :
						   MUTATION_CONNECT,
			       device->connected ?
				       fplinux_bluez_adapter_disconnect_device(
					       bluetooth->adapter,
					       bluetooth->device_path,
					       REQUEST_TIMEOUT_MS) :
				       fplinux_bluez_adapter_connect_device(
					       bluetooth->adapter,
					       bluetooth->device_path,
					       REQUEST_TIMEOUT_MS),
			       device->connected ? _("Disconnecting…") :
						   _("Connecting…"));
		break;
	case DEVICE_TRUST:
		mutation_begin(bluetooth, MUTATION_TRUST,
			       fplinux_bluez_adapter_set_trusted(
				       bluetooth->adapter,
				       bluetooth->device_path, !device->trusted,
				       REQUEST_TIMEOUT_MS),
			       _("Changing trust…"));
		break;
	case DEVICE_RENAME:
		open_editor(bluetooth, EDITOR_ALIAS);
		break;
	case DEVICE_FORGET:
		confirm_forget(bluetooth);
		break;
	default:
		break;
	}
}

static void open_device(struct fplinux_settings_bluetooth *bluetooth,
			const char *path)
{
	snprintf(bluetooth->device_path, sizeof(bluetooth->device_path), "%s",
		 path);
	bluetooth->device = fplinux_ui_list_new(bluetooth->ui, _("Device"),
						FPLINUX_UI_LIST_IMPLICIT);
	if (!bluetooth->device)
		return;
	fplinux_ui_list_set_value_details(bluetooth->device);
	add_back(bluetooth, bluetooth->device);
	fplinux_ui_screen_set_listener(bluetooth->device, device_command, NULL,
				       bluetooth);
	device_refresh(bluetooth);
	push_screen(bluetooth, bluetooth->device);
}

static void devices_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *list, int id,
			    void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	size_t row = fplinux_ui_list_get_focused(list);

	(void)ui;
	if (id == COMMAND_BACK) {
		if (!bluetooth->paired_only)
			release_discovery(bluetooth);
		close_screen(bluetooth, &bluetooth->devices);
		bluetooth->row_count = 0;
		bluetooth->devices_empty = false;
		update_answering(bluetooth);
		return;
	}
	if (id == COMMAND_SEARCH) {
		scan(bluetooth, !fplinux_bluez_adapter_discovery_owned(
					bluetooth->adapter));
		return;
	}
	if (id != FPLINUX_UI_SELECT_COMMAND || row >= bluetooth->row_count)
		return;
	if (bluetooth->peer_selection == PEER_INSPECT)
		open_device(bluetooth, bluetooth->row_paths[row]);
	else
		start_transfer(bluetooth, bluetooth->row_paths[row]);
}

static bool open_devices(struct fplinux_settings_bluetooth *bluetooth,
			 bool paired, enum peer_selection selection)
{
	bluetooth->paired_only = paired;
	bluetooth->peer_selection = selection;
	bluetooth->row_count = 0;
	bluetooth->devices_empty = false;
	bluetooth->devices = fplinux_ui_list_new(bluetooth->ui,
						 selection == PEER_SEND ?
							 _("Send to") :
						 paired ? _("Paired devices") :
							  _("Nearby devices"),
						 FPLINUX_UI_LIST_IMPLICIT);
	if (!bluetooth->devices)
		return false;
	add_back(bluetooth, bluetooth->devices);
	fplinux_ui_screen_set_listener(bluetooth->devices, devices_command,
				       NULL, bluetooth);
	devices_refresh(bluetooth);
	push_screen(bluetooth, bluetooth->devices);
	if (!bluetooth->devices)
		return false;
	update_answering(bluetooth);
	if (!paired && usable(bluetooth) &&
	    !fplinux_bluez_adapter_discovery_owned(bluetooth->adapter))
		scan(bluetooth, true);
	return true;
}

static void visibility_command(struct fplinux_ui *ui,
			       struct fplinux_ui_screen *list, int id,
			       void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	struct fplinux_bluez_adapter *adapter = bluetooth->adapter;
	size_t row = fplinux_ui_list_get_focused(list);
	bool queued;

	(void)ui;
	if (id == COMMAND_BACK) {
		close_screen(bluetooth, &bluetooth->visibility);
		return;
	}
	if (id != FPLINUX_UI_SELECT_COMMAND ||
	    row > FPLINUX_BLUEZ_VISIBLE_ALWAYS)
		return;
	close_screen(bluetooth, &bluetooth->visibility);
	/* BlueZ may leave an unchanged Connectable request pending. */
	if (row == FPLINUX_BLUEZ_HIDDEN && usable(bluetooth) &&
	    fplinux_bluez_adapter_visibility(adapter) == FPLINUX_BLUEZ_HIDDEN &&
	    fplinux_bluez_adapter_connectable(adapter) &&
	    !fplinux_bluez_adapter_pairable(adapter)) {
		bluetooth->incoming_pairing = INCOMING_PAIRING_CLOSED;
		update_answering(bluetooth);
		return;
	}
	queued = fplinux_bluez_adapter_set_visibility(adapter, row,
						      REQUEST_TIMEOUT_MS);
	if (queued) {
		bluetooth->incoming_pairing = row == FPLINUX_BLUEZ_HIDDEN ?
						      INCOMING_PAIRING_CLOSED :
						      INCOMING_PAIRING_OPENING;
		update_answering(bluetooth);
	}
	mutation_begin(bluetooth, MUTATION_VISIBILITY, queued,
		       _("Changing visibility…"));
}

static void visibility_change(struct fplinux_ui *ui,
			      struct fplinux_ui_screen *list, size_t row,
			      void *context)
{
	(void)row;
	visibility_command(ui, list, FPLINUX_UI_SELECT_COMMAND, context);
}

static void open_visibility(struct fplinux_settings_bluetooth *bluetooth)
{
	bluetooth->visibility = fplinux_ui_list_new(
		bluetooth->ui, _("Visibility"), FPLINUX_UI_LIST_EXCLUSIVE);
	if (!bluetooth->visibility)
		return;
	for (int i = FPLINUX_BLUEZ_HIDDEN; i <= FPLINUX_BLUEZ_VISIBLE_ALWAYS;
	     ++i)
		fplinux_ui_list_append(bluetooth->visibility,
				       visibility_value(i), NULL);
	fplinux_ui_list_set_selected(
		bluetooth->visibility,
		fplinux_bluez_adapter_visibility(bluetooth->adapter), true);
	add_back(bluetooth, bluetooth->visibility);
	fplinux_ui_screen_set_listener(bluetooth->visibility,
				       visibility_command, visibility_change,
				       bluetooth);
	push_screen(bluetooth, bluetooth->visibility);
}

static void agent_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int id,
			  void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	const struct fplinux_bluez_agent_request *request =
		fplinux_bluez_adapter_agent_request(bluetooth->adapter);
	bool allow = id == COMMAND_ACCEPT;
	char code[17];
	uint64_t serial = bluetooth->agent_serial;
	bool alert = bluetooth->agent_is_alert;

	(void)ui;
	code[0] = '\0';
	if (allow && (bluetooth->agent_kind == FPLINUX_BLUEZ_AGENT_PIN ||
		      bluetooth->agent_kind == FPLINUX_BLUEZ_AGENT_PASSKEY)) {
		const char *text =
			fplinux_ui_form_get_text(screen, bluetooth->agent_code);

		if (text && strlen(text) >= sizeof(code)) {
			show_notice(bluetooth, _("Pairing"),
				    _("The code is too long."), true);
			return;
		}
		snprintf(code, sizeof(code), "%s", text ? text : "");
		if (!*code) {
			show_notice(bluetooth, _("Pairing"),
				    _("Enter the code."), true);
			return;
		}
	}
	if (alert)
		forget_screen(bluetooth, screen);
	else
		close_screen(bluetooth, &bluetooth->agent);
	bluetooth->agent_serial = 0;
	if (!request || request->serial != serial)
		return;
	if (bluetooth->agent_kind == FPLINUX_BLUEZ_AGENT_DISPLAY_PIN ||
	    bluetooth->agent_kind == FPLINUX_BLUEZ_AGENT_DISPLAY_PASSKEY) {
		fplinux_bluez_adapter_cancel_pairing(bluetooth->adapter);
	} else if (!fplinux_bluez_adapter_agent_reply(bluetooth->adapter,
						      serial, allow, code)) {
		agent_refresh(bluetooth);
		show_error(bluetooth, EINVAL);
	}
}

static void agent_refresh(struct fplinux_settings_bluetooth *bluetooth)
{
	const struct fplinux_bluez_agent_request *request =
		fplinux_bluez_adapter_agent_request(bluetooth->adapter);
	const struct fplinux_bluez_device *device;
	char message[768];
	char code[64];
	const char *name;

	if (!can_answer(bluetooth) || !request ||
	    !fplinux_bluez_adapter_device(bluetooth->adapter,
					  request->device)) {
		close_screen(bluetooth, &bluetooth->agent);
		bluetooth->agent_serial = 0;
		return;
	}
	if (bluetooth->agent_serial == request->serial && bluetooth->agent) {
		if (request->kind == FPLINUX_BLUEZ_AGENT_DISPLAY_PASSKEY) {
			snprintf(code, sizeof(code), _("%06u · %u entered"),
				 request->passkey, request->entered);
			fplinux_ui_form_set_string(bluetooth->agent, 1, code);
		}
		return;
	}
	close_screen(bluetooth, &bluetooth->agent);
	device = fplinux_bluez_adapter_device(bluetooth->adapter,
					      request->device);
	name = device_name(device);
	bluetooth->agent_serial = request->serial;
	bluetooth->agent_kind = request->kind;
	bluetooth->agent_is_alert = false;
	if (request->kind == FPLINUX_BLUEZ_AGENT_PIN ||
	    request->kind == FPLINUX_BLUEZ_AGENT_PASSKEY) {
		bluetooth->agent =
			fplinux_ui_form_new(bluetooth->ui, _("Pairing"));
		if (!bluetooth->agent)
			return;
		fplinux_ui_form_append_property(bluetooth->agent, _("Device"),
						name);
		bluetooth->agent_code = fplinux_ui_form_append_text_field(
			bluetooth->agent,
			request->kind == FPLINUX_BLUEZ_AGENT_PIN ? _("PIN") :
								   _("Passkey"),
			"", request->kind == FPLINUX_BLUEZ_AGENT_PIN ? 16 : 6,
			request->kind == FPLINUX_BLUEZ_AGENT_PIN ?
				FPLINUX_UI_TEXT_ANY :
				FPLINUX_UI_TEXT_NUMERIC);
		fplinux_ui_form_set_focus(bluetooth->agent,
					  bluetooth->agent_code);
		add_action_row(bluetooth->agent, COMMAND_ACCEPT, _("Pair"));
	} else if (request->kind == FPLINUX_BLUEZ_AGENT_DISPLAY_PIN ||
		   request->kind == FPLINUX_BLUEZ_AGENT_DISPLAY_PASSKEY) {
		bluetooth->agent =
			fplinux_ui_form_new(bluetooth->ui, _("Pairing"));
		if (!bluetooth->agent)
			return;
		fplinux_ui_form_append_property(bluetooth->agent, _("Device"),
						name);
		if (request->kind == FPLINUX_BLUEZ_AGENT_DISPLAY_PIN)
			snprintf(code, sizeof(code), "%s", request->pin);
		else
			snprintf(code, sizeof(code), _("%06u · %u entered"),
				 request->passkey, request->entered);
		fplinux_ui_form_append_property(
			bluetooth->agent, _("Enter on the other device"), code);
	} else {
		if (request->kind == FPLINUX_BLUEZ_AGENT_CONFIRMATION)
			snprintf(message, sizeof(message),
				 _("Does this code match on %s?\n%06u"), name,
				 request->passkey);
		else if (request->kind == FPLINUX_BLUEZ_AGENT_SERVICE)
			snprintf(message, sizeof(message),
				 _("Allow %s to use this service?\n%s"), name,
				 service_name(request->uuid));
		else
			snprintf(message, sizeof(message),
				 _("Allow pairing with %s?"), name);
		bluetooth->agent = fplinux_ui_alert_new(
			bluetooth->ui, FPLINUX_UI_ALERT_CONFIRMATION,
			_("Pairing"), message, 0);
		bluetooth->agent_is_alert = true;
		if (!bluetooth->agent)
			return;
		add_command(bluetooth, bluetooth->agent, COMMAND_ACCEPT,
			    FPLINUX_UI_COMMAND_OK, _("Accept"), 0);
	}
	add_command(bluetooth, bluetooth->agent, COMMAND_REJECT,
		    FPLINUX_UI_COMMAND_CANCEL, _("Reject"), 0);
	fplinux_ui_screen_set_listener(bluetooth->agent, agent_command, NULL,
				       bluetooth);
	push_screen(bluetooth, bluetooth->agent);
}

static void notify_transfer(struct fplinux_settings_bluetooth *bluetooth)
{
	uint64_t value = 1;
	ssize_t bytes;

	do {
		bytes = write(bluetooth->event_fd, &value, sizeof(value));
	} while (bytes < 0 && errno == EINTR);
}

static bool transfer_cancelled(void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;

	return atomic_load(&bluetooth->cancelled);
}

static void
transfer_progress(const struct fplinux_bluetooth_opp_progress *progress,
		  void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;

	pthread_mutex_lock(&bluetooth->mutex);
	bluetooth->shared.state = progress->state;
	bluetooth->shared.transferred_bytes = progress->transferred_bytes;
	bluetooth->shared.total_bytes = progress->total_bytes;
	bluetooth->shared.total_known = progress->total_known;
	pthread_mutex_unlock(&bluetooth->mutex);
	notify_transfer(bluetooth);
}

static void *transfer_worker(void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	const struct fplinux_bluetooth_opp_callbacks callbacks = {
		.cancelled = transfer_cancelled,
		.progress = transfer_progress,
		.data = bluetooth,
	};
	struct fplinux_bluetooth_opp_result result = { 0 };

	fplinux_bluetooth_opp_send(bluetooth->transfer_peer,
				   bluetooth->transfer_path, &callbacks,
				   &result);
	pthread_mutex_lock(&bluetooth->mutex);
	bluetooth->shared.result = result;
	bluetooth->shared.done = true;
	pthread_mutex_unlock(&bluetooth->mutex);
	notify_transfer(bluetooth);
	return NULL;
}

static const char *transfer_state(enum fplinux_bluetooth_opp_state state)
{
	switch (state) {
	case FPLINUX_BLUETOOTH_OPP_CONNECTING:
		return _("Connecting…");
	case FPLINUX_BLUETOOTH_OPP_TRANSFERRING:
		return _("Transferring…");
	case FPLINUX_BLUETOOTH_OPP_COMPLETE:
		return _("Finishing…");
	default:
		return "";
	}
}

static const char *outcome_name(enum fplinux_bluetooth_opp_outcome outcome)
{
	return outcome == FPLINUX_BLUETOOTH_OPP_SUCCEEDED ? _("Complete") :
	       outcome == FPLINUX_BLUETOOTH_OPP_CANCELLED ? _("Cancelled") :
							    _("Failed");
}

static void transfer_refresh(struct fplinux_settings_bluetooth *bluetooth)
{
	struct transfer_snapshot snapshot;
	char bytes[96];
	const char *message;

	pthread_mutex_lock(&bluetooth->mutex);
	snapshot = bluetooth->shared;
	pthread_mutex_unlock(&bluetooth->mutex);
	if (bluetooth->transfer) {
		fplinux_ui_form_set_string(bluetooth->transfer, 0,
					   bluetooth->transfer_peer_name);
		fplinux_ui_form_set_string(bluetooth->transfer, 1,
					   snapshot.filename);
		fplinux_ui_form_set_hidden(bluetooth->transfer, 1,
					   !*snapshot.filename);
		fplinux_ui_form_set_string(
			bluetooth->transfer, 2,
			bluetooth->transfer_running ?
				transfer_state(snapshot.state) :
				outcome_name(snapshot.result.outcome));
		if (snapshot.total_known)
			snprintf(bytes, sizeof(bytes), _("%llu / %llu bytes"),
				 (unsigned long long)snapshot.transferred_bytes,
				 (unsigned long long)snapshot.total_bytes);
		else
			snprintf(
				bytes, sizeof(bytes),
				_("%llu bytes · total unknown"),
				(unsigned long long)snapshot.transferred_bytes);
		fplinux_ui_form_set_string(bluetooth->transfer, 3, bytes);
		fplinux_ui_form_set_hidden(bluetooth->transfer, 4,
					   !snapshot.total_known ||
						   !snapshot.total_bytes);
		if (snapshot.total_known && snapshot.total_bytes) {
			int percent = snapshot.transferred_bytes >=
						      snapshot.total_bytes ?
					      100 :
					      (int)((long double)snapshot
							    .transferred_bytes *
						    100 / snapshot.total_bytes);

			fplinux_ui_form_set_value(bluetooth->transfer, 4,
						  percent);
		}
		message = bluetooth->transfer_running ?
				  "" :
				  transfer_problem(&snapshot.result);
		fplinux_ui_form_set_string(bluetooth->transfer, 5, message);
		fplinux_ui_form_set_hidden(bluetooth->transfer, 5, !*message);
		fplinux_ui_screen_remove_command(bluetooth->transfer,
						 COMMAND_CANCEL);
		if (bluetooth->transfer_running)
			add_command(bluetooth, bluetooth->transfer,
				    COMMAND_CANCEL, FPLINUX_UI_COMMAND_STOP,
				    NULL, 0);
	}
	update_screen_hold(bluetooth);
	root_refresh(bluetooth);
}

static void transfer_event(struct fplinux_ui *ui, int fd, short events,
			   void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	struct transfer_snapshot snapshot;
	uint64_t value;
	ssize_t bytes;

	(void)ui;
	if (!(events & POLLIN))
		return;
	do {
		bytes = read(fd, &value, sizeof(value));
	} while (bytes == (ssize_t)sizeof(value) ||
		 (bytes < 0 && errno == EINTR));
	pthread_mutex_lock(&bluetooth->mutex);
	snapshot = bluetooth->shared;
	pthread_mutex_unlock(&bluetooth->mutex);
	if (snapshot.done && bluetooth->transfer_running) {
		struct transfer_history *record;

		pthread_join(bluetooth->thread, NULL);
		bluetooth->transfer_running = false;
		if (bluetooth->record_count == HISTORY_COUNT)
			--bluetooth->record_count;
		memmove(&bluetooth->records[1], &bluetooth->records[0],
			bluetooth->record_count *
				sizeof(bluetooth->records[0]));
		++bluetooth->record_count;
		record = &bluetooth->records[0];
		snprintf(record->peer, sizeof(record->peer), "%s",
			 bluetooth->transfer_peer_name);
		snprintf(record->filename, sizeof(record->filename), "%s",
			 snapshot.filename);
		record->result = snapshot.result;
		history_refresh(bluetooth, true);
		if (bluetooth->close_pending || bluetooth->close_question) {
			bluetooth->close_pending = false;
			bluetooth->close_question = false;
			fplinux_ui_close_reply(bluetooth->ui,
					       FPLINUX_UI_CLOSE_READY);
			return;
		}
	}
	transfer_refresh(bluetooth);
}

static void cancel_transfer(struct fplinux_settings_bluetooth *bluetooth)
{
	atomic_store(&bluetooth->cancelled, true);
}

static void transfer_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *screen, int id,
			     void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;

	(void)ui;
	(void)screen;
	if (id == COMMAND_BACK)
		close_screen(bluetooth, &bluetooth->transfer);
	else if (id == COMMAND_CANCEL)
		cancel_transfer(bluetooth);
}

static void open_transfer(struct fplinux_settings_bluetooth *bluetooth)
{
	if (!bluetooth->transfer_running && !bluetooth->record_count) {
		show_notice(bluetooth, _("File transfer"),
			    _("No file transfers in this session."), false);
		return;
	}
	if (bluetooth->transfer)
		return;
	bluetooth->transfer =
		fplinux_ui_form_new(bluetooth->ui, _("File transfer"));
	if (!bluetooth->transfer)
		return;
	fplinux_ui_form_append_property(bluetooth->transfer, _("Device"), "");
	fplinux_ui_form_append_property(bluetooth->transfer, _("File"), "");
	fplinux_ui_form_append_property(bluetooth->transfer, _("Status"), "");
	fplinux_ui_form_append_property(bluetooth->transfer, _("Transferred"),
					"");
	fplinux_ui_form_append_gauge(bluetooth->transfer, NULL, 0, 100, 0,
				     false);
	fplinux_ui_form_append_string(bluetooth->transfer, NULL, "");
	add_back(bluetooth, bluetooth->transfer);
	fplinux_ui_screen_set_listener(bluetooth->transfer, transfer_command,
				       NULL, bluetooth);
	push_screen(bluetooth, bluetooth->transfer);
	transfer_refresh(bluetooth);
}

static void start_transfer(struct fplinux_settings_bluetooth *bluetooth,
			   const char *device_path)
{
	const struct fplinux_bluez_device *device =
		fplinux_bluez_adapter_device(bluetooth->adapter, device_path);
	int error;
	const char *filename;

	if (!device || !device->paired || !usable(bluetooth)) {
		show_error(bluetooth, ENODEV);
		return;
	}
	if (bluetooth->transfer_running) {
		open_transfer(bluetooth);
		return;
	}
	snprintf(bluetooth->transfer_peer, sizeof(bluetooth->transfer_peer),
		 "%s", device->address);
	snprintf(bluetooth->transfer_peer_name,
		 sizeof(bluetooth->transfer_peer_name), "%s",
		 device_name(device));
	pthread_mutex_lock(&bluetooth->mutex);
	memset(&bluetooth->shared, 0, sizeof(bluetooth->shared));
	bluetooth->shared.state = FPLINUX_BLUETOOTH_OPP_CONNECTING;
	filename = strrchr(bluetooth->transfer_path, '/');
	snprintf(bluetooth->shared.filename, sizeof(bluetooth->shared.filename),
		 "%s", filename + 1);
	pthread_mutex_unlock(&bluetooth->mutex);
	atomic_store(&bluetooth->cancelled, false);
	error = pthread_create(&bluetooth->thread, NULL, transfer_worker,
			       bluetooth);
	if (error) {
		show_error(bluetooth, error);
		return;
	}
	bluetooth->transfer_running = true;
	close_screen(bluetooth, &bluetooth->devices);
	open_transfer(bluetooth);
}

static void history_detail_command(struct fplinux_ui *ui,
				   struct fplinux_ui_screen *screen, int id,
				   void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;

	(void)ui;
	(void)screen;
	if (id == COMMAND_BACK)
		close_screen(bluetooth, &bluetooth->history_detail);
}

static void history_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *list, int id,
			    void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	const struct transfer_history *record;
	size_t row = fplinux_ui_list_get_focused(list);

	(void)ui;
	if (id == COMMAND_BACK) {
		close_screen(bluetooth, &bluetooth->history);
		return;
	}
	if (id != FPLINUX_UI_SELECT_COMMAND || row >= bluetooth->record_count)
		return;
	record = &bluetooth->records[row];
	bluetooth->history_detail =
		fplinux_ui_form_new(bluetooth->ui, _("Transfer details"));
	if (!bluetooth->history_detail)
		return;
	fplinux_ui_form_append_property(bluetooth->history_detail, _("Device"),
					record->peer);
	fplinux_ui_form_append_property(bluetooth->history_detail, _("File"),
					record->filename);
	fplinux_ui_form_append_property(bluetooth->history_detail, _("Status"),
					outcome_name(record->result.outcome));
	if (*transfer_problem(&record->result))
		fplinux_ui_form_append_string(
			bluetooth->history_detail, NULL,
			transfer_problem(&record->result));
	add_back(bluetooth, bluetooth->history_detail);
	fplinux_ui_screen_set_listener(bluetooth->history_detail,
				       history_detail_command, NULL, bluetooth);
	push_screen(bluetooth, bluetooth->history_detail);
}

static void history_refresh(struct fplinux_settings_bluetooth *bluetooth,
			    bool added)
{
	char detail[384];
	size_t focus;
	size_t first;

	if (!bluetooth->history)
		return;
	focus = fplinux_ui_list_get_focused(bluetooth->history);
	first = fplinux_ui_list_get_first_visible(bluetooth->history);
	if (added && bluetooth->record_count > 1) {
		if (focus != SIZE_MAX)
			++focus;
		if (first != SIZE_MAX)
			++first;
	}
	fplinux_ui_list_delete_all(bluetooth->history);
	for (size_t i = 0; i < bluetooth->record_count; ++i) {
		const struct transfer_history *record = &bluetooth->records[i];

		snprintf(detail, sizeof(detail), "%s · %s", record->peer,
			 outcome_name(record->result.outcome));
		fplinux_ui_list_append(bluetooth->history,
				       *record->filename ? record->filename :
							   _("Send file"),
				       detail);
		set_glyph(bluetooth, bluetooth->history, i, "send");
	}
	if (!bluetooth->record_count)
		fplinux_ui_list_append(bluetooth->history,
				       _("No file transfers in this session."),
				       NULL);
	if (focus != SIZE_MAX && bluetooth->record_count) {
		if (focus >= bluetooth->record_count)
			focus = bluetooth->record_count - 1;
		fplinux_ui_list_set_focused(bluetooth->history, focus);
	}
	if (first != SIZE_MAX)
		fplinux_ui_list_set_first_visible(bluetooth->history, first);
}

static void open_history(struct fplinux_settings_bluetooth *bluetooth)
{
	bluetooth->history = fplinux_ui_list_new(
		bluetooth->ui, _("Transfer history"), FPLINUX_UI_LIST_IMPLICIT);
	if (!bluetooth->history)
		return;
	history_refresh(bluetooth, false);
	add_back(bluetooth, bluetooth->history);
	fplinux_ui_screen_set_listener(bluetooth->history, history_command,
				       NULL, bluetooth);
	push_screen(bluetooth, bluetooth->history);
}

static void root_command(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
			 int id, void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;
	size_t row = fplinux_ui_list_get_focused(list);
	bool powered;

	(void)ui;
	if (id == COMMAND_BACK) {
		fplinux_settings_bluetooth_reset(bluetooth);
		return;
	}
	if (id == COMMAND_HISTORY) {
		open_history(bluetooth);
		return;
	}
	if (id == COMMAND_TRANSFER) {
		open_transfer(bluetooth);
		return;
	}
	if (id != FPLINUX_UI_SELECT_COMMAND || row == ROOT_RECEIVE_STATUS)
		return;
	if (!fplinux_bluez_adapter_present(bluetooth->adapter) ||
	    fplinux_bluez_adapter_pending(bluetooth->adapter))
		return;
	if (row == ROOT_POWER) {
		powered = !fplinux_bluez_adapter_powered(bluetooth->adapter);
		mutation_begin(bluetooth,
			       powered ? MUTATION_POWER_ON : MUTATION_POWER_OFF,
			       fplinux_bluez_adapter_set_powered(
				       bluetooth->adapter, powered,
				       REQUEST_TIMEOUT_MS),
			       powered ? _("Turning Bluetooth on…") :
					 _("Turning Bluetooth off…"));
		return;
	}
	if (row == ROOT_NAME)
		open_editor(bluetooth, EDITOR_NAME);
	else if (row == ROOT_PAIRED)
		open_devices(bluetooth, true, PEER_INSPECT);
	else if (!usable(bluetooth))
		show_notice(bluetooth, _("Bluetooth"),
			    _("Turn Bluetooth on first."), false);
	else if (row == ROOT_VISIBILITY)
		open_visibility(bluetooth);
	else if (row == ROOT_NEARBY)
		open_devices(bluetooth, false, PEER_INSPECT);
}

struct fplinux_settings_bluetooth *
fplinux_settings_bluetooth_new(struct fplinux_ui *ui,
			       struct fplinux_bluez_adapter *adapter)
{
	struct fplinux_settings_bluetooth *bluetooth;
	int error;

	if (!ui || !adapter) {
		errno = EINVAL;
		return NULL;
	}
	bluetooth = calloc(1, sizeof(*bluetooth));
	if (!bluetooth)
		return NULL;
	bluetooth->ui = ui;
	bluetooth->adapter = adapter;
	bluetooth->event_fd = -1;
	atomic_init(&bluetooth->cancelled, false);
	error = pthread_mutex_init(&bluetooth->mutex, NULL);
	if (error)
		goto fail;
	bluetooth->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (bluetooth->event_fd < 0) {
		error = errno;
		goto fail_mutex;
	}
	if (!fplinux_ui_watch_fd(ui, bluetooth->event_fd, POLLIN,
				 transfer_event, bluetooth)) {
		error = errno;
		close(bluetooth->event_fd);
		goto fail_mutex;
	}
	bluetooth->visible = fplinux_ui_is_visible(ui);
	return bluetooth;

fail_mutex:
	pthread_mutex_destroy(&bluetooth->mutex);
fail:
	free(bluetooth);
	errno = error;
	return NULL;
}

void fplinux_settings_bluetooth_show(
	struct fplinux_settings_bluetooth *bluetooth)
{
	static const char *const labels[] = {
		"Power",	  "Visibility",	    "Phone name",
		"Nearby devices", "Paired devices", "File reception",
	};
	static const char *const glyphs[] = {
		"bluetooth", "view", "edit", "search", "check", "file",
	};

	if (!bluetooth || bluetooth->root)
		return;
	bluetooth->root = fplinux_ui_list_new(bluetooth->ui, _("Bluetooth"),
					      FPLINUX_UI_LIST_IMPLICIT);
	if (!bluetooth->root)
		return;
	fplinux_ui_list_set_value_details(bluetooth->root);
	for (size_t i = 0; i < ARRAY_SIZE(labels); ++i) {
		fplinux_ui_list_append(bluetooth->root, _(labels[i]), NULL);
		set_glyph(bluetooth, bluetooth->root, i, glyphs[i]);
	}
	add_back(bluetooth, bluetooth->root);
	bluetooth->transfer_options = false;
	fplinux_ui_screen_set_listener(bluetooth->root, root_command, NULL,
				       bluetooth);
	fplinux_ui_list_set_focus_listener(bluetooth->root, root_focused,
					   bluetooth);
	root_refresh(bluetooth);
	push_screen(bluetooth, bluetooth->root);
	update_answering(bluetooth);
}

bool fplinux_settings_bluetooth_send(
	struct fplinux_settings_bluetooth *bluetooth, const char *path)
{
	if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) {
		errno = EINVAL;
		return false;
	}
	if (!bluetooth || !fplinux_bluez_adapter_present(bluetooth->adapter)) {
		errno = ENODEV;
		return false;
	}
	if (!usable(bluetooth)) {
		errno = ENETDOWN;
		return false;
	}
	if (bluetooth->transfer_running ||
	    bluetooth->mutation != MUTATION_NONE ||
	    fplinux_bluez_adapter_pending(bluetooth->adapter)) {
		errno = EBUSY;
		return false;
	}
	if (bluetooth->root)
		fplinux_settings_bluetooth_reset(bluetooth);
	fplinux_settings_bluetooth_show(bluetooth);
	if (!bluetooth->root || !open_devices(bluetooth, true, PEER_SEND)) {
		errno = ENOMEM;
		return false;
	}
	snprintf(bluetooth->transfer_path, sizeof(bluetooth->transfer_path),
		 "%s", path);
	return true;
}

void fplinux_settings_bluetooth_receive_status(
	struct fplinux_settings_bluetooth *bluetooth, bool ready)
{
	if (!bluetooth)
		return;
	bluetooth->receive_ready = ready;
	root_refresh(bluetooth);
}

void fplinux_settings_bluetooth_changed(
	struct fplinux_settings_bluetooth *bluetooth)
{
	if (!bluetooth)
		return;
	root_refresh(bluetooth);
	devices_refresh(bluetooth);
	device_refresh(bluetooth);
	if (bluetooth->stop_discovery &&
	    !fplinux_bluez_adapter_pending(bluetooth->adapter))
		release_discovery(bluetooth);
	update_answering(bluetooth);
	agent_refresh(bluetooth);
}

bool fplinux_settings_bluetooth_shown(
	const struct fplinux_settings_bluetooth *bluetooth)
{
	return bluetooth && bluetooth->root;
}

void fplinux_settings_bluetooth_finished(
	struct fplinux_settings_bluetooth *bluetooth, bool applied)
{
	if (!bluetooth)
		return;
	if (bluetooth->mutation != MUTATION_NONE) {
		bluetooth->mutation_failed = !applied;
		bluetooth->mutation_error =
			fplinux_bluez_adapter_error(bluetooth->adapter);
		if (!bluetooth->wait ||
		    !fplinux_ui_wait_end(bluetooth->ui, mutation_done,
					 bluetooth))
			mutation_done(bluetooth->ui, bluetooth);
	}
	if (bluetooth->stop_discovery)
		release_discovery(bluetooth);
}

void fplinux_settings_bluetooth_event(
	struct fplinux_settings_bluetooth *bluetooth,
	const struct fplinux_ui_event *event)
{
	if (!bluetooth)
		return;
	if (event->type == FPLINUX_UI_EVENT_VISIBILITY)
		bluetooth->visible = event->visible;
	if (event->type == FPLINUX_UI_EVENT_VISIBILITY ||
	    event->type == FPLINUX_UI_EVENT_LIFECYCLE) {
		if (!can_answer(bluetooth))
			release_discovery(bluetooth);
		update_answering(bluetooth);
		agent_refresh(bluetooth);
		transfer_refresh(bluetooth);
	} else if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		bluetooth->visible = false;
		cancel_transfer(bluetooth);
		release_discovery(bluetooth);
		update_answering(bluetooth);
	}
}

void fplinux_settings_bluetooth_reset(
	struct fplinux_settings_bluetooth *bluetooth)
{
	if (!bluetooth)
		return;
	if (bluetooth->root)
		close_screen(bluetooth, &bluetooth->root);
	else if (bluetooth->screen_count) {
		struct fplinux_ui_screen *first = bluetooth->screens[0];

		close_screen(bluetooth, &first);
	}
	bluetooth->devices = NULL;
	bluetooth->device = NULL;
	bluetooth->services = NULL;
	bluetooth->visibility = NULL;
	bluetooth->editor = NULL;
	bluetooth->agent = NULL;
	bluetooth->notice = NULL;
	bluetooth->confirm = NULL;
	bluetooth->history = NULL;
	bluetooth->history_detail = NULL;
	bluetooth->transfer = NULL;
	bluetooth->wait = NULL;
	bluetooth->agent_serial = 0;
	bluetooth->row_count = 0;
	release_discovery(bluetooth);
	bluetooth->mutation = MUTATION_NONE;
	update_answering(bluetooth);
}

static void close_transfer_command(struct fplinux_ui *ui,
				   struct fplinux_ui_screen *screen, int id,
				   void *context)
{
	struct fplinux_settings_bluetooth *bluetooth = context;

	(void)ui;
	forget_screen(bluetooth, screen);
	bluetooth->close_question = false;
	if (id == COMMAND_CLOSE_TRANSFER) {
		bluetooth->close_pending = true;
		cancel_transfer(bluetooth);
		if (!bluetooth->transfer_running) {
			bluetooth->close_pending = false;
			fplinux_ui_close_reply(bluetooth->ui,
					       FPLINUX_UI_CLOSE_READY);
		}
	} else {
		fplinux_ui_close_reply(bluetooth->ui, FPLINUX_UI_CLOSE_CANCEL);
		fplinux_settings_bluetooth_changed(bluetooth);
		transfer_refresh(bluetooth);
	}
}

enum fplinux_ui_close_response fplinux_settings_bluetooth_prepare_close(
	struct fplinux_settings_bluetooth *bluetooth)
{
	if (!bluetooth || !bluetooth->transfer_running) {
		if (bluetooth) {
			bluetooth->visible = false;
			release_discovery(bluetooth);
			update_answering(bluetooth);
		}
		return FPLINUX_UI_CLOSE_READY;
	}
	if (bluetooth->close_question || bluetooth->close_pending)
		return FPLINUX_UI_CLOSE_WAIT;
	close_screen(bluetooth, &bluetooth->confirm);
	bluetooth->confirm = fplinux_ui_alert_new(
		bluetooth->ui, FPLINUX_UI_ALERT_CONFIRMATION,
		_("File transfer"),
		_("Stop the current file transfer and close Settings?"), 0);
	if (!bluetooth->confirm)
		return FPLINUX_UI_CLOSE_CANCEL;
	add_command(bluetooth, bluetooth->confirm, COMMAND_CANCEL,
		    FPLINUX_UI_COMMAND_CANCEL, NULL, 0);
	add_command(bluetooth, bluetooth->confirm, COMMAND_CLOSE_TRANSFER,
		    FPLINUX_UI_COMMAND_OK, _("Close"), 0);
	fplinux_ui_screen_set_listener(bluetooth->confirm,
				       close_transfer_command, NULL, bluetooth);
	bluetooth->close_question = true;
	push_screen(bluetooth, bluetooth->confirm);
	return FPLINUX_UI_CLOSE_WAIT;
}

void fplinux_settings_bluetooth_free(
	struct fplinux_settings_bluetooth *bluetooth)
{
	if (!bluetooth)
		return;
	cancel_transfer(bluetooth);
	if (bluetooth->transfer_running)
		pthread_join(bluetooth->thread, NULL);
	fplinux_settings_bluetooth_reset(bluetooth);
	fplinux_ui_unwatch_fd(bluetooth->ui, bluetooth->event_fd);
	close(bluetooth->event_fd);
	pthread_mutex_destroy(&bluetooth->mutex);
	free(bluetooth);
}
