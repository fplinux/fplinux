/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_SESSION_H
#define FPLINUX_UI_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "fplinux-storage.h"
#include "fplinux-ui-document.h"
#include "fplinux-ui-vibration.h"
#include "fplinux-volumes.h"

#define FPLINUX_UI_SESSION_SOCKET "/run/fplinux-gui/control.sock"
/* The logical Wayland seat assigned to the phone's built-in keypad. */
#define FPLINUX_UI_KEYPAD_SEAT "fplinux-keypad"
#define FPLINUX_UI_SESSION_ID_BYTES 128
#define FPLINUX_UI_SESSION_TEXT_BYTES 128
#define FPLINUX_UI_SESSION_CLOCK_BYTES 16384
/* Clock text may double when quotes and backslashes are escaped. */
#define FPLINUX_UI_SESSION_PACKET_BYTES \
	(2 * FPLINUX_UI_SESSION_CLOCK_BYTES + 4096)
#define FPLINUX_UI_SESSION_LOCALE_BYTES 8
#define FPLINUX_UI_SESSION_NAME_BYTES 32
#define FPLINUX_UI_SESSION_FRAME_MAX_WIDTH 320
#define FPLINUX_UI_SESSION_FRAME_MAX_HEIGHT 320
/* Portrait and landscape share the same bounded RGB565 pixel budget. */
#define FPLINUX_UI_SESSION_FRAME_MAX_PIXELS (240U * 320U)
#define FPLINUX_UI_SESSION_TASK_MAX 4
#define FPLINUX_UI_SESSION_PATTERN FPLINUX_UI_VIBRATION_PATTERN_MAX
#define FPLINUX_UI_SESSION_PATH_BYTES 4096
#define FPLINUX_UI_SESSION_DOCUMENT_CHOICE_MAX 32

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
	FPLINUX_UI_SESSION_REDRAW_ACK,
	FPLINUX_UI_SESSION_CLOCK,
	FPLINUX_UI_SESSION_VIBRATE,
	FPLINUX_UI_SESSION_PREPARE_CLOSE,
	FPLINUX_UI_SESSION_CLOSE_REPLY,
	FPLINUX_UI_SESSION_CLOSE_ACK,
	FPLINUX_UI_SESSION_ACTIVATION,
	FPLINUX_UI_SESSION_ACTIVATION_ACK,
	FPLINUX_UI_SESSION_OPEN_FILE,
	FPLINUX_UI_SESSION_DOCUMENT_CHOICES,
	FPLINUX_UI_SESSION_DOCUMENT_CHOICE,
	FPLINUX_UI_SESSION_DOCUMENT,
	FPLINUX_UI_SESSION_DOCUMENT_RESULT,
	FPLINUX_UI_SESSION_STORAGE_REQUEST,
	FPLINUX_UI_SESSION_STORAGE_RESULT,
};

struct fplinux_ui_session_document_choice {
	char app_id[FPLINUX_UI_SESSION_ID_BYTES];
	char title[FPLINUX_UI_SESSION_TEXT_BYTES];
	char icon[FPLINUX_UI_SESSION_TEXT_BYTES];
};

enum fplinux_ui_session_activation {
	FPLINUX_UI_SESSION_ACTIVATION_OPEN,
	FPLINUX_UI_SESSION_ACTIVATION_RESUME,
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

struct fplinux_ui_session_clock_presentation {
	bool analog;
	/* The nearest alarm uses UTC, or a monotonic snooze deadline. */
	int64_t alarm_utc;
	uint64_t snooze_deadline_ms;
	char alarm_label[FPLINUX_UI_SESSION_TEXT_BYTES + 1];
	/* Running and paused timers are mutually exclusive; zero means absent. */
	uint64_t timer_deadline_ms;
	uint64_t timer_remaining_ms;
	uint64_t missed_total;
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
 * HOME backgrounds the foreground application. EXIT requests ordinary close
 * and a return to the catalogue; a cancelled close keeps the application.
 * LIFECYCLE reports foreground, background or closing independently of surface
 * VISIBILITY. PREPARE_CLOSE(serial, reason) starts an ordinary user close.
 * CLOSE_REPLY returns the same serial with value ready, cancel or wait. Wait
 * leaves the application interactive for its decision. Repeated Close keeps
 * that decision. CLOSE_ACK confirms the decision frame after its commit and
 * following Wayland sync. An unanswered preparation has a bounded timeout.
 * CLOSE is final, including forced termination, and cannot be cancelled.
 * MODE requests integrated or fullscreen mode.
 * BUSY(enabled, title) extends launch progress through a blocking operation.
 * Ordinary input arrives through Wayland keyboard events.
 * KEEP_SCREEN_ON applies only while the requesting application is visible.
 *
 * SETTING(name, value) belongs to org.fplinux.settings: empty value queries,
 * otherwise it sets a value. The manager replies with the applied SETTING or
 * ERROR. Reply applied records whether the value already took effect;
 * storage_state and storage_error report publication and synchronization
 * separately. An applied ERROR still supplies the current value.
 * input_locale selects the keypad language independently of locale.
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
 * cannot affect a restarted application. CLOSE_TASK starts PREPARE_CLOSE;
 * a waiting background task becomes visible to present its decision.
 * LAUNCH requests OPEN; ACTIVATE and an internal close-decision return request
 * RESUME. ACTIVATION(serial, activation) follows CONFIG on cold start. Its
 * ACTIVATION_ACK follows a new application frame's commit and Wayland sync;
 * an older retained buffer or the launcher's ACTIVE_ACK cannot reveal it.
 *
 * OPEN_FILE(serial, path) belongs to the foreground application instance.
 * DOCUMENT_CHOICES returns installed candidates; DOCUMENT_CHOICE selects one
 * app_id or cancels with an empty app_id. Cold and retained handlers receive
 * DOCUMENT after CONFIG, independently of OPEN and RESUME. DOCUMENT_RESULT
 * carries accepted, wait, cancelled, error (with positive document_error), or
 * restart-required. WAIT's decision frame uses ACTIVATION_ACK; only the UI
 * runtime sends accepted after the new document frame's commit and sync.
 * The manager translates handler serials to the origin's request serial.
 * restart-required uses one ordinary close decision and at most one restart.
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
	char locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char input_locale[FPLINUX_UI_SESSION_LOCALE_BYTES];
	char name[FPLINUX_UI_SESSION_NAME_BYTES];
	char value[FPLINUX_UI_SESSION_TEXT_BYTES];
	int battery;
	bool charging;
	char notice[FPLINUX_UI_SESSION_NAME_BYTES];
	/* ACTIVE/ACK, ACTIVATION/ACK, CLOCK, settings/ACK or close preparation. */
	uint32_t serial;
	/* LAUNCH: one tightly packed RGB565 SCM_RIGHTS fd. */
	bool has_frame;
	int frame_fd;
	/* LIFECYCLE only: independent of display visibility. */
	enum fplinux_ui_session_lifecycle lifecycle;
	/* ACTIVE: active_id's instance; ACTIVATE/CLOSE_TASK: app_id's. */
	uint32_t token;
	/* TASKS: false queries; true supplies the complete snapshot. */
	bool has_tasks;
	unsigned int task_count;
	struct fplinux_ui_session_task tasks[FPLINUX_UI_SESSION_TASK_MAX];
	/* CLOCK or ACTIVE: control-free text interpreted by the clock owner. */
	char clock_data[FPLINUX_UI_SESSION_CLOCK_BYTES];
	/* ACTIVE: stable Home state; countdowns use CLOCK_MONOTONIC locally. */
	struct fplinux_ui_session_clock_presentation clock_presentation;
	/* CONFIG: capabilities available to this application. */
	bool audio;
	bool vibration;
	/* VIBRATE: pause, pulse, pause, pulse... in ms; [] stops. */
	unsigned int pattern_count;
	unsigned int pattern[FPLINUX_UI_SESSION_PATTERN];
	bool repeat;
	/* SETTING/ERROR replies: owner acceptance is separate from file durability. */
	bool applied;
	enum fplinux_storage_state storage_state;
	int storage_error;
	/* ACTIVATION only: independent of lifecycle and surface visibility. */
	enum fplinux_ui_session_activation activation;
	/* Document serials name the request on their respective connection. */
	char path[FPLINUX_UI_SESSION_PATH_BYTES];
	enum fplinux_ui_document_status document_status;
	int document_error;
	unsigned int document_choice_count;
	struct fplinux_ui_session_document_choice
		document_choices[FPLINUX_UI_SESSION_DOCUMENT_CHOICE_MAX];
	/* Storage requests contain an opaque ID; only results contain paths. */
	enum fplinux_volume_operation volume_operation;
	uint32_t volume_id;
	struct fplinux_volume_result volume_result;
};

struct fplinux_ui;

/* Raw system-app requests; ordinary applications use the typed UI APIs. */
bool fplinux_ui_send(struct fplinux_ui *ui,
		     const struct fplinux_ui_session_message *message);

#endif
