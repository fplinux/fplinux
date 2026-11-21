/* SPDX-License-Identifier: GPL-2.0-only */
#define _DEFAULT_SOURCE
#include "settings-wifi.h"
#include "fplinux-ui-menu.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui-text.h"
#include "fplinux-ui.h"

#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _(text) gettext(text)

enum command {
	COMMAND_BACK,
	COMMAND_POWER,
	COMMAND_CONNECTION,
	COMMAND_AVAILABLE,
	COMMAND_SAVED,
	COMMAND_REFRESH,
	COMMAND_ADD,
	COMMAND_CONNECT,
	COMMAND_CANCEL,
	COMMAND_DISCONNECT,
	COMMAND_RETRY_ADDRESS,
	COMMAND_OTHER_PASSWORD,
	COMMAND_FORGET,
	COMMAND_CONFIRM_FORGET,
};

struct fplinux_settings_wifi {
	struct fplinux_ui *ui;
	struct fplinux_ui_wifi_state state;
	struct fplinux_ui_screen *root;
	struct fplinux_ui_screen *networks;
	struct fplinux_ui_screen *properties;
	struct fplinux_ui_screen *editor;
	struct fplinux_ui_screen *dialog;
	bool current_properties;
	bool saved;
	bool hidden;
	bool scan_on_open;
	size_t ssid_item;
	size_t security_item;
	size_t key_item;
	size_t save_item;
	uint32_t row_ids[FPLINUX_UI_WIFI_ROW_COUNT];
	size_t row_count;
	struct fplinux_ui_wifi_network selected;
	struct fplinux_ui_wifi_network attempted;
	uint32_t serial;
	uint32_t pending;
	enum fplinux_ui_wifi_action action;
	enum fplinux_ui_wifi_result scan_result;
};

static void root_refresh(struct fplinux_settings_wifi *wifi);
static void networks_refresh(struct fplinux_settings_wifi *wifi);
static void root_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context);
static void network_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *screen, int id,
			    void *context);
static void editor_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context);
static void dialog_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context);

static void add_command(struct fplinux_ui_screen *screen, int id,
			enum fplinux_ui_command_type type, const char *label)
{
	fplinux_ui_screen_add_command(screen,
				      &(const struct fplinux_ui_command){
					      .id = id,
					      .type = type,
					      .label = label,
				      });
}

static void pop_screen(struct fplinux_settings_wifi *wifi,
		       struct fplinux_ui_screen **screen)
{
	if (!*screen)
		return;
	*screen = NULL;
	fplinux_ui_pop(wifi->ui);
}

static void erase_key(struct fplinux_settings_wifi *wifi)
{
	if (wifi->editor)
		fplinux_ui_form_set_text(wifi->editor, wifi->key_item, "");
}

static void close_editor(struct fplinux_settings_wifi *wifi)
{
	erase_key(wifi);
	pop_screen(wifi, &wifi->editor);
}

static void close_children(struct fplinux_settings_wifi *wifi)
{
	fplinux_ui_menu_close(wifi->ui);
	pop_screen(wifi, &wifi->dialog);
	close_editor(wifi);
	pop_screen(wifi, &wifi->properties);
	pop_screen(wifi, &wifi->networks);
	wifi->row_count = 0;
	wifi->scan_on_open = false;
}

static bool send_request(struct fplinux_settings_wifi *wifi,
			 const struct fplinux_ui_wifi_request *request)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_WIFI_REQUEST,
	};
	bool sent;

	if (wifi->pending && request->action != FPLINUX_UI_WIFI_CANCEL)
		return false;
	message.wifi_request = *request;
	if (!++wifi->serial)
		++wifi->serial;
	message.serial = wifi->serial;
	sent = fplinux_ui_send(wifi->ui, &message);
	explicit_bzero(message.wifi_request.key,
		       sizeof(message.wifi_request.key));
	if (sent) {
		wifi->pending = message.serial;
		wifi->action = request->action;
		if (wifi->action == FPLINUX_UI_WIFI_SCAN)
			wifi->scan_result = FPLINUX_UI_WIFI_APPLIED;
		if (wifi->action == FPLINUX_UI_WIFI_SET_POWER)
			root_refresh(wifi);
		networks_refresh(wifi);
	}
	return sent;
}

static bool request_action(struct fplinux_settings_wifi *wifi,
			   enum fplinux_ui_wifi_action action)
{
	return send_request(wifi, &(const struct fplinux_ui_wifi_request){
					  .action = action,
				  });
}

static bool can_scan(const struct fplinux_settings_wifi *wifi)
{
	return !wifi->pending &&
	       (wifi->state.scan != FPLINUX_UI_WIFI_SCANNING ||
		wifi->scan_result != FPLINUX_UI_WIFI_APPLIED);
}

static void start_pending_scan(struct fplinux_settings_wifi *wifi)
{
	if (!wifi->scan_on_open || wifi->pending)
		return;
	wifi->scan_on_open = false;
	if (can_scan(wifi))
		request_action(wifi, FPLINUX_UI_WIFI_SCAN);
}

static const char *security_name(enum fplinux_ui_wifi_security security)
{
	switch (security) {
	case FPLINUX_UI_WIFI_OPEN:
		return _("Open network");
	case FPLINUX_UI_WIFI_WPA2:
		return "WPA2-Personal";
	case FPLINUX_UI_WIFI_WPA3:
		return "WPA3";
	case FPLINUX_UI_WIFI_ENTERPRISE:
		return _("Enterprise");
	case FPLINUX_UI_WIFI_UNSUPPORTED:
		return _("Unsupported");
	}
	return "";
}

static bool can_connect(const struct fplinux_settings_wifi *wifi,
			const struct fplinux_ui_wifi_network *network)
{
	return network->security == FPLINUX_UI_WIFI_WPA2 ||
	       (network->security == FPLINUX_UI_WIFI_OPEN &&
		wifi->state.open_supported);
}

static const char *link_status(enum fplinux_ui_wifi_link link)
{
	switch (link) {
	case FPLINUX_UI_WIFI_DISCONNECTED:
		return _("Disconnected");
	case FPLINUX_UI_WIFI_CONNECTING:
		return _("Connecting…");
	case FPLINUX_UI_WIFI_ADDRESSING:
		return _("Getting IP address…");
	case FPLINUX_UI_WIFI_CONNECTED:
		return _("Connected");
	case FPLINUX_UI_WIFI_NO_ADDRESS:
		return _("No IP address");
	}
	return "";
}

static struct fplinux_ui_screen *new_alert(struct fplinux_settings_wifi *wifi,
					   enum fplinux_ui_alert_kind kind,
					   const char *text)
{
	struct fplinux_ui_screen *alert =
		fplinux_ui_alert_new(wifi->ui, kind, _("Wi-Fi"), text, 0);

	if (alert)
		fplinux_ui_screen_set_listener(alert, dialog_command, NULL,
					       wifi);
	return alert;
}

static void show_dialog(struct fplinux_settings_wifi *wifi,
			struct fplinux_ui_screen *alert)
{
	wifi->dialog = alert;
	fplinux_ui_push(wifi->ui, alert);
}

static void warning(struct fplinux_settings_wifi *wifi, const char *text)
{
	struct fplinux_ui_screen *alert =
		new_alert(wifi, FPLINUX_UI_ALERT_WARNING, text);

	if (!alert)
		return;
	add_command(alert, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, _("Close"));
	show_dialog(wifi, alert);
}

static void show_progress(struct fplinux_settings_wifi *wifi)
{
	struct fplinux_ui_screen *alert =
		new_alert(wifi, FPLINUX_UI_ALERT_INFO, _("Connecting…"));

	if (!alert)
		return;
	fplinux_ui_alert_set_progress(alert, 0, 0);
	add_command(alert, COMMAND_CANCEL, FPLINUX_UI_COMMAND_CANCEL, NULL);
	show_dialog(wifi, alert);
}

static void root_row(struct fplinux_ui_screen *list, const char *label,
		     const char *value, enum command id, const char *action)
{
	size_t index = fplinux_ui_list_append(list, label, value);

	if (index != SIZE_MAX && action)
		fplinux_ui_list_add_item_command(
			list, index,
			&(const struct fplinux_ui_command){
				.id = id,
				.type = FPLINUX_UI_COMMAND_OK,
				.label = action,
			});
}

static void root_refresh(struct fplinux_settings_wifi *wifi)
{
	struct fplinux_ui_screen *list = wifi->root;
	const struct fplinux_ui_wifi_state *state = &wifi->state;
	size_t focus;
	const char *connection;
	bool changing_power = wifi->pending &&
			      wifi->action == FPLINUX_UI_WIFI_SET_POWER;

	if (!list)
		return;
	focus = fplinux_ui_list_get_focused(list);
	if (fplinux_ui_current(wifi->ui) == list)
		fplinux_ui_menu_close(wifi->ui);
	fplinux_ui_list_delete_all(list);
	fplinux_ui_list_set_select_label(list, "");
	fplinux_ui_screen_set_header_status(
		list, changing_power ? _("Applying…") : NULL, NULL);
	if (state->power == FPLINUX_UI_WIFI_UNAVAILABLE) {
		root_row(list, _("Wi-Fi unavailable"), NULL, COMMAND_POWER,
			 NULL);
		return;
	}
	if (state->power == FPLINUX_UI_WIFI_OFF) {
		root_row(list, _("Wi-Fi"), _("Off"), COMMAND_POWER,
			 changing_power ? NULL : _("Turn on"));
		return;
	}
	root_row(list, _("Wi-Fi"), _("On"), COMMAND_POWER,
		 changing_power ? NULL : _("Turn off"));
	connection = state->link == FPLINUX_UI_WIFI_CONNECTED ?
			     state->current.name :
			     link_status(state->link);
	root_row(list, _("Connection"), connection, COMMAND_CONNECTION,
		 state->link == FPLINUX_UI_WIFI_DISCONNECTED ? NULL :
							       _("Properties"));
	root_row(list, _("Available networks"), NULL, COMMAND_AVAILABLE,
		 _("Open"));
	root_row(list, _("Saved networks"), NULL, COMMAND_SAVED, _("Open"));
	if (focus < fplinux_ui_list_size(list))
		fplinux_ui_list_set_focused(list, focus);
}

static void render_networks(struct fplinux_settings_wifi *wifi,
			    const struct fplinux_ui_wifi_network *rows,
			    size_t count, const char *empty)
{
	struct fplinux_ui_screen *list = wifi->networks;
	size_t old = fplinux_ui_list_get_focused(list);
	uint32_t id = old < wifi->row_count ? wifi->row_ids[old] : 0;
	size_t focus = old == SIZE_MAX ? 0 : old;

	if (fplinux_ui_current(wifi->ui) == list)
		fplinux_ui_menu_close(wifi->ui);
	fplinux_ui_list_delete_all(list);
	wifi->row_count = 0;
	if (!count) {
		fplinux_ui_list_append(list, empty, NULL);
		fplinux_ui_list_set_select_label(list, "");
		return;
	}
	fplinux_ui_list_set_select_label(list, wifi->pending ? "" : NULL);
	for (size_t i = 0; i < count; ++i) {
		size_t index = fplinux_ui_list_append(
			list, rows[i].name, security_name(rows[i].security));

		if (index == SIZE_MAX)
			break;
		wifi->row_ids[index] = rows[i].id;
		++wifi->row_count;
		if (rows[i].id == id)
			focus = index;
		if (wifi->pending)
			continue;
		if (wifi->saved)
			fplinux_ui_list_add_item_command(
				list, index,
				&(const struct fplinux_ui_command){
					.id = COMMAND_FORGET,
					.type = FPLINUX_UI_COMMAND_ITEM,
					.label = _("Forget network"),
				});
		if (!can_connect(wifi, &rows[i]))
			fplinux_ui_list_add_item_command(
				list, index,
				&(const struct fplinux_ui_command){
					.id = COMMAND_CONNECTION,
					.type = FPLINUX_UI_COMMAND_OK,
					.label = _("Properties"),
				});
	}
	fplinux_ui_list_set_focused(list, focus);
}

static const char *scan_status(const struct fplinux_settings_wifi *wifi)
{
	enum fplinux_ui_wifi_scan phase = wifi->state.scan;

	if (wifi->pending)
		return wifi->action == FPLINUX_UI_WIFI_SCAN &&
				       phase == FPLINUX_UI_WIFI_SCANNING ?
			       _("Scanning…") :
			       _("Waiting to scan…");
	if (wifi->scan_result == FPLINUX_UI_WIFI_BUSY)
		return _("Wi-Fi busy");
	if (wifi->scan_result != FPLINUX_UI_WIFI_APPLIED ||
	    phase == FPLINUX_UI_WIFI_SCAN_FAILED)
		return _("Scan failed");
	if (phase == FPLINUX_UI_WIFI_SCANNING)
		return _("Scanning…");
	if (phase == FPLINUX_UI_WIFI_SCAN_IDLE || wifi->scan_on_open)
		return _("Waiting to scan…");
	return NULL;
}

static void networks_refresh(struct fplinux_settings_wifi *wifi)
{
	const struct fplinux_ui_wifi_state *state = &wifi->state;
	struct fplinux_ui_screen *list = wifi->networks;
	const char *status;

	if (!list)
		return;
	if (wifi->saved) {
		render_networks(wifi, state->saved, state->saved_count,
				_("No saved networks"));
		return;
	}
	if (can_scan(wifi))
		add_command(list, COMMAND_REFRESH, FPLINUX_UI_COMMAND_SCREEN,
			    _("Refresh"));
	else
		fplinux_ui_screen_remove_command(list, COMMAND_REFRESH);
	if (!wifi->pending)
		add_command(list, COMMAND_ADD, FPLINUX_UI_COMMAND_SCREEN,
			    _("Add network"));
	else
		fplinux_ui_screen_remove_command(list, COMMAND_ADD);
	status = scan_status(wifi);
	fplinux_ui_screen_set_header_status(
		list, state->available_count ? status : NULL, NULL);
	render_networks(wifi, state->available, state->available_count,
			status ? status : _("No networks found"));
}

static void open_networks(struct fplinux_settings_wifi *wifi, bool saved)
{
	struct fplinux_ui_screen *list = fplinux_ui_list_new(
		wifi->ui, saved ? _("Saved networks") : _("Available networks"),
		FPLINUX_UI_LIST_IMPLICIT);

	if (!list)
		return;
	wifi->networks = list;
	wifi->saved = saved;
	wifi->scan_on_open = !saved && !(wifi->pending &&
					 wifi->action == FPLINUX_UI_WIFI_SCAN);
	add_command(list, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL);
	fplinux_ui_screen_set_listener(list, network_command, NULL, wifi);
	networks_refresh(wifi);
	fplinux_ui_push(wifi->ui, list);
	start_pending_scan(wifi);
}

static void property_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *screen, int id,
			     void *context)
{
	struct fplinux_settings_wifi *wifi = context;

	(void)ui;
	(void)screen;
	if (id == COMMAND_DISCONNECT &&
	    !request_action(wifi, FPLINUX_UI_WIFI_DISCONNECT))
		return;
	if (id == COMMAND_RETRY_ADDRESS &&
	    !request_action(wifi, FPLINUX_UI_WIFI_RETRY_ADDRESS))
		return;
	pop_screen(wifi, &wifi->properties);
}

static void open_properties(struct fplinux_settings_wifi *wifi,
			    const struct fplinux_ui_wifi_network *network,
			    bool current)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(wifi->ui, _("Properties"));

	if (!form)
		return;
	fplinux_ui_form_append_property(form, _("Network name"), network->name);
	fplinux_ui_form_append_property(form, _("Security"),
					security_name(network->security));
	if (current) {
		fplinux_ui_form_append_property(form, _("Connection"),
						link_status(wifi->state.link));
		if (wifi->state.link == FPLINUX_UI_WIFI_CONNECTED)
			fplinux_ui_form_append_property(form, _("IP address"),
							wifi->state.address);
		if (wifi->state.link == FPLINUX_UI_WIFI_NO_ADDRESS)
			add_command(form, COMMAND_RETRY_ADDRESS,
				    FPLINUX_UI_COMMAND_SCREEN, _("Retry"));
		add_command(form, COMMAND_DISCONNECT, FPLINUX_UI_COMMAND_SCREEN,
			    _("Disconnect"));
	} else {
		fplinux_ui_form_append_property(
			form, NULL,
			network->security == FPLINUX_UI_WIFI_OPEN ?
				_("Open networks are unavailable") :
				_("This security type is unsupported"));
	}
	add_command(form, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL);
	fplinux_ui_screen_set_listener(form, property_command, NULL, wifi);
	wifi->properties = form;
	wifi->current_properties = current;
	fplinux_ui_push(wifi->ui, form);
}

static void root_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context)
{
	struct fplinux_settings_wifi *wifi = context;

	(void)ui;
	(void)screen;
	switch (id) {
	case COMMAND_BACK:
		pop_screen(wifi, &wifi->root);
		break;
	case COMMAND_POWER:
		send_request(wifi, &(const struct fplinux_ui_wifi_request){
					   .action = FPLINUX_UI_WIFI_SET_POWER,
					   .enabled = wifi->state.power ==
						      FPLINUX_UI_WIFI_OFF,
				   });
		break;
	case COMMAND_CONNECTION:
		open_properties(wifi, &wifi->state.current, true);
		break;
	case COMMAND_AVAILABLE:
	case COMMAND_SAVED:
		open_networks(wifi, id == COMMAND_SAVED);
		break;
	}
}

static void editor_changed(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *form, size_t item,
			   void *context)
{
	struct fplinux_settings_wifi *wifi = context;
	bool open;

	(void)ui;
	if (item != wifi->security_item)
		return;
	open = fplinux_ui_form_get_choice(form, item) == 1;
	if (open)
		erase_key(wifi);
	fplinux_ui_form_set_hidden(form, wifi->key_item, open);
}

static void open_editor(struct fplinux_settings_wifi *wifi, bool hidden)
{
	const char *const security[] = { "WPA2-Personal", _("Open network") };
	const char *const save_choices[] = { _("No"), _("Yes") };
	const struct fplinux_ui_command connect = {
		.id = COMMAND_CONNECT,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Connect"),
	};
	struct fplinux_ui_screen *form = fplinux_ui_form_new(
		wifi->ui, hidden ? _("Add network") : _("Connect"));
	size_t connect_item;

	if (!form)
		return;
	wifi->ssid_item = SIZE_MAX;
	wifi->security_item = SIZE_MAX;
	wifi->save_item = SIZE_MAX;
	wifi->hidden = hidden;
	if (hidden) {
		wifi->ssid_item = fplinux_ui_form_append_text_field(
			form, _("Network name"), "", FPLINUX_UI_WIFI_SSID_BYTES,
			FPLINUX_UI_TEXT_ANY);
		wifi->security_item = fplinux_ui_form_append_choice(
			form, _("Security"), FPLINUX_UI_CHOICE_POPUP, security,
			wifi->state.open_supported ? 2 : 1);
	} else {
		fplinux_ui_form_append_string(form, NULL, wifi->selected.name);
	}
	wifi->key_item = fplinux_ui_form_append_text_field(
		form, _("Password"), "", FPLINUX_UI_WIFI_KEY_BYTES,
		FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_PASSWORD);
	if (hidden || !wifi->selected.profile_id)
		wifi->save_item = fplinux_ui_form_append_choice(
			form, _("Save network"), FPLINUX_UI_CHOICE_POPUP,
			save_choices, 2);
	connect_item = fplinux_ui_form_append_value(form, connect.label, NULL);
	fplinux_ui_form_add_item_command(form, connect_item, &connect);
	add_command(form, COMMAND_CANCEL, FPLINUX_UI_COMMAND_CANCEL, NULL);
	fplinux_ui_screen_set_listener(form, editor_command, editor_changed,
				       wifi);
	wifi->editor = form;
	fplinux_ui_push(wifi->ui, form);
}

static bool is_wpa2_key(const char *text)
{
	size_t length = strlen(text);

	if (length == FPLINUX_UI_WIFI_KEY_BYTES)
		return strspn(text, "0123456789abcdefABCDEF") == length;
	if (length < 8 || length > 63)
		return false;
	for (size_t i = 0; i < length; ++i)
		if ((unsigned char)text[i] < 0x20 ||
		    (unsigned char)text[i] > 0x7e)
			return false;
	return true;
}

static void connect_network(struct fplinux_settings_wifi *wifi,
			    const struct fplinux_ui_wifi_request *request)
{
	bool sent = send_request(wifi, request);

	erase_key(wifi);
	if (sent) {
		wifi->attempted = wifi->selected;
		close_editor(wifi);
		show_progress(wifi);
	} else if (!wifi->pending) {
		warning(wifi, _("Wi-Fi unavailable"));
	}
}

static void editor_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct fplinux_settings_wifi *wifi = context;
	struct fplinux_ui_wifi_request request = { 0 };

	(void)ui;
	if (id == COMMAND_CANCEL) {
		close_editor(wifi);
		return;
	}
	if (id != COMMAND_CONNECT || wifi->pending)
		return;
	if (wifi->hidden) {
		const char *ssid =
			fplinux_ui_form_get_text(screen, wifi->ssid_item);

		if (!ssid || !*ssid ||
		    strlen(ssid) > FPLINUX_UI_WIFI_SSID_BYTES) {
			warning(wifi,
				_("Enter a network name of up to 32 bytes"));
			return;
		}
		request.action = FPLINUX_UI_WIFI_CONNECT_HIDDEN;
		request.security = fplinux_ui_form_get_choice(
					   screen, wifi->security_item) == 1 ?
					   FPLINUX_UI_WIFI_OPEN :
					   FPLINUX_UI_WIFI_WPA2;
		strcpy(request.ssid, ssid);
	} else {
		request.action = wifi->selected.profile_id ?
					 FPLINUX_UI_WIFI_CONNECT_SAVED :
					 FPLINUX_UI_WIFI_CONNECT;
		request.target = wifi->selected.profile_id ?
					 wifi->selected.profile_id :
					 wifi->selected.id;
		request.security = FPLINUX_UI_WIFI_WPA2;
	}
	if (request.security == FPLINUX_UI_WIFI_WPA2) {
		const char *key =
			fplinux_ui_form_get_text(screen, wifi->key_item);

		if (!key || !is_wpa2_key(key)) {
			warning(wifi,
				_("Enter 8–63 ASCII characters or 64 hex digits"));
			return;
		}
		strcpy(request.key, key);
	}
	request.save =
		request.action == FPLINUX_UI_WIFI_CONNECT_SAVED ||
		(wifi->save_item != SIZE_MAX &&
		 fplinux_ui_form_get_choice(screen, wifi->save_item) == 1);
	connect_network(wifi, &request);
	explicit_bzero(request.key, sizeof(request.key));
}

static void confirm_forget(struct fplinux_settings_wifi *wifi)
{
	bool current = wifi->state.link != FPLINUX_UI_WIFI_DISCONNECTED &&
		       wifi->state.current.profile_id ==
			       wifi->selected.profile_id;
	char text[FPLINUX_UI_SESSION_TEXT_BYTES + FPLINUX_UI_WIFI_DISPLAY_BYTES];
	struct fplinux_ui_screen *alert;

	snprintf(text, sizeof(text), "%s\n%s",
		 current ? _("Forget this network and disconnect?") :
			   _("Forget this saved network?"),
		 wifi->selected.name);
	alert = new_alert(wifi, FPLINUX_UI_ALERT_CONFIRMATION, text);

	if (!alert)
		return;
	add_command(alert, COMMAND_CONFIRM_FORGET, FPLINUX_UI_COMMAND_OK,
		    _("Forget"));
	add_command(alert, COMMAND_BACK, FPLINUX_UI_COMMAND_CANCEL, NULL);
	show_dialog(wifi, alert);
}

static void network_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *screen, int id,
			    void *context)
{
	struct fplinux_settings_wifi *wifi = context;
	const struct fplinux_ui_wifi_network *rows =
		wifi->saved ? wifi->state.saved : wifi->state.available;
	size_t focus = fplinux_ui_list_get_focused(screen);

	(void)ui;
	switch (id) {
	case COMMAND_BACK:
		pop_screen(wifi, &wifi->networks);
		wifi->row_count = 0;
		wifi->scan_on_open = false;
		return;
	case COMMAND_REFRESH:
		if (can_scan(wifi))
			request_action(wifi, FPLINUX_UI_WIFI_SCAN);
		return;
	case COMMAND_ADD:
		if (!wifi->pending) {
			wifi->selected = (struct fplinux_ui_wifi_network){ 0 };
			open_editor(wifi, true);
		}
		return;
	}
	if (focus >= wifi->row_count || wifi->pending)
		return;
	wifi->selected = rows[focus];
	if (id == COMMAND_FORGET) {
		confirm_forget(wifi);
	} else if (id == COMMAND_CONNECTION) {
		open_properties(wifi, &wifi->selected, false);
	} else if (id == FPLINUX_UI_SELECT_COMMAND) {
		const struct fplinux_ui_wifi_network *network = &wifi->selected;

		if (!can_connect(wifi, network))
			return;
		if (network->profile_id ||
		    network->security == FPLINUX_UI_WIFI_OPEN)
			connect_network(
				wifi,
				&(const struct fplinux_ui_wifi_request){
					.action =
						network->profile_id ?
							FPLINUX_UI_WIFI_CONNECT_SAVED :
							FPLINUX_UI_WIFI_CONNECT,
					.target = network->profile_id ?
							  network->profile_id :
							  network->id,
					.security = network->security,
				});
		else
			open_editor(wifi, false);
	}
}

static void dialog_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct fplinux_settings_wifi *wifi = context;

	(void)ui;
	(void)screen;
	wifi->dialog = NULL;
	switch (id) {
	case COMMAND_CANCEL:
		send_request(wifi, &(const struct fplinux_ui_wifi_request){
					   .action = FPLINUX_UI_WIFI_CANCEL,
					   .target = wifi->pending,
				   });
		break;
	case COMMAND_OTHER_PASSWORD:
		wifi->selected = wifi->attempted;
		open_editor(wifi, false);
		break;
	case COMMAND_RETRY_ADDRESS:
		request_action(wifi, FPLINUX_UI_WIFI_RETRY_ADDRESS);
		break;
	case COMMAND_DISCONNECT:
		request_action(wifi, FPLINUX_UI_WIFI_DISCONNECT);
		break;
	case COMMAND_CONFIRM_FORGET:
		send_request(wifi, &(const struct fplinux_ui_wifi_request){
					   .action = FPLINUX_UI_WIFI_FORGET,
					   .target = wifi->selected.profile_id,
				   });
		break;
	}
}

static void receive_result(struct fplinux_settings_wifi *wifi,
			   const struct fplinux_ui_session_message *message)
{
	const char *text;
	struct fplinux_ui_screen *alert;
	bool saved_auth_error = false;
	bool address_error = false;

	if (message->serial != wifi->pending)
		return;
	wifi->pending = 0;
	if (wifi->action == FPLINUX_UI_WIFI_SCAN)
		wifi->scan_result = message->wifi_result;
	networks_refresh(wifi);
	if (wifi->action == FPLINUX_UI_WIFI_SET_POWER)
		root_refresh(wifi);
	if (wifi->action == FPLINUX_UI_WIFI_CONNECT ||
	    wifi->action == FPLINUX_UI_WIFI_CONNECT_SAVED ||
	    wifi->action == FPLINUX_UI_WIFI_CONNECT_HIDDEN)
		pop_screen(wifi, &wifi->dialog);
	if (!wifi->root || wifi->dialog ||
	    message->wifi_result == FPLINUX_UI_WIFI_APPLIED ||
	    wifi->action == FPLINUX_UI_WIFI_SCAN)
		return;
	switch (message->wifi_result) {
	case FPLINUX_UI_WIFI_BUSY:
		text = _("Wi-Fi busy");
		break;
	case FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE:
		text = _("Network unavailable");
		break;
	case FPLINUX_UI_WIFI_AUTH_FAILED:
	case FPLINUX_UI_WIFI_BAD_PASSWORD:
		text = message->wifi_result == FPLINUX_UI_WIFI_BAD_PASSWORD ?
			       _("Incorrect password") :
			       _("Could not join the network");
		saved_auth_error =
			wifi->action == FPLINUX_UI_WIFI_CONNECT_SAVED &&
			wifi->attempted.security == FPLINUX_UI_WIFI_WPA2;
		break;
	case FPLINUX_UI_WIFI_ADDRESS_FAILED:
		text = _("No IP address");
		address_error = true;
		break;
	case FPLINUX_UI_WIFI_PROFILE_FAILED:
		text = _("Could not save network changes");
		break;
	default:
		text = _("Wi-Fi operation failed");
		break;
	}
	alert = new_alert(wifi, FPLINUX_UI_ALERT_ERROR, text);
	if (!alert)
		return;
	if (saved_auth_error)
		add_command(alert, COMMAND_OTHER_PASSWORD,
			    FPLINUX_UI_COMMAND_SCREEN,
			    _("Enter another password"));
	if (address_error) {
		add_command(alert, COMMAND_RETRY_ADDRESS,
			    FPLINUX_UI_COMMAND_SCREEN, _("Retry"));
		add_command(alert, COMMAND_DISCONNECT,
			    FPLINUX_UI_COMMAND_SCREEN, _("Disconnect"));
	}
	add_command(alert, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, _("Close"));
	show_dialog(wifi, alert);
}

struct fplinux_settings_wifi *fplinux_settings_wifi_new(struct fplinux_ui *ui)
{
	struct fplinux_settings_wifi *wifi = calloc(1, sizeof(*wifi));

	if (wifi)
		wifi->ui = ui;
	return wifi;
}

void fplinux_settings_wifi_free(struct fplinux_settings_wifi *wifi)
{
	if (!wifi)
		return;
	erase_key(wifi);
	free(wifi);
}

void fplinux_settings_wifi_show(struct fplinux_settings_wifi *wifi)
{
	struct fplinux_ui_screen *root;

	if (!wifi || wifi->root ||
	    wifi->state.power == FPLINUX_UI_WIFI_UNAVAILABLE)
		return;
	root = fplinux_ui_list_new(wifi->ui, _("Wi-Fi"),
				   FPLINUX_UI_LIST_IMPLICIT);
	if (!root)
		return;
	wifi->root = root;
	fplinux_ui_list_set_value_details(root);
	add_command(root, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL);
	fplinux_ui_screen_set_listener(root, root_command, NULL, wifi);
	root_refresh(wifi);
	fplinux_ui_push(wifi->ui, root);
}

void fplinux_settings_wifi_reset(struct fplinux_settings_wifi *wifi)
{
	if (!wifi || !wifi->root)
		return;
	close_children(wifi);
	pop_screen(wifi, &wifi->root);
}

void fplinux_settings_wifi_background(struct fplinux_settings_wifi *wifi)
{
	if (!wifi || !wifi->editor)
		return;
	fplinux_ui_menu_close(wifi->ui);
	pop_screen(wifi, &wifi->dialog);
	close_editor(wifi);
}

bool fplinux_settings_wifi_shown(const struct fplinux_settings_wifi *wifi)
{
	return wifi && wifi->root;
}

enum fplinux_ui_wifi_power
fplinux_settings_wifi_power(const struct fplinux_settings_wifi *wifi)
{
	return wifi ? wifi->state.power : FPLINUX_UI_WIFI_UNAVAILABLE;
}

void fplinux_settings_wifi_receive(
	struct fplinux_settings_wifi *wifi,
	const struct fplinux_ui_session_message *message)
{
	if (message->type == FPLINUX_UI_SESSION_WIFI_RESULT) {
		receive_result(wifi, message);
		start_pending_scan(wifi);
		return;
	}
	if (message->wifi_state.power != FPLINUX_UI_WIFI_ON && wifi->root)
		close_children(wifi);
	else if (wifi->properties && wifi->current_properties &&
		 (wifi->state.link != message->wifi_state.link ||
		  wifi->state.current.id != message->wifi_state.current.id ||
		  strcmp(wifi->state.address, message->wifi_state.address)))
		pop_screen(wifi, &wifi->properties);
	if (wifi->state.power != message->wifi_state.power)
		wifi->scan_result = FPLINUX_UI_WIFI_APPLIED;
	wifi->state = message->wifi_state;
	root_refresh(wifi);
	networks_refresh(wifi);
	if (wifi->dialog && wifi->pending &&
	    wifi->state.link == FPLINUX_UI_WIFI_ADDRESSING)
		fplinux_ui_alert_set_text(wifi->dialog,
					  link_status(wifi->state.link));
}
