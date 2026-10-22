/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_CORE_H
#define FPLINUX_SESSION_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "fplinux-ui-session.h"
#include "session-power.h"

struct fplinux_session;

/*
 * Applications and the launcher run as user (app_uid, app_gid) with home;
 * desktop entries in applications must belong to metadata_uid and not be
 * writable by group or others.
 *
 * The optional system paths select the persisted settings file, the language
 * files that define the system languages, the brightness service socket, the
 * LED class directory searched for the keypad light, the power-supply class
 * directory polled for chargers and the RTC. NULL disables that boundary;
 * without language files the system language is the built-in English.
 * clock_ms returns
 * CLOCK_MONOTONIC milliseconds; NULL selects the system clock.
 * clock_preferences_directory is the private directory for the clock owner;
 * its parent must exist. The owner creates its directory and preferences file.
 *
 * width and height are the output size. They select the screen profile,
 * whose status and soft-key bars frame integrated applications, and must
 * leave such an application at least one row.
 */
struct fplinux_session_options {
	const char *applications;
	const char *socket_path;
	const char *user;
	const char *home;
	uid_t app_uid;
	gid_t app_gid;
	uid_t metadata_uid;
	int width;
	int height;
	const char *settings_path;
	const char *languages_dir;
	const char *brightness_socket;
	const char *leds_dir;
	const char *power_supply_dir;
	const char *rtc_path;
	/* Playback capability uses the installed service and playback node. */
	const char *audio_socket;
	const char *audio_service;
	const char *audio_device;
	/* Directory of evdev devices; NULL disables application vibration. */
	const char *input_dir;
	/* NULL disables the bounded audio/vibration helper. */
	const char *clock_signal_path;
	/* Owner-only nonempty private preferences directory. */
	const char *clock_preferences_directory;
	uint64_t (*clock_ms)(void *context);
	/* UTC seconds; NULL selects the system wall clock. */
	int64_t (*clock_utc)(void *context);
	void (*changed)(void *context);
	/* Borrowed RGB565 LAUNCH card, or -1 to clear; called before CLOSE. */
	void (*launch_frame)(void *context, int fd, int width, int height);
	/* Retain the current rendered scene until its replacement is complete. */
	void (*hold_frame)(void *context, bool enabled);
	/* Return an owned close-on-exec RGB565 scene descriptor, or -1. */
	int (*capture_frame)(void *context, int *width, int *height);
	/* Optional vibrator boundary; false reports errno and loses the device. */
	bool (*vibrate)(void *context, bool on);
	void *context;
};

struct fplinux_session_process {
	pid_t pid;
	/*
	 * The box the shell places the surface in: the declared mode, or for an
	 * application that asked for another mode with MODE, the mode whose box
	 * has the size of the last buffer it committed, so that no frame shows
	 * a buffer in the other box. The launcher's mode never changes.
	 */
	enum fplinux_ui_session_mode mode;
	/*
	 * The mode whose box size and state, fullscreen or maximized, the shell
	 * configures the toplevel with: the declared mode until MODE asks for
	 * another one; see FPLINUX_UI_SESSION_MODE.
	 */
	enum fplinux_ui_session_mode configured_mode;
	char app_id[FPLINUX_UI_SESSION_ID_BYTES];
	char title[FPLINUX_UI_SESSION_TEXT_BYTES];
	/* Task state is independent of display visibility and modal overlays. */
	enum fplinux_ui_session_lifecycle lifecycle;
	/* Nonzero instance identity; a restart receives a new token. */
	uint32_t token;
	bool closing;
	bool ready;
	bool framed;
	/*
	 * The shell may show the application's surface: it has committed its
	 * first buffer and, in integrated mode, the launcher has drawn the bars
	 * for it. It stays set while the application leaves after EXIT or Home,
	 * until the launcher has drawn the view that replaces it; the
	 * application then closes after EXIT or stays hidden after Home.
	 * The launcher's placement does not use it.
	 */
	bool shown;
};

/* Private shell state; strings stay owned by the manager. */
struct fplinux_session_launch {
	bool active;
	/* At least 200 ms old, display lit, below keyguard and battery notices. */
	bool shown;
	/* One nonzero generation per LAUNCH or fresh internal BUSY operation. */
	uint32_t generation;
	const char *app_id;
	const char *title;
};

void fplinux_session_get_launch(struct fplinux_session *session,
				struct fplinux_session_launch *state);
void fplinux_session_cancel_launch(struct fplinux_session *session);

struct fplinux_session *
fplinux_session_create(const struct fplinux_session_options *options);
void fplinux_session_destroy(struct fplinux_session *session);
int fplinux_session_get_fd(struct fplinux_session *session);
void fplinux_session_dispatch(struct fplinux_session *session);
const struct fplinux_session_process *
fplinux_session_find_process(struct fplinux_session *session, pid_t pid);
/* The surface that owns keyboard focus: the launcher while it is on top. */
pid_t fplinux_session_focused_pid(struct fplinux_session *session);
bool fplinux_session_is_ready(struct fplinux_session *session);
/* The keyguard is engaged. */
bool fplinux_session_is_locked(struct fplinux_session *session);
/*
 * The shell places the launcher surface above every application, which it
 * hides: the keyguard is engaged, or a system notice or clock ring is due and
 * the launcher has drawn its view.
 */
bool fplinux_session_launcher_on_top(struct fplinux_session *session);
/*
 * The shell reports, after each placement, whether the launcher's view is
 * above the applications. A notice hides the application and takes
 * the focus and keys only while it is; the keyguard does not wait for it.
 */
void fplinux_session_set_launcher_shown(struct fplinux_session *session,
					bool shown);
/*
 * The box of a surface in output coordinates, shared by the shell's
 * placement and Wayland configure: the launcher and
 * fullscreen applications cover the output, an integrated application sits
 * between the status bar and the soft-key bar of the output's profile.
 */
void fplinux_session_surface_box(struct fplinux_session *session,
				 enum fplinux_ui_session_mode mode, int *x,
				 int *y, int *width, int *height);
/*
 * The output changed size: placement follows it. Running clients keep the
 * size and display profile, which is not sent again; the shell
 * configures no toplevel for the new size until its mode changes.
 */
void fplinux_session_output_resized(struct fplinux_session *session, int width,
				    int height);
/* The system theme, as CONFIG and ACTIVE carry it. */
enum fplinux_ui_session_theme
fplinux_session_theme(struct fplinux_session *session);
void fplinux_session_frame(struct fplinux_session *session, pid_t pid);
/*
 * A client committed a buffer; width and height are the committed surface
 * size. The shell reports it before it places the surfaces again, so that an
 * application shown, closed or placed in another box for this commit changes
 * with it in one repaint: the application's first buffer, its first buffer of
 * another mode's box size, or the launcher's frame that answers a change of
 * the foreground application.
 */
void fplinux_session_surface_committed(struct fplinux_session *session,
				       pid_t pid, int width, int height);
/*
 * At the output's latch, snapshot the mapped client buffers in the scene.
 * serial identifies that submission until successful presentation or reset.
 * This records readiness without changing the scene inside the repaint.
 */
void fplinux_session_output_latched(struct fplinux_session *session,
				    uint64_t serial, const pid_t *pids,
				    size_t count);
/*
 * Only successful presentation of the matching latched wake scene may light
 * the display. Renderer callbacks and elapsed time do not establish it.
 */
void fplinux_session_output_presented(struct fplinux_session *session,
				      uint64_t serial);
/* An output was disabled, replaced or resized; its submission cannot finish. */
void fplinux_session_output_reset(struct fplinux_session *session);
/* A manager event can request another repaint while waiting for the scene. */
bool fplinux_session_wake_pending(const struct fplinux_session *session);
/* Home retains the application and its state in the bounded task registry. */
void fplinux_session_home(struct fplinux_session *session);
/* A phone keypad key; true when Wayland may deliver it to the focus. */
bool fplinux_session_key(struct fplinux_session *session, unsigned int code,
			 bool pressed, uint64_t time_ms);
/* An external keyboard key; returns true when it may reach the focus. */
bool fplinux_session_keyboard_key(struct fplinux_session *session,
				  unsigned int code, bool pressed);
/* The external keyboard Home shortcut. */
void fplinux_session_keyboard_home(struct fplinux_session *session);
/* Input ownership was lost; cancel only this source's held gestures. */
void fplinux_session_cancel_input(struct fplinux_session *session,
				  enum fplinux_session_key_source source);

#endif
