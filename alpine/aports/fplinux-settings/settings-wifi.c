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
	COMMAND_SAVED,
	COMMAND_REFRESH,
	COMMAND_ADD,
	COMMAND_CONNECT,
	COMMAND_ONCE,
	COMMAND_SAVE_PROFILE,
	COMMAND_CANCEL,
	COMMAND_DISCONNECT,
	COMMAND_RETRY_ADDRESS,
	COMMAND_OTHER_PASSWORD,
	COMMAND_RETRY_CONNECTION,
	COMMAND_FORGET,
	COMMAND_CONFIRM_FORGET,
};

struct wifi_list {
	struct fplinux_ui_screen *screen;
	size_t count;
	struct fplinux_ui_wifi_network rows[FPLINUX_UI_WIFI_ROW_COUNT + 2];
};

struct fplinux_settings_wifi {
	struct fplinux_ui *ui;
	struct fplinux_ui_wifi_state state;
	struct wifi_list root;
	struct wifi_list saved;
	struct fplinux_ui_screen *properties;
	struct fplinux_ui_screen *editor;
	struct fplinux_ui_screen *dialog;
	bool current_properties;
	size_t connection_item;
	size_t address_item;
	struct fplinux_ui_wifi_network property_network;
	bool hidden;
	bool once;
	bool scan_on_open;
	size_t ssid_item;
	size_t security_item;
	size_t key_item;
	size_t once_item;
	struct fplinux_ui_wifi_network selected;
	struct fplinux_ui_wifi_network attempted;
	char attempted_ssid[FPLINUX_UI_WIFI_SSID_BYTES + 1];
	bool attempted_open;
	bool attempted_once;
	bool attempted_credentials;
	uint32_t serial;
	uint32_t pending;
	uint32_t cancel_pending;
	bool result_deferred;
	enum fplinux_ui_wifi_result deferred_result;
	enum fplinux_ui_wifi_action action;
	enum fplinux_ui_wifi_result scan_result;
};

static void refresh_lists(struct fplinux_settings_wifi *wifi);
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
	pop_screen(wifi, &wifi->saved.screen);
	wifi->saved.count = 0;
	wifi->scan_on_open = false;
}

static bool is_join(enum fplinux_ui_wifi_action action)
{
	return action == FPLINUX_UI_WIFI_CONNECT ||
	       action == FPLINUX_UI_WIFI_CONNECT_SAVED ||
	       action == FPLINUX_UI_WIFI_CONNECT_HIDDEN;
}

static bool is_connecting(enum fplinux_ui_wifi_action action)
{
	return is_join(action) || action == FPLINUX_UI_WIFI_RETRY_ADDRESS;
}

static bool same_network(const struct fplinux_ui_wifi_network *first,
			 const struct fplinux_ui_wifi_network *second)
{
	return first->id && second->id && first->security == second->security &&
	       !strcmp(first->name, second->name);
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
		if (request->action == FPLINUX_UI_WIFI_CANCEL) {
			wifi->cancel_pending = message.serial;
			return true;
		}
		wifi->pending = message.serial;
		wifi->action = request->action;
		if (wifi->action == FPLINUX_UI_WIFI_SCAN)
			wifi->scan_result = FPLINUX_UI_WIFI_APPLIED;
		refresh_lists(wifi);
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
	if (!wifi->scan_on_open || wifi->state.power != FPLINUX_UI_WIFI_ON ||
	    wifi->pending ||
	    fplinux_ui_lifecycle(wifi->ui) != FPLINUX_UI_FOREGROUND)
		return;
	wifi->scan_on_open = false;
	if (can_scan(wifi) && !request_action(wifi, FPLINUX_UI_WIFI_SCAN)) {
		wifi->scan_result = FPLINUX_UI_WIFI_FAILED;
		refresh_lists(wifi);
	}
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
	struct fplinux_ui_screen *alert = new_alert(
		wifi, FPLINUX_UI_ALERT_INFO,
		link_status(wifi->action == FPLINUX_UI_WIFI_RETRY_ADDRESS ?
				    FPLINUX_UI_WIFI_ADDRESSING :
				    FPLINUX_UI_WIFI_CONNECTING));

	if (!alert)
		return;
	fplinux_ui_alert_set_progress(alert, 0, 0);
	add_command(alert, COMMAND_CANCEL, FPLINUX_UI_COMMAND_CANCEL, NULL);
	show_dialog(wifi, alert);
}

static void add_row(struct wifi_list *list,
		    const struct fplinux_ui_wifi_network *network,
		    const char *label, const char *value, enum command id,
		    const char *action)
{
	size_t index = fplinux_ui_list_append(list->screen, label, value);

	if (index == SIZE_MAX)
		return;
	list->rows[list->count++] =
		network ? *network : (struct fplinux_ui_wifi_network){ 0 };
	if (action)
		fplinux_ui_list_add_item_command(
			list->screen, index,
			&(const struct fplinux_ui_command){
				.id = id,
				.type = FPLINUX_UI_COMMAND_OK,
				.label = action,
			});
}

static const struct fplinux_ui_wifi_network *
focused_network(const struct wifi_list *list)
{
	size_t index = fplinux_ui_list_get_focused(list->screen);

	return index < list->count && list->rows[index].id ?
		       &list->rows[index] :
		       NULL;
}

static bool is_current(const struct fplinux_settings_wifi *wifi,
		       const struct fplinux_ui_wifi_network *network)
{
	return wifi->state.link != FPLINUX_UI_WIFI_DISCONNECTED &&
	       same_network(&wifi->state.current, network);
}

static void add_network(struct fplinux_settings_wifi *wifi,
			struct wifi_list *list,
			const struct fplinux_ui_wifi_network *network)
{
	bool current = is_current(wifi, network);
	bool properties = current || !can_connect(wifi, network);

	add_row(list, network, network->name,
		current ? link_status(wifi->state.link) :
			  security_name(network->security),
		properties ? COMMAND_CONNECTION : COMMAND_CONNECT,
		properties    ? _("Properties") :
		wifi->pending ? NULL :
				_("Connect"));
}

static const char *scan_status(const struct fplinux_settings_wifi *wifi)
{
	enum fplinux_ui_wifi_scan phase = wifi->state.scan;

	if (wifi->pending && wifi->action == FPLINUX_UI_WIFI_SCAN)
		return _("Scanning…");
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

static void refresh_root_rows(struct fplinux_settings_wifi *wifi)
{
	struct wifi_list *list = &wifi->root;
	const struct fplinux_ui_wifi_state *state = &wifi->state;
	bool on = state->power == FPLINUX_UI_WIFI_ON;
	bool power_pending = wifi->pending &&
			     wifi->action == FPLINUX_UI_WIFI_SET_POWER;
	const char *status = power_pending ? _("Applying…") :
			     on		   ? scan_status(wifi) :
					     NULL;

	fplinux_ui_screen_set_header_status(
		list->screen, status,
		wifi->scan_on_open ? _("Scanning…") : NULL);
	if (!on) {
		fplinux_ui_screen_remove_command(list->screen, COMMAND_REFRESH);
		fplinux_ui_screen_remove_command(list->screen, COMMAND_SAVED);
		fplinux_ui_screen_remove_command(list->screen, COMMAND_ADD);
	}
	if (state->power == FPLINUX_UI_WIFI_UNAVAILABLE) {
		add_row(list, NULL, _("Wi-Fi unavailable"), NULL, COMMAND_POWER,
			NULL);
		return;
	}
	add_row(list, NULL, _("Wi-Fi"), on ? _("On") : _("Off"), COMMAND_POWER,
		wifi->pending ? NULL :
		on	      ? _("Turn off") :
				_("Turn on"));
	if (!on)
		return;
	add_command(list->screen, COMMAND_REFRESH, FPLINUX_UI_COMMAND_SCREEN,
		    _("Refresh"));
	add_command(list->screen, COMMAND_SAVED, FPLINUX_UI_COMMAND_SCREEN,
		    _("Saved networks"));
	add_command(list->screen, COMMAND_ADD, FPLINUX_UI_COMMAND_SCREEN,
		    _("Add hidden network"));
	if (state->link != FPLINUX_UI_WIFI_DISCONNECTED)
		add_network(wifi, list, &state->current);
	for (size_t i = 0; i < state->available_count; ++i)
		if (!is_current(wifi, &state->available[i]))
			add_network(wifi, list, &state->available[i]);
	if (!status && list->count == 1)
		add_row(list, NULL, _("No networks found"), NULL,
			COMMAND_CONNECT, NULL);
}

static void refresh_list(struct fplinux_settings_wifi *wifi,
			 struct wifi_list *list)
{
	struct fplinux_ui_screen *screen = list->screen;
	struct fplinux_ui_wifi_network old_network = { 0 };
	size_t focus, first;
	bool found = false;

	if (!screen)
		return;
	focus = fplinux_ui_list_get_focused(screen);
	first = fplinux_ui_list_get_first_visible(screen);
	if (focus < list->count)
		old_network = list->rows[focus];
	fplinux_ui_list_delete_all(screen);
	list->count = 0;
	if (list == &wifi->root)
		refresh_root_rows(wifi);
	else
		for (size_t i = 0; i < wifi->state.saved_count; ++i)
			add_network(wifi, list, &wifi->state.saved[i]);
	if (!list->count && list == &wifi->saved)
		add_row(list, NULL, _("No saved networks"), NULL,
			COMMAND_CONNECT, NULL);
	for (size_t i = 0; i < list->count; ++i) {
		if (same_network(&list->rows[i], &old_network)) {
			focus = i;
			found = true;
			break;
		}
	}
	if (!found && old_network.id &&
	    fplinux_ui_current(wifi->ui) == screen && list == &wifi->saved)
		fplinux_ui_menu_close(wifi->ui);
	if (focus >= list->count)
		focus = focus == SIZE_MAX ? 0 : list->count - 1;
	fplinux_ui_list_set_focused(screen, focus);
	if (first != SIZE_MAX)
		fplinux_ui_list_set_first_visible(screen, first);
}

static void saved_focused(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, size_t index,
			  void *context)
{
	struct fplinux_settings_wifi *wifi = context;

	(void)ui;
	(void)index;
	if (!screen)
		return;
	if (focused_network(&wifi->saved) && !wifi->pending)
		add_command(screen, COMMAND_FORGET, FPLINUX_UI_COMMAND_SCREEN,
			    _("Forget network"));
	else
		fplinux_ui_screen_remove_command(screen, COMMAND_FORGET);
}

static void refresh_lists(struct fplinux_settings_wifi *wifi)
{
	refresh_list(wifi, &wifi->root);
	refresh_list(wifi, &wifi->saved);
	saved_focused(wifi->ui, wifi->saved.screen, 0, wifi);
}

static void open_saved(struct fplinux_settings_wifi *wifi)
{
	struct fplinux_ui_screen *screen = fplinux_ui_list_new(
		wifi->ui, _("Saved networks"), FPLINUX_UI_LIST_IMPLICIT);

	if (!screen)
		return;
	wifi->saved.screen = screen;
	add_command(screen, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL);
	fplinux_ui_list_set_select_label(screen, "");
	fplinux_ui_screen_set_listener(screen, network_command, NULL, wifi);
	fplinux_ui_list_set_focus_listener(screen, saved_focused, wifi);
	refresh_lists(wifi);
	fplinux_ui_push(wifi->ui, screen);
}

static void property_command(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *screen, int id,
			     void *context)
{
	struct fplinux_settings_wifi *wifi = context;

	(void)ui;
	(void)screen;
	if (id == COMMAND_DISCONNECT || id == COMMAND_RETRY_ADDRESS) {
		enum fplinux_ui_wifi_action action =
			id == COMMAND_DISCONNECT ?
				FPLINUX_UI_WIFI_DISCONNECT :
				FPLINUX_UI_WIFI_RETRY_ADDRESS;

		if (!request_action(wifi, action)) {
			warning(wifi, wifi->pending ? _("Wi-Fi busy") :
						      _("Wi-Fi unavailable"));
			return;
		}
	}
	pop_screen(wifi, &wifi->properties);
	if (id == COMMAND_RETRY_ADDRESS)
		show_progress(wifi);
}

static void refresh_properties(struct fplinux_settings_wifi *wifi)
{
	struct fplinux_ui_screen *form = wifi->properties;
	enum fplinux_ui_wifi_link link = wifi->state.link;

	if (!form || !wifi->current_properties)
		return;
	if (!is_current(wifi, &wifi->property_network) && !wifi->dialog) {
		pop_screen(wifi, &wifi->properties);
		return;
	}
	fplinux_ui_form_set_string(form, wifi->connection_item,
				   link_status(link));
	fplinux_ui_form_set_string(form, wifi->address_item,
				   wifi->state.address);
	fplinux_ui_form_set_hidden(form, wifi->address_item,
				   link != FPLINUX_UI_WIFI_CONNECTED);
	if (link == FPLINUX_UI_WIFI_NO_ADDRESS)
		add_command(form, COMMAND_RETRY_ADDRESS,
			    FPLINUX_UI_COMMAND_SCREEN, _("Retry"));
	else
		fplinux_ui_screen_remove_command(form, COMMAND_RETRY_ADDRESS);
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
		wifi->property_network = *network;
		wifi->connection_item = fplinux_ui_form_append_property(
			form, _("Connection"), NULL);
		wifi->address_item = fplinux_ui_form_append_property(
			form, _("IP address"), NULL);
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
	refresh_properties(wifi);
	fplinux_ui_push(wifi->ui, form);
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

static void update_save_option(struct fplinux_settings_wifi *wifi)
{
	struct fplinux_ui_screen *form = wifi->editor;

	fplinux_ui_screen_remove_command(
		form, wifi->once ? COMMAND_ONCE : COMMAND_SAVE_PROFILE);
	add_command(form, wifi->once ? COMMAND_SAVE_PROFILE : COMMAND_ONCE,
		    FPLINUX_UI_COMMAND_SCREEN,
		    wifi->once ? _("Save network") : _("Only once"));
	fplinux_ui_form_set_hidden(form, wifi->once_item, !wifi->once);
}

static void open_editor(struct fplinux_settings_wifi *wifi, const char *ssid,
			bool open, bool once)
{
	const char *const security[] = { "WPA2-Personal", _("Open network") };
	struct fplinux_ui_screen *form = fplinux_ui_form_new(
		wifi->ui, ssid ? _("Add hidden network") : wifi->selected.name);

	if (!form)
		return;
	wifi->ssid_item = SIZE_MAX;
	wifi->security_item = SIZE_MAX;
	wifi->hidden = ssid != NULL;
	wifi->once = once;
	if (ssid) {
		wifi->ssid_item = fplinux_ui_form_append_text_field(
			form, _("Network name"), ssid,
			FPLINUX_UI_WIFI_SSID_BYTES, FPLINUX_UI_TEXT_ANY);
		wifi->security_item = fplinux_ui_form_append_choice(
			form, _("Security"), FPLINUX_UI_CHOICE_POPUP, security,
			wifi->state.open_supported ? 2 : 1);
		if (open && wifi->state.open_supported)
			fplinux_ui_form_set_choice(form, wifi->security_item,
						   1);
	}
	wifi->key_item = fplinux_ui_form_append_text_field(
		form, _("Password"), "", FPLINUX_UI_WIFI_KEY_BYTES,
		FPLINUX_UI_TEXT_ANY | FPLINUX_UI_TEXT_PASSWORD);
	wifi->once_item =
		fplinux_ui_form_append_string(form, NULL, _("Only once"));
	add_command(form, COMMAND_CONNECT, FPLINUX_UI_COMMAND_OK, _("Connect"));
	add_command(form, COMMAND_CANCEL, FPLINUX_UI_COMMAND_CANCEL, NULL);
	fplinux_ui_screen_set_listener(form, editor_command, editor_changed,
				       wifi);
	wifi->editor = form;
	update_save_option(wifi);
	if (ssid && *ssid && !open)
		fplinux_ui_form_set_focus(form, wifi->key_item);
	if (ssid)
		editor_changed(wifi->ui, form, wifi->security_item, wifi);
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
		memcpy(wifi->attempted_ssid, request->ssid,
		       sizeof(wifi->attempted_ssid));
		wifi->attempted_open = request->security ==
				       FPLINUX_UI_WIFI_OPEN;
		wifi->attempted_once = wifi->editor && wifi->once;
		wifi->attempted_credentials = request->key[0] != '\0';
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
	if (id == COMMAND_ONCE || id == COMMAND_SAVE_PROFILE) {
		wifi->once = id == COMMAND_ONCE;
		update_save_option(wifi);
		return;
	}
	if (id != COMMAND_CONNECT)
		return;
	if (wifi->pending) {
		warning(wifi, _("Wi-Fi busy"));
		return;
	}
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
	request.save = !wifi->once;
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

static void connect_selected(struct fplinux_settings_wifi *wifi)
{
	const struct fplinux_ui_wifi_network *network = &wifi->selected;

	if (!network->profile_id && network->security == FPLINUX_UI_WIFI_WPA2) {
		open_editor(wifi, NULL, false, false);
		return;
	}
	connect_network(
		wifi,
		&(const struct fplinux_ui_wifi_request){
			.action = network->profile_id ?
					  FPLINUX_UI_WIFI_CONNECT_SAVED :
					  FPLINUX_UI_WIFI_CONNECT,
			.target = network->profile_id ? network->profile_id :
							network->id,
			.security = network->security,
			.save = !network->profile_id,
		});
}

static void network_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *screen, int id,
			    void *context)
{
	struct fplinux_settings_wifi *wifi = context;
	struct wifi_list *list = screen == wifi->root.screen ? &wifi->root :
							       &wifi->saved;
	const struct fplinux_ui_wifi_network *network = focused_network(list);

	(void)ui;
	switch (id) {
	case COMMAND_BACK:
		pop_screen(wifi, &list->screen);
		list->count = 0;
		if (list == &wifi->root)
			wifi->scan_on_open = false;
		return;
	case COMMAND_POWER:
		if (!send_request(wifi,
				  &(const struct fplinux_ui_wifi_request){
					  .action = FPLINUX_UI_WIFI_SET_POWER,
					  .enabled = wifi->state.power ==
						     FPLINUX_UI_WIFI_OFF,
				  }))
			warning(wifi, _("Wi-Fi unavailable"));
		return;
	case COMMAND_REFRESH:
		if (can_scan(wifi) &&
		    !request_action(wifi, FPLINUX_UI_WIFI_SCAN)) {
			wifi->scan_result = FPLINUX_UI_WIFI_FAILED;
			refresh_lists(wifi);
		}
		return;
	case COMMAND_SAVED:
		open_saved(wifi);
		return;
	case COMMAND_ADD:
		wifi->selected = (struct fplinux_ui_wifi_network){ 0 };
		open_editor(wifi, "", false, false);
		return;
	}
	if (!network)
		return;
	wifi->selected = *network;
	if (id == COMMAND_FORGET && !wifi->pending)
		confirm_forget(wifi);
	else if (id == COMMAND_CONNECTION)
		open_properties(wifi, &wifi->selected,
				is_current(wifi, network));
	else if (id == COMMAND_CONNECT && !wifi->pending)
		connect_selected(wifi);
}

static void retry_connection(struct fplinux_settings_wifi *wifi, bool password)
{
	const struct fplinux_ui_wifi_network *rows;
	size_t count;

	wifi->selected = wifi->attempted;
	if (wifi->attempted_ssid[0]) {
		open_editor(wifi, wifi->attempted_ssid, wifi->attempted_open,
			    wifi->attempted_once);
		return;
	}
	rows = wifi->selected.profile_id ? wifi->state.saved :
					   wifi->state.available;
	count = wifi->selected.profile_id ? wifi->state.saved_count :
					    wifi->state.available_count;

	for (size_t i = 0; i < count; ++i)
		if (same_network(&rows[i], &wifi->selected)) {
			wifi->selected = rows[i];
			break;
		}
	if (password || wifi->attempted_credentials)
		open_editor(wifi, NULL, false, wifi->attempted_once);
	else
		connect_selected(wifi);
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
		if (wifi->pending &&
		    !send_request(wifi,
				  &(const struct fplinux_ui_wifi_request){
					  .action = FPLINUX_UI_WIFI_CANCEL,
					  .target = wifi->pending,
				  }))
			show_progress(wifi);
		break;
	case COMMAND_OTHER_PASSWORD:
	case COMMAND_RETRY_CONNECTION:
		retry_connection(wifi, id == COMMAND_OTHER_PASSWORD);
		break;
	case COMMAND_RETRY_ADDRESS:
		if (request_action(wifi, FPLINUX_UI_WIFI_RETRY_ADDRESS))
			show_progress(wifi);
		else
			warning(wifi, _("Wi-Fi unavailable"));
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
	bool password_error = false;
	bool address_error = false;
	bool retry = false;

	if (message->serial == wifi->cancel_pending) {
		bool deferred = wifi->result_deferred;

		wifi->cancel_pending = 0;
		wifi->result_deferred = false;
		if (message->wifi_result == FPLINUX_UI_WIFI_APPLIED) {
			wifi->pending = 0;
			refresh_lists(wifi);
		} else if (deferred) {
			receive_result(
				wifi,
				&(const struct fplinux_ui_session_message){
					.serial = wifi->pending,
					.wifi_result = wifi->deferred_result,
				});
		} else if (wifi->pending) {
			if (message->wifi_result == FPLINUX_UI_WIFI_BUSY &&
			    wifi->root.screen)
				show_progress(wifi);
			else if (message->wifi_result != FPLINUX_UI_WIFI_BUSY) {
				wifi->pending = 0;
				refresh_lists(wifi);
				if (wifi->root.screen)
					warning(wifi,
						_("Wi-Fi operation failed"));
			}
		}
		return;
	}
	if (message->serial != wifi->pending)
		return;
	if (wifi->cancel_pending) {
		wifi->result_deferred = true;
		wifi->deferred_result = message->wifi_result;
		return;
	}
	wifi->pending = 0;
	if (wifi->action == FPLINUX_UI_WIFI_SCAN)
		wifi->scan_result = message->wifi_result;
	if (is_connecting(wifi->action) && wifi->dialog)
		pop_screen(wifi, &wifi->dialog);
	refresh_lists(wifi);
	if (!wifi->root.screen || wifi->dialog ||
	    message->wifi_result == FPLINUX_UI_WIFI_APPLIED ||
	    wifi->action == FPLINUX_UI_WIFI_SCAN)
		return;
	switch (message->wifi_result) {
	case FPLINUX_UI_WIFI_BUSY:
		text = _("Wi-Fi busy");
		retry = is_join(wifi->action);
		break;
	case FPLINUX_UI_WIFI_NETWORK_UNAVAILABLE:
		text = _("Network unavailable");
		retry = is_join(wifi->action);
		break;
	case FPLINUX_UI_WIFI_AUTH_FAILED:
		text = _("Connection timed out");
		password_error = wifi->action == FPLINUX_UI_WIFI_CONNECT_SAVED;
		retry = is_join(wifi->action);
		break;
	case FPLINUX_UI_WIFI_BAD_PASSWORD:
		text = _("Incorrect password");
		password_error = is_join(wifi->action);
		break;
	case FPLINUX_UI_WIFI_ADDRESS_FAILED:
		text = _("No IP address");
		address_error = true;
		break;
	case FPLINUX_UI_WIFI_PROFILE_FAILED:
		if (is_join(wifi->action) &&
		    wifi->state.link == FPLINUX_UI_WIFI_CONNECTED)
			text = _("Connected, but could not save the network");
		else if (is_join(wifi->action) &&
			 wifi->state.link == FPLINUX_UI_WIFI_NO_ADDRESS) {
			text = _("No IP address. Could not save the network.");
			address_error = true;
		} else
			text = _("Could not save network changes");
		break;
	default:
		text = _("Wi-Fi operation failed");
		break;
	}
	alert = new_alert(wifi, FPLINUX_UI_ALERT_ERROR, text);
	if (!alert)
		return;
	if (password_error)
		add_command(alert, COMMAND_OTHER_PASSWORD,
			    retry ? FPLINUX_UI_COMMAND_SCREEN :
				    FPLINUX_UI_COMMAND_OK,
			    _("Enter another password"));
	if (retry)
		add_command(alert, COMMAND_RETRY_CONNECTION,
			    FPLINUX_UI_COMMAND_OK, _("Retry"));
	if (address_error) {
		add_command(alert, COMMAND_RETRY_ADDRESS, FPLINUX_UI_COMMAND_OK,
			    _("Retry"));
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

	if (!wifi || wifi->root.screen ||
	    wifi->state.power == FPLINUX_UI_WIFI_UNAVAILABLE)
		return;
	root = fplinux_ui_list_new(wifi->ui, _("Wi-Fi"),
				   FPLINUX_UI_LIST_IMPLICIT);
	if (!root)
		return;
	wifi->root.screen = root;
	wifi->scan_on_open =
		wifi->state.power == FPLINUX_UI_WIFI_ON &&
		!(wifi->pending && wifi->action == FPLINUX_UI_WIFI_SCAN);
	fplinux_ui_list_set_value_details(root);
	fplinux_ui_list_set_select_label(root, "");
	add_command(root, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL);
	fplinux_ui_screen_set_listener(root, network_command, NULL, wifi);
	refresh_lists(wifi);
	fplinux_ui_push(wifi->ui, root);
	start_pending_scan(wifi);
}

void fplinux_settings_wifi_reset(struct fplinux_settings_wifi *wifi)
{
	if (!wifi || !wifi->root.screen)
		return;
	close_children(wifi);
	pop_screen(wifi, &wifi->root.screen);
	wifi->root.count = 0;
}

void fplinux_settings_wifi_background(struct fplinux_settings_wifi *wifi)
{
	if (!wifi || !wifi->editor)
		return;
	fplinux_ui_menu_close(wifi->ui);
	pop_screen(wifi, &wifi->dialog);
	close_editor(wifi);
}

void fplinux_settings_wifi_foreground(struct fplinux_settings_wifi *wifi)
{
	if (wifi)
		start_pending_scan(wifi);
}

bool fplinux_settings_wifi_shown(const struct fplinux_settings_wifi *wifi)
{
	return wifi && wifi->root.screen;
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
	if (message->wifi_state.power != FPLINUX_UI_WIFI_ON &&
	    wifi->root.screen)
		close_children(wifi);
	if (wifi->state.power != message->wifi_state.power) {
		wifi->scan_result = FPLINUX_UI_WIFI_APPLIED;
		wifi->scan_on_open = wifi->root.screen &&
				     message->wifi_state.power ==
					     FPLINUX_UI_WIFI_ON;
	}
	wifi->state = message->wifi_state;
	refresh_lists(wifi);
	refresh_properties(wifi);
	if (wifi->dialog && wifi->pending && is_connecting(wifi->action) &&
	    wifi->state.link == FPLINUX_UI_WIFI_ADDRESSING)
		fplinux_ui_alert_set_text(wifi->dialog,
					  link_status(wifi->state.link));
	start_pending_scan(wifi);
}
