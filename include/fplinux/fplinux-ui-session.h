/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_SESSION_H
#define FPLINUX_UI_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#define FPLINUX_UI_SESSION_SOCKET "/run/fplinux-gui/control.sock"
/* The logical Wayland seat assigned to the phone's built-in keypad. */
#define FPLINUX_UI_KEYPAD_SEAT "fplinux-keypad"
#define FPLINUX_UI_SESSION_ID_BYTES 128
#define FPLINUX_UI_SESSION_TEXT_BYTES 128
/* Bounded labels, including escaped quotes and backslashes, and state fields. */
#define FPLINUX_UI_SESSION_PACKET_BYTES 4096
#define FPLINUX_UI_SESSION_LOCALE_BYTES 8
#define FPLINUX_UI_SESSION_NAME_BYTES 32
#define FPLINUX_UI_SESSION_FRAME_MAX_WIDTH 320
#define FPLINUX_UI_SESSION_FRAME_MAX_HEIGHT 320
/* Portrait and landscape share the same bounded RGB565 pixel budget. */
#define FPLINUX_UI_SESSION_FRAME_MAX_PIXELS (240U * 320U)
#define FPLINUX_UI_SESSION_TASK_MAX 4

/* Each packet is one JSON object on a Unix SOCK_SEQPACKET connection. */
enum fplinux_ui_session_message_type {
	FPLINUX_UI_SESSION_HELLO,
	FPLINUX_UI_SESSION_READY,
	FPLINUX_UI_SESSION_LAUNCH,
	FPLINUX_UI_SESSION_TITLE,
	FPLINUX_UI_SESSION_SOFTKEYS,
	FPLINUX_UI_SESSION_HOME,
	FPLINUX_UI_SESSION_EXIT,
	FPLINUX_UI_SESSION_SELECT,
	FPLINUX_UI_SESSION_CONFIG,
	FPLINUX_UI_SESSION_ACTIVE,
	FPLINUX_UI_SESSION_CLOSE,
	FPLINUX_UI_SESSION_ERROR,
	FPLINUX_UI_SESSION_VISIBILITY,
	FPLINUX_UI_SESSION_KEEP_SCREEN_ON,
	FPLINUX_UI_SESSION_SETTING,
	FPLINUX_UI_SESSION_DISMISS,
	FPLINUX_UI_SESSION_MODE,
	FPLINUX_UI_SESSION_ACTIVE_ACK,
	FPLINUX_UI_SESSION_CANCEL,
	FPLINUX_UI_SESSION_BUSY,
	FPLINUX_UI_SESSION_LIFECYCLE,
	FPLINUX_UI_SESSION_TASKS,
	FPLINUX_UI_SESSION_ACTIVATE,
	FPLINUX_UI_SESSION_CLOSE_TASK,
	FPLINUX_UI_SESSION_END,
	FPLINUX_UI_SESSION_REDRAW_ACK,
};

enum fplinux_ui_session_lifecycle {
	FPLINUX_UI_SESSION_FOREGROUND,
	FPLINUX_UI_SESSION_BACKGROUND,
	FPLINUX_UI_SESSION_CLOSING,
};

struct fplinux_ui_session_task {
	char app_id[FPLINUX_UI_SESSION_ID_BYTES];
	char title[FPLINUX_UI_SESSION_TEXT_BYTES];
	enum fplinux_ui_session_lifecycle lifecycle;
	uint32_t token;
};

enum fplinux_ui_session_display {
	FPLINUX_UI_SESSION_DISPLAY_ON,
	FPLINUX_UI_SESSION_DISPLAY_DIM,
	FPLINUX_UI_SESSION_DISPLAY_OFF,
};

enum fplinux_ui_session_mode {
	FPLINUX_UI_SESSION_LAUNCHER,
	FPLINUX_UI_SESSION_INTEGRATED,
	FPLINUX_UI_SESSION_FULLSCREEN,
};

/* The values of enum fplinux_ui_theme_kind. */
enum fplinux_ui_session_theme {
	FPLINUX_UI_SESSION_THEME_LIGHT,
	FPLINUX_UI_SESSION_THEME_DARK,
};

/*
 * HELLO identifies the launched app_id; the manager verifies the peer PID.
 * CONFIG supplies the assigned mode and the whole display's screen_width and
 * screen_height for the UI profile. Wayland configures the surface dimensions.
 * CONFIG and ACTIVE carry separate system and keypad language codes in locale
 * and input_locale. Both codes are lowercase ASCII, two or three letters.
 *
 * READY follows the first submitted frame. ACTIVE names an application only
 * after that frame, and supplies the launcher's title, three softkey labels,
 * view (value), presentation (name), theme, display, lock and battery state.
 * A nonzero ACTIVE serial is acknowledged after the launcher commits its new
 * view and receives the following Wayland sync reply. ACTIVE_ACK carries that
 * serial; the manager may finish the handoff after 500 ms without an answer.
 *
 * Only the launcher may LAUNCH, SELECT, CANCEL or DISMISS. SELECT reports the
 * selected_id, base view (home/catalogue) and presentation (grid/list).
 * SELECT(value=tasks, enabled) reports whether Tasks covers that base without
 * changing its selection or presentation. ACTIVE returns the base view,
 * including after a launcher restart. LAUNCH may
 * attach one descriptor containing tightly packed RGB565 pixels bounded by
 * FRAME_MAX_WIDTH/HEIGHT/PIXELS. SCM_RIGHTS transfers it outside the JSON object.
 * CANCEL ends startup or BUSY without an application-crashed notice.
 *
 * HOME backgrounds the foreground application. EXIT returns to the catalogue.
 * LIFECYCLE reports foreground, background or closing independently of surface
 * VISIBILITY. CLOSE asks a client to save and exit; the manager may terminate a
 * client which does not respond. MODE requests integrated or fullscreen mode.
 * BUSY(enabled, title) extends launch progress through a blocking operation.
 * Ordinary input arrives through Wayland keyboard events.
 * KEEP_SCREEN_ON applies only while the requesting application is visible.
 *
 * SETTING(name, value) belongs to org.fplinux.settings: empty value queries,
 * otherwise it sets a value. The manager replies with the applied SETTING or
 * ERROR. input_locale selects the keypad language independently of locale.
 * Applied theme/locale replies, including save-failed errors, carry a nonzero
 * serial. REDRAW_ACK acknowledges the complete view drawn for that reply after
 * its Wayland commit and following server sync. Queries carry no redraw serial.
 * ACTIVE.battery is 0..100 or -1 without a battery. notice is empty,
 * battery-low or battery-critical; DISMISS(notice) acknowledges that notice.
 *
 * TASKS without a tasks array is a launcher-only query. The manager supplies
 * the complete bounded snapshot, including [] when no tasks remain. Each
 * task has an app_id, title, lifecycle and nonzero instance token. Launcher
 * ACTIVATE and CLOSE_TASK carry app_id and token together, so a stale action
 * cannot affect a restarted application. CLOSE_TASK uses the normal CLOSE
 * lifecycle and its bounded termination policy.
 *
 * ACTIVE.end_prompt asks the launcher to offer Minimize and Close for the
 * foreground active_id and token. Launcher END answers with that app_id and
 * token and value minimize, close or cancel. Repeated Red closes the same
 * foreground instance through the normal CLOSE lifecycle.
 * An ACTIVE end prompt may attach the current scene as bounded RGB565 pixels
 * using the same descriptor and dimensions as a LAUNCH frame.
 */
struct fplinux_ui_session_message {
	enum fplinux_ui_session_message_type type;
	enum fplinux_ui_session_mode mode;
	char app_id[FPLINUX_UI_SESSION_ID_BYTES];
	char active_id[FPLINUX_UI_SESSION_ID_BYTES];
	char selected_id[FPLINUX_UI_SESSION_ID_BYTES];
	char title[FPLINUX_UI_SESSION_TEXT_BYTES];
	char left[FPLINUX_UI_SESSION_TEXT_BYTES];
	char centre[FPLINUX_UI_SESSION_TEXT_BYTES];
	char right[FPLINUX_UI_SESSION_TEXT_BYTES];
	char reason[FPLINUX_UI_SESSION_TEXT_BYTES];
	/* Attached RGB565 frame dimensions; Wayland owns window dimensions. */
	int width;
	int height;
	int screen_width;
	int screen_height;
	enum fplinux_ui_session_theme theme;
	enum fplinux_ui_session_display display;
	bool locked;
	bool visible;
	bool enabled;
	/* ACTIVE: the system's Minimize/Close choice covers the application. */
	bool end_prompt;
	char locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char input_locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char name[FPLINUX_UI_SESSION_NAME_BYTES];
	char value[FPLINUX_UI_SESSION_TEXT_BYTES];
	int battery;
	bool charging;
	char notice[FPLINUX_UI_SESSION_NAME_BYTES];
	/* ACTIVE/ACTIVE_ACK or applied SETTING/ERROR/REDRAW_ACK correlation. */
	uint32_t serial;
	/* LAUNCH or ACTIVE end prompt: one tightly packed RGB565 SCM_RIGHTS fd. */
	bool has_frame;
	int frame_fd;
	/* LIFECYCLE only: independent of display visibility. */
	enum fplinux_ui_session_lifecycle lifecycle;
	/* ACTIVE: active_id's instance; ACTIVATE/CLOSE_TASK/END: app_id's. */
	uint32_t token;
	/* TASKS: false queries; true supplies the complete snapshot. */
	bool has_tasks;
	unsigned int task_count;
	struct fplinux_ui_session_task tasks[FPLINUX_UI_SESSION_TASK_MAX];
};

/* connect sends HELLO and returns a nonblocking, close-on-exec descriptor. */
int fplinux_ui_session_connect(const char *app_id);
int fplinux_ui_session_connect_at(const char *path, const char *app_id);
bool fplinux_ui_session_send(int fd,
			     const struct fplinux_ui_session_message *message);
/*
 * Returns 1 for a packet, 0 for EAGAIN, -1 for a disconnect or invalid packet.
 * A successful packet with has_frame transfers frame_fd ownership to the
 * caller. Rejected packets close every attached descriptor internally.
 */
int fplinux_ui_session_receive(int fd,
			       struct fplinux_ui_session_message *message);

#endif
