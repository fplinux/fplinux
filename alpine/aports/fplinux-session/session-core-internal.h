/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_CORE_INTERNAL_H
#define FPLINUX_SESSION_CORE_INTERNAL_H

#include "session-core.h"

#include "fplinux-brightness-client.h"
#include "fplinux-keypad-light.h"
#include "session-audio.h"
#include "session-battery.h"
#include "session-clock.h"
#include "session-settings.h"
#include "session-storage.h"
#include "session-vibration.h"
#include "session-vibrator.h"

#define FPLINUX_SESSION_PEERS 8

struct process {
	struct fplinux_session_process policy;
	/* The only UID whose connection from this PID is accepted. */
	uid_t uid;
	int pid_fd;
	int control_fd;
	uint64_t started_ms;
	uint64_t closing_ms;
	uint32_t close_serial;
	uint64_t close_requested_ms;
	/* WAIT keeps the client's decision open across Home and repeated Close. */
	bool close_waiting;
	bool close_activate;
	bool close_return_tasks;
	bool killed;
	/* The control connection ended before the manager requested close. */
	bool disconnected;
	/* Startup timed out, or the launcher requested an unsolicited exit. */
	bool failed;
	/* Cancellation is an ordinary requested close, even after a hangup. */
	bool cancelled;
	/* The process identified itself at least once. */
	bool connected;
	bool keep_screen_on;
	bool background_audio;
	bool light_effects;
	enum fplinux_ui_session_playback playback;
	/* Home retains the task after the usual launcher frame handoff. */
	bool park_on_leave;
	/* Visibility as the client last learned it; the runtime starts visible. */
	bool visible;
	/* Local legends in the client's last committed frame. */
	bool local_softkeys;
	/* The client has committed a buffer. */
	bool drawn;
	/* TITLE owns opaque application text; otherwise desktop metadata owns it. */
	bool title_supplied;
	/* A live task instance to resume after ordinary foreground close. */
	uint32_t return_origin;
	/* A lost reply may retry the last successful mutation with its serial. */
	uint32_t clock_serial;
	struct clock_request clock_request;
	/*
	 * A wake showed the client again: the display lights once a buffer it
	 * committed since then is in a presented output frame. wake_committed
	 * says that it has committed one.
	 */
	bool wake_due;
	bool wake_committed;
	/*
	 * EXIT or Home left the shown application: it stays on screen without
	 * keys until the launcher has drawn the view that replaces it.
	 */
	bool leaving;
	/* When the application drew its first buffer or started leaving. */
	uint64_t handoff_ms;
	/*
	 * The mode of the desktop entry and the mode that the last MODE asked
	 * for, at mode_ms; see switch_mode(). Once the application has asked for
	 * another mode, its buffers place it; see place_app().
	 */
	enum fplinux_ui_session_mode declared_mode;
	enum fplinux_ui_session_mode requested_mode;
	uint64_t mode_ms;
	bool placed_by_buffer;
	char left[FPLINUX_UI_SESSION_TEXT_BYTES];
	char centre[FPLINUX_UI_SESSION_TEXT_BYTES];
	char right[FPLINUX_UI_SESSION_TEXT_BYTES];
};

struct peer {
	int fd;
	pid_t pid;
	uid_t uid;
	uint64_t accepted_ms;
	bool authenticated;
};

struct wake_submission {
	uint64_t serial;
	pid_t launcher_pid;
	pid_t app_pid;
	uint32_t active_serial;
	bool ready;
};

struct document_request {
	uint32_t request_id;
	uint32_t serial;
	uint32_t origin_token;
	uint32_t handler_token;
	/* A self-open inherits the caller's previous return target. */
	uint32_t self_return_origin;
	char handler_id[FPLINUX_UI_SESSION_ID_BYTES];
	char path[FPLINUX_UI_SESSION_PATH_BYTES];
	bool waiting;
	bool restarted;
	bool restarting;
	bool detached;
	bool self_target;
	bool system_handler;
};

struct setting_request {
	pid_t pid;
	uint32_t token;
	uint32_t serial;
	unsigned int level;
	unsigned int restore_level;
	int audio_fd;
	bool pending;
	bool save;
	bool preview;
	bool feedback;
	bool step;
	enum fplinux_ui_setting_kind kind;
	enum fplinux_audio_route route;
};

struct volume_control {
	struct setting_request request;
	/* Latest application step behind an explicit editor save. */
	struct setting_request step_request;
	bool preview_active;
	bool preview_launcher;
	unsigned int preview_restore;
	/* Key steps accepted while a client's save or rollback awaits its ACK. */
	bool key_pending;
	unsigned int key_level;
};

struct volume_keys {
	bool held[2];
	unsigned int repeat_code;
	uint64_t repeat_ms;
	pid_t focus_pid;
	bool quick_settings;
	bool preview;
	enum fplinux_session_audio_class class;
};

struct fplinux_session_volume {
	struct volume_control controls[FPLINUX_SESSION_AUDIO_CLASSES];
	struct volume_keys keys[FPLINUX_SESSION_KEY_SOURCE_COUNT];
	struct fplinux_session_indicator indicator;
	struct fplinux_session_indicator last_indicator;
	uint64_t indicator_deadline_ms;
	bool indicator_active;
	bool indicator_alarm_keys;
	pid_t indicator_focus_pid;
};

enum held_scene {
	HELD_SCENE_NONE,
	HELD_SCENE_LAUNCH = 1U << 0,
	HELD_SCENE_HOME = 1U << 1,
	HELD_SCENE_APPEARANCE = 1U << 2,
	HELD_SCENE_CLOSE = 1U << 3,
};

struct fplinux_session {
	struct fplinux_session_options options;
	/* The output size and its profile, which place every surface. */
	int output_width;
	int output_height;
	const struct fplinux_ui_profile_spec *profile;
	int epoll_fd;
	int listener_fd;
	int timer_fd;
	int alarm_fd;
	int64_t alarm_due_utc;
	struct process launcher;
	/* Stable records preserve peer and pidfd ownership across activation. */
	struct process tasks[FPLINUX_UI_SESSION_TASK_MAX];
	uint32_t next_task_token;
	uint32_t next_close_serial;
	struct document_request document;
	struct fplinux_session_bluetooth *bluetooth;
	struct fplinux_ui_bluetooth_prompt bluetooth_prompt;
	uint32_t bluetooth_drawn_id;
	enum fplinux_ui_bluetooth_phase bluetooth_drawn_phase;
	uint32_t bluetooth_hidden_id;
	char bluetooth_received_path[FPLINUX_UI_SESSION_PATH_BYTES];
	char bluetooth_handler_id[FPLINUX_UI_SESSION_ID_BYTES];
	bool bluetooth_ready;
	struct fplinux_session_storage *storage;
	struct fplinux_volume_snapshot storage_snapshot;
	uint32_t storage_origin_token;
	int storage_origin_fd;
	uint32_t return_token;
	unsigned int held_scene;
	pid_t close_pid;
	uint32_t close_token;
	uint32_t close_scene_serial;
	bool close_frame_ready;
	uint32_t visual_serial;
	uint32_t redraw_serial;
	pid_t redraw_pid;
	uint32_t redraw_token;
	bool redraw_ready;
	struct process idle_app;
	struct process *app;
	struct peer peers[FPLINUX_SESSION_PEERS];
	char pending_id[FPLINUX_UI_SESSION_ID_BYTES];
	char selected_id[FPLINUX_UI_SESSION_ID_BYTES];
	/* LAUNCH through first buffer, extended by an explicit application BUSY. */
	char launch_id[FPLINUX_UI_SESSION_ID_BYTES];
	char launch_title[FPLINUX_UI_SESSION_TEXT_BYTES];
	uint64_t launch_ms;
	bool launch_starting;
	bool app_busy;
	/* BUSY(false) waits for the application's next usable committed buffer. */
	bool app_busy_finishing;
	uint32_t launch_generation;
	enum fplinux_ui_session_activation launch_activation;
	bool launch_retained;
	bool activation_ready;
	/* Last shell visibility, so crossing the delay requests a repaint. */
	bool launch_shown;
	/* Home notice code shown when no application is active. */
	char error[FPLINUX_UI_SESSION_TEXT_BYTES];
	unsigned int launcher_failures;
	/*
	 * The launcher's view, Home or the catalogue, and the catalogue's list
	 * presentation; a restarted launcher opens them again.
	 */
	bool launcher_home_view;
	bool launcher_list;
	/* Tasks temporarily covers the remembered launcher view. */
	bool launcher_tasks_view;
	/* A completed Tasks close still needs its launcher presentation. */
	bool launcher_restore_tasks;
	/* So does Quick settings, which may preview and save the brightness. */
	bool launcher_quick_settings_view;
	bool ready;
	bool destroying;
	bool owns_socket;
	struct fplinux_session_settings settings;
	struct fplinux_storage_result settings_storage;
	uint64_t settings_retry_ms;
	unsigned int settings_retry_delay_ms;
	struct fplinux_session_power power;
	bool was_locked;
	/* The shell's placement and what the user sees, as last announced. */
	bool was_raised;
	bool was_on_top;
	/* The shell placed the launcher's view above the applications. */
	bool launcher_shown;
	/*
	 * What the launcher's frame shows, which an application's swap waits
	 * for. The launcher answers each ACTIVE with ACTIVE_ACK after a new
	 * commit and Wayland sync. named_app is whether the last ACTIVE named an
	 * application, wait_serial is that ACTIVE's serial until its answer
	 * arrives, and draws_app is what the last matching ACK drew.
	 */
	bool launcher_named_app;
	uint32_t launcher_wait_serial;
	/* A handoff timeout may end its wait without confirming a drawn view. */
	uint32_t launcher_ack_serial;
	bool launcher_draws_app;
	/* The last ACTIVE sent to the running launcher. */
	struct fplinux_ui_session_message last_active;
	uint64_t last_active_clock_event;
	bool last_active_sent;
	/* The display as last applied; a wake from OFF lights after a frame. */
	enum fplinux_ui_session_display lit_display;
	bool light_pending;
	struct wake_submission wake_submission;
	bool active_known;
	enum fplinux_ui_session_display active_display;
	bool active_locked;
	enum fplinux_session_notice active_notice;
	int active_battery;
	bool active_charging;
	enum fplinux_session_battery_alert active_battery_notice;
	struct fplinux_brightness_client brightness;
	unsigned int user_level;
	/*
	 * A preview level is shown; restore_level was the level before. The
	 * launcher's Quick settings, rather than Settings, owns preview_launcher.
	 */
	bool preview_active;
	bool preview_launcher;
	unsigned int preview_restore_level;
	bool brightness_leased;
	int brightness_shown;
	/*
	 * user_level must be SET on the next connection: the saved level at
	 * start, or a preview's restore level that the service missed.
	 */
	bool brightness_pending_set;
	bool brightness_failed;
	bool brightness_known;
	unsigned int brightness_applied;
	uint64_t brightness_retry_ms;
	struct fplinux_keypad_light keypad_light;
	bool led_open;
	int led_state;
	pid_t light_effect_pid;
	uint32_t light_effect_token;
	uint32_t light_effect_serial;
	struct fplinux_ui_light_effect light_effect;
	bool charger_known;
	bool charger_online;
	uint64_t charger_poll_ms;
	/*
	 * The manager owns the battery level and its alerts, so they survive
	 * launcher restarts and are watched while no launcher runs. The
	 * notice waits for the launcher's DISMISS.
	 */
	struct fplinux_session_battery battery;
	struct fplinux_session_battery_alerts battery_alerts;
	enum fplinux_session_battery_alert battery_notice;
	bool battery_notice_drawn;
	uint64_t battery_poll_ms;
	struct fplinux_session_clock clock;
	bool clock_ready;
	uint64_t clock_wake_event;
	uint64_t clock_drawn_event;
	bool clock_display_suppressed;
	uint64_t clock_key_events[FPLINUX_SESSION_KEY_SOURCE_COUNT][6];
	bool clock_blocked_keys[FPLINUX_SESSION_KEY_SOURCE_COUNT][KEY_CNT];
	pid_t clock_signal_pid;
	int clock_signal_fd;
	uint64_t clock_signal_event;
	uint64_t clock_signal_stop_ms;
	bool clock_signal_stopping;
	struct fplinux_session_audio audio;
	bool has_audio;
	struct fplinux_session_volume volume;
	struct fplinux_system_levels system_levels;
	struct setting_request output_request;
	enum fplinux_audio_route output_restore;
	struct setting_request capabilities_request;
	int audio_status_fd;
	uint64_t audio_status_deadline_ms;
	bool audio_speaker;
	bool audio_headphones;
	bool audio_capabilities_known;
	struct fplinux_session_wireless *wireless;
	bool wireless_initialized;
	struct setting_request wireless_request;
	struct setting_request wifi_request;
	uint32_t wifi_ticket;
	struct fplinux_ui_wifi_network wifi_notice;
	uint64_t wifi_notice_deadline_ms;
	struct fplinux_session_vibration vibration;
	struct fplinux_session_vibrator vibrator;
	bool has_vibrator;
	bool vibrating;
	pid_t vibration_pid;
	/* Right soft key was consumed while cancelling a launch. */
	bool launch_right_held;
	/* Escape was consumed by the native launch modal, including its release. */
	bool launch_escape_held;
};

uint64_t fplinux_session_now_ms(const struct fplinux_session *session);
bool fplinux_session_copy_value(char *destination, size_t capacity,
				const char *value);

bool fplinux_session_clock_keys_eligible(const struct fplinux_session *session);
bool fplinux_session_quick_settings_shown(const struct fplinux_session *session);
void fplinux_session_save_settings(struct fplinux_session *session);
struct process *
fplinux_session_setting_requester(struct fplinux_session *session,
				  const struct setting_request *request);
void fplinux_session_remember_setting_request(
	struct setting_request *request, const struct process *process,
	const struct fplinux_ui_session_message *message);
void fplinux_session_setting_error(
	struct process *process,
	const struct fplinux_ui_session_message *request, const char *reason);
void fplinux_session_setting_reply(struct fplinux_session *session,
				   struct process *process,
				   enum fplinux_ui_setting_kind kind,
				   union fplinux_ui_setting_value value,
				   uint32_t serial, bool mutation);

bool fplinux_session_volume_setting(
	struct fplinux_session *session, struct process *process,
	const struct fplinux_ui_session_message *message);
void fplinux_session_volume_load(struct fplinux_session *session);
void fplinux_session_volume_read_levels(struct fplinux_session *session,
					struct fplinux_system_levels *levels);
void fplinux_session_volume_finish_requests(struct fplinux_session *session);
void fplinux_session_volume_end_app_previews(struct fplinux_session *session);
void fplinux_session_volume_end_quick_settings_preview(
	struct fplinux_session *session);
void fplinux_session_volume_end_previews(struct fplinux_session *session);
void fplinux_session_volume_repeat_keys(struct fplinux_session *session,
					uint64_t time, uint64_t *earliest);
bool fplinux_session_volume_key(struct fplinux_session *session,
				enum fplinux_session_key_source source,
				unsigned int code, bool pressed, bool deliver);
void fplinux_session_volume_cancel_input(struct fplinux_session *session,
					 enum fplinux_session_key_source source);
/* Reports a presentation change so Session can invalidate the shell. */
bool fplinux_session_volume_advance_indicator(struct fplinux_session *session,
					      uint64_t time, uint64_t *earliest,
					      bool notice, bool softkeys);

#endif
