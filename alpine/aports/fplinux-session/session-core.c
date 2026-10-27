/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-core.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/rtc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fplinux-brightness-client.h"
#include "fplinux-desktop-entry.h"
#include "fplinux-keypad-light.h"
#include "fplinux-keypad.h"
#include "fplinux-ui-profile.h"
#include "session-audio.h"
#include "session-battery.h"
#include "session-clock.h"
#include "session-power.h"
#include "session-settings.h"
#include "session-storage.h"
#include "session-vibration.h"
#include "session-vibrator.h"

#define FPLINUX_SESSION_PEERS 8
#define FPLINUX_SESSION_LAUNCHER FPLINUX_DESKTOP_LAUNCHER_ID
#define FPLINUX_SESSION_SETTINGS "org.fplinux.settings"
#define FPLINUX_SESSION_CLOCK "org.fplinux.clock"
#define FPLINUX_SESSION_PROGRESS_MS 200U
#define FPLINUX_SESSION_PROCESS_TICK_MS 100U
#define FPLINUX_SESSION_STARTUP_MS 10000U
/* How long a swap waits for the launcher's frame; see hand_off(). */
#define FPLINUX_SESSION_HANDOFF_MS 500U
#define FPLINUX_SESSION_CHARGER_POLL_MS 2000U
#define FPLINUX_SESSION_BRIGHTNESS_RETRY_MS 2000U
#define FPLINUX_SESSION_BRIGHTNESS_TIMEOUT_MS 250
#define FPLINUX_SESSION_CLOSE_TERM_MS 500U
#define FPLINUX_SESSION_CLOSE_KILL_MS 1000U
#define FPLINUX_SESSION_PREPARE_CLOSE_MS 3000U
#define FPLINUX_SESSION_SETTINGS_RETRY_MS 2000U
#define FPLINUX_SESSION_SETTINGS_RETRY_MAX_MS 30000U

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
	/* Home retains the task after the usual launcher frame handoff. */
	bool park_on_leave;
	/* Visibility as the client last learned it; the runtime starts visible. */
	bool visible;
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
	char handler_id[FPLINUX_UI_SESSION_ID_BYTES];
	char path[FPLINUX_UI_SESSION_PATH_BYTES];
	bool waiting;
	bool restarted;
	bool restarting;
	bool detached;
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
	/* A Settings preview level is shown; restore_level was the level before. */
	bool preview_active;
	unsigned int preview_restore_level;
	bool brightness_leased;
	int brightness_shown;
	/*
	 * user_level must be SET on the next connection: the saved level at
	 * start, or a preview's restore level that the service missed.
	 */
	bool brightness_pending_set;
	bool brightness_failed;
	uint64_t brightness_retry_ms;
	struct fplinux_keypad_light keypad_light;
	bool led_open;
	int led_state;
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

static uint64_t system_ms(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static uint64_t now_ms(const struct fplinux_session *session)
{
	if (session->options.clock_ms)
		return session->options.clock_ms(session->options.context);
	return system_ms();
}

static int64_t wall_utc(const struct fplinux_session *session)
{
	return session->options.clock_utc ?
		       session->options.clock_utc(session->options.context) :
		       (int64_t)time(NULL);
}

static void changed(struct fplinux_session *session)
{
	if (!session->destroying && session->options.changed)
		session->options.changed(session->options.context);
}

static bool add_fd(struct fplinux_session *session, int fd)
{
	struct epoll_event event = { .events = EPOLLIN, .data.fd = fd };

	return epoll_ctl(session->epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}

static void remove_fd(struct fplinux_session *session, int fd)
{
	if (fd < 0)
		return;
	epoll_ctl(session->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	close(fd);
}

static bool copy_value(char *destination, size_t capacity, const char *value)
{
	size_t bytes = strlen(value);

	if (bytes >= capacity)
		return false;
	memcpy(destination, value, bytes + 1);
	return true;
}

static bool read_entry(struct fplinux_session *session, const char *id,
		       struct fplinux_desktop_entry *entry)
{
	return fplinux_desktop_entry_read(session->options.applications, id,
					  session->settings.locale,
					  session->options.metadata_uid, entry,
					  NULL, NULL) &&
	       !entry->hidden;
}

static struct process *process_for_pid(struct fplinux_session *session,
				       pid_t pid)
{
	unsigned int i;

	if (pid > 0 && session->launcher.policy.pid == pid)
		return &session->launcher;
	for (i = 0; pid > 0 && i < FPLINUX_UI_SESSION_TASK_MAX; ++i)
		if (session->tasks[i].policy.pid == pid)
			return &session->tasks[i];
	return NULL;
}

static struct process *task_for_id(struct fplinux_session *session,
				   const char *id)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_UI_SESSION_TASK_MAX; ++i)
		if (session->tasks[i].policy.pid &&
		    !strcmp(session->tasks[i].policy.app_id, id))
			return &session->tasks[i];
	return NULL;
}

static struct process *free_task(struct fplinux_session *session)
{
	unsigned int i;

	for (i = 0; i < FPLINUX_UI_SESSION_TASK_MAX; ++i)
		if (!session->tasks[i].policy.pid)
			return &session->tasks[i];
	return NULL;
}

static struct process *task_for_token(struct fplinux_session *session,
				      uint32_t token)
{
	for (unsigned int i = 0; token && i < FPLINUX_UI_SESSION_TASK_MAX; ++i)
		if (session->tasks[i].policy.pid &&
		    session->tasks[i].policy.token == token)
			return &session->tasks[i];
	return NULL;
}

static bool app_in_foreground(const struct fplinux_session *session);

static void storage_reply(struct process *process, uint32_t request_id,
			  int error,
			  const struct fplinux_volume_snapshot *snapshot)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_STORAGE_RESULT,
		.serial = request_id,
		.volume_result = {
			.request_id = request_id,
			.error = error,
			.snapshot = *snapshot,
		},
	};

	if (process && process->control_fd >= 0 && !process->policy.closing)
		fplinux_ui_session_send(process->control_fd, &message);
}

static bool ensure_storage(struct fplinux_session *session)
{
	struct fplinux_session_storage_config config = { 0 };
	int error;

	if (session->storage)
		return true;
	if (session->options.storage)
		config = *session->options.storage;
	config.uid = session->options.app_uid;
	config.gid = session->options.app_gid;
	config.home_path = session->options.home;
	session->storage = fplinux_session_storage_create(&config);
	if (!session->storage)
		return false;
	if (add_fd(session, fplinux_session_storage_fd(session->storage)))
		return true;
	error = errno;
	fplinux_session_storage_destroy(session->storage);
	session->storage = NULL;
	errno = error;
	return false;
}

static void storage_request(struct fplinux_session *session,
			    struct process *process,
			    const struct fplinux_ui_session_message *message)
{
	int error = 0;

	if (process->policy.closing ||
	    (message->volume_operation != FPLINUX_VOLUME_REFRESH &&
	     (process != session->app || !app_in_foreground(session) ||
	      session->power.locked)))
		error = EPERM;
	else if (!ensure_storage(session) ||
		 fplinux_session_storage_request(
			 session->storage, message->volume_operation,
			 message->volume_id, message->serial) < 0)
		error = errno;
	if (error) {
		storage_reply(process, message->serial, error,
			      &session->storage_snapshot);
		return;
	}
	session->storage_origin_token = process->policy.token;
	session->storage_origin_fd = process->control_fd;
}

static void dispatch_storage(struct fplinux_session *session)
{
	struct fplinux_volume_result result;
	struct process *origin;

	if (!session->storage ||
	    fplinux_session_storage_take_result(session->storage, &result) != 1)
		return;
	session->storage_snapshot = result.snapshot;
	origin = task_for_token(session, session->storage_origin_token);
	if (origin && origin->control_fd == session->storage_origin_fd)
		storage_reply(origin, result.request_id, result.error,
			      &result.snapshot);
	session->storage_origin_token = 0;
	session->storage_origin_fd = -1;
}

static void document_result(struct fplinux_session *session,
			    enum fplinux_ui_document_status status, int error)
{
	struct document_request *request = &session->document;
	struct process *origin = task_for_token(session, request->origin_token);
	/* The idle slot owns the handoff until a handler has been started. */
	struct process *handler =
		request->handler_token ?
			task_for_token(session, request->handler_token) :
			&session->idle_app;
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_DOCUMENT_RESULT,
		.serial = request->request_id,
		.document_status = status,
		.document_error = error,
	};

	if (!request->request_id)
		return;
	if (origin && origin->control_fd >= 0 && !origin->policy.closing)
		fplinux_ui_session_send(origin->control_fd, &message);
	if (status == FPLINUX_UI_DOCUMENT_WAIT)
		return;
	if (status != FPLINUX_UI_DOCUMENT_ACCEPTED && origin &&
	    ((session->app == handler && origin != session->app) ||
	     (session->launch_starting && request->serial &&
	      request->serial == session->launch_generation)))
		session->return_token = origin->policy.token;
	memset(request, 0, sizeof(*request));
}

static void document_launch_result(struct fplinux_session *session,
				   enum fplinux_ui_document_status status,
				   int error)
{
	if (session->document.request_id && session->document.serial &&
	    session->document.serial == session->launch_generation &&
	    !strcmp(session->document.handler_id, session->launch_id))
		document_result(session, status, error);
}

static bool document_delivered(struct fplinux_session *session)
{
	struct process *handler =
		task_for_token(session, session->document.handler_token);

	return handler && handler->connected;
}

/* The launcher is separate from the bounded application registry. */
static struct process *process_at(struct fplinux_session *session,
				  unsigned int index)
{
	return index ? &session->tasks[index - 1] : &session->launcher;
}

static void send_tasks(struct fplinux_session *session)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_TASKS,
		.has_tasks = true,
	};

	if (session->launcher.control_fd < 0 || session->destroying)
		return;
	for (unsigned int i = 0; i < FPLINUX_UI_SESSION_TASK_MAX; ++i) {
		const struct fplinux_session_process *policy =
			&session->tasks[i].policy;
		struct fplinux_ui_session_task *task;

		if (!policy->pid)
			continue;
		task = &message.tasks[message.task_count++];
		copy_value(task->app_id, sizeof(task->app_id), policy->app_id);
		copy_value(task->title, sizeof(task->title),
			   policy->title[0] ? policy->title : policy->app_id);
		task->lifecycle = policy->lifecycle;
		task->token = policy->token;
	}
	fplinux_ui_session_send(session->launcher.control_fd, &message);
}

static void set_lifecycle(struct fplinux_session *session,
			  struct process *process,
			  enum fplinux_ui_session_lifecycle lifecycle)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
		.lifecycle = lifecycle,
	};

	if (process == &session->launcher || !process->policy.pid ||
	    process->policy.lifecycle == lifecycle)
		return;
	process->policy.lifecycle = lifecycle;
	if (process->control_fd >= 0)
		fplinux_ui_session_send(process->control_fd, &message);
	send_tasks(session);
}

/* A leaving application is still shown but no longer gets keys or focus. */
static bool app_in_foreground(const struct fplinux_session *session)
{
	return session->app && session->app->policy.pid &&
	       !session->app->policy.closing && !session->app->leaving;
}

/*
 * ACTIVE names the application only once it has a buffer, so that the
 * launcher keeps its catalogue on screen until the application can replace
 * it.
 */
static bool app_announced(const struct fplinux_session *session)
{
	return app_in_foreground(session) && session->app->drawn &&
	       session->app->policy.ready &&
	       (!session->launch_starting || session->activation_ready);
}

static bool is_running(const struct process *process)
{
	return process->policy.pid && !process->policy.closing;
}

static bool launch_in_progress(const struct fplinux_session *session)
{
	return session->launch_starting || session->app_busy ||
	       session->app_busy_finishing;
}

static void release_scene(struct fplinux_session *session, unsigned int scenes)
{
	unsigned int previous = session->held_scene;

	session->held_scene &= ~scenes;
	if (previous && !session->held_scene && session->options.hold_frame)
		session->options.hold_frame(session->options.context, false);
	if (scenes & HELD_SCENE_APPEARANCE) {
		session->redraw_serial = 0;
		session->redraw_pid = 0;
		session->redraw_token = 0;
		session->redraw_ready = false;
	}
	if (scenes & HELD_SCENE_CLOSE) {
		session->close_pid = 0;
		session->close_token = 0;
		session->close_scene_serial = 0;
		session->close_frame_ready = false;
	}
}

static void clear_held_scene(struct fplinux_session *session)
{
	release_scene(session, HELD_SCENE_LAUNCH | HELD_SCENE_HOME |
				       HELD_SCENE_APPEARANCE |
				       HELD_SCENE_CLOSE);
}

static void hold_scene(struct fplinux_session *session, enum held_scene scene)
{
	if (session->power.locked ||
	    session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF)
		return;
	if (!session->held_scene && session->options.hold_frame)
		session->options.hold_frame(session->options.context, true);
	session->held_scene |= scene;
}

/*
 * A system dialog is due while unlocked with the display on or dimmed; the
 * lock screen shows no notices and nothing is drawn while the display is off.
 */
static bool notice_due(const struct fplinux_session *session)
{
	return session->battery_notice != FPLINUX_SESSION_BATTERY_ALERT_NONE &&
	       !session->power.locked &&
	       session->power.display != FPLINUX_UI_SESSION_DISPLAY_OFF;
}

/*
 * The shell raises the launcher above the foreground application for a due
 * notice once the launcher has drawn the current dialog, so the application
 * keeps its frame and keys until the replacement is ready.
 */
static bool notice_raises_launcher(const struct fplinux_session *session)
{
	const struct process *launcher = &session->launcher;

	return notice_due(session) &&
	       (app_in_foreground(session) || launch_in_progress(session)) &&
	       is_running(launcher) && launcher->control_fd >= 0 &&
	       launcher->policy.ready && launcher->policy.framed &&
	       session->battery_notice_drawn;
}

/*
 * The notice covers the application only once the shell reports the
 * launcher's view above it: until then the application stays visible and
 * keeps the keys, so a raise that did not happen never takes them.
 */
static bool notice_covers_app(const struct fplinux_session *session)
{
	return notice_raises_launcher(session) && session->launcher_shown;
}

static bool clock_due(const struct fplinux_session *session)
{
	return fplinux_session_clock_ringing(&session->clock);
}

static bool clock_raises_launcher(const struct fplinux_session *session)
{
	const struct process *launcher = &session->launcher;

	return clock_due(session) && is_running(launcher) &&
	       launcher->control_fd >= 0 && launcher->policy.ready &&
	       launcher->policy.framed &&
	       session->clock_drawn_event == session->clock.model.active.id;
}

static bool clock_covers_app(const struct fplinux_session *session)
{
	return clock_raises_launcher(session) && session->launcher_shown;
}

static void clock_power(struct fplinux_session *session, uint64_t time)
{
	uint64_t event = session->clock.model.active.id;

	if (!clock_due(session)) {
		session->clock_wake_event = 0;
		session->clock_display_suppressed = false;
		return;
	}
	if (event != session->clock_wake_event) {
		session->clock_wake_event = event;
		session->clock_display_suppressed = false;
	}
	if (!session->clock_display_suppressed)
		fplinux_session_power_activity(&session->power, true, time);
}

/* Visibility, focus and keys: the launcher is what the user sees on top. */
static bool launcher_on_top(const struct fplinux_session *session)
{
	return session->power.locked || notice_covers_app(session) ||
	       clock_covers_app(session);
}

static bool launch_is_shown(const struct fplinux_session *session)
{
	return launch_in_progress(session) &&
	       now_ms(session) - session->launch_ms >=
		       FPLINUX_SESSION_PROGRESS_MS &&
	       session->power.display != FPLINUX_UI_SESSION_DISPLAY_OFF &&
	       !launcher_on_top(session) && !clock_due(session);
}

static void initialize_process(struct process *process)
{
	memset(process, 0, sizeof(*process));
	process->pid_fd = -1;
	process->control_fd = -1;
	process->visible = true;
}

/*
 * Switches the child to the application account. A manager that is not root
 * runs as that account already, which is checked instead.
 */
static bool use_app_account(const struct fplinux_session_options *options)
{
	if (geteuid() != 0)
		return getuid() == options->app_uid &&
		       getgid() == options->app_gid;
	return initgroups(options->user, options->app_gid) == 0 &&
	       setgid(options->app_gid) == 0 && setuid(options->app_uid) == 0;
}

/*
 * The session runs with a low OOM score (see fplinux-session) so that the
 * kernel kills applications first; they start with the default score. Raising
 * the score needs no privilege, but after the credential switch /proc/self
 * belongs to root until exec, so this runs first. Errors are ignored.
 */
static void reset_oom_score(void)
{
	int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, "0\n", 2) < 0) {
			/* Ignored: the score stays low, which only protects the app. */
		}
		close(fd);
	}
}

/*
 * The kernel clears the parent-death signal on a credential change, so it is set after the
 * switch; a manager that died before that is detected through getppid().
 */
static void initialize_child(struct fplinux_session *session,
			     const struct fplinux_desktop_entry *entry,
			     pid_t manager)
{
	const struct fplinux_session_options *options = &session->options;
	const char *home = options->home;
	sigset_t signals;

	sigemptyset(&signals);
	sigprocmask(SIG_SETMASK, &signals, NULL);
	reset_oom_score();
	if (setpgid(0, 0) < 0 || !use_app_account(options) ||
	    prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != manager)
		_exit(126);
	umask(0077);
	setenv("HOME", home, 1);
	setenv("USER", options->user, 1);
	setenv("LOGNAME", options->user, 1);
	if (getenv("FPLINUX_APP_RUNTIME_DIR"))
		setenv("XDG_RUNTIME_DIR", getenv("FPLINUX_APP_RUNTIME_DIR"), 1);
	setenv("LANG", session->settings.lang, 1);
	setenv("FPLINUX_UI_SESSION_SOCKET", options->socket_path, 1);
	setenv("FPLINUX_APP_ID", entry->app_id, 1);
	chdir(home);
	execl(entry->executable, entry->executable, (char *)NULL);
	_exit(127);
}

static bool start_process(struct fplinux_session *session,
			  struct process *process,
			  const struct fplinux_desktop_entry *entry)
{
	pid_t manager = getpid();
	pid_t pid;

	pid = fork();

	if (pid < 0)
		return false;
	if (!pid)
		initialize_child(session, entry, manager);
	initialize_process(process);
	process->uid = session->options.app_uid;
	process->policy.pid = pid;
	if (process != &session->launcher) {
		process->policy.lifecycle = FPLINUX_UI_SESSION_FOREGROUND;
		if (!++session->next_task_token)
			++session->next_task_token;
		process->policy.token = session->next_task_token;
		if (session->document.request_id &&
		    session->document.serial == session->launch_generation &&
		    !strcmp(entry->app_id, session->document.handler_id))
			session->document.handler_token = process->policy.token;
	}
	/* Each start begins in the declared mode. */
	process->policy.mode = entry->mode;
	process->policy.configured_mode = entry->mode;
	process->declared_mode = entry->mode;
	process->requested_mode = entry->mode;
	copy_value(process->policy.app_id, sizeof(process->policy.app_id),
		   entry->app_id);
	copy_value(process->policy.title, sizeof(process->policy.title),
		   entry->title);
	process->started_ms = now_ms(session);
	if (process != &session->launcher)
		fplinux_session_audio_app(&session->audio, pid, process->uid,
					  process->policy.app_id,
					  process->started_ms);
	/* A new launcher's view is not placed yet and shows nothing. */
	if (process == &session->launcher) {
		session->battery_notice_drawn = false;
		session->clock_drawn_event = 0;
		session->launcher_tasks_view = false;
		session->launcher_shown = false;
		session->launcher_named_app = false;
		session->launcher_wait_serial = 0;
		session->launcher_ack_serial = 0;
		session->launcher_draws_app = false;
		session->last_active_sent = false;
	}
	setpgid(pid, pid);
	process->pid_fd = syscall(SYS_pidfd_open, pid, 0);
	if (process->pid_fd >= 0 && !add_fd(session, process->pid_fd)) {
		close(process->pid_fd);
		process->pid_fd = -1;
	}
	/*
	 * Launching an application is activity; starting the launcher, e.g.
	 * for a battery notice above a fullscreen application, is not.
	 */
	if (process != &session->launcher) {
		fplinux_session_power_activity(&session->power, false,
					       process->started_ms);
	}
	fprintf(stderr, "fplinux-session: started %s pid=%ld\n", entry->app_id,
		(long)pid);
	send_tasks(session);
	changed(session);
	return true;
}

static void stop_clock_signal(struct fplinux_session *session, uint64_t time)
{
	if (!session->clock_signal_pid || session->clock_signal_stopping)
		return;
	fplinux_session_audio_gone(&session->audio, session->clock_signal_pid,
				   now_ms(session));
	kill(session->clock_signal_pid, SIGTERM);
	session->clock_signal_stopping = true;
	session->clock_signal_stop_ms = time;
}

/* The helper owns its stream and evdev effect; the manager owns authorization. */
static void manage_clock_signal(struct fplinux_session *session, uint64_t time,
				bool *needs_tick)
{
	uint64_t event = session->clock.model.active.id;
	const char *path = session->options.clock_signal_path;

	if (session->clock_signal_pid) {
		pid_t reaped =
			waitpid(session->clock_signal_pid, NULL, WNOHANG);

		if (reaped == session->clock_signal_pid ||
		    (reaped < 0 && errno == ECHILD)) {
			fplinux_session_audio_gone(&session->audio,
						   session->clock_signal_pid,
						   time);
			remove_fd(session, session->clock_signal_fd);
			session->clock_signal_fd = -1;
			session->clock_signal_pid = 0;
			session->clock_signal_stopping = false;
		} else {
			if (!clock_due(session) ||
			    event != session->clock_signal_event ||
			    session->destroying)
				stop_clock_signal(session, time);
			if (session->clock_signal_stopping &&
			    time - session->clock_signal_stop_ms >=
				    FPLINUX_SESSION_CLOSE_KILL_MS)
				kill(session->clock_signal_pid, SIGKILL);
			if (session->clock_signal_fd < 0 ||
			    session->clock_signal_stopping)
				*needs_tick = true;
		}
	}
	if (!session->clock_signal_pid && !session->destroying &&
	    clock_due(session) && event != session->clock_signal_event &&
	    path) {
		pid_t manager = getpid();
		pid_t pid;

		/* A failed helper is not restarted repeatedly for the same ring. */
		session->clock_signal_event = event;
		pid = fork();
		if (pid == 0) {
			sigset_t signals;

			sigemptyset(&signals);
			sigprocmask(SIG_SETMASK, &signals, NULL);
			signal(SIGTERM, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 ||
			    getppid() != manager)
				_exit(126);
			execl(path, path, (char *)NULL);
			_exit(127);
		}
		if (pid < 0) {
			fprintf(stderr,
				"fplinux-session: cannot start clock signal: %s\n",
				strerror(errno));
			return;
		}
		session->clock_signal_pid = pid;
		if (fplinux_session_audio_clock(&session->audio, pid, geteuid(),
						time))
			fplinux_session_audio_audible(&session->audio, pid,
						      true, time);
		session->clock_signal_fd = syscall(SYS_pidfd_open, pid, 0);
		if (session->clock_signal_fd >= 0 &&
		    !add_fd(session, session->clock_signal_fd)) {
			close(session->clock_signal_fd);
			session->clock_signal_fd = -1;
		}
		if (session->clock_signal_fd < 0)
			*needs_tick = true;
	}
}

static const char *notice_code(enum fplinux_session_notice notice)
{
	switch (notice) {
	case FPLINUX_SESSION_NOTICE_UNLOCK_PRESS_STAR:
		return "unlock-press-star";
	case FPLINUX_SESSION_NOTICE_UNLOCK_HELP:
		return "unlock-help";
	default:
		return "";
	}
}

static const char *
battery_notice_code(enum fplinux_session_battery_alert notice)
{
	switch (notice) {
	case FPLINUX_SESSION_BATTERY_ALERT_LOW:
		return "battery-low";
	case FPLINUX_SESSION_BATTERY_ALERT_CRITICAL:
		return "battery-critical";
	default:
		return "";
	}
}

static void send_active(struct fplinux_session *session)
{
	struct fplinux_ui_session_message message;
	struct clock_ring ring;
	struct process *app = session->app;
	bool named = app_announced(session);

	if (session->launcher.control_fd < 0)
		return;
	/* Zeroed padding too, so that a repeated message compares equal. */
	memset(&message, 0, sizeof(message));
	message.type = FPLINUX_UI_SESSION_ACTIVE;
	message.display = session->power.display;
	message.locked = session->power.locked;
	message.battery =
		session->battery.known ?
			fplinux_session_battery_percent(&session->battery) :
			-1;
	message.charging = session->charger_online;
	message.theme = session->settings.theme;
	clock_wire_ring_from_model(&session->clock.model, &ring);
	clock_wire_ring_encode(&ring, ring.event.id, message.clock_data,
			       sizeof(message.clock_data));
	fplinux_session_clock_presentation(&session->clock,
					   &message.clock_presentation);
	copy_value(message.notice, sizeof(message.notice),
		   battery_notice_code(session->battery_notice));
	copy_value(message.selected_id, sizeof(message.selected_id),
		   session->selected_id);
	copy_value(message.locale, sizeof(message.locale),
		   session->settings.locale);
	copy_value(message.input_locale, sizeof(message.input_locale),
		   session->settings.input_locale);
	if (named) {
		message.token = app->policy.token;
		copy_value(message.active_id, sizeof(message.active_id),
			   app->policy.app_id);
		copy_value(message.title, sizeof(message.title),
			   app->policy.title);
		copy_value(message.left, sizeof(message.left), app->left);
		copy_value(message.centre, sizeof(message.centre), app->centre);
		copy_value(message.right, sizeof(message.right), app->right);
	} else {
		/* The launcher localizes its own legends. */
		copy_value(message.title, sizeof(message.title),
			   session->launcher.policy.title);
	}
	message.enabled = launch_in_progress(session);
	if (message.enabled) {
		copy_value(message.app_id, sizeof(message.app_id),
			   session->launch_id);
		copy_value(message.title, sizeof(message.title),
			   session->launch_title);
	}
	/* A new launcher opens the view that the previous one showed. */
	copy_value(message.value, sizeof(message.value),
		   session->launcher_home_view ? "home" : "catalogue");
	copy_value(message.name, sizeof(message.name),
		   session->launcher_list ? "list" : "grid");
	/*
	 * While locked, reason is the current unlock hint. Otherwise it reports
	 * an application notice once, so later updates do not raise it again.
	 */
	if (session->power.locked) {
		copy_value(message.reason, sizeof(message.reason),
			   notice_code(session->power.notice));
	} else {
		copy_value(message.reason, sizeof(message.reason),
			   session->error);
	}
	/*
	 * The launcher redraws its whole view for each ACTIVE; a repeat, for
	 * instance for a starting application's legends, would only take the
	 * processor from that application.
	 */
	message.serial = session->last_active.serial;
	if (session->last_active_sent &&
	    !memcmp(&message, &session->last_active, sizeof(message)))
		return;
	if (!++message.serial)
		++message.serial;
	if (!fplinux_ui_session_send(session->launcher.control_fd, &message))
		return;
	session->last_active = message;
	session->last_active_sent = true;
	session->active_known = true;
	session->active_display = session->power.display;
	session->active_locked = session->power.locked;
	session->active_notice = session->power.notice;
	session->active_battery = message.battery;
	session->active_charging = message.charging;
	session->active_battery_notice = session->battery_notice;
	if (!session->power.locked)
		session->error[0] = '\0';
	session->launcher_named_app = named;
	session->launcher_wait_serial = message.serial;
}

static void request_close(struct fplinux_session *session,
			  struct process *process)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CLOSE,
	};

	if (!process->policy.pid || process->policy.closing)
		return;
	fplinux_session_audio_gone(&session->audio, process->policy.pid,
				   now_ms(session));
	process->policy.closing = true;
	process->close_serial = 0;
	process->close_waiting = false;
	process->close_activate = false;
	set_lifecycle(session, process, FPLINUX_UI_SESSION_CLOSING);
	process->closing_ms = now_ms(session);
	copy_value(message.reason, sizeof(message.reason), "home");
	if (process->control_fd >= 0)
		fplinux_ui_session_send(process->control_fd, &message);
	else
		kill(-process->policy.pid, SIGTERM);
	changed(session);
}

static void send_activation(struct fplinux_session *session,
			    struct process *process)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_ACTIVATION,
		.serial = session->launch_generation,
		.activation = session->launch_activation,
	};

	if (session->document.request_id &&
	    session->document.serial == session->launch_generation &&
	    !strcmp(process->policy.app_id, session->document.handler_id) &&
	    process == session->app && session->launch_starting) {
		message.type = FPLINUX_UI_SESSION_DOCUMENT;
		message.activation = FPLINUX_UI_SESSION_ACTIVATION_OPEN;
		message.serial = session->document.serial;
		copy_value(message.path, sizeof(message.path),
			   session->document.path);
		session->document.handler_token = process->policy.token;
	}
	if (process == session->app && session->launch_starting &&
	    process->control_fd >= 0 &&
	    !strcmp(process->policy.app_id, session->launch_id))
		fplinux_ui_session_send(process->control_fd, &message);
}

static void send_config(struct fplinux_session *session,
			struct process *process)
{
	/* MODE cannot precede CONFIG, which therefore gives the declared mode. */
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = process->declared_mode,
		.screen_width = session->output_width,
		.screen_height = session->output_height,
		.theme = session->settings.theme,
		.audio = session->has_audio && process != &session->launcher,
		.vibration = session->has_vibrator &&
			     process != &session->launcher,
	};

	copy_value(message.app_id, sizeof(message.app_id),
		   process->policy.app_id);
	copy_value(message.title, sizeof(message.title), process->policy.title);
	copy_value(message.selected_id, sizeof(message.selected_id),
		   session->selected_id);
	copy_value(message.locale, sizeof(message.locale),
		   session->settings.locale);
	copy_value(message.input_locale, sizeof(message.input_locale),
		   session->settings.input_locale);
	fplinux_ui_session_send(process->control_fd, &message);
	if (process != &session->launcher) {
		struct fplinux_ui_session_message lifecycle = {
			.type = FPLINUX_UI_SESSION_LIFECYCLE,
			.lifecycle = process->policy.lifecycle,
		};

		fplinux_ui_session_send(process->control_fd, &lifecycle);
		send_activation(session, process);
	}
	send_active(session);
	if (process == &session->launcher)
		send_tasks(session);
}

static void finish_launch(struct fplinux_session *session)
{
	bool active = launch_in_progress(session) || session->launch_id[0];

	session->launch_starting = false;
	session->launch_retained = false;
	session->activation_ready = false;
	session->app_busy = false;
	session->app_busy_finishing = false;
	session->launch_id[0] = '\0';
	session->launch_title[0] = '\0';
	session->launch_shown = false;
	if (active)
		changed(session);
}

static void clear_pending_launch(struct fplinux_session *session)
{
	session->pending_id[0] = '\0';
}

static bool leave_app(struct fplinux_session *session, bool retain);

static void finish_activation(struct fplinux_session *session)
{
	if (!session->launch_starting || !session->activation_ready ||
	    !session->app->policy.ready || !session->app->drawn)
		return;
	session->launch_starting = false;
	if (!session->app_busy && !session->app_busy_finishing)
		finish_launch(session);
}

void fplinux_session_cancel_launch(struct fplinux_session *session)
{
	struct itimerspec soon = { .it_value.tv_nsec = 1000000 };
	bool owns_operation =
		session->app && !session->pending_id[0] &&
		!strcmp(session->app->policy.app_id, session->launch_id);
	bool retained = session->launch_retained;

	if (!launch_in_progress(session))
		return;
	document_launch_result(session, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
	clear_pending_launch(session);
	session->launcher_home_view = false;
	session->error[0] = '\0';
	/* Before the fork, the selected record may still be an unrelated task
	 * completing its Home handoff. Cancelling the new launch retains it. */
	if (owns_operation) {
		if (retained) {
			leave_app(session, true);
		} else {
			session->app->cancelled = true;
			request_close(session, session->app);
		}
	}
	finish_launch(session);
	send_active(session);
	changed(session);
	/* Shell cancellation also wakes the manager's close/restart timers. */
	timerfd_settime(session->timer_fd, 0, &soon, NULL);
}

static void set_busy(struct fplinux_session *session,
		     const struct fplinux_ui_session_message *message)
{
	if (!app_in_foreground(session))
		return;
	if (message->enabled) {
		bool fresh = !launch_in_progress(session);

		if (fresh) {
			session->launch_retained = false;
			session->launch_ms = now_ms(session);
			if (!++session->launch_generation)
				++session->launch_generation;
		}
		copy_value(session->launch_id, sizeof(session->launch_id),
			   session->app->policy.app_id);
		copy_value(session->launch_title, sizeof(session->launch_title),
			   message->title[0] ? message->title :
					       session->app->policy.title);
		session->app_busy = true;
		session->app_busy_finishing = false;
		if (fresh && session->options.launch_frame)
			session->options.launch_frame(session->options.context,
						      -1, 0, 0);
	} else {
		if (session->app_busy)
			session->app_busy_finishing = true;
		session->app_busy = false;
	}
	send_active(session);
	changed(session);
}

/* Retaining a task never moves its record, peer or pidfd. */
static void park_app(struct fplinux_session *session)
{
	struct process *app = session->app;

	if (!app->policy.pid)
		return;
	app->policy.shown = false;
	app->leaving = false;
	app->park_on_leave = false;
	app->wake_due = false;
	app->wake_committed = false;
	set_lifecycle(session, app, FPLINUX_UI_SESSION_BACKGROUND);
	session->app = &session->idle_app;
	changed(session);
}

/*
 * A shown application leaves without keys, until the launcher draws its
 * replacement; hand_off() then closes it or retains it in one repaint.
 * Off-screen clients close/park at once. EXIT always closes.
 */
static void settings_gone(struct fplinux_session *session,
			  const struct process *process);

static bool leave_app(struct fplinux_session *session, bool retain)
{
	struct process *app = session->app;

	if (is_running(app) && app->policy.shown &&
	    !(session->held_scene & HELD_SCENE_LAUNCH))
		hold_scene(session, HELD_SCENE_HOME);
	if (!is_running(app) || app->leaving)
		return false;
	if (!session->pending_id[0])
		finish_launch(session);
	app->park_on_leave = retain;
	/* A brightness preview is a temporary foreground operation. */
	if (retain)
		settings_gone(session, app);
	set_lifecycle(session, app,
		      retain ? FPLINUX_UI_SESSION_BACKGROUND :
			       FPLINUX_UI_SESSION_CLOSING);
	if (!app->policy.shown || launcher_on_top(session) ||
	    session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
		if (app->park_on_leave)
			park_app(session);
		else
			request_close(session, app);
		return true;
	}
	app->leaving = true;
	app->handoff_ms = now_ms(session);
	if (!retain)
		fplinux_session_audio_gone(&session->audio, app->policy.pid,
					   app->handoff_ms);
	changed(session);
	return true;
}

static bool begin_launch(struct fplinux_session *session,
			 const struct fplinux_desktop_entry *entry,
			 enum fplinux_ui_session_activation activation,
			 int frame_fd, int width, int height)
{
	struct process *existing = task_for_id(session, entry->app_id);

	/* A full registry keeps its existing applications intact. */
	if (!existing && !free_task(session)) {
		copy_value(session->error, sizeof(session->error),
			   "tasks-full");
		send_active(session);
		return false;
	}
	clear_pending_launch(session);
	copy_value(session->pending_id, sizeof(session->pending_id),
		   entry->app_id);
	copy_value(session->selected_id, sizeof(session->selected_id),
		   entry->app_id);
	copy_value(session->launch_id, sizeof(session->launch_id),
		   entry->app_id);
	copy_value(session->launch_title, sizeof(session->launch_title),
		   entry->title);
	session->launch_ms = now_ms(session);
	session->launch_starting = true;
	session->launch_activation = activation;
	session->launch_retained = existing != NULL;
	session->activation_ready = false;
	session->app_busy = false;
	session->app_busy_finishing = false;
	if (!++session->launch_generation)
		++session->launch_generation;
	session->error[0] = '\0';
	hold_scene(session, HELD_SCENE_LAUNCH);
	/* The shell copies the launch card before fullscreen releases its owner. */
	if (session->options.launch_frame)
		session->options.launch_frame(session->options.context,
					      frame_fd, width, height);
	send_active(session);
	leave_app(session, true);
	if (entry->mode == FPLINUX_UI_SESSION_FULLSCREEN)
		request_close(session, &session->launcher);
	send_active(session);
	changed(session);
	return true;
}

static void launch(struct fplinux_session *session, const char *id,
		   enum fplinux_ui_session_activation activation, int frame_fd,
		   int width, int height)
{
	struct fplinux_desktop_entry entry;

	/* An impatient second press cannot replace the operation being cancelled. */
	if (launch_in_progress(session))
		return;
	clear_pending_launch(session);
	if (!read_entry(session, id, &entry) ||
	    entry.mode == FPLINUX_UI_SESSION_LAUNCHER) {
		copy_value(session->error, sizeof(session->error),
			   "app-unavailable");
		send_active(session);
		return;
	}
	if (activation == FPLINUX_UI_SESSION_ACTIVATION_OPEN) {
		struct process *existing = task_for_id(session, id);

		if (existing)
			existing->return_origin = 0;
		if (existing &&
		    session->document.handler_token == existing->policy.token)
			session->document.detached = true;
	}
	begin_launch(session, &entry, activation, frame_fd, width, height);
}

static void open_document_with(struct fplinux_session *session, const char *id)
{
	struct fplinux_desktop_entry entry;
	struct document_request *request = &session->document;
	int error = fplinux_ui_session_document_path_error(request->path);

	if (error || !read_entry(session, id, &entry) ||
	    entry.mode == FPLINUX_UI_SESSION_LAUNCHER ||
	    !fplinux_desktop_entry_handles(&entry, request->path)) {
		document_result(session, FPLINUX_UI_DOCUMENT_ERROR,
				error ? error : ENOENT);
		return;
	}
	if (!task_for_id(session, id) && !free_task(session)) {
		document_result(session, FPLINUX_UI_DOCUMENT_ERROR, EUSERS);
		return;
	}
	copy_value(request->handler_id, sizeof(request->handler_id), id);
	if (!begin_launch(session, &entry, FPLINUX_UI_SESSION_ACTIVATION_OPEN,
			  -1, 0, 0)) {
		document_result(session, FPLINUX_UI_DOCUMENT_ERROR, EBUSY);
		return;
	}
	request->serial = session->launch_generation;
}

static int compare_document_choices(const void *first, const void *second)
{
	const struct fplinux_ui_session_document_choice *left = first;
	const struct fplinux_ui_session_document_choice *right = second;
	int order = strcmp(left->title, right->title);

	return order ? order : strcmp(left->app_id, right->app_id);
}

static void open_document(struct fplinux_session *session,
			  struct process *origin,
			  const struct fplinux_ui_session_message *message)
{
	struct fplinux_ui_session_message choices = {
		.type = FPLINUX_UI_SESSION_DOCUMENT_CHOICES,
		.serial = message->serial,
	};
	DIR *directory;
	struct dirent *item;
	int error = 0;

	if (origin != session->app || !app_in_foreground(session) ||
	    session->power.locked || session->document.request_id ||
	    launch_in_progress(session)) {
		struct fplinux_ui_session_message busy = {
			.type = FPLINUX_UI_SESSION_DOCUMENT_RESULT,
			.serial = message->serial,
			.document_status = FPLINUX_UI_DOCUMENT_ERROR,
			.document_error = EBUSY,
		};

		fplinux_ui_session_send(origin->control_fd, &busy);
		return;
	}
	session->document.request_id = message->serial;
	session->document.origin_token = origin->policy.token;
	copy_value(session->document.path, sizeof(session->document.path),
		   message->path);
	error = fplinux_ui_session_document_path_error(message->path);
	if (error) {
		document_result(session, FPLINUX_UI_DOCUMENT_ERROR, error);
		return;
	}
	directory = opendir(session->options.applications);
	if (!directory) {
		document_result(session, FPLINUX_UI_DOCUMENT_ERROR, errno);
		return;
	}
	while ((item = readdir(directory))) {
		struct fplinux_desktop_entry entry;
		struct fplinux_ui_session_document_choice *choice;
		char id[FPLINUX_UI_SESSION_ID_BYTES];
		size_t bytes = strlen(item->d_name);

		if (bytes <= 8 || bytes - 8 >= sizeof(id) ||
		    strcmp(item->d_name + bytes - 8, ".desktop"))
			continue;
		memcpy(id, item->d_name, bytes - 8);
		id[bytes - 8] = '\0';
		if (!read_entry(session, id, &entry) ||
		    entry.mode == FPLINUX_UI_SESSION_LAUNCHER ||
		    !fplinux_desktop_entry_handles(&entry, message->path))
			continue;
		if (choices.document_choice_count ==
		    FPLINUX_UI_SESSION_DOCUMENT_CHOICE_MAX) {
			error = EOVERFLOW;
			break;
		}
		choice = &choices.document_choices
				  [choices.document_choice_count++];
		copy_value(choice->app_id, sizeof(choice->app_id),
			   entry.app_id);
		copy_value(choice->title, sizeof(choice->title), entry.title);
		if (entry.icon_valid)
			copy_value(choice->icon, sizeof(choice->icon),
				   entry.icon);
	}
	closedir(directory);
	if (error || !choices.document_choice_count)
		document_result(session, FPLINUX_UI_DOCUMENT_ERROR,
				error ? error : ENOEXEC);
	else if (choices.document_choice_count == 1)
		open_document_with(session, choices.document_choices[0].app_id);
	else {
		qsort(choices.document_choices, choices.document_choice_count,
		      sizeof(choices.document_choices[0]),
		      compare_document_choices);
		fplinux_ui_session_send(origin->control_fd, &choices);
	}
}

static void return_home(struct fplinux_session *session)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_HOME,
	};
	unsigned int i;

	if (session->document.handler_id[0] && !document_delivered(session))
		document_result(session, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
	session->return_token = 0;
	clear_pending_launch(session);
	finish_launch(session);
	for (i = 0; i < FPLINUX_UI_SESSION_TASK_MAX; ++i)
		session->tasks[i].close_activate = false;
	/* Home preserves the decision, but no longer waits for its hidden frame. */
	if (app_in_foreground(session) && session->app->policy.shown)
		hold_scene(session, HELD_SCENE_HOME);
	release_scene(session, HELD_SCENE_CLOSE);
	/* The launcher shows its home view once the application is gone. */
	if (session->launcher.control_fd >= 0)
		fplinux_ui_session_send(session->launcher.control_fd, &message);
	session->launcher_home_view = true;
	session->launcher_tasks_view = false;
	leave_app(session, true);
	send_active(session);
}

void fplinux_session_home(struct fplinux_session *session)
{
	return_home(session);
}

static void finish_user_close(struct fplinux_session *session,
			      struct process *process)
{
	process->close_serial = 0;
	process->close_waiting = false;
	process->close_activate = false;
	if (process != session->app) {
		if (session->close_pid == process->policy.pid &&
		    session->close_token == process->policy.token)
			release_scene(session, HELD_SCENE_CLOSE);
		request_close(session, process);
		return;
	}
	process->park_on_leave = false;
	if (process->policy.shown)
		hold_scene(session, HELD_SCENE_HOME);
	if (session->close_pid == process->policy.pid &&
	    session->close_token == process->policy.token)
		release_scene(session, HELD_SCENE_CLOSE);
	session->launcher_home_view = false;
	if (process->leaving)
		request_close(session, process);
	else
		leave_app(session, false);
	send_active(session);
}

static void prepare_user_close(struct fplinux_session *session,
			       struct process *process)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_PREPARE_CLOSE,
	};

	if (!is_running(process) || process->policy.closing)
		return;
	if (process->close_serial && !process->close_waiting)
		return;
	if (!process->close_serial) {
		if (!++session->next_close_serial)
			++session->next_close_serial;
		process->close_serial = session->next_close_serial;
		process->close_requested_ms = now_ms(session);
	}
	/* Capture before PREPARE_CLOSE can replace a covering menu or dialog. */
	hold_scene(session, HELD_SCENE_CLOSE);
	session->close_pid = process->policy.pid;
	session->close_token = process->policy.token;
	session->close_scene_serial = process->close_serial;
	session->close_frame_ready = false;
	process->close_activate = process != session->app;
	message.serial = process->close_serial;
	copy_value(message.reason, sizeof(message.reason), "exit");
	if (process->control_fd < 0 ||
	    !fplinux_ui_session_send(process->control_fd, &message))
		finish_user_close(session, process);
	changed(session);
}

static void close_reply(struct fplinux_session *session,
			struct process *process,
			const struct fplinux_ui_session_message *message)
{
	if (!process->close_serial ||
	    process->close_serial != message->serial || process->policy.closing)
		return;
	if (!strcmp(message->value, "ready")) {
		if (session->document.restarting &&
		    session->document.handler_token == process->policy.token) {
			release_scene(session, HELD_SCENE_CLOSE);
			request_close(session, process);
		} else {
			finish_user_close(session, process);
		}
	} else if (!strcmp(message->value, "cancel")) {
		if (session->document.restarting &&
		    session->document.handler_token == process->policy.token)
			document_result(session, FPLINUX_UI_DOCUMENT_CANCELLED,
					0);
		process->close_serial = 0;
		process->close_waiting = false;
		process->close_activate = false;
		if (process != session->app &&
		    session->close_pid == process->policy.pid &&
		    session->close_token == process->policy.token)
			release_scene(session, HELD_SCENE_CLOSE);
	} else if (!strcmp(message->value, "wait")) {
		process->close_waiting = true;
		process->close_activate &= process != session->app;
	}
	changed(session);
}

static void
handler_document_result(struct fplinux_session *session,
			struct process *process,
			const struct fplinux_ui_session_message *message)
{
	struct document_request *request = &session->document;
	bool owns_launch = session->launch_starting &&
			   request->serial == session->launch_generation &&
			   process == session->app;

	if (!request->request_id || request->serial != message->serial ||
	    request->handler_token != process->policy.token ||
	    process->policy.closing)
		return;
	if (message->document_status == FPLINUX_UI_DOCUMENT_RESTART_REQUIRED) {
		if (request->restarted) {
			document_result(session, FPLINUX_UI_DOCUMENT_ERROR,
					ENOTSUP);
			if (owns_launch)
				finish_launch(session);
			return;
		}
		request->restarted = true;
		request->restarting = true;
		request->waiting = true;
		/* The existing close callback owns the only ready/cancel/wait decision. */
		if (owns_launch)
			finish_launch(session);
		prepare_user_close(session, process);
		return;
	}
	if (message->document_status == FPLINUX_UI_DOCUMENT_WAIT) {
		request->waiting = true;
		document_result(session, FPLINUX_UI_DOCUMENT_WAIT, 0);
		return;
	}
	if (message->document_status == FPLINUX_UI_DOCUMENT_ACCEPTED) {
		process->return_origin =
			request->detached || request->origin_token ==
						     process->policy.token ?
				0 :
				request->origin_token;
		if (owns_launch) {
			session->activation_ready = true;
			finish_activation(session);
		}
	} else if (owns_launch) {
		finish_launch(session);
	}
	document_result(session, message->document_status,
			message->document_error);
	send_active(session);
	changed(session);
}

/* ---- Lights: backlight through the brightness service, keypad LED ---- */

static void brightness_lost(struct fplinux_session *session,
			    const char *operation)
{
	if (!session->brightness_failed)
		fprintf(stderr,
			"fplinux-session: brightness service %s failed: %s\n",
			operation, strerror(errno));
	session->brightness_failed = true;
	fplinux_brightness_close(&session->brightness);
	session->brightness_leased = false;
	session->brightness_shown = -1;
	session->brightness_retry_ms =
		now_ms(session) + FPLINUX_SESSION_BRIGHTNESS_RETRY_MS;
}

static bool brightness_connect(struct fplinux_session *session)
{
	struct timeval timeout = {
		.tv_usec = FPLINUX_SESSION_BRIGHTNESS_TIMEOUT_MS * 1000,
	};
	unsigned int level;

	if (session->brightness.fd >= 0)
		return true;
	if (!session->options.brightness_socket ||
	    now_ms(session) < session->brightness_retry_ms)
		return false;
	if (fplinux_brightness_connect(&session->brightness,
				       session->options.brightness_socket) <
	    0) {
		brightness_lost(session, "connection");
		return false;
	}
	/* A stalled service must not stall the compositor. */
	setsockopt(session->brightness.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		   sizeof(timeout));
	setsockopt(session->brightness.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
		   sizeof(timeout));
	session->brightness_leased = false;
	session->brightness_shown = -1;
	if (session->brightness_pending_set) {
		if (fplinux_brightness_set(&session->brightness,
					   session->user_level) < 0) {
			brightness_lost(session, "set");
			return false;
		}
		session->brightness_pending_set = false;
	}
	if (fplinux_brightness_get(&session->brightness, &level) < 0) {
		brightness_lost(session, "get");
		return false;
	}
	session->user_level = level;
	if (session->brightness_failed)
		fprintf(stderr, "fplinux-session: brightness service ready\n");
	session->brightness_failed = false;
	return true;
}

/* Negative level releases the lease and restores the user's level. */
static void show_backlight(struct fplinux_session *session, int level)
{
	if (level < 0 && !session->brightness_leased &&
	    !session->brightness_pending_set)
		return;
	if (level >= 0 && session->brightness_leased &&
	    session->brightness_shown == level)
		return;
	if (!brightness_connect(session))
		return;
	if (level < 0) {
		if (session->brightness_leased &&
		    fplinux_brightness_release(&session->brightness) < 0) {
			brightness_lost(session, "release");
			return;
		}
		session->brightness_leased = false;
		session->brightness_shown = -1;
		return;
	}
	if (!session->brightness_leased) {
		if (fplinux_brightness_claim(&session->brightness) < 0) {
			brightness_lost(session, "claim");
			return;
		}
		session->brightness_leased = true;
	}
	if (fplinux_brightness_show(&session->brightness, (unsigned int)level) <
	    0) {
		brightness_lost(session, "show");
		return;
	}
	session->brightness_shown = level;
}

static void open_keypad_light(struct fplinux_session *session)
{
	const char *directory = session->options.leds_dir;
	char path[512];

	if (!directory)
		return;
	if (fplinux_keypad_light_find(path, sizeof(path), directory) < 0)
		return;
	session->led_open =
		fplinux_keypad_light_open(&session->keypad_light, path) == 0;
}

static void show_keypad_light(struct fplinux_session *session, bool lit)
{
	if (!session->led_open || session->led_state == (int)lit)
		return;
	if (fplinux_keypad_light_set(&session->keypad_light, lit) == 0)
		session->led_state = lit;
}

static unsigned int dim_level(unsigned int user_level)
{
	unsigned int level = user_level / 3;

	if (!level)
		level = 1;
	return level < user_level ? level : user_level;
}

static void apply_lights(struct fplinux_session *session)
{
	enum fplinux_ui_session_display display = session->power.display;

	if (session->light_pending)
		display = FPLINUX_UI_SESSION_DISPLAY_OFF;
	show_keypad_light(session, display == FPLINUX_UI_SESSION_DISPLAY_ON);
	if (display == FPLINUX_UI_SESSION_DISPLAY_ON)
		show_backlight(session, -1);
	else if (display == FPLINUX_UI_SESSION_DISPLAY_DIM)
		show_backlight(session, (int)dim_level(session->user_level));
	else
		show_backlight(session, 0);
}

/*
 * A charger replaces a battery notice; otherwise each alert of the discharge
 * cycle raises its notice, critical over low, until the launcher dismisses it.
 */
static void check_battery(struct fplinux_session *session)
{
	enum fplinux_session_battery_alert alert;

	if (!session->battery.known)
		return;
	alert = fplinux_session_battery_alert(
		&session->battery_alerts,
		fplinux_session_battery_percent(&session->battery),
		fplinux_session_battery_raw_percent(&session->battery),
		session->charger_online);
	if (session->charger_online) {
		session->battery_notice = FPLINUX_SESSION_BATTERY_ALERT_NONE;
		session->battery_notice_drawn = false;
	} else if (alert > session->battery_notice) {
		fprintf(stderr, "fplinux-session: battery %s at %d%%\n",
			alert == FPLINUX_SESSION_BATTERY_ALERT_CRITICAL ?
				"critical" :
				"low",
			fplinux_session_battery_raw_percent(&session->battery));
		session->battery_notice = alert;
		session->battery_notice_drawn = false;
	}
}

/*
 * The phone charger raises no uevent, so the state is polled every 2 s. The
 * battery is read every minute while the display is on or dimmed, and at once
 * after it was off.
 */
static void poll_power(struct fplinux_session *session, uint64_t time)
{
	struct fplinux_session_supply_sample sample;
	bool battery;

	if (!session->options.power_supply_dir)
		return;
	if (session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF)
		session->battery_poll_ms = 0;
	battery = session->power.display != FPLINUX_UI_SESSION_DISPLAY_OFF &&
		  time >= session->battery_poll_ms;
	if (!battery && time < session->charger_poll_ms)
		return;
	session->charger_poll_ms = time + FPLINUX_SESSION_CHARGER_POLL_MS;
	fplinux_session_supply_read(session->options.power_supply_dir, battery,
				    &sample);
	if (session->charger_known &&
	    sample.charging != session->charger_online) {
		fprintf(stderr, "fplinux-session: charger %s\n",
			sample.charging ? "connected" : "disconnected");
		if (session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
			fplinux_session_supply_read(
				session->options.power_supply_dir, true,
				&sample);
			battery = true;
		}
		fplinux_session_power_activity(&session->power, true, time);
	}
	session->charger_known = true;
	session->charger_online = sample.charging;
	if (battery) {
		session->battery_poll_ms =
			time + FPLINUX_SESSION_BATTERY_POLL_MS;
		fplinux_session_battery_update(&session->battery, &sample,
					       time);
	}
	check_battery(session);
}

/* ---- Visibility, keep-screen-on and settings ---- */

static bool is_visible(const struct fplinux_session *session,
		       const struct process *process)
{
	if (session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF)
		return false;
	/* The launcher draws the bars or the lock screen whenever it runs. */
	if (process == &session->launcher)
		return true;
	return process == session->app && !process->leaving &&
	       process->policy.lifecycle == FPLINUX_UI_SESSION_FOREGROUND &&
	       !launcher_on_top(session);
}

static void update_visibility(struct fplinux_session *session)
{
	unsigned int i;

	for (i = 0; i <= FPLINUX_UI_SESSION_TASK_MAX; ++i) {
		struct process *process = process_at(session, i);
		struct fplinux_ui_session_message message = {
			.type = FPLINUX_UI_SESSION_VISIBILITY,
		};

		if (!is_running(process) || process->control_fd < 0)
			continue;
		message.visible = is_visible(session, process);
		if (message.visible == process->visible)
			continue;
		if (!fplinux_ui_session_send(process->control_fd, &message))
			continue;
		process->visible = message.visible;
		process->wake_due = message.visible && session->light_pending;
		process->wake_committed = false;
	}
}

static bool foreground_media_eligible(const struct fplinux_session *session)
{
	const struct process *app = session->app;

	return is_running(app) && app->control_fd >= 0 && !app->leaving &&
	       app->visible && is_visible(session, app);
}

static void vibration_error(struct fplinux_session *session, int error)
{
	struct process *owner =
		process_for_pid(session, session->vibration_pid);
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_ERROR,
	};

	fprintf(stderr, "fplinux-session: vibration failed: %s\n",
		strerror(error));
	if (!owner || owner->control_fd < 0)
		return;
	copy_value(message.name, sizeof(message.name), "vibration");
	copy_value(message.reason, sizeof(message.reason), strerror(error));
	fplinux_ui_session_send(owner->control_fd, &message);
}

static void show_vibration(struct fplinux_session *session, bool on)
{
	bool ok;

	if (session->vibrating == on)
		return;
	if (session->options.vibrate)
		ok = session->options.vibrate(session->options.context, on);
	else
		ok = fplinux_session_vibrator_set(&session->vibrator, on);
	if (ok) {
		session->vibrating = on;
		return;
	}
	vibration_error(session, errno);
	fplinux_session_vibration_stop(&session->vibration);
	session->has_vibrator = false;
	session->vibrating = false;
}

static void advance_media(struct fplinux_session *session, uint64_t time,
			  uint64_t *earliest)
{
	struct fplinux_session_vibration_state state = {
		.eligible = foreground_media_eligible(session) &&
			    session->vibration_pid == session->app->policy.pid,
		.shown = session->app->policy.shown,
		.lit = session->power.display !=
			       FPLINUX_UI_SESSION_DISPLAY_OFF &&
		       !session->light_pending,
		.battery_low =
			session->battery.known && !session->charger_online &&
			fplinux_session_battery_percent(&session->battery) <
				FPLINUX_SESSION_BATTERY_LOW_PERCENT,
		.activity_ms = session->power.activity_ms,
	};
	unsigned int i;
	uint64_t deadline;

	for (i = 0; i < FPLINUX_UI_SESSION_TASK_MAX; ++i) {
		struct process *process = &session->tasks[i];

		fplinux_session_audio_audible(
			&session->audio, process->policy.pid,
			process == session->app &&
				foreground_media_eligible(session) &&
				state.shown && state.lit,
			time);
	}
	fplinux_session_vibration_advance(&session->vibration, &state, time);
	show_vibration(session, session->vibration.vibrating);
	deadline = fplinux_session_vibration_deadline(&session->vibration,
						      &state, time);
	if (deadline && (!*earliest || deadline < *earliest))
		*earliest = deadline;
	if (!session->vibration.playing)
		session->vibration_pid = 0;
	deadline = fplinux_session_audio_deadline(&session->audio);
	if (deadline && (!*earliest || deadline < *earliest))
		*earliest = deadline;
}

static bool keeps_screen_on(const struct fplinux_session *session)
{
	const struct process *processes[] = { &session->launcher,
					      session->app };
	unsigned int i;

	if (session->power.locked)
		return false;
	for (i = 0; i < 2; ++i)
		if (is_running(processes[i]) && processes[i]->keep_screen_on &&
		    is_visible(session, processes[i]))
			return true;
	return false;
}

static void reply(struct process *process,
		  enum fplinux_ui_session_message_type type, const char *name,
		  const char *value, const char *reason)
{
	struct fplinux_ui_session_message message = {
		.type = type,
		.applied = type == FPLINUX_UI_SESSION_SETTING,
	};

	copy_value(message.name, sizeof(message.name), name);
	copy_value(message.value, sizeof(message.value), value);
	copy_value(message.reason, sizeof(message.reason), reason);
	fplinux_ui_session_send(process->control_fd, &message);
}

static void save_settings(struct fplinux_session *session)
{
	struct fplinux_storage_result previous = session->settings_storage;
	struct fplinux_storage_result result = {
		.state = FPLINUX_STORAGE_UNPUBLISHED,
	};

	if (session->options.settings_path)
		result = fplinux_session_settings_save(
			&session->settings, session->options.settings_path);
	session->settings_storage = result;
	if (!result.error) {
		session->settings_retry_ms = 0;
		session->settings_retry_delay_ms = 0;
		if (previous.error)
			fprintf(stderr, "fplinux-session: settings saved: %s\n",
				session->options.settings_path);
		return;
	}
	if (!session->settings_retry_delay_ms)
		session->settings_retry_delay_ms =
			FPLINUX_SESSION_SETTINGS_RETRY_MS;
	else if (session->settings_retry_delay_ms <
		 FPLINUX_SESSION_SETTINGS_RETRY_MAX_MS)
		session->settings_retry_delay_ms *= 2;
	if (session->settings_retry_delay_ms >
	    FPLINUX_SESSION_SETTINGS_RETRY_MAX_MS)
		session->settings_retry_delay_ms =
			FPLINUX_SESSION_SETTINGS_RETRY_MAX_MS;
	session->settings_retry_ms =
		now_ms(session) + session->settings_retry_delay_ms;
	if (previous.state != result.state || previous.error != result.error)
		fprintf(stderr, "fplinux-session: cannot save %s: %s\n",
			session->options.settings_path, strerror(result.error));
}

static void setting_reply(struct fplinux_session *session,
			  struct process *process, const char *name,
			  const char *value, uint32_t serial, bool mutation)
{
	struct fplinux_ui_session_message message = {
		.type = mutation && session->settings_storage.error ?
				FPLINUX_UI_SESSION_ERROR :
				FPLINUX_UI_SESSION_SETTING,
		.serial = serial,
		.applied = true,
		.storage_state = session->settings_storage.state,
		.storage_error = session->settings_storage.error,
	};

	copy_value(message.name, sizeof(message.name), name);
	copy_value(message.value, sizeof(message.value), value);
	if (message.storage_error)
		copy_value(message.reason, sizeof(message.reason),
			   "save-failed");
	fplinux_ui_session_send(process->control_fd, &message);
}

static void set_rtc(const char *path, time_t seconds)
{
	struct rtc_time value = { 0 };
	struct tm time;
	int fd;

	if (!path || !gmtime_r(&seconds, &time))
		return;
	value.tm_sec = time.tm_sec;
	value.tm_min = time.tm_min;
	value.tm_hour = time.tm_hour;
	value.tm_mday = time.tm_mday;
	value.tm_mon = time.tm_mon;
	value.tm_year = time.tm_year;
	value.tm_wday = time.tm_wday;
	value.tm_yday = time.tm_yday;
	/* The RTC holds UTC; the system clock stays set if it fails. */
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, RTC_SET_TIME, &value) < 0)
		fprintf(stderr, "fplinux-session: cannot set %s: %s\n", path,
			strerror(errno));
	if (fd >= 0)
		close(fd);
}

static void time_setting(struct fplinux_session *session,
			 struct process *process, const char *value)
{
	struct timespec clock_time = { 0 };
	char current[32];

	if (value[0]) {
		if (!fplinux_session_time_parse(value, &clock_time.tv_sec)) {
			reply(process, FPLINUX_UI_SESSION_ERROR, "time", value,
			      "invalid-value");
			return;
		}
		if (clock_settime(CLOCK_REALTIME, &clock_time) < 0) {
			fprintf(stderr,
				"fplinux-session: cannot set the clock: %s\n",
				strerror(errno));
			reply(process, FPLINUX_UI_SESSION_ERROR, "time", value,
			      "failed");
			return;
		}
		set_rtc(session->options.rtc_path, clock_time.tv_sec);
		send_active(session);
	}
	if (!fplinux_session_time_format(time(NULL), current,
					 sizeof(current))) {
		reply(process, FPLINUX_UI_SESSION_ERROR, "time", value,
		      "failed");
		return;
	}
	reply(process, FPLINUX_UI_SESSION_SETTING, "time", current, "");
}

static void brightness_setting(struct fplinux_session *session,
			       struct process *process, const char *value)
{
	struct fplinux_session_settings next = session->settings;
	unsigned int level = 0;
	char current[16];
	int result;

	if (value[0] && !fplinux_session_settings_parse(
				&next, session->options.languages_dir,
				FPLINUX_SESSION_SETTING_BRIGHTNESS, value)) {
		reply(process, FPLINUX_UI_SESSION_ERROR, "brightness", value,
		      "invalid-value");
		return;
	}
	if (!brightness_connect(session)) {
		reply(process, FPLINUX_UI_SESSION_ERROR, "brightness", value,
		      "unavailable");
		return;
	}
	/* While dimmed or off, the service applies a new level on release. */
	if (value[0])
		result = fplinux_brightness_set(&session->brightness,
						next.brightness);
	else
		result = fplinux_brightness_get(&session->brightness, &level);
	if (result < 0) {
		brightness_lost(session, value[0] ? "set" : "get");
		reply(process, FPLINUX_UI_SESSION_ERROR, "brightness", value,
		      "unavailable");
		return;
	}
	if (value[0]) {
		session->settings = next;
		session->user_level = next.brightness;
		/* The saved level replaces the one a preview would restore. */
		session->preview_active = false;
		session->settings_retry_delay_ms = 0;
		save_settings(session);
	} else {
		session->user_level = level;
	}
	snprintf(current, sizeof(current), "%u", session->user_level);
	setting_reply(session, process, "brightness", current, 0, value[0]);
}

/*
 * Ends a Settings brightness preview with the level from before it. Returns
 * false when the service is unreachable; the level is then applied when the
 * connection comes back.
 */
static bool end_brightness_preview(struct fplinux_session *session)
{
	unsigned int level = session->preview_restore_level;

	if (!session->preview_active)
		return true;
	session->preview_active = false;
	if (!brightness_connect(session)) {
		session->user_level = level;
		session->brightness_pending_set = true;
		return false;
	}
	/* A new connection read the preview level from the service. */
	session->user_level = level;
	if (fplinux_brightness_set(&session->brightness, level) < 0) {
		brightness_lost(session, "set");
		session->brightness_pending_set = true;
		return false;
	}
	return true;
}

/*
 * A level shown while the Settings slider moves; nothing is saved. The first
 * step remembers the level that ending the preview restores.
 */
static void brightness_preview(struct fplinux_session *session,
			       struct process *process, const char *value)
{
	struct fplinux_session_settings next = session->settings;
	char current[16];

	if (!value[0]) {
		if (!end_brightness_preview(session)) {
			reply(process, FPLINUX_UI_SESSION_ERROR, "brightness",
			      value, "unavailable");
			return;
		}
		snprintf(current, sizeof(current), "%u", session->user_level);
		setting_reply(session, process, "brightness", current, 0,
			      false);
		return;
	}
	if (!fplinux_session_settings_parse(
		    &next, session->options.languages_dir,
		    FPLINUX_SESSION_SETTING_BRIGHTNESS, value)) {
		reply(process, FPLINUX_UI_SESSION_ERROR, "brightness_preview",
		      value, "invalid-value");
		return;
	}
	if (!brightness_connect(session)) {
		reply(process, FPLINUX_UI_SESSION_ERROR, "brightness_preview",
		      value, "unavailable");
		return;
	}
	if (!session->preview_active)
		session->preview_restore_level = session->user_level;
	if (fplinux_brightness_set(&session->brightness, next.brightness) < 0) {
		brightness_lost(session, "set");
		reply(process, FPLINUX_UI_SESSION_ERROR, "brightness_preview",
		      value, "unavailable");
		return;
	}
	session->preview_active = true;
	session->user_level = next.brightness;
	snprintf(current, sizeof(current), "%u", next.brightness);
	reply(process, FPLINUX_UI_SESSION_SETTING, "brightness_preview",
	      current, "");
}

/* A preview ends when Settings leaves the foreground or its process ends. */
static void settings_gone(struct fplinux_session *session,
			  const struct process *process)
{
	if (!strcmp(process->policy.app_id, FPLINUX_SESSION_SETTINGS) &&
	    session->preview_active) {
		fprintf(stderr,
			"fplinux-session: restoring brightness after a preview\n");
		end_brightness_preview(session);
	}
}

/*
 * Applications read the system language and theme when they start. A new
 * language or theme therefore closes every client, saving state through the
 * usual CLOSE and forced termination, except the requesting Settings
 * application: it switches live and keeps the focus. The launcher restarts
 * with the new CONFIG.
 */
static void close_for_restart(struct fplinux_session *session,
			      const struct process *requester)
{
	unsigned int i;

	for (i = 0; i <= FPLINUX_UI_SESSION_TASK_MAX; ++i) {
		struct process *process = process_at(session, i);

		if (process != requester)
			request_close(session, process);
	}
}

/* Retained applications keep their draft and learn the default for new fields. */
static void send_input_locale(struct fplinux_session *session,
			      const struct process *requester)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SETTING,
		.applied = true,
	};
	unsigned int i;

	copy_value(message.name, sizeof(message.name), "input_locale");
	copy_value(message.value, sizeof(message.value),
		   session->settings.input_locale);
	for (i = 0; i < FPLINUX_UI_SESSION_TASK_MAX; ++i) {
		struct process *process = &session->tasks[i];

		if (process != requester && is_running(process) &&
		    process->control_fd >= 0)
			fplinux_ui_session_send(process->control_fd, &message);
	}
	send_active(session);
}

static void refresh_default_titles(struct fplinux_session *session)
{
	for (unsigned int i = 0; i <= FPLINUX_UI_SESSION_TASK_MAX; ++i) {
		struct process *process = process_at(session, i);
		struct fplinux_desktop_entry entry;

		if (process->policy.pid && !process->title_supplied &&
		    read_entry(session, process->policy.app_id, &entry))
			copy_value(process->policy.title,
				   sizeof(process->policy.title), entry.title);
	}
	send_active(session);
	send_tasks(session);
}

static void handle_setting(struct fplinux_session *session,
			   struct process *process,
			   const struct fplinux_ui_session_message *message)
{
	enum fplinux_session_setting setting =
		fplinux_session_setting_find(message->name);
	struct fplinux_session_settings next = session->settings;
	char current[FPLINUX_UI_SESSION_TEXT_BYTES];
	bool input_changed = false;
	bool locale_changed = false;
	uint32_t visual_serial = 0;

	if (!strcmp(message->name, "brightness_preview")) {
		brightness_preview(session, process, message->value);
		return;
	}
	if (setting == FPLINUX_SESSION_SETTING_UNKNOWN) {
		reply(process, FPLINUX_UI_SESSION_ERROR, message->name,
		      message->value, "unknown-setting");
		return;
	}
	if (setting == FPLINUX_SESSION_SETTING_TIME) {
		time_setting(session, process, message->value);
		return;
	}
	if (setting == FPLINUX_SESSION_SETTING_BRIGHTNESS) {
		brightness_setting(session, process, message->value);
		return;
	}
	if (message->value[0]) {
		if (!fplinux_session_settings_parse(
			    &next, session->options.languages_dir, setting,
			    message->value)) {
			reply(process, FPLINUX_UI_SESSION_ERROR, message->name,
			      message->value, "invalid-value");
			return;
		}
		if (strcmp(next.locale, session->settings.locale) ||
		    next.theme != session->settings.theme) {
			hold_scene(session, HELD_SCENE_APPEARANCE);
			if (!++session->visual_serial)
				++session->visual_serial;
			visual_serial = session->visual_serial;
			session->redraw_serial = visual_serial;
			session->redraw_pid = process->policy.pid;
			session->redraw_token = process->policy.token;
			session->redraw_ready = false;
			close_for_restart(session, process);
		}
		locale_changed =
			strcmp(next.locale, session->settings.locale) != 0;
		if (locale_changed) {
			fprintf(stderr, "fplinux-session: locale %s\n",
				next.locale);
		}
		if (next.theme != session->settings.theme) {
			fprintf(stderr, "fplinux-session: theme %s\n",
				message->value);
		}
		input_changed = strcmp(next.input_locale,
				       session->settings.input_locale) != 0;
		session->settings = next;
		if (locale_changed)
			refresh_default_titles(session);
		fplinux_session_power_configure(&session->power,
						next.screen_off_seconds,
						next.auto_lock_seconds,
						now_ms(session));
		if (input_changed)
			send_input_locale(session, process);
		session->settings_retry_delay_ms = 0;
		save_settings(session);
	}
	fplinux_session_settings_format(&session->settings, setting, current,
					sizeof(current));
	setting_reply(session, process, message->name, current, visual_serial,
		      message->value[0]);
}

static void clock_reply(struct fplinux_session *session,
			struct process *process,
			const struct fplinux_ui_session_message *request,
			const struct clock_result *result)
{
	struct fplinux_ui_session_message response = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = request->serial,
	};

	copy_value(response.name, sizeof(response.name), request->name);
	if (!session->clock_ready) {
		copy_value(response.reason, sizeof(response.reason),
			   "clock-unavailable");
		fplinux_ui_session_send(process->control_fd, &response);
		return;
	}
	if (result->status != CLOCK_OK)
		copy_value(response.reason, sizeof(response.reason),
			   clock_model_status_name(result->status));
	if (process == &session->launcher) {
		struct clock_ring ring;

		clock_wire_ring_from_model(&session->clock.model, &ring);
		clock_wire_ring_encode(&ring, ring.event.id,
				       response.clock_data,
				       sizeof(response.clock_data));
	} else {
		clock_wire_snapshot_encode(&session->clock.model, result,
					   request->serial, response.clock_data,
					   sizeof(response.clock_data));
	}
	fplinux_ui_session_send(process->control_fd, &response);
}

static bool same_clock_request(const struct clock_request *left,
			       const struct clock_request *right)
{
	char left_json[1024], right_json[1024];

	return clock_wire_request_encode(left, left_json, sizeof(left_json)) ==
		       CLOCK_OK &&
	       clock_wire_request_encode(right, right_json,
					 sizeof(right_json)) == CLOCK_OK &&
	       !strcmp(left_json, right_json);
}

static bool handle_clock(struct fplinux_session *session,
			 struct process *process,
			 const struct fplinux_ui_session_message *message)
{
	struct clock_request request;
	struct clock_result result = {
		.storage = session->clock.storage,
	};

	if (process != &session->launcher) {
		if (strcmp(process->policy.app_id, FPLINUX_SESSION_CLOCK))
			return false;
		/* Commands queued before Home cannot control a retained app. */
		if (process != session->app || !app_in_foreground(session))
			return true;
	}
	if (process->policy.closing || process->leaving)
		return true;
	result.status = clock_wire_request_decode(
		message->clock_data, strlen(message->clock_data), &request);
	if (result.status == CLOCK_OK &&
	    strcmp(message->name, clock_wire_request_name(request.op)))
		result.status = CLOCK_INVALID;
	if (!message->serial)
		result.status = CLOCK_INVALID;
	if (result.status == CLOCK_OK && process == &session->launcher &&
	    request.op != CLOCK_REQUEST_DISMISS &&
	    request.op != CLOCK_REQUEST_SNOOZE)
		return false;
	if (result.status == CLOCK_OK && session->clock_ready) {
		if (request.op != CLOCK_REQUEST_SNAPSHOT &&
		    process->clock_serial == message->serial) {
			if (same_clock_request(&process->clock_request,
					       &request))
				result = fplinux_session_clock_retry(
					&session->clock, now_ms(session));
			else
				result.status = CLOCK_INVALID;
		} else {
			result = fplinux_session_clock_apply(&session->clock,
							     &request,
							     wall_utc(session),
							     now_ms(session));
			if (result.applied &&
			    request.op != CLOCK_REQUEST_SNAPSHOT) {
				process->clock_serial = message->serial;
				process->clock_request = request;
			}
		}
	}
	clock_reply(session, process, message, &result);
	clock_power(session, now_ms(session));
	send_active(session);
	changed(session);
	return true;
}

static bool receive_message(struct fplinux_session *session, struct peer *peer,
			    const struct fplinux_ui_session_message *message)
{
	struct process *process = process_for_pid(session, peer->pid);

	if (!process || peer->uid != process->uid)
		return false;
	if (!peer->authenticated) {
		if (message->type != FPLINUX_UI_SESSION_HELLO ||
		    strcmp(message->app_id, process->policy.app_id) ||
		    process->control_fd >= 0)
			return false;
		peer->authenticated = true;
		process->control_fd = peer->fd;
		process->connected = true;
		send_config(session, process);
		return true;
	}
	switch (message->type) {
	case FPLINUX_UI_SESSION_STORAGE_REQUEST:
		if (process == &session->launcher)
			return false;
		storage_request(session, process, message);
		break;
	case FPLINUX_UI_SESSION_OPEN_FILE:
		if (process == &session->launcher)
			return false;
		open_document(session, process, message);
		break;
	case FPLINUX_UI_SESSION_DOCUMENT_CHOICE:
		if (process->policy.token != session->document.origin_token ||
		    message->serial != session->document.request_id)
			break;
		if (!message->app_id[0])
			document_result(session, FPLINUX_UI_DOCUMENT_CANCELLED,
					0);
		else
			open_document_with(session, message->app_id);
		break;
	case FPLINUX_UI_SESSION_DOCUMENT_RESULT:
		if (process == &session->launcher)
			return false;
		handler_document_result(session, process, message);
		break;
	case FPLINUX_UI_SESSION_READY:
		if (!process->policy.ready && process->drawn)
			process->handoff_ms = now_ms(session);
		process->policy.ready = true;
		if (process == session->app)
			finish_activation(session);
		send_active(session);
		break;
	case FPLINUX_UI_SESSION_LAUNCH:
		if (process != &session->launcher)
			return false;
		if (launch_in_progress(session) || clock_due(session))
			break;
		if (process->policy.closing)
			return false;
		/* Nothing starts behind the keyguard. */
		if (!session->power.locked)
			launch(session, message->app_id,
			       FPLINUX_UI_SESSION_ACTIVATION_OPEN,
			       message->has_frame ? message->frame_fd : -1,
			       message->has_frame ? message->width : 0,
			       message->has_frame ? message->height : 0);
		break;
	case FPLINUX_UI_SESSION_SELECT:
		if (process != &session->launcher)
			return false;
		if (!strcmp(message->value, "tasks")) {
			session->launcher_tasks_view = message->enabled;
			break;
		}
		if (message->selected_id[0] &&
		    !fplinux_desktop_entry_valid_id(message->selected_id))
			return false;
		if (!strcmp(message->value, "home"))
			session->launcher_home_view = true;
		else if (!strcmp(message->value, "catalogue"))
			session->launcher_home_view = false;
		else if (message->value[0])
			return false;
		if (!strcmp(message->name, "grid"))
			session->launcher_list = false;
		else if (!strcmp(message->name, "list"))
			session->launcher_list = true;
		else if (message->name[0])
			return false;
		copy_value(session->selected_id, sizeof(session->selected_id),
			   message->selected_id);
		break;
	case FPLINUX_UI_SESSION_ACTIVE_ACK:
		if (process != &session->launcher)
			return false;
		/* The runtime has received a sync reply after this ACTIVE's commit. */
		if (message->serial &&
		    message->serial == session->last_active.serial) {
			struct clock_ring ring;
			uint64_t serial;

			session->launcher_ack_serial = message->serial;
			if (clock_wire_ring_decode(
				    session->last_active.clock_data,
				    strlen(session->last_active.clock_data),
				    &ring, &serial) == CLOCK_OK)
				session->clock_drawn_event = ring.event.id;
			if (session->battery_notice !=
				    FPLINUX_SESSION_BATTERY_ALERT_NONE &&
			    !strcmp(session->last_active.notice,
				    battery_notice_code(
					    session->battery_notice)))
				session->battery_notice_drawn = true;
		}
		if (message->serial &&
		    message->serial == session->launcher_wait_serial) {
			session->launcher_wait_serial = 0;
			session->launcher_draws_app =
				session->launcher_named_app;
		}
		break;
	case FPLINUX_UI_SESSION_REDRAW_ACK:
		if (process->policy.pid == session->redraw_pid &&
		    process->policy.token == session->redraw_token &&
		    message->serial &&
		    message->serial == session->redraw_serial)
			session->redraw_ready = true;
		break;
	case FPLINUX_UI_SESSION_ACTIVATION_ACK:
		if (process == &session->launcher)
			return false;
		if (session->launch_starting && !session->pending_id[0] &&
		    process == session->app && !process->policy.closing &&
		    !strcmp(process->policy.app_id, session->launch_id) &&
		    message->serial == session->launch_generation) {
			session->activation_ready = true;
			finish_activation(session);
			send_active(session);
		}
		break;
	case FPLINUX_UI_SESSION_CLOSE_ACK:
		if (process->policy.pid == session->close_pid &&
		    process->policy.token == session->close_token &&
		    message->serial == session->close_scene_serial)
			session->close_frame_ready = true;
		break;
	case FPLINUX_UI_SESSION_TITLE:
		process->title_supplied = true;
		copy_value(process->policy.title, sizeof(process->policy.title),
			   message->title);
		send_active(session);
		send_tasks(session);
		break;
	case FPLINUX_UI_SESSION_SOFTKEYS:
		copy_value(process->left, sizeof(process->left), message->left);
		copy_value(process->centre, sizeof(process->centre),
			   message->centre);
		copy_value(process->right, sizeof(process->right),
			   message->right);
		send_active(session);
		break;
	case FPLINUX_UI_SESSION_HOME:
		if (clock_due(session))
			break;
		/* A queued Home from a retained task cannot send another app Home. */
		if (process != session->app && process != &session->launcher)
			break;
		fplinux_session_home(session);
		break;
	case FPLINUX_UI_SESSION_CANCEL:
		if (process != &session->launcher)
			return false;
		if (!session->power.locked && !notice_covers_app(session) &&
		    !clock_due(session))
			fplinux_session_cancel_launch(session);
		break;
	case FPLINUX_UI_SESSION_BUSY:
		/* A command queued before Home cannot start a hidden launch modal. */
		if (process != session->app && process != &session->launcher)
			break;
		if (process != session->app)
			return false;
		set_busy(session, message);
		break;
	case FPLINUX_UI_SESSION_EXIT:
		if (process == &session->launcher) {
			if (!process->policy.closing)
				process->failed = true;
			request_close(session, process);
		} else if (!process->policy.closing) {
			prepare_user_close(session, process);
		}
		break;
	case FPLINUX_UI_SESSION_CLOSE_REPLY:
		if (process == &session->launcher)
			return false;
		close_reply(session, process, message);
		break;
	case FPLINUX_UI_SESSION_KEEP_SCREEN_ON:
		process->keep_screen_on = message->enabled;
		break;
	case FPLINUX_UI_SESSION_VIBRATE:
		if (process != session->app)
			break;
		if (!message->pattern_count) {
			fplinux_session_vibration_stop(&session->vibration);
		} else if (!session->has_vibrator ||
			   !foreground_media_eligible(session)) {
			reply(process, FPLINUX_UI_SESSION_ERROR, "vibration",
			      "",
			      strerror(session->has_vibrator ? EAGAIN :
							       ENODEV));
		} else {
			session->vibration_pid = process->policy.pid;
			fplinux_session_vibration_play(&session->vibration,
						       message->pattern,
						       message->pattern_count,
						       message->repeat,
						       now_ms(session));
		}
		break;
	case FPLINUX_UI_SESSION_SETTING:
		if (strcmp(process->policy.app_id, FPLINUX_SESSION_SETTINGS))
			return false;
		/* Settings parked on Home has no active settings authority. */
		if (process != session->app || !app_in_foreground(session) ||
		    launcher_on_top(session)) {
			reply(process, FPLINUX_UI_SESSION_ERROR, message->name,
			      message->value, "not-foreground");
			break;
		}
		handle_setting(session, process, message);
		break;
	case FPLINUX_UI_SESSION_CLOCK:
		return handle_clock(session, process, message);
	case FPLINUX_UI_SESSION_DISMISS:
		if (process != &session->launcher ||
		    (strcmp(message->notice, "battery-low") &&
		     strcmp(message->notice, "battery-critical")))
			return false;
		/* A low notice closed after it became critical leaves that. */
		if (!strcmp(message->notice,
			    battery_notice_code(session->battery_notice))) {
			session->battery_notice =
				FPLINUX_SESSION_BATTERY_ALERT_NONE;
			session->battery_notice_drawn = false;
		}
		break;
	case FPLINUX_UI_SESSION_MODE:
		if (process == &session->launcher ||
		    (message->mode != FPLINUX_UI_SESSION_INTEGRATED &&
		     message->mode != FPLINUX_UI_SESSION_FULLSCREEN))
			return false;
		/* switch_mode() applies retained requests when the task returns. */
		if (!process->policy.closing &&
		    process->policy.lifecycle != FPLINUX_UI_SESSION_CLOSING &&
		    message->mode != process->requested_mode) {
			process->requested_mode = message->mode;
			process->mode_ms = now_ms(session);
			process->placed_by_buffer = true;
		}
		break;
	case FPLINUX_UI_SESSION_TASKS:
		if (process != &session->launcher || message->has_tasks)
			return false;
		send_tasks(session);
		break;
	case FPLINUX_UI_SESSION_ACTIVATE:
	case FPLINUX_UI_SESSION_CLOSE_TASK: {
		struct process *task;

		if (process != &session->launcher)
			return false;
		task = task_for_id(session, message->app_id);
		if (!task || task->policy.token != message->token ||
		    task->policy.closing ||
		    task->policy.lifecycle == FPLINUX_UI_SESSION_CLOSING ||
		    session->power.locked || notice_covers_app(session) ||
		    launch_in_progress(session) || clock_due(session)) {
			reply(process, FPLINUX_UI_SESSION_ERROR, "", "",
			      "task-unavailable");
			send_tasks(session);
			break;
		}
		if (message->type == FPLINUX_UI_SESSION_ACTIVATE) {
			launch(session, message->app_id,
			       FPLINUX_UI_SESSION_ACTIVATION_RESUME, -1, 0, 0);
		} else {
			prepare_user_close(session, task);
		}
		break;
	}
	default:
		return false;
	}
	return true;
}

static void accept_peers(struct fplinux_session *session)
{
	int fd;

	while ((fd = accept4(session->listener_fd, NULL, NULL,
			     SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
		struct ucred credentials = { 0 };
		socklen_t bytes = sizeof(credentials);
		struct process *process = NULL;
		struct peer *peer = NULL;
		unsigned int i;

		for (i = 0; i < FPLINUX_SESSION_PEERS; ++i)
			if (session->peers[i].fd < 0) {
				peer = &session->peers[i];
				break;
			}
		if (peer && getsockopt(fd, SOL_SOCKET, SO_PEERCRED,
				       &credentials, &bytes) == 0)
			process = process_for_pid(session, credentials.pid);
		if (!process || credentials.uid != process->uid ||
		    !add_fd(session, fd)) {
			close(fd);
			continue;
		}
		peer->fd = fd;
		peer->pid = credentials.pid;
		peer->uid = credentials.uid;
		peer->accepted_ms = now_ms(session);
	}
}

static void disconnect_peer(struct fplinux_session *session, struct peer *peer)
{
	struct process *process = process_for_pid(session, peer->pid);

	if (peer->fd == session->storage_origin_fd) {
		session->storage_origin_token = 0;
		session->storage_origin_fd = -1;
	}
	if (process && process->control_fd == peer->fd) {
		if (!process->policy.closing)
			process->disconnected = true;
		process->control_fd = -1;
		settings_gone(session, process);
		request_close(session, process);
	}
	remove_fd(session, peer->fd);
	memset(peer, 0, sizeof(*peer));
	peer->fd = -1;
}

static void reap_process(struct fplinux_session *session,
			 struct process *process)
{
	int status = -1;
	pid_t pid = process->policy.pid;
	pid_t reaped;
	unsigned int i;
	bool restart_document;

	if (!pid)
		return;
	reaped = waitpid(pid, &status, WNOHANG);
	if (reaped != pid && !(reaped < 0 && errno == ECHILD))
		return;
	if (session->document.request_id &&
	    session->document.origin_token == process->policy.token) {
		bool delivered = document_delivered(session);
		bool owns_launch = session->launch_starting &&
				   session->document.serial ==
					   session->launch_generation;

		memset(&session->document, 0, sizeof(session->document));
		if (owns_launch) {
			if (delivered)
				finish_launch(session);
			else
				fplinux_session_cancel_launch(session);
		}
	}
	restart_document =
		session->document.request_id && session->document.restarting &&
		session->document.handler_token == process->policy.token;
	if (session->document.request_id &&
	    session->document.handler_token == process->policy.token &&
	    !restart_document)
		document_result(session, FPLINUX_UI_DOCUMENT_ERROR,
				process->failed ? ETIMEDOUT : EIO);
	if (!restart_document && process == session->app &&
	    process->policy.closing && !process->failed &&
	    !process->disconnected && process->return_origin)
		session->return_token = process->return_origin;
	/* Descendants cannot outlive the application process group. */
	kill(-pid, SIGKILL);
	/*
	 * An unrequested exit hangs up first, which marks the process closing;
	 * its exit status still decides whether that was a crash. A child that
	 * never connected and exited with the exec/credential failure status
	 * could not start.
	 */
	if (!process->cancelled &&
	    (!process->policy.closing || process->disconnected) &&
	    process != &session->launcher &&
	    (status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
		bool start_failed = !process->connected && WIFEXITED(status) &&
				    (WEXITSTATUS(status) == 126 ||
				     WEXITSTATUS(status) == 127);

		copy_value(session->error, sizeof(session->error),
			   start_failed ? "app-start-failed" : "app-crashed");
	}
	if (process == &session->launcher &&
	    (!process->policy.closing || process->disconnected ||
	     process->failed))
		++session->launcher_failures;
	for (i = 0; i < FPLINUX_SESSION_PEERS; ++i)
		if (session->peers[i].fd >= 0 && session->peers[i].pid == pid) {
			remove_fd(session, session->peers[i].fd);
			memset(&session->peers[i], 0,
			       sizeof(session->peers[i]));
			session->peers[i].fd = -1;
		}
	remove_fd(session, process->pid_fd);
	fplinux_session_audio_gone(&session->audio, pid, now_ms(session));
	fprintf(stderr, "fplinux-session: reaped %s pid=%ld status=%d\n",
		process->policy.app_id, (long)pid, status);
	settings_gone(session, process);
	if (process == session->app && !session->pending_id[0])
		finish_launch(session);
	if (process == session->app) {
		session->app = &session->idle_app;
	}
	initialize_process(process);
	if (restart_document) {
		session->document.waiting = false;
		session->document.handler_token = 0;
	}
	fplinux_session_power_activity(&session->power, false, now_ms(session));
	send_active(session);
	send_tasks(session);
	changed(session);
}

/*
 * Swaps the foreground application and the launcher in one repaint, so that
 * no frame shows neither. An application is shown with its first buffer, an
 * integrated one only with the launcher's frame that draws its bars. A
 * leaving application gets CLOSE with the launcher's frame that no longer
 * draws them; the launcher of a fullscreen application starts for it first.
 * After FPLINUX_SESSION_HANDOFF_MS the swap no longer waits for the launcher.
 */
static void hand_off(struct fplinux_session *session, uint64_t time,
		     bool *needs_tick)
{
	struct process *app = session->app;
	const struct process *launcher = &session->launcher;
	bool connected = is_running(launcher) && launcher->control_fd >= 0;
	bool settled = connected && !session->launcher_wait_serial;
	bool waits;

	if (!is_running(app) || !app->drawn || !app->policy.ready ||
	    (app->policy.shown && !app->leaving))
		return;
	/* An old buffer and the launcher timeout cannot acknowledge activation. */
	if (!app->leaving && session->launch_starting &&
	    !session->activation_ready) {
		*needs_tick = true;
		return;
	}
	/* Nothing to swap on screen; a hidden launcher does not draw either. */
	if (launcher_on_top(session) ||
	    session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF)
		waits = false;
	else if (app->leaving)
		waits = !(settled && !session->launcher_draws_app) &&
			(connected || (!session->destroying &&
				       session->launcher_failures < 3));
	else
		waits = app->policy.mode == FPLINUX_UI_SESSION_INTEGRATED &&
			connected && !(settled && session->launcher_draws_app);
	if (waits && time - app->handoff_ms < FPLINUX_SESSION_HANDOFF_MS) {
		*needs_tick = true;
		return;
	}
	if (waits) {
		fprintf(stderr,
			"fplinux-session: the launcher did not draw within %u ms; %s %s without it\n",
			FPLINUX_SESSION_HANDOFF_MS,
			app->leaving ? "closing" : "showing",
			app->policy.app_id);
		/* Later swaps wait for its answers afresh. */
		session->launcher_wait_serial = 0;
		session->launcher_draws_app = session->launcher_named_app;
	}
	if (app->leaving) {
		if (app->park_on_leave)
			park_app(session);
		else
			request_close(session, app);
		*needs_tick = true;
		return;
	}
	app->policy.shown = true;
	changed(session);
}

/*
 * Configures the foreground application for the mode that MODE asked for.
 * Going fullscreen is configured at once. A shown application goes between
 * the bars only once the launcher has drawn them for it, which the launcher
 * of a fullscreen application starts for; after FPLINUX_SESSION_HANDOFF_MS
 * it no longer waits. The shell places the surface in the new box with the
 * first buffer of its size; see place_app().
 */
static void switch_mode(struct fplinux_session *session, uint64_t time,
			bool *needs_tick)
{
	struct process *app = session->app;
	const struct process *launcher = &session->launcher;
	bool connected = is_running(launcher) && launcher->control_fd >= 0;
	bool settled = connected && !session->launcher_wait_serial;
	bool waits = false;

	if (!app_in_foreground(session) ||
	    app->requested_mode == app->policy.configured_mode)
		return;
	/* Nothing to wait for off screen; a hidden launcher does not draw. */
	if (app->requested_mode == FPLINUX_UI_SESSION_INTEGRATED &&
	    app->policy.shown && !launcher_on_top(session) &&
	    session->power.display != FPLINUX_UI_SESSION_DISPLAY_OFF)
		waits = !(settled && session->launcher_draws_app) &&
			(connected || (!session->destroying &&
				       session->launcher_failures < 3));
	if (waits && time - app->mode_ms < FPLINUX_SESSION_HANDOFF_MS) {
		*needs_tick = true;
		return;
	}
	if (waits) {
		fprintf(stderr,
			"fplinux-session: the launcher did not draw within %u ms; %s goes between the bars without it\n",
			FPLINUX_SESSION_HANDOFF_MS, app->policy.app_id);
		/* Later swaps wait for its answers afresh. */
		session->launcher_wait_serial = 0;
		session->launcher_draws_app = session->launcher_named_app;
	}
	app->policy.configured_mode = app->requested_mode;
	changed(session);
}

/*
 * A fullscreen application runs without the launcher, unless it asked for
 * the integrated mode: the launcher then draws the bars, until the
 * application is configured and placed fullscreen again.
 */
static bool app_runs_alone(const struct fplinux_session *session)
{
	const struct process *app = session->app;

	return app->declared_mode == FPLINUX_UI_SESSION_FULLSCREEN &&
	       app->requested_mode == FPLINUX_UI_SESSION_FULLSCREEN &&
	       app->policy.configured_mode == FPLINUX_UI_SESSION_FULLSCREEN &&
	       app->policy.mode == FPLINUX_UI_SESSION_FULLSCREEN;
}

static void manage_processes(struct fplinux_session *session, uint64_t time,
			     bool *needs_tick)
{
	struct fplinux_desktop_entry entry;
	bool locked = session->power.locked;
	unsigned int i;

	for (i = 0; i <= FPLINUX_UI_SESSION_TASK_MAX; ++i) {
		struct process *process = process_at(session, i);

		reap_process(session, process);
		if (!process->policy.pid)
			continue;
		if (!process->policy.ready || !process->policy.framed) {
			*needs_tick = true;
			uint64_t started =
				process == session->app &&
						session->launch_starting ?
					session->launch_ms :
					process->started_ms;

			if (time - started >= FPLINUX_SESSION_STARTUP_MS &&
			    !process->policy.closing &&
			    !(session->document.waiting &&
			      session->document.handler_token ==
				      process->policy.token) &&
			    !(process == session->app &&
			      session->launch_retained)) {
				fprintf(stderr,
					"fplinux-session: startup timed out for %s ready=%d frame=%d\n",
					process->policy.app_id,
					process->policy.ready,
					process->policy.framed);
				process->failed = true;
				request_close(session, process);
			}
		}
		if (process->policy.closing) {
			*needs_tick = true;
			if (time - process->closing_ms >=
				    FPLINUX_SESSION_CLOSE_KILL_MS &&
			    !process->killed) {
				kill(-process->policy.pid, SIGKILL);
				process->killed = true;
			} else if (time - process->closing_ms >=
				   FPLINUX_SESSION_CLOSE_TERM_MS) {
				kill(-process->policy.pid, SIGTERM);
			}
		}
		if (process->close_serial && !process->close_waiting) {
			*needs_tick = true;
			if (time - process->close_requested_ms >=
			    FPLINUX_SESSION_PREPARE_CLOSE_MS)
				finish_user_close(session, process);
		}
		if (process->pid_fd < 0)
			*needs_tick = true;
	}
	if (session->launch_starting && !session->document.waiting) {
		*needs_tick = true;
		if (time - session->launch_ms >= FPLINUX_SESSION_STARTUP_MS) {
			document_launch_result(
				session, FPLINUX_UI_DOCUMENT_ERROR, ETIMEDOUT);
			fplinux_session_cancel_launch(session);
			copy_value(session->error, sizeof(session->error),
				   "app-start-failed");
			send_active(session);
		}
	}
	if (session->return_token && !launch_in_progress(session) &&
	    !session->power.locked && !clock_due(session)) {
		struct process *origin =
			task_for_token(session, session->return_token);

		session->return_token = 0;
		if (origin && is_running(origin) && !origin->policy.closing)
			launch(session, origin->policy.app_id,
			       FPLINUX_UI_SESSION_ACTIVATION_RESUME, -1, 0, 0);
	}
	if (session->document.restarting && !session->document.handler_token &&
	    !launch_in_progress(session)) {
		session->document.restarting = false;
		open_document_with(session, session->document.handler_id);
	}
	if (!session->destroying && !launch_in_progress(session) &&
	    !session->power.locked && !notice_covers_app(session) &&
	    !clock_due(session)) {
		for (i = 0; i < FPLINUX_UI_SESSION_TASK_MAX; ++i) {
			struct process *process = &session->tasks[i];

			if (!process->close_activate ||
			    !process->close_waiting || !is_running(process))
				continue;
			process->close_activate = false;
			session->launcher_tasks_view = false;
			launch(session, process->policy.app_id,
			       FPLINUX_UI_SESSION_ACTIVATION_RESUME, -1, 0, 0);
			break;
		}
	}
	if (!session->destroying && session->pending_id[0] &&
	    !session->app->policy.pid) {
		struct process *existing =
			task_for_id(session, session->pending_id);

		if (session->launch_activation ==
			    FPLINUX_UI_SESSION_ACTIVATION_RESUME &&
		    (!existing || !is_running(existing))) {
			reply(&session->launcher, FPLINUX_UI_SESSION_ERROR, "",
			      "", "task-unavailable");
			clear_pending_launch(session);
			finish_launch(session);
			send_tasks(session);
		} else if (!read_entry(session, session->pending_id, &entry) ||
			   entry.mode == FPLINUX_UI_SESSION_LAUNCHER) {
			document_launch_result(
				session, FPLINUX_UI_DOCUMENT_ERROR, ENOENT);
			copy_value(session->error, sizeof(session->error),
				   "app-unavailable");
			clear_pending_launch(session);
			finish_launch(session);
		} else if (existing && existing->policy.closing) {
			/* No replacement fork before the old instance is reaped. */
			request_close(session, existing);
			*needs_tick = true;
		} else if (entry.mode == FPLINUX_UI_SESSION_FULLSCREEN &&
			   !clock_due(session) &&
			   session->launcher.policy.pid) {
			/* A Home handoff may have restarted the fullscreen launcher. */
			request_close(session, &session->launcher);
			*needs_tick = true;
		} else if (entry.mode != FPLINUX_UI_SESSION_FULLSCREEN ||
			   !session->launcher.policy.pid) {
			if (existing && is_running(existing)) {
				session->app = existing;
				session->app->policy.shown = false;
				session->app->handoff_ms = time;
				set_lifecycle(session, session->app,
					      FPLINUX_UI_SESSION_FOREGROUND);
				send_activation(session, session->app);
				fplinux_session_power_activity(&session->power,
							       false, time);
				changed(session);
			} else {
				struct process *slot = free_task(session);

				if (!slot) {
					document_launch_result(
						session,
						FPLINUX_UI_DOCUMENT_ERROR,
						EUSERS);
					copy_value(session->error,
						   sizeof(session->error),
						   "tasks-full");
					finish_launch(session);
				} else {
					session->app = slot;
					session->launch_retained = false;
					if (!start_process(session, slot,
							   &entry)) {
						document_launch_result(
							session,
							FPLINUX_UI_DOCUMENT_ERROR,
							errno ? errno : EIO);
						session->app =
							&session->idle_app;
						copy_value(
							session->error,
							sizeof(session->error),
							"app-start-failed");
						finish_launch(session);
					}
				}
			}
			clear_pending_launch(session);
			send_active(session);
			*needs_tick = true;
		}
	}
	/*
	 * The keyguard, a notice and a leaving application need
	 * the launcher even above a fullscreen application.
	 */
	if (!session->destroying && !session->launcher.policy.pid &&
	    (!session->app->policy.pid || session->app->policy.closing ||
	     !app_runs_alone(session) || session->app->leaving || locked ||
	     notice_due(session) || clock_due(session)) &&
	    (!session->pending_id[0] || session->app->leaving ||
	     clock_due(session)) &&
	    session->launcher_failures < 3) {
		if (!read_entry(session, FPLINUX_SESSION_LAUNCHER, &entry) ||
		    !start_process(session, &session->launcher, &entry))
			++session->launcher_failures;
		*needs_tick = true;
	}
	/* An unanswered notice keeps it, also while the display is off. */
	if (!locked && app_in_foreground(session) && app_runs_alone(session) &&
	    session->battery_notice == FPLINUX_SESSION_BATTERY_ALERT_NONE &&
	    !clock_due(session) && is_running(&session->launcher))
		request_close(session, &session->launcher);
	for (i = 0; i < FPLINUX_SESSION_PEERS; ++i) {
		struct peer *peer = &session->peers[i];

		if (peer->fd < 0 || peer->authenticated)
			continue;
		*needs_tick = true;
		if (time - peer->accepted_ms >= 1000)
			disconnect_peer(session, peer);
	}
	if (is_running(&session->launcher) && session->launcher.policy.ready &&
	    session->launcher.policy.framed) {
		session->launcher_failures = 0;
		if (!session->ready) {
			session->ready = true;
			changed(session);
		}
	}
	hand_off(session, time, needs_tick);
	switch_mode(session, time, needs_tick);
}

static void consider(uint64_t *earliest, uint64_t time)
{
	if (!*earliest || time < *earliest)
		*earliest = time;
}

static void settle_held_scene(struct fplinux_session *session)
{
	const struct process *launcher = &session->launcher;
	const struct process *requester;
	bool launcher_ready = is_running(launcher) && launcher->policy.ready &&
			      launcher->policy.framed &&
			      session->last_active_sent;
	bool launcher_current = launcher_ready &&
				session->launcher_ack_serial ==
					session->last_active.serial;

	if (session->held_scene == HELD_SCENE_NONE)
		return;
	if (session->power.locked ||
	    session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
		clear_held_scene(session);
		return;
	}
	if (session->held_scene & HELD_SCENE_APPEARANCE) {
		requester = process_for_pid(session, session->redraw_pid);
		if (!requester || !is_running(requester) ||
		    requester->policy.token != session->redraw_token ||
		    requester != session->app || requester->leaving ||
		    (session->redraw_ready &&
		     (launcher_current || session->launcher_failures >= 3)))
			release_scene(session, HELD_SCENE_APPEARANCE);
	}
	if (session->held_scene & HELD_SCENE_CLOSE) {
		requester = process_for_pid(session, session->close_pid);
		if (!requester || !is_running(requester) ||
		    requester->policy.token != session->close_token ||
		    (requester == session->app && requester->policy.shown &&
		     session->close_frame_ready &&
		     (requester->policy.mode == FPLINUX_UI_SESSION_FULLSCREEN ||
		      launcher_current || session->launcher_failures >= 3)))
			release_scene(session, HELD_SCENE_CLOSE);
	}
	if (launch_in_progress(session) || session->pending_id[0])
		return;
	if ((session->held_scene & HELD_SCENE_LAUNCH) &&
	    app_in_foreground(session) && session->app->policy.shown)
		release_scene(session, HELD_SCENE_LAUNCH | HELD_SCENE_HOME);
	if (((launcher_ready && !session->launcher_wait_serial &&
	      !session->launcher_draws_app) ||
	     session->launcher_failures >= 3) &&
	    (!is_running(session->app) || !session->app->policy.shown))
		release_scene(session, HELD_SCENE_HOME | HELD_SCENE_LAUNCH);
}

static void arm_alarm(struct fplinux_session *session)
{
	int64_t due = fplinux_session_clock_alarm_deadline(&session->clock);
	struct itimerspec timer = { 0 };

	if (due == session->alarm_due_utc)
		return;
	timer.it_value.tv_sec = due;
	if (timerfd_settime(session->alarm_fd,
			    TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &timer,
			    NULL) < 0) {
		fprintf(stderr, "fplinux-session: cannot arm alarm: %s\n",
			strerror(errno));
		return;
	}
	session->alarm_due_utc = due;
}

static void advance(struct fplinux_session *session)
{
	struct itimerspec timer = { 0 };
	struct fplinux_ui_session_clock_presentation clock_presentation;
	uint64_t time = now_ms(session);
	uint64_t earliest = 0;
	bool needs_tick = false;
	uint64_t old_clock_event = session->clock.model.active.id;
	uint64_t old_clock_missed = session->clock.model.missed_total;

	fplinux_session_power_advance(&session->power, time);
	fplinux_session_audio_dispatch(&session->audio, time);
	if (!session->clock_ready) {
		struct clock_result result = fplinux_session_clock_init(
			&session->clock, wall_utc(session), time,
			session->options.clock_preferences_directory);

		session->clock_ready = result.applied;
	} else {
		fplinux_session_clock_tick(&session->clock, wall_utc(session),
					   time);
	}
	arm_alarm(session);
	fplinux_session_clock_presentation(&session->clock,
					   &clock_presentation);
	if (session->settings_retry_ms && time >= session->settings_retry_ms)
		save_settings(session);
	clock_power(session, time);
	manage_clock_signal(session, time, &needs_tick);
	if (clock_due(session))
		clear_held_scene(session);
	poll_power(session, time);
	manage_processes(session, time, &needs_tick);
	settle_held_scene(session);
	fplinux_session_power_set_keep_screen_on(
		&session->power, keeps_screen_on(session), time);
	if (session->lit_display == FPLINUX_UI_SESSION_DISPLAY_OFF &&
	    session->power.display != FPLINUX_UI_SESSION_DISPLAY_OFF) {
		/* Clients redraw before the backlight comes back. */
		session->light_pending = true;
		fplinux_session_output_reset(session);
	}
	session->lit_display = session->power.display;
	if (session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
		session->light_pending = false;
		fplinux_session_output_reset(session);
	}
	if (session->was_locked != session->power.locked) {
		session->was_locked = session->power.locked;
		fprintf(stderr, "fplinux-session: keyguard %s\n",
			session->power.locked ? "engaged" : "released");
		changed(session);
	}
	if (session->was_raised != fplinux_session_launcher_on_top(session) ||
	    session->was_on_top != launcher_on_top(session)) {
		session->was_raised = fplinux_session_launcher_on_top(session);
		session->was_on_top = launcher_on_top(session);
		changed(session);
	}
	update_visibility(session);
	advance_media(session, time, &earliest);
	if (session->launch_shown != launch_is_shown(session)) {
		session->launch_shown = launch_is_shown(session);
		changed(session);
	}
	if (old_clock_event != session->clock.model.active.id ||
	    old_clock_missed != session->clock.model.missed_total ||
	    memcmp(&clock_presentation,
		   &session->last_active.clock_presentation,
		   sizeof(clock_presentation)) ||
	    !session->active_known ||
	    session->active_display != session->power.display ||
	    session->active_locked != session->power.locked ||
	    session->active_notice != session->power.notice ||
	    session->active_battery_notice != session->battery_notice ||
	    session->active_charging != session->charger_online ||
	    session->active_battery != (session->battery.known ?
						fplinux_session_battery_percent(
							&session->battery) :
						-1))
		send_active(session);
	apply_lights(session);
	if (needs_tick)
		consider(&earliest, time + FPLINUX_SESSION_PROCESS_TICK_MS);
	if (launch_in_progress(session) &&
	    time - session->launch_ms < FPLINUX_SESSION_PROGRESS_MS)
		consider(&earliest,
			 session->launch_ms + FPLINUX_SESSION_PROGRESS_MS);
	if (fplinux_session_power_deadline(&session->power))
		consider(&earliest,
			 fplinux_session_power_deadline(&session->power));
	if (session->options.power_supply_dir)
		consider(&earliest, session->charger_poll_ms);
	if (session->settings_retry_ms)
		consider(&earliest, session->settings_retry_ms);
	if (fplinux_session_clock_deadline(&session->clock, time))
		consider(&earliest,
			 fplinux_session_clock_deadline(&session->clock, time));
	if (earliest) {
		uint64_t delay = earliest > time ? earliest - time : 1;

		timer.it_value.tv_sec = delay / 1000;
		timer.it_value.tv_nsec = (long)(delay % 1000) * 1000000;
	}
	timerfd_settime(session->timer_fd, 0, &timer, NULL);
}

void fplinux_session_dispatch(struct fplinux_session *session)
{
	struct epoll_event events[FPLINUX_SESSION_PEERS + 4];
	struct fplinux_ui_session_message message;
	uint64_t ticks;
	unsigned int i;

	epoll_wait(session->epoll_fd, events,
		   sizeof(events) / sizeof(events[0]), 0);
	read(session->timer_fd, &ticks, sizeof(ticks));
	if (read(session->alarm_fd, &ticks, sizeof(ticks)) < 0 &&
	    errno == ECANCELED)
		/* The next advance reticks wall alarms and rearms the same UTC. */
		session->alarm_due_utc = -1;
	accept_peers(session);
	for (i = 0; i < FPLINUX_SESSION_PEERS; ++i) {
		struct peer *peer = &session->peers[i];
		unsigned int count;

		if (peer->fd < 0)
			continue;
		for (count = 0; count < 32; ++count) {
			int result =
				fplinux_ui_session_receive(peer->fd, &message);

			if (!result)
				break;
			bool accepted =
				result > 0 &&
				receive_message(session, peer, &message);

			/* receive() owns ancillary fds until this borrowed callback ends. */
			if (result > 0 && message.has_frame)
				close(message.frame_fd);
			if (!accepted) {
				disconnect_peer(session, peer);
				break;
			}
		}
	}
	dispatch_storage(session);
	advance(session);
}

static void load_settings(struct fplinux_session *session)
{
	const char *path = session->options.settings_path;

	fplinux_session_settings_defaults(&session->settings,
					  session->options.languages_dir);
	if (path &&
	    !fplinux_session_settings_load(
		    &session->settings, session->options.languages_dir, path))
		fprintf(stderr, "fplinux-session: cannot read %s: %s\n", path,
			strerror(errno));
	else if (path && access(path, F_OK) == 0)
		session->settings_storage.state = FPLINUX_STORAGE_DURABLE;
	/* The user's saved level replaces the service's boot default. */
	session->brightness_pending_set = session->settings.has_brightness;
	session->user_level = session->settings.brightness;
}

struct fplinux_session *
fplinux_session_create(const struct fplinux_session_options *options)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	const struct fplinux_ui_profile_spec *smallest =
		fplinux_ui_profile_spec(0);
	const struct fplinux_ui_profile_spec *profile;
	struct fplinux_session *session;
	unsigned int i;
	int error;

	if (!options || options->width <= 0 || options->height <= 0 ||
	    strlen(options->socket_path) >= sizeof(address.sun_path)) {
		errno = EINVAL;
		return NULL;
	}
	/* An integrated application needs at least one row between the bars. */
	profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(options->width, options->height));
	if (options->height <=
	    profile->status_height + profile->softkey_height) {
		errno = EINVAL;
		return NULL;
	}
	if (options->width < smallest->width ||
	    options->height < smallest->height)
		fprintf(stderr,
			"fplinux-session: output %dx%d is below the smallest profile %dx%d\n",
			options->width, options->height, smallest->width,
			smallest->height);
	session = calloc(1, sizeof(*session));
	if (!session)
		return NULL;
	session->options = *options;
	session->output_width = options->width;
	session->output_height = options->height;
	session->profile = profile;
	/* The profile's catalogue presentation, until the user picks one. */
	session->launcher_list = profile->catalogue_list;
	session->epoll_fd = -1;
	session->listener_fd = -1;
	session->timer_fd = -1;
	session->alarm_fd = -1;
	session->storage_origin_fd = -1;
	session->clock_signal_fd = -1;
	fplinux_session_vibrator_init(&session->vibrator);
	fplinux_session_audio_init(&session->audio, NULL, -1);
	fplinux_session_vibration_init(&session->vibration, now_ms(session));
	session->brightness.fd = -1;
	session->brightness_shown = -1;
	session->led_state = -1;
	session->launcher_home_view = true;
	initialize_process(&session->launcher);
	initialize_process(&session->idle_app);
	session->app = &session->idle_app;
	for (i = 0; i < FPLINUX_UI_SESSION_TASK_MAX; ++i)
		initialize_process(&session->tasks[i]);
	for (i = 0; i < FPLINUX_SESSION_PEERS; ++i)
		session->peers[i].fd = -1;
	load_settings(session);
	/* A reset RTC must not prevent the graphical session from starting. */
	session->clock.model.snooze_minutes = 5;
	fplinux_session_power_init(&session->power,
				   session->settings.screen_off_seconds,
				   session->settings.auto_lock_seconds,
				   now_ms(session));
	session->lit_display = session->power.display;
	session->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	session->listener_fd = socket(
		AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	session->timer_fd =
		timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	session->alarm_fd =
		timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
	if (session->epoll_fd < 0 || session->listener_fd < 0 ||
	    session->timer_fd < 0 || session->alarm_fd < 0)
		goto fail;
	strcpy(address.sun_path, options->socket_path);
	if (bind(session->listener_fd, (struct sockaddr *)&address,
		 sizeof(address)) < 0)
		goto fail;
	session->owns_socket = true;
	if (chmod(options->socket_path, 0600) < 0 ||
	    (geteuid() == 0 && chown(options->socket_path, options->app_uid,
				     options->app_gid) < 0) ||
	    listen(session->listener_fd, FPLINUX_SESSION_PEERS) < 0 ||
	    !add_fd(session, session->listener_fd) ||
	    !add_fd(session, session->timer_fd) ||
	    !add_fd(session, session->alarm_fd))
		goto fail;
	open_keypad_light(session);
	session->has_audio = options->audio_socket && options->audio_service &&
			     options->audio_device &&
			     access(options->audio_service, X_OK) == 0 &&
			     access(options->audio_device, F_OK) == 0;
	fplinux_session_audio_init(&session->audio,
				   session->has_audio ? options->audio_socket :
							NULL,
				   session->epoll_fd);
	if (options->vibrate)
		session->has_vibrator = true;
	else if (options->input_dir) {
		session->has_vibrator = fplinux_session_vibrator_open(
			&session->vibrator, options->input_dir,
			FPLINUX_SESSION_VIBRATION_PULSE_MS, UINT16_MAX);
		if (!session->has_vibrator)
			fprintf(stderr,
				"fplinux-session: vibration unavailable: %s\n",
				strerror(errno));
	}
	advance(session);
	return session;
fail:
	error = errno;
	fplinux_session_destroy(session);
	errno = error;
	return NULL;
}

void fplinux_session_destroy(struct fplinux_session *session)
{
	uint64_t started;
	unsigned int i;

	if (!session)
		return;
	session->destroying = true;
	fplinux_session_vibration_stop(&session->vibration);
	show_vibration(session, false);
	clear_held_scene(session);
	/* All clients get the same save window, independent of the session clock. */
	started = system_ms();
	stop_clock_signal(session, started);
	for (i = 0; i <= FPLINUX_UI_SESSION_TASK_MAX; ++i)
		request_close(session, process_at(session, i));
	for (;;) {
		struct timespec pause = { .tv_nsec = 10000000 };
		uint64_t elapsed_ms = system_ms() - started;
		bool waiting = false;

		if (session->clock_signal_pid) {
			pid_t reaped = waitpid(session->clock_signal_pid, NULL,
					       WNOHANG);

			if (reaped == session->clock_signal_pid ||
			    (reaped < 0 && errno == ECHILD)) {
				session->clock_signal_pid = 0;
			} else if (elapsed_ms >=
				   FPLINUX_SESSION_CLOSE_KILL_MS) {
				kill(session->clock_signal_pid, SIGKILL);
				while (waitpid(session->clock_signal_pid, NULL,
					       0) < 0 &&
				       errno == EINTR)
					;
				session->clock_signal_pid = 0;
			} else {
				waiting = true;
			}
		}

		for (i = 0; i <= FPLINUX_UI_SESSION_TASK_MAX; ++i) {
			struct process *process = process_at(session, i);
			pid_t pid = process->policy.pid;
			pid_t reaped;

			if (!pid)
				continue;
			reaped = waitpid(pid, NULL, WNOHANG);
			if (reaped == pid || (reaped < 0 && errno == ECHILD)) {
				kill(-pid, SIGKILL);
				process->policy.pid = 0;
				continue;
			}
			if (elapsed_ms >= FPLINUX_SESSION_CLOSE_KILL_MS) {
				kill(-pid, SIGKILL);
				while (waitpid(pid, NULL, 0) < 0 &&
				       errno == EINTR)
					;
				process->policy.pid = 0;
			} else {
				waiting = true;
				if (elapsed_ms >= FPLINUX_SESSION_CLOSE_TERM_MS)
					kill(-pid, SIGTERM);
			}
		}
		if (!waiting)
			break;
		nanosleep(&pause, NULL);
	}
	for (i = 0; i <= FPLINUX_UI_SESSION_TASK_MAX; ++i) {
		struct process *process = process_at(session, i);

		remove_fd(session, process->pid_fd);
	}
	for (i = 0; i < FPLINUX_SESSION_PEERS; ++i)
		remove_fd(session, session->peers[i].fd);
	remove_fd(session, session->alarm_fd);
	remove_fd(session, session->timer_fd);
	remove_fd(session, session->listener_fd);
	remove_fd(session, session->clock_signal_fd);
	fplinux_session_audio_close(&session->audio);
	if (session->storage) {
		epoll_ctl(session->epoll_fd, EPOLL_CTL_DEL,
			  fplinux_session_storage_fd(session->storage), NULL);
		fplinux_session_storage_destroy(session->storage);
	}
	fplinux_session_clock_cleanup(&session->clock);
	if (!fplinux_session_vibrator_close(&session->vibrator))
		fprintf(stderr, "fplinux-session: cannot close vibrator: %s\n",
			strerror(errno));
	if (session->epoll_fd >= 0)
		close(session->epoll_fd);
	if (session->owns_socket)
		unlink(session->options.socket_path);
	/* The service keeps the last SET level; a preview must not stay. */
	end_brightness_preview(session);
	/* Closing the client ends any lease; the service restores the level. */
	fplinux_brightness_close(&session->brightness);
	fplinux_keypad_light_close(&session->keypad_light);
	free(session);
}

int fplinux_session_get_fd(struct fplinux_session *session)
{
	return session->epoll_fd;
}

const struct fplinux_session_process *
fplinux_session_find_process(struct fplinux_session *session, pid_t pid)
{
	struct process *process = process_for_pid(session, pid);

	return process ? &process->policy : NULL;
}

pid_t fplinux_session_focused_pid(struct fplinux_session *session)
{
	if (launcher_on_top(session))
		return is_running(&session->launcher) ?
			       session->launcher.policy.pid :
			       0;
	if (launch_in_progress(session))
		return 0;
	if (app_in_foreground(session))
		return session->app->policy.pid;
	if (!session->launcher.policy.closing)
		return session->launcher.policy.pid;
	return 0;
}

bool fplinux_session_is_ready(struct fplinux_session *session)
{
	return session->ready;
}

void fplinux_session_get_launch(struct fplinux_session *session,
				struct fplinux_session_launch *state)
{
	*state = (struct fplinux_session_launch){
		.active = launch_in_progress(session),
		.shown = launch_is_shown(session),
		.generation = session->launch_generation,
		.app_id = session->launch_id,
		.title = session->launch_title,
	};
}

bool fplinux_session_is_locked(struct fplinux_session *session)
{
	return session->power.locked;
}

bool fplinux_session_launcher_on_top(struct fplinux_session *session)
{
	return session->power.locked || notice_raises_launcher(session) ||
	       clock_raises_launcher(session);
}

/*
 * Focus and key routing read the new state at once. Visibility messages
 * follow from the next dispatch, which the timer makes immediate, so the
 * shell's placement pass does not run back into the manager.
 */
void fplinux_session_set_launcher_shown(struct fplinux_session *session,
					bool shown)
{
	struct itimerspec soon = { .it_value.tv_nsec = 1000000 };

	if (session->launcher_shown == shown)
		return;
	session->launcher_shown = shown;
	timerfd_settime(session->timer_fd, 0, &soon, NULL);
}

void fplinux_session_surface_box(struct fplinux_session *session,
				 enum fplinux_ui_session_mode mode, int *x,
				 int *y, int *width, int *height)
{
	int bars = session->profile->status_height +
		   session->profile->softkey_height;

	*x = 0;
	*y = 0;
	*width = session->output_width;
	*height = session->output_height;
	if (mode != FPLINUX_UI_SESSION_INTEGRATED)
		return;
	*y = session->profile->status_height;
	/* Only a resize after start can leave less; the surface keeps a row. */
	*height = session->output_height > bars ?
			  session->output_height - bars :
			  1;
}

void fplinux_session_output_resized(struct fplinux_session *session, int width,
				    int height)
{
	if (width <= 0 || height <= 0 ||
	    (width == session->output_width &&
	     height == session->output_height))
		return;
	fprintf(stderr,
		"fplinux-session: output resized to %dx%d; running clients keep their size\n",
		width, height);
	session->output_width = width;
	session->output_height = height;
	clear_held_scene(session);
	session->profile = fplinux_ui_profile_spec(
		fplinux_ui_profile_for_screen(width, height));
	send_active(session);
}

enum fplinux_ui_session_theme
fplinux_session_theme(struct fplinux_session *session)
{
	return session->settings.theme;
}

void fplinux_session_frame(struct fplinux_session *session, pid_t pid)
{
	struct process *process = process_for_pid(session, pid);

	if (process && !process->policy.framed) {
		process->policy.framed = true;
		advance(session);
	}
}

/*
 * An application that asked for another mode is placed in the box whose size
 * its buffer has, so that the placement matches the buffer on the screen
 * whatever the order of its configures and commits. A buffer of neither size
 * keeps the placement, and so does a closing application. Returns whether the
 * placement changed.
 */
static bool place_app(struct fplinux_session *session, int width, int height)
{
	static const enum fplinux_ui_session_mode modes[] = {
		FPLINUX_UI_SESSION_FULLSCREEN,
		FPLINUX_UI_SESSION_INTEGRATED,
	};
	struct process *app = session->app;
	unsigned int i;

	if (!app->placed_by_buffer || app->policy.closing)
		return false;
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
		int x, y, box_width, box_height;

		fplinux_session_surface_box(session, modes[i], &x, &y,
					    &box_width, &box_height);
		if (width != box_width || height != box_height)
			continue;
		if (app->policy.mode == modes[i])
			return false;
		app->policy.mode = modes[i];
		changed(session);
		return true;
	}
	return false;
}

void fplinux_session_surface_committed(struct fplinux_session *session,
				       pid_t pid, int width, int height)
{
	struct process *process = process_for_pid(session, pid);

	/* A late buffer cannot reappear or finish a replacement launch. */
	if (!process || process->policy.closing || process->leaving)
		return;
	bool placed = process == session->app &&
		      place_app(session, width, height);
	bool busy_finished = process == session->app && process->policy.ready &&
			     session->app_busy_finishing;

	if (process->wake_due)
		process->wake_committed = true;
	if (busy_finished) {
		session->app_busy_finishing = false;
		if (!session->app_busy && !session->launch_starting)
			finish_launch(session);
	}
	if (process == session->app && !process->drawn) {
		process->drawn = true;
		process->handoff_ms = now_ms(session);
		finish_activation(session);
		/* The launcher may draw its bars now. */
		send_active(session);
	} else if (busy_finished) {
		send_active(session);
	} else if (!placed) {
		return;
	}
	/* A swap may be due, or a new placement closes the launcher. */
	advance(session);
}

/* The launcher supplies the whole view, or the bars of an integrated app. */
static void wake_clients(struct fplinux_session *session,
			 struct process **launcher, struct process **app)
{
	bool show_app = session->app->policy.shown &&
			is_running(session->app) &&
			!fplinux_session_launcher_on_top(session);

	*app = show_app ? session->app : NULL;
	*launcher = !show_app || session->app->policy.mode !=
					    FPLINUX_UI_SESSION_FULLSCREEN ?
			    &session->launcher :
			    NULL;
}

static bool wake_client_ready(const struct fplinux_session *session,
			      const struct process *process, const pid_t *pids,
			      size_t count)
{
	if (!process)
		return true;
	if (!is_running(process) || process->control_fd < 0 ||
	    !process->policy.ready || !process->visible ||
	    (process->wake_due && !process->wake_committed))
		return false;
	/* ACTIVE_ACK follows the commit containing the current launcher view. */
	if (process == &session->launcher &&
	    (!session->last_active_sent ||
	     session->launcher_ack_serial != session->last_active.serial))
		return false;
	for (size_t i = 0; i < count; ++i)
		if (pids[i] == process->policy.pid)
			return true;
	return false;
}

void fplinux_session_output_latched(struct fplinux_session *session,
				    uint64_t serial, const pid_t *pids,
				    size_t count)
{
	struct process *launcher, *app;

	if (!session->light_pending)
		return;
	wake_clients(session, &launcher, &app);
	session->wake_submission = (struct wake_submission){
		.serial = serial,
		.launcher_pid = launcher ? launcher->policy.pid : 0,
		.app_pid = app ? app->policy.pid : 0,
		.active_serial = launcher ? session->last_active.serial : 0,
		.ready = wake_client_ready(session, launcher, pids, count) &&
			 wake_client_ready(session, app, pids, count),
	};
}

void fplinux_session_output_presented(struct fplinux_session *session,
				      uint64_t serial)
{
	const struct wake_submission *submission = &session->wake_submission;
	struct process *launcher, *app;

	if (!session->light_pending || !serial ||
	    serial != submission->serial || !submission->ready)
		return;
	wake_clients(session, &launcher, &app);
	if (submission->launcher_pid != (launcher ? launcher->policy.pid : 0) ||
	    submission->app_pid != (app ? app->policy.pid : 0) ||
	    submission->active_serial !=
		    (launcher ? session->last_active.serial : 0))
		return;
	session->light_pending = false;
	fplinux_session_output_reset(session);
	advance(session);
}

void fplinux_session_output_reset(struct fplinux_session *session)
{
	memset(&session->wake_submission, 0, sizeof(session->wake_submission));
}

bool fplinux_session_wake_pending(const struct fplinux_session *session)
{
	return session->light_pending;
}

static int clock_key_slot(enum fplinux_session_key_source source,
			  unsigned int code)
{
	if (code == (source == FPLINUX_SESSION_KEY_PHONE ?
			     FPLINUX_KEY_SOFT_LEFT :
			     KEY_COMPOSE))
		return 0;
	if (code == (source == FPLINUX_SESSION_KEY_PHONE ?
			     FPLINUX_KEY_SOFT_RIGHT :
			     KEY_ESC))
		return 1;
	switch (code) {
	case KEY_UP:
		return 2;
	case KEY_DOWN:
		return 3;
	case KEY_LEFT:
		return 4;
	case KEY_RIGHT:
		return 5;
	default:
		return -1;
	}
}

/* A press/release belongs to one shown ring, including across queue promotion. */
static bool clock_key(struct fplinux_session *session,
		      enum fplinux_session_key_source source, unsigned int code,
		      bool pressed, bool *deliver)
{
	int slot = clock_key_slot(source, code);
	uint64_t event_id = session->clock.model.active.id;
	bool eligible = clock_covers_app(session) &&
			session->power.display !=
				FPLINUX_UI_SESSION_DISPLAY_OFF;

	*deliver = false;
	if (code < KEY_CNT && session->clock_blocked_keys[source][code]) {
		if (!pressed)
			session->clock_blocked_keys[source][code] = false;
		return true;
	}
	if (slot >= 0) {
		uint64_t *owner = &session->clock_key_events[source][slot];

		if (!pressed && *owner) {
			*deliver = eligible && *owner == event_id;
			*owner = 0;
			return true;
		}
		if (eligible) {
			if (pressed && !*owner)
				*owner = event_id;
			/* A release from the application never becomes a ring action. */
			*deliver = pressed && *owner == event_id;
			return true;
		}
	}
	if (pressed && clock_due(session) && session->power.locked) {
		if (session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF) {
			fplinux_session_power_activity(&session->power, true,
						       now_ms(session));
			session->clock_display_suppressed = false;
		}
		if (code < KEY_CNT)
			session->clock_blocked_keys[source][code] = true;
		return true;
	}
	return false;
}

bool fplinux_session_key(struct fplinux_session *session, unsigned int code,
			 bool pressed, uint64_t time_ms)
{
	uint64_t time = now_ms(session);
	bool deliver = false;

	if (code == FPLINUX_KEY_POWER) {
		bool home_view = !launch_in_progress(session) &&
				 !app_in_foreground(session) &&
				 session->launcher_home_view &&
				 !session->launcher_tasks_view &&
				 !clock_due(session);

		if (fplinux_session_power_red_key(&session->power, pressed,
						  time_ms, home_view, time) ==
			    FPLINUX_SESSION_RED_HOME &&
		    !clock_due(session)) {
			if (launch_in_progress(session))
				fplinux_session_cancel_launch(session);
			else
				fplinux_session_home(session);
		}
		if (clock_due(session) && !pressed)
			session->clock_display_suppressed =
				session->power.display ==
				FPLINUX_UI_SESSION_DISPLAY_OFF;
	} else {
		bool clock_handled = clock_key(session,
					       FPLINUX_SESSION_KEY_PHONE, code,
					       pressed, &deliver);

		if (!clock_handled)
			deliver = fplinux_session_power_key(
				&session->power, FPLINUX_SESSION_KEY_PHONE,
				code, pressed, time);
		else if (!pressed)
			fplinux_session_power_key(&session->power,
						  FPLINUX_SESSION_KEY_PHONE,
						  code, false, time);

		if (code == FPLINUX_KEY_SOFT_RIGHT &&
		    session->launch_right_held) {
			if (!pressed)
				session->launch_right_held = false;
			deliver = false;
		} else if (deliver && pressed &&
			   code == FPLINUX_KEY_SOFT_RIGHT &&
			   launch_in_progress(session) &&
			   !launcher_on_top(session)) {
			session->launch_right_held = true;
			fplinux_session_cancel_launch(session);
			deliver = false;
		}
	}
	advance(session);
	return deliver;
}

bool fplinux_session_keyboard_key(struct fplinux_session *session,
				  unsigned int code, bool pressed)
{
	bool deliver = false;
	bool clock_handled = clock_key(session, FPLINUX_SESSION_KEY_KEYBOARD,
				       code, pressed, &deliver);

	if (!clock_handled)
		deliver = fplinux_session_power_key(
			&session->power, FPLINUX_SESSION_KEY_KEYBOARD, code,
			pressed, now_ms(session));
	else if (!pressed)
		fplinux_session_power_key(&session->power,
					  FPLINUX_SESSION_KEY_KEYBOARD, code,
					  false, now_ms(session));

	if (code == KEY_ESC && session->launch_escape_held) {
		if (!pressed)
			session->launch_escape_held = false;
		deliver = false;
	} else if (deliver && pressed && code == KEY_ESC &&
		   launch_in_progress(session) && !launcher_on_top(session)) {
		session->launch_escape_held = true;
		fplinux_session_cancel_launch(session);
		deliver = false;
	}

	advance(session);
	return deliver;
}

void fplinux_session_keyboard_home(struct fplinux_session *session)
{
	if (clock_due(session))
		return;
	if (fplinux_session_power_system_key(&session->power,
					     now_ms(session))) {
		if (launch_in_progress(session))
			fplinux_session_cancel_launch(session);
		else if (!clock_due(session))
			fplinux_session_home(session);
	}
	advance(session);
}

void fplinux_session_cancel_input(struct fplinux_session *session,
				  enum fplinux_session_key_source source)
{
	fplinux_session_power_cancel_source(&session->power, source);
	if ((unsigned int)source < FPLINUX_SESSION_KEY_SOURCE_COUNT) {
		memset(session->clock_key_events[source], 0,
		       sizeof(session->clock_key_events[source]));
		memset(session->clock_blocked_keys[source], 0,
		       sizeof(session->clock_blocked_keys[source]));
	}
	if (source == FPLINUX_SESSION_KEY_PHONE)
		session->launch_right_held = false;
	else if (source == FPLINUX_SESSION_KEY_KEYBOARD)
		session->launch_escape_held = false;
}
