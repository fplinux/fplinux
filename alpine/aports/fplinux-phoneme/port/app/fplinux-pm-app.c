// SPDX-License-Identifier: GPL-2.0-only
/*
 * phoneME: open a JAR and run its MIDlet in the in-process VM.
 *
 * The app is integrated. Its own screens are the MIDlet chooser
 * of a suite, the suite screen and alerts. Once a MIDlet is chosen, the
 * suite screen stays at the bottom of the stack and the MIDlet's screens go
 * over it. The VM runs in slices from the vm_tick timer on the runtime
 * thread; fplinux-pm-lifecycle.c decides when.
 */
#define _XOPEN_SOURCE 700

#include "fplinux-pm-app.h"
#include "fplinux-cli.h"
#include "fplinux-pm-lifecycle.h"
#include "fplinux-pm-store.h"
#include "../canvas/fplinux-pm-canvas.h"
#include "../../vm/fplinux-pm-vm.h"

#include <errno.h>
#include <fcntl.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-ui-file-picker.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-recent.h>
#include <fplinux/fplinux-ui-view-settings.h>
#include <fplinux/fplinux-ui.h>
#include <libintl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define _(text) gettext(text)
/* A MIDlet icon is a small PNG; larger ones are not shown. */
#define ICON_MAX_BYTES (64u * 1024u)
#define MESSAGE_BYTES 1024

enum command {
	COMMAND_BACK = 1,
	COMMAND_CLOSE,
	COMMAND_CANCEL,
	COMMAND_CONFIRM_CLOSE,
	COMMAND_REMOVE,
	COMMAND_RESET,
};

enum { AUX_RECENT = 1, AUX_SETTINGS };

struct pm_app {
	struct fplinux_ui *ui;
	struct fplinux_pm_lifecycle lifecycle;
	lv_timer_t *tick;
	/* A slice runs: its result covers wakeups, so they ready no timer. */
	bool in_slice;
	bool busy;
	bool loaded;
	bool close_wait;
	uint32_t document_id;
	/* Deferred resource reads keep the containing filesystem in use. */
	int jar_directory;
	/* The chosen JAR and its suite, for the chooser and the running VM. */
	char jar_path[PATH_MAX];
	struct fplinux_pm_suite suite;
	const struct fplinux_pm_midlet *midlet;
	char *storage;
	char state_root[PATH_MAX];
	char home_path[PATH_MAX];
	struct fplinux_ui_file_picker *picker;
	struct fplinux_ui_screen *home, *auxiliary;
	struct fplinux_ui_screen *settings_scope, *settings_choice;
	struct fplinux_ui_prefs *prefs, *program_prefs;
	struct fplinux_ui_recent recent;
	struct fplinux_ui_view_settings defaults, settings;
	size_t setting_row;
	unsigned int default_size, program_size, run_size;
	unsigned int auxiliary_kind;
	bool program_settings;
	bool guest_active;
	bool guest_fullscreen;
	uint32_t open_request;
	uint32_t storage_request;
	bool picker_requested, prefs_error, program_prefs_error;
	fplinux_pm_app_resize_fn resize;
	void *resize_context;
};

static struct pm_app app = { .jar_directory = -1 };

static const struct fplinux_ui_command back_command = {
	.id = COMMAND_BACK,
	.type = FPLINUX_UI_COMMAND_BACK,
};

static void show_home(struct pm_app *app);
static void sync_visibility(struct pm_app *app);
static void show_alert(struct pm_app *app, const char *title, const char *text);

static bool save_preferences(struct pm_app *app, struct fplinux_ui_prefs *prefs,
			     bool prepared, bool program)
{
	struct fplinux_storage_result result = { FPLINUX_STORAGE_UNPUBLISHED,
						 errno ? errno : EIO };
	bool failed;

	if (prepared && prefs)
		result = fplinux_ui_prefs_save(prefs);
	failed = result.state != FPLINUX_STORAGE_DURABLE;
	if (program)
		app->program_prefs_error = failed;
	else
		app->prefs_error = failed;
	if (app->home)
		fplinux_ui_screen_set_header_status(
			app->home,
			app->prefs_error || app->program_prefs_error ?
				_("Unsaved") :
				NULL,
			NULL);
	if (app->auxiliary && app->auxiliary_kind == AUX_SETTINGS)
		fplinux_ui_screen_set_header_status(
			app->auxiliary,
			(app->program_settings ? app->program_prefs_error :
						 app->prefs_error) ?
				_("Unsaved") :
				NULL,
			NULL);
	if (failed) {
		show_alert(app,
			   result.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED ?
				   _("Saving not confirmed") :
				   _("Cannot save"),
			   fplinux_ui_storage_error(result));
		return false;
	}
	return true;
}

static void document_reply(struct pm_app *app,
			   enum fplinux_ui_document_status status, int error)
{
	if (!app->document_id)
		return;
	if (!fplinux_ui_document_reply(app->ui, app->document_id, status,
				       error))
		perror("fplinux-phoneme: document reply");
	app->document_id = 0;
}

/* The manager owns the delayed indicator, including cancellation during IO. */
static void set_busy(struct pm_app *app, const char *title)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_BUSY,
		.enabled = title != NULL,
	};

	if (!app->ui || (!title && !app->busy))
		return;
	if (title) {
		app->loaded = false;
		size_t length = strlen(title);

		if (length >= sizeof(message.title)) {
			length = sizeof(message.title) - 1;
			while (length &&
			       ((unsigned char)title[length] & 0xc0) == 0x80)
				--length;
		}
		for (size_t i = 0; i < length; ++i) {
			unsigned char c = (unsigned char)title[i];

			message.title[i] = c < 0x20 || c == 0x7f ? ' ' :
								   title[i];
		}
	}
	app->busy = message.enabled;
	if (!fplinux_ui_send(app->ui, &message))
		perror("fplinux-phoneme: busy notification");
}

void fplinux_pm_app_loaded(void)
{
	if (!app.loaded) {
		char *path = realpath(app.jar_path, NULL);

		if (path) {
			bool stored =
				fplinux_ui_recent_record(
					&app.recent, path,
					app.suite.name[0] ? app.suite.name :
							    NULL) &&
				fplinux_ui_recent_store(&app.recent, app.prefs);

			save_preferences(&app, app.prefs, stored, false);
			free(path);
		}
	}
	app.loaded = true;
	document_reply(&app, FPLINUX_UI_DOCUMENT_ACCEPTED, 0);
	if (app.ui && fplinux_ui_is_visible(app.ui))
		set_busy(&app, NULL);
}

struct fplinux_ui *fplinux_pm_app_ui(void)
{
	return app.ui;
}

bool fplinux_pm_app_set_fullscreen(bool fullscreen)
{
	if (!app.ui || !fplinux_ui_set_fullscreen(app.ui, fullscreen))
		return false;
	app.guest_fullscreen = fullscreen;
	return true;
}

unsigned int fplinux_pm_app_canvas_size(void)
{
	return app.run_size;
}

struct fplinux_ui_view_settings fplinux_pm_app_view_settings(void)
{
	return app.settings;
}

void fplinux_pm_app_set_resize_handler(fplinux_pm_app_resize_fn handler,
				       void *context)
{
	app.resize = handler;
	app.resize_context = context;
}

/* Alerts */

/* Close returns to the screen below, or leaves after a selected-file error. */
static void show_alert(struct pm_app *app, const char *title, const char *text)
{
	set_busy(app, NULL);
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_ERROR, title, text, 0);

	if (alert)
		fplinux_ui_push(app->ui, alert);
}

static void leave_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int command_id,
			  void *context)
{
	(void)screen;
	(void)command_id;
	(void)context;
	fplinux_ui_back(ui);
}

/*
 * Once the VM cannot go on, the MIDlet's screens go and an alert explains
 * why over the suite screen at the bottom of the stack. Close leaves the
 * app, because one VM runs per process.
 */
static void show_final_alert(struct pm_app *app, const char *title,
			     const char *text)
{
	set_busy(app, NULL);
	const struct fplinux_ui_command close = {
		.id = COMMAND_CLOSE,
		.type = FPLINUX_UI_COMMAND_BACK,
		.label = _("Close"),
	};
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_ERROR, title, text, 0);

	if (!alert) {
		fplinux_ui_quit(app->ui);
		return;
	}
	fplinux_ui_screen_add_command(alert, &close);
	fplinux_ui_screen_set_listener(alert, leave_command, NULL, app);
	while (fplinux_ui_pop(app->ui))
		;
	fplinux_ui_push(app->ui, alert);
}

static void show_jar_problem(struct pm_app *app,
			     enum fplinux_pm_jar_status status)
{
	switch (status) {
	case FPLINUX_PM_JAR_IO:
		show_alert(app, _("File unreadable"),
			   _("phoneME cannot read this file."));
		break;
	case FPLINUX_PM_JAR_NO_ENTRY:
	case FPLINUX_PM_JAR_NO_MIDLETS:
		show_alert(app, _("No MIDlets"),
			   _("The JAR file has no MIDlets. It may be a program "
			     "for a computer."));
		break;
	case FPLINUX_PM_JAR_NO_MEMORY:
		show_alert(app, _("Out of memory"),
			   _("Close phoneME and open it again."));
		break;
	case FPLINUX_PM_JAR_FORMAT:
	case FPLINUX_PM_JAR_TOO_LARGE:
	case FPLINUX_PM_JAR_OK:
	default:
		show_alert(app, _("File damaged"),
			   _("The JAR file is damaged."));
		break;
	}
}

/*
 * A PNG resource of the JAR, such as a MIDlet icon, as an image source for
 * the runtime, which decodes it once and keeps its own copy. Returns the
 * data for the caller to free, or NULL without an icon.
 */
static unsigned char *read_icon(struct pm_app *app, const char *path,
				lv_image_dsc_t *image)
{
	static const unsigned char signature[8] = { 0x89, 'P',	'N',  'G',
						    '\r', '\n', 0x1a, '\n' };
	unsigned char *png = NULL;
	size_t size = 0;

	if (!path[0] ||
	    fplinux_pm_jar_read_entry(app->jar_path, path, ICON_MAX_BYTES, &png,
				      &size) != FPLINUX_PM_JAR_OK)
		return NULL;
	/* The signature, then IHDR with a width and height below 65536. */
	if (size < 24 || memcmp(png, signature, sizeof(signature)) ||
	    memcmp(png + 12, "IHDR", 4) || png[16] || png[17] || png[20] ||
	    png[21]) {
		free(png);
		return NULL;
	}
	*image = (lv_image_dsc_t){
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_RAW_ALPHA,
			.w = (uint32_t)png[18] << 8 | png[19],
			.h = (uint32_t)png[22] << 8 | png[23],
		},
		.data_size = (uint32_t)size,
		.data = png,
	};
	return png;
}

/* Record stores */

/* The VM */

static void apply(struct pm_app *app, struct fplinux_pm_schedule schedule,
		  bool in_tick)
{
	if (!app->tick)
		return;
	switch (schedule.timer) {
	case FPLINUX_PM_TIMER_PAUSE:
		lv_timer_pause(app->tick);
		break;
	case FPLINUX_PM_TIMER_RUN:
		/* A tick has just set the timer's start; a period from then. */
		lv_timer_set_period(app->tick,
				    schedule.delay_ms ? schedule.delay_ms : 1);
		lv_timer_resume(app->tick);
		if (!in_tick && !schedule.delay_ms)
			lv_timer_ready(app->tick);
		else if (!in_tick)
			lv_timer_reset(app->tick);
		break;
	case FPLINUX_PM_TIMER_KEEP:
		break;
	}
}

static void vm_wakeup(void *context)
{
	struct pm_app *app = context;

	if (fplinux_pm_lifecycle_wakeup(&app->lifecycle) && !app->in_slice &&
	    !app->close_wait)
		apply(app,
		      (struct fplinux_pm_schedule){ FPLINUX_PM_TIMER_RUN, 0 },
		      false);
}

static void vm_finished(struct pm_app *app, bool started)
{
	char text[MESSAGE_BYTES];
	int code = fplinux_pm_vm_exit_code();

	set_busy(app, NULL);

	if (app->tick) {
		lv_timer_delete(app->tick);
		app->tick = NULL;
	}
	fprintf(stderr, "fplinux-phoneme: VM exited with status %d\n", code);
	if (started && !code) {
		document_reply(app, FPLINUX_UI_DOCUMENT_ACCEPTED, 0);
		fplinux_ui_back_after_document(app->ui);
		return;
	}
	document_reply(app, FPLINUX_UI_DOCUMENT_ERROR, EIO);
	if (!started) {
		snprintf(text, sizeof(text),
			 _("“%s” could not start. Open phoneME again."),
			 app->midlet->name);
		show_final_alert(app, _("Not started"), text);
		return;
	}
	snprintf(text, sizeof(text),
		 _("The MIDlet “%s” ended with an error. Open phoneME again."),
		 app->midlet->name);
	show_final_alert(app, _("MIDlet stopped"), text);
}

static void start_vm(struct pm_app *app)
{
	struct fplinux_pm_vm_config config = {
		.jar_path = app->jar_path,
		.midlet_class = app->midlet->class_name,
		.storage_dir = app->storage,
		.wakeup = vm_wakeup,
		.wakeup_context = app,
	};
	bool started;

	fprintf(stderr, "fplinux-phoneme: starting %s from %s\n",
		config.midlet_class, config.jar_path);
	started = fplinux_pm_vm_start(&config);
	apply(app, fplinux_pm_lifecycle_started(&app->lifecycle, started),
	      true);
	if (!started)
		vm_finished(app, false);
}

static void run_slice(struct pm_app *app)
{
	int result;

	app->in_slice = true;
	fplinux_pm_lifecycle_slice_begin(&app->lifecycle);
	result = fplinux_pm_vm_slice();
	app->in_slice = false;
	apply(app,
	      fplinux_pm_lifecycle_slice(&app->lifecycle, result,
					 fplinux_ui_now_ms()),
	      true);
	if (app->lifecycle.phase == FPLINUX_PM_PHASE_EXITED)
		vm_finished(app, true);
}

static void vm_tick(lv_timer_t *timer)
{
	struct pm_app *app = lv_timer_get_user_data(timer);

	if (app->close_wait) {
		lv_timer_pause(timer);
		return;
	}
	switch (app->lifecycle.phase) {
	case FPLINUX_PM_PHASE_LOADING:
		/* The starting screen is drawn before the VM takes the thread. */
		lv_refr_now(NULL);
		apply(app, fplinux_pm_lifecycle_loaded(&app->lifecycle), true);
		break;
	case FPLINUX_PM_PHASE_STARTING:
		start_vm(app);
		break;
	case FPLINUX_PM_PHASE_RUNNING:
	case FPLINUX_PM_PHASE_HIDING:
		run_slice(app);
		break;
	case FPLINUX_PM_PHASE_PICKING:
	case FPLINUX_PM_PHASE_HIDDEN:
	case FPLINUX_PM_PHASE_EXITED:
		lv_timer_pause(timer);
		break;
	}
}

static void sleep_ms(uint32_t milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (long)(milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&delay, &delay) && errno == EINTR)
		;
}

/*
 * CLOSE: destroyApp(true) within FPLINUX_PM_CLOSE_BUDGET_MS. A MIDlet that
 * does not finish in time is left behind: the process exits anyway.
 */
static void close_vm(struct pm_app *app)
{
	enum fplinux_pm_close_step step;
	uint32_t wait_ms;

	if (!fplinux_pm_lifecycle_close_begin(&app->lifecycle,
					      fplinux_ui_now_ms()))
		return;
	if (app->tick)
		lv_timer_pause(app->tick);
	app->in_slice = true;
	fplinux_pm_vm_request_destroy();
	do {
		int result;

		fplinux_pm_lifecycle_slice_begin(&app->lifecycle);
		result = fplinux_pm_vm_slice();
		step = fplinux_pm_lifecycle_close_step(
			&app->lifecycle, result, fplinux_ui_now_ms(), &wait_ms);
		if (step == FPLINUX_PM_CLOSE_WAIT)
			sleep_ms(wait_ms);
	} while (step == FPLINUX_PM_CLOSE_CONTINUE ||
		 step == FPLINUX_PM_CLOSE_WAIT);
	app->in_slice = false;
	fprintf(stderr, "fplinux-phoneme: %s\n",
		step == FPLINUX_PM_CLOSE_DONE ?
			"MIDlet destroyed" :
			"MIDlet did not finish destroyApp in time; exiting");
}

/*
 * The suite screen replaces the chooser and stays at the
 * bottom of the stack while the MIDlet runs: the loading alert and then the
 * MIDlet's screens go over it, so that an alert always dims a real screen.
 * Back on it, should it ever be on top, leaves the app.
 */
static struct fplinux_ui_screen *
create_suite_screen(struct pm_app *app, const struct fplinux_pm_midlet *midlet)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, midlet->name);
	lv_image_dsc_t image;
	unsigned char *png;

	if (!form)
		return NULL;
	png = read_icon(app, midlet->icon, &image);
	if (png)
		fplinux_ui_form_append_image(form, NULL, &image);
	free(png);
	if (app->suite.vendor[0])
		fplinux_ui_form_append_property(form, _("Vendor"),
						app->suite.vendor);
	if (app->suite.version[0])
		fplinux_ui_form_append_property(form, _("Version"),
						app->suite.version);
	fplinux_ui_screen_add_command(form, &back_command);
	fplinux_ui_screen_set_listener(form, leave_command, NULL, app);
	return form;
}

/*
 * The MIDlet's first Display.setCurrent() replaces the loading alert, the
 * top screen, and leaves the suite screen under it.
 */
static int start_midlet(struct pm_app *app,
			const struct fplinux_pm_midlet *midlet)
{
	struct fplinux_ui_screen *suite;
	struct fplinux_ui_screen *loading;
	const struct fplinux_ui_command cancel = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
	};

	app->tick = lv_timer_create(vm_tick, 1, app);
	suite = app->tick ? create_suite_screen(app, midlet) : NULL;
	if (!suite) {
		if (app->tick)
			lv_timer_delete(app->tick);
		app->tick = NULL;
		return ENOMEM;
	}
	app->midlet = midlet;
	while (fplinux_ui_pop(app->ui))
		;
	app->home = NULL;
	app->auxiliary = NULL;
	app->settings_scope = app->settings_choice = NULL;
	app->guest_active = true;
	fplinux_ui_replace(app->ui, suite);
	loading = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_INFO,
				       _("Starting"), midlet->name, 0);
	if (loading) {
		/* The manager owns the delayed spinner; this is its underlying screen. */
		fplinux_ui_screen_add_command(loading, &cancel);
		fplinux_ui_screen_set_listener(loading, leave_command, NULL,
					       app);
		fplinux_ui_push(app->ui, loading);
	}
	sync_visibility(app);
	apply(app, fplinux_pm_lifecycle_load(&app->lifecycle), false);
	return 0;
}

/* The JAR and its MIDlets */

/* "com.example.Game" -> "com/example/Game.class". */
static bool class_entry(const char *class_name, char *entry, size_t capacity)
{
	size_t length = strlen(class_name);

	if (!length || length + sizeof(".class") > capacity ||
	    strchr(class_name, '/'))
		return false;
	for (size_t i = 0; i < length; ++i)
		entry[i] = class_name[i] == '.' ? '/' : class_name[i];
	memcpy(entry + length, ".class", sizeof(".class"));
	return true;
}

static int check_midlet_class(const char *jar_path,
			      const struct fplinux_pm_midlet *midlet)
{
	enum fplinux_pm_jar_status status = FPLINUX_PM_JAR_NO_ENTRY;
	char entry[PATH_MAX];
	unsigned char *data = NULL;
	size_t size = 0;
	int error;

	if (class_entry(midlet->class_name, entry, sizeof(entry)))
		status = fplinux_pm_jar_read_entry(jar_path, entry, 0, &data,
						   &size);
	error = errno;
	free(data);
	if (status == FPLINUX_PM_JAR_OK || status == FPLINUX_PM_JAR_TOO_LARGE)
		return 0;
	return status == FPLINUX_PM_JAR_IO	  ? (error ? error : EIO) :
	       status == FPLINUX_PM_JAR_NO_MEMORY ? ENOMEM :
						    ENOEXEC;
}

/* Check the class before the VM starts, so a broken suite stays in the GUI. */
static int choose_midlet(struct pm_app *app, size_t index)
{
	const struct fplinux_pm_midlet *midlet = &app->suite.midlets[index];
	int error;

	set_busy(app, midlet->name);
	error = check_midlet_class(app->jar_path, midlet);
	if (!error) {
		const char *key;

		free(app->storage);
		app->storage = fplinux_pm_store_open(
			app->state_root, &app->suite, app->jar_path);
		if (!app->storage)
			return errno ? errno : EIO;
		key = strrchr(app->storage, '/');
		fplinux_ui_prefs_close(app->program_prefs);
		app->program_prefs = fplinux_ui_program_prefs_open(
			FPLINUX_PM_APP_ID, key + 1);
		app->program_prefs_error = app->program_prefs == NULL;
		app->settings = app->defaults;
		app->program_size = app->default_size;
		if (app->program_prefs) {
			long long size = fplinux_ui_prefs_get_int(
				app->program_prefs, "canvas-size",
				app->default_size);

			fplinux_ui_view_settings_load(&app->settings,
						      app->program_prefs,
						      &app->defaults);
			if (size >= 0 && size <= 6)
				app->program_size = (unsigned int)size;
		}
		app->run_size = app->program_size;
		return start_midlet(app, midlet);
	}
	set_busy(app, NULL);
	return error;
}

/* A PNG icon from the JAR beside a chooser row; the runtime keeps a copy. */
static void set_midlet_icon(struct pm_app *app, struct fplinux_ui_screen *list,
			    size_t index, const char *icon)
{
	lv_image_dsc_t image;
	unsigned char *png = read_icon(app, icon, &image);

	if (png)
		fplinux_ui_list_set_icon(list, index, &image,
					 FPLINUX_UI_LIST_ICON_IMAGE);
	free(png);
}

static void chooser_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *list, int command_id,
			    void *context)
{
	struct pm_app *app = context;
	size_t index = fplinux_ui_list_get_focused(list);

	if (command_id == FPLINUX_UI_SELECT_COMMAND &&
	    index < app->suite.midlet_count) {
		int error = choose_midlet(app, index);

		if (error) {
			document_reply(app, FPLINUX_UI_DOCUMENT_ERROR, error);
			if (error == ENOEXEC || error == ENOMEM)
				show_jar_problem(
					app, error == ENOMEM ?
						     FPLINUX_PM_JAR_NO_MEMORY :
						     FPLINUX_PM_JAR_FORMAT);
			else
				show_alert(app, _("Cannot start"),
					   strerror(error));
		}
	} else if (command_id == COMMAND_BACK) {
		/* The runtime deletes the chooser; its rows are copies. */
		fplinux_ui_go_back(ui);
		document_reply(app, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
		fplinux_pm_suite_cleanup(&app->suite);
		if (app->jar_directory >= 0)
			close(app->jar_directory);
		app->jar_directory = -1;
	}
}

static bool show_chooser(struct pm_app *app)
{
	const char *title = app->suite.name[0] ?
				    app->suite.name :
				    strrchr(app->jar_path, '/') + 1;
	struct fplinux_ui_screen *list =
		fplinux_ui_list_new(app->ui, title, FPLINUX_UI_LIST_IMPLICIT);

	if (!list)
		return false;
	for (size_t i = 0; i < app->suite.midlet_count; ++i) {
		fplinux_ui_list_append(list, app->suite.midlets[i].name, NULL);
		set_midlet_icon(app, list, i, app->suite.midlets[i].icon);
	}
	fplinux_ui_list_set_select_label(list, _("Start"));
	fplinux_ui_screen_add_command(list, &back_command);
	fplinux_ui_screen_set_listener(list, chooser_command, NULL, app);
	fplinux_ui_push(app->ui, list);
	set_busy(app, NULL);
	return true;
}

static int jar_error(enum fplinux_pm_jar_status status)
{
	if (status == FPLINUX_PM_JAR_IO)
		return errno ? errno : EIO;
	return status == FPLINUX_PM_JAR_NO_MEMORY ? ENOMEM : ENOEXEC;
}

/* Validate the new suite before asking to close the current VM. */
static struct fplinux_ui_document_result
open_document(struct fplinux_ui *ui,
	      const struct fplinux_ui_document_request *request, void *context)
{
	struct pm_app *app = context;
	struct fplinux_ui_document_result result = {
		.request_id = request->request_id,
		.status = FPLINUX_UI_DOCUMENT_ERROR,
		.error = EINVAL,
	};
	struct fplinux_pm_suite suite = { 0 };
	enum fplinux_pm_jar_status status;
	char directory[PATH_MAX];
	const char *slash;
	size_t length;
	int descriptor;
	int error;

	if (!request->path || request->path[0] != '/')
		return result;
	length = strlen(request->path);
	if (length >= sizeof(app->jar_path)) {
		result.error = ENAMETOOLONG;
		return result;
	}
	memcpy(directory, request->path, length + 1);
	slash = strrchr(request->path, '/');
	directory[slash == request->path ? 1 : (size_t)(slash - request->path)] =
		'\0';
	descriptor = open(directory,
			  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0) {
		result.error = errno;
		return result;
	}
	status = fplinux_pm_jar_read_suite(request->path, &suite);
	if (status != FPLINUX_PM_JAR_OK) {
		result.error = jar_error(status);
		close(descriptor);
		return result;
	}
	if (app->lifecycle.phase != FPLINUX_PM_PHASE_PICKING) {
		int class_error = 0;

		for (size_t i = 0; i < suite.midlet_count; ++i) {
			error = check_midlet_class(request->path,
						   &suite.midlets[i]);
			if (error) {
				class_error = error;
				break;
			}
		}
		fplinux_pm_suite_cleanup(&suite);
		close(descriptor);
		if (class_error) {
			result.error = class_error;
			return result;
		}
		result.status = FPLINUX_UI_DOCUMENT_RESTART_REQUIRED;
		result.error = 0;
		return result;
	}
	document_reply(app, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
	while (fplinux_ui_pop(ui))
		;
	app->home = NULL;
	app->auxiliary = NULL;
	app->settings_scope = app->settings_choice = NULL;
	app->picker_requested = false;
	fplinux_pm_suite_cleanup(&app->suite);
	if (app->jar_directory >= 0)
		close(app->jar_directory);
	app->jar_directory = descriptor;
	app->suite = suite;
	memcpy(app->jar_path, request->path, length + 1);
	app->loaded = false;
	if (suite.midlet_count == 1)
		error = choose_midlet(app, 0);
	else
		error = show_chooser(app) ? 0 : ENOMEM;
	if (error) {
		set_busy(app, NULL);
		result.error = error;
		return result;
	}
	app->document_id = request->request_id;
	result.status = FPLINUX_UI_DOCUMENT_WAIT;
	result.error = 0;
	return result;
}

static void add_command(struct pm_app *app, struct fplinux_ui_screen *screen,
			int id, enum fplinux_ui_command_type type,
			const char *label, const char *glyph)
{
	const struct fplinux_ui_command command = {
		.id = id,
		.type = type,
		.label = label,
		.icon = fplinux_ui_glyph(app->ui, glyph, FPLINUX_UI_GLYPH_ROW),
	};

	fplinux_ui_screen_add_command(screen, &command);
}

static void open_path(struct pm_app *app, const char *path)
{
	if (app->open_request)
		return;
	app->open_request = fplinux_ui_open_file(app->ui, path);
	if (!app->open_request)
		show_alert(app, _("Open JAR"), strerror(errno));
}

static void picker_result(struct fplinux_ui_file_picker *picker,
			  enum fplinux_ui_file_picker_status status,
			  const char *path, int error, void *context)
{
	struct pm_app *app = context;
	char *selected = path ? strdup(path) : NULL;

	app->picker = NULL;
	fplinux_ui_file_picker_close(picker);
	if (status == FPLINUX_UI_FILE_PICKER_SELECTED) {
		if (selected)
			open_path(app, selected);
		else
			show_alert(app, _("Open JAR"), _("Not enough memory."));
	} else if (status == FPLINUX_UI_FILE_PICKER_ERROR) {
		show_alert(app, _("Open JAR"), strerror(error));
	}
	free(selected);
}

static void open_picker(struct pm_app *app)
{
	static const char *const extensions[] = { "jar", NULL };
	const struct fplinux_ui_file_picker_options options = {
		.mode = FPLINUX_UI_FILE_PICKER_FILE,
		.home = app->home_path,
		.title = _("Open JAR"),
		.extensions = extensions,
	};

	if (app->storage_request) {
		app->picker_requested = true;
		return;
	}
	app->picker_requested = false;
	app->picker = fplinux_ui_file_picker_open(app->ui, &options,
						  picker_result, app);
	if (!app->picker)
		show_alert(app, _("Open JAR"), strerror(errno));
}

static void storage_result(struct fplinux_ui *ui,
			   const struct fplinux_volume_result *result,
			   void *context)
{
	struct pm_app *app = context;

	(void)ui;
	if (app->picker)
		fplinux_ui_file_picker_storage_result(app->picker, result);
	if (result->request_id == app->storage_request &&
	    app->storage_request) {
		app->storage_request = 0;
		if (app->picker_requested && app->home)
			open_picker(app);
	}
}

static void home_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context);

static void show_recent(struct pm_app *app)
{
	struct fplinux_ui_screen *screen = fplinux_ui_list_new(
		app->ui, _("Recent"), FPLINUX_UI_LIST_IMPLICIT);

	if (!screen)
		return;
	for (size_t i = 0; i < app->recent.count; ++i) {
		struct fplinux_ui_recent_entry *entry = &app->recent.items[i];
		const char *name = strrchr(entry->path, '/');

		fplinux_ui_list_append(screen,
				       entry->title ? entry->title : name + 1,
				       access(entry->path, R_OK) == 0 ?
					       entry->path :
					       _("Unavailable"));
	}
	if (app->recent.count)
		add_command(app, screen, COMMAND_REMOVE,
			    FPLINUX_UI_COMMAND_SCREEN, _("Remove"), "delete");
	fplinux_ui_screen_add_command(screen, &back_command);
	fplinux_ui_screen_set_listener(screen, home_command, NULL, app);
	app->auxiliary = screen;
	app->auxiliary_kind = AUX_RECENT;
	fplinux_ui_push(app->ui, screen);
}

static const char *size_label(unsigned int size)
{
	static const char *const sizes[] = { "128x128", "128x160", "176x208",
					     "176x220", "240x320", "320x240" };

	return size ? sizes[size - 1] : _("Auto");
}

static void show_settings(struct pm_app *app, bool program)
{
	struct fplinux_ui_view_settings shown = program ? app->settings :
							  app->defaults;
	const char *title = program		? _("This program") :
			    app->settings_scope ? _("All programs") :
						  _("Settings");
	struct fplinux_ui_screen *screen =
		fplinux_ui_view_settings_list_new(app->ui, title, &shown);
	bool replacing = app->auxiliary && app->auxiliary_kind == AUX_SETTINGS;
	size_t row;

	if (!screen)
		return;
	row = fplinux_ui_list_append(screen, _("Size"), NULL);
	fplinux_ui_list_set_icon(screen, row,
				 fplinux_ui_glyph(app->ui, "display",
						  FPLINUX_UI_GLYPH_ROW),
				 FPLINUX_UI_LIST_ICON_IMAGE);
	fplinux_ui_list_set_trailing_value(
		screen, row,
		size_label(program ? app->program_size : app->default_size));
	if (program)
		add_command(app, screen, COMMAND_RESET,
			    FPLINUX_UI_COMMAND_SCREEN, _("Reset"), "refresh");
	fplinux_ui_screen_add_command(screen, &back_command);
	fplinux_ui_screen_set_listener(screen, home_command, NULL, app);
	if (program ? app->program_prefs_error : app->prefs_error)
		fplinux_ui_screen_set_header_status(screen, _("Unsaved"), NULL);
	if (replacing)
		fplinux_ui_list_set_focused(screen, app->setting_row);
	app->program_settings = program;
	app->auxiliary = screen;
	app->auxiliary_kind = AUX_SETTINGS;
	if (replacing)
		fplinux_ui_replace(app->ui, screen);
	else
		fplinux_ui_push(app->ui, screen);
}

static void show_settings_scope(struct pm_app *app)
{
	struct fplinux_ui_screen *screen = fplinux_ui_list_new(
		app->ui, _("Settings"), FPLINUX_UI_LIST_IMPLICIT);

	if (!screen)
		return;
	fplinux_ui_list_append(screen, _("This program"), NULL);
	fplinux_ui_list_append(screen, _("All programs"), NULL);
	fplinux_ui_screen_add_command(screen, &back_command);
	fplinux_ui_screen_set_listener(screen, home_command, NULL, app);
	app->settings_scope = screen;
	fplinux_ui_push(app->ui, screen);
}

static void save_settings(struct pm_app *app, bool reset, size_t selected)
{
	struct fplinux_ui_view_settings settings =
		app->program_settings ? app->settings : app->defaults;
	struct fplinux_ui_prefs *prefs =
		app->program_settings ? app->program_prefs : app->prefs;
	unsigned int size = app->program_settings ? app->program_size :
						    app->default_size;
	bool ok = prefs != NULL;

	if (!ok)
		errno = EIO;
	if (reset) {
		settings = app->defaults;
		size = app->default_size;
		ok = ok && fplinux_ui_view_settings_reset(prefs) &&
		     fplinux_ui_prefs_remove(prefs, "canvas-size");
	} else {
		if (app->setting_row == FPLINUX_UI_VIEW_SCALE)
			settings.scale = selected;
		else if (app->setting_row == FPLINUX_UI_VIEW_ROTATION)
			settings.rotation = selected;
		else
			size = selected;
		ok = ok && fplinux_ui_view_settings_store(&settings, prefs) &&
		     fplinux_ui_prefs_set_int(prefs, "canvas-size", size);
	}
	if (ok) {
		fplinux_pm_keys_cancel(FPLINUX_UI_KEY_PHONE);
		fplinux_pm_keys_cancel(FPLINUX_UI_KEY_KEYBOARD);
		if (app->program_settings) {
			app->settings = settings;
			app->program_size = size;
		} else {
			app->defaults = settings;
			app->default_size = size;
			app->settings = settings;
			app->program_size = size;
			if (app->program_prefs) {
				long long stored = fplinux_ui_prefs_get_int(
					app->program_prefs, "canvas-size",
					app->default_size);

				fplinux_ui_view_settings_load(
					&app->settings, app->program_prefs,
					&app->defaults);
				if (stored >= 0 && stored <= 6)
					app->program_size =
						(unsigned int)stored;
			}
		}
		fplinux_pm_canvas_view_changed();
	}
	show_settings(app, app->program_settings);
	save_preferences(app, prefs, ok, app->program_settings);
}

static void settings_changed(struct fplinux_ui *ui,
			     struct fplinux_ui_screen *list, size_t selected,
			     void *context)
{
	struct pm_app *app = context;

	if (list != app->settings_choice ||
	    selected >= (app->setting_row == 2 ? 7U : 3U))
		return;
	app->settings_choice = NULL;
	fplinux_ui_pop(ui);
	save_settings(app, false, selected);
}

static void open_settings_choice(struct pm_app *app)
{
	struct fplinux_ui_view_settings shown =
		app->program_settings ? app->settings : app->defaults;
	struct fplinux_ui_screen *list;

	app->setting_row = fplinux_ui_list_get_focused(app->auxiliary);
	if (app->setting_row < 2)
		list = fplinux_ui_view_settings_choice_new(
			app->ui, app->setting_row, &shown);
	else if (app->setting_row == 2) {
		unsigned int size = app->program_settings ? app->program_size :
							    app->default_size;

		list = fplinux_ui_list_new(app->ui, _("Size"),
					   FPLINUX_UI_LIST_EXCLUSIVE);
		if (list) {
			for (unsigned int i = 0; i < 7; ++i)
				fplinux_ui_list_append(list, size_label(i),
						       NULL);
			fplinux_ui_list_set_selected(list, size, true);
			fplinux_ui_list_set_focused(list, size);
		}
	} else
		return;
	if (!list)
		return;
	fplinux_ui_screen_add_command(list, &back_command);
	fplinux_ui_screen_set_listener(list, home_command, settings_changed,
				       app);
	app->settings_choice = list;
	fplinux_ui_push(app->ui, list);
}

static void leave_home(struct pm_app *app)
{
	if (!app->home)
		return;
	fplinux_ui_pop_to(app->ui, app->home);
	fplinux_ui_pop(app->ui);
	app->home = NULL;
	app->auxiliary = NULL;
	app->settings_scope = app->settings_choice = NULL;
	app->picker_requested = false;
	app->guest_active = true;
	fplinux_ui_set_fullscreen(app->ui, app->guest_fullscreen);
	if (app->resize)
		app->resize(app->resize_context);
	sync_visibility(app);
}

static void home_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int command_id,
			 void *context)
{
	struct pm_app *app = context;
	bool guest = app->lifecycle.phase != FPLINUX_PM_PHASE_PICKING &&
		     app->lifecycle.phase != FPLINUX_PM_PHASE_EXITED;

	if (screen == app->home) {
		if (command_id == FPLINUX_UI_SELECT_COMMAND) {
			size_t row = fplinux_ui_list_get_focused(screen);

			if (guest && row == 0) {
				leave_home(app);
				return;
			}
			if (guest && row != SIZE_MAX)
				--row;
			if (row == 0)
				open_picker(app);
			else if (row == 1)
				show_recent(app);
			else if (row == 2) {
				if (guest)
					show_settings_scope(app);
				else
					show_settings(app, false);
			}
		} else if (command_id == COMMAND_BACK) {
			if (guest)
				leave_home(app);
			else
				fplinux_ui_back(ui);
		}
		return;
	}
	if (screen == app->settings_scope) {
		if (command_id == FPLINUX_UI_SELECT_COMMAND)
			show_settings(app,
				      fplinux_ui_list_get_focused(screen) == 0);
		else if (command_id == COMMAND_BACK) {
			app->settings_scope = NULL;
			fplinux_ui_pop(ui);
		}
		return;
	}
	if (screen == app->settings_choice) {
		if (command_id == COMMAND_BACK) {
			app->settings_choice = NULL;
			fplinux_ui_pop(ui);
		}
		return;
	}
	if (screen != app->auxiliary)
		return;
	if (app->auxiliary_kind == AUX_RECENT &&
	    command_id == FPLINUX_UI_SELECT_COMMAND) {
		size_t row = fplinux_ui_list_get_focused(screen);

		if (row < app->recent.count) {
			char *path = strdup(app->recent.items[row].path);

			app->auxiliary = NULL;
			fplinux_ui_pop(ui);
			if (path) {
				open_path(app, path);
				free(path);
			}
		}
	} else if (app->auxiliary_kind == AUX_RECENT &&
		   command_id == COMMAND_REMOVE) {
		size_t row = fplinux_ui_list_get_focused(screen);

		if (fplinux_ui_recent_remove(&app->recent, row)) {
			fplinux_ui_list_delete(screen, row);
			if (!app->recent.count)
				fplinux_ui_screen_remove_command(
					screen, COMMAND_REMOVE);
			save_preferences(app, app->prefs,
					 fplinux_ui_recent_store(&app->recent,
								 app->prefs),
					 false);
		}
	} else if (app->auxiliary_kind == AUX_SETTINGS &&
		   command_id == FPLINUX_UI_SELECT_COMMAND) {
		open_settings_choice(app);
	} else if (app->auxiliary_kind == AUX_SETTINGS &&
		   command_id == COMMAND_RESET && app->program_settings) {
		save_settings(app, true, 0);
	} else if (command_id == COMMAND_BACK || command_id == COMMAND_CANCEL) {
		app->auxiliary = NULL;
		fplinux_ui_pop(ui);
	}
}

static void append_home_row(struct pm_app *app,
			    struct fplinux_ui_screen *screen, const char *label,
			    const char *glyph)
{
	size_t row = fplinux_ui_list_append(screen, label, NULL);

	if (row != SIZE_MAX)
		fplinux_ui_list_set_icon(screen, row,
					 fplinux_ui_glyph(app->ui, glyph,
							  FPLINUX_UI_GLYPH_ROW),
					 FPLINUX_UI_LIST_ICON_IMAGE);
}

static void show_home(struct pm_app *app)
{
	bool guest = app->lifecycle.phase != FPLINUX_PM_PHASE_PICKING &&
		     app->lifecycle.phase != FPLINUX_PM_PHASE_EXITED;
	struct fplinux_ui_screen *screen;

	if (app->home)
		return;
	screen = fplinux_ui_list_new(app->ui, "phoneME",
				     FPLINUX_UI_LIST_IMPLICIT);
	if (!screen)
		return;
	if (guest) {
		append_home_row(app, screen, _("Continue"), "play");
		if (app->midlet)
			set_midlet_icon(app, screen, 0, app->midlet->icon);
	}
	append_home_row(app, screen, _("Open JAR"), "folder");
	append_home_row(app, screen, _("Recent"), "history");
	append_home_row(app, screen, _("Settings"), "settings");
	if (app->prefs_error || app->program_prefs_error)
		fplinux_ui_screen_set_header_status(screen, _("Unsaved"), NULL);
	fplinux_ui_screen_add_command(screen, &back_command);
	fplinux_ui_screen_set_listener(screen, home_command, NULL, app);
	app->home = screen;
	fplinux_pm_keys_cancel(FPLINUX_UI_KEY_PHONE);
	fplinux_pm_keys_cancel(FPLINUX_UI_KEY_KEYBOARD);
	app->guest_active = false;
	sync_visibility(app);
	if (guest)
		fplinux_ui_set_fullscreen(app->ui, false);
	fplinux_ui_push(app->ui, screen);
	if (!app->storage_request && !app->picker)
		app->storage_request = fplinux_ui_storage_request(
			app->ui, FPLINUX_VOLUME_REFRESH, 0);
}

static void close_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int command_id,
			  void *context)
{
	struct pm_app *app = context;
	bool confirmed = command_id == COMMAND_CONFIRM_CLOSE;

	(void)screen;
	if (!app->close_wait)
		return;
	if (!confirmed) {
		app->close_wait = false;
		if (app->tick && app->lifecycle.visible)
			apply(app,
			      (struct fplinux_pm_schedule){
				      FPLINUX_PM_TIMER_RUN, 0 },
			      false);
	}
	fplinux_ui_close_reply(ui, confirmed ? FPLINUX_UI_CLOSE_READY :
					       FPLINUX_UI_CLOSE_CANCEL);
}

static enum fplinux_ui_close_response prepare_close(struct fplinux_ui *ui,
						    void *context)
{
	struct pm_app *app = context;
	const struct fplinux_ui_command confirm = {
		.id = COMMAND_CONFIRM_CLOSE,
		.type = FPLINUX_UI_COMMAND_OK,
		.label = _("Close"),
	};
	const struct fplinux_ui_command cancel = {
		.id = COMMAND_CANCEL,
		.type = FPLINUX_UI_COMMAND_CANCEL,
	};
	struct fplinux_ui_screen *alert;

	if (app->close_wait)
		return FPLINUX_UI_CLOSE_WAIT;
	if (fplinux_pm_vm_get_state() != FPLINUX_PM_VM_RUNNING)
		return FPLINUX_UI_CLOSE_READY;
	alert = fplinux_ui_alert_new(ui, FPLINUX_UI_ALERT_CONFIRMATION,
				     _("Close MIDlet?"),
				     _("Close the running MIDlet?"), 0);
	if (!alert)
		return FPLINUX_UI_CLOSE_CANCEL;
	app->close_wait = true;
	if (app->tick)
		lv_timer_pause(app->tick);
	fplinux_ui_screen_add_command(alert, &confirm);
	fplinux_ui_screen_add_command(alert, &cancel);
	fplinux_ui_screen_set_listener(alert, close_command, NULL, app);
	fplinux_ui_push(ui, alert);
	return FPLINUX_UI_CLOSE_WAIT;
}

/* Events */

static void sync_visibility(struct pm_app *app)
{
	struct fplinux_pm_visibility visibility =
		fplinux_pm_lifecycle_visibility(
			&app->lifecycle,
			app->guest_active && fplinux_ui_is_visible(app->ui) &&
				fplinux_ui_lifecycle(app->ui) ==
					FPLINUX_UI_FOREGROUND,
			fplinux_ui_now_ms());

	if (visibility.notify_vm)
		fplinux_pm_vm_set_visible(app->lifecycle.visible);
	if (!app->close_wait)
		apply(app, visibility.schedule, false);
	if (app->lifecycle.visible && app->loaded)
		set_busy(app, NULL);
}

static void handle_event(struct fplinux_ui *ui,
			 const struct fplinux_ui_event *event, void *context)
{
	struct pm_app *app = context;
	(void)ui;
	switch (event->type) {
	case FPLINUX_UI_EVENT_VISIBILITY:
	case FPLINUX_UI_EVENT_LIFECYCLE:
		sync_visibility(app);
		break;
	case FPLINUX_UI_EVENT_CLOSE:
		set_busy(app, NULL);
		app->picker_requested = false;
		if (app->picker) {
			fplinux_ui_file_picker_close(app->picker);
			app->picker = NULL;
		}
		document_reply(app, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
		close_vm(app);
		break;
	case FPLINUX_UI_EVENT_ACTIVATION:
		if (event->activation == FPLINUX_UI_ACTIVATION_OPEN &&
		    !app->document_id && !app->open_request)
			show_home(app);
		break;
	case FPLINUX_UI_EVENT_DOCUMENT_RESULT:
		if (event->document_result.request_id == app->open_request &&
		    event->document_result.status != FPLINUX_UI_DOCUMENT_WAIT) {
			app->open_request = 0;
			if (event->document_result.status ==
			    FPLINUX_UI_DOCUMENT_ERROR)
				show_alert(
					app, _("Open JAR"),
					strerror(event->document_result.error));
		}
		break;
	case FPLINUX_UI_EVENT_RESIZE:
		if (app->guest_active && app->resize)
			app->resize(app->resize_context);
		break;
	case FPLINUX_UI_EVENT_INPUT_CANCEL:
		fplinux_pm_keys_cancel(event->key.source);
		break;
	case FPLINUX_UI_EVENT_KEY:
	case FPLINUX_UI_EVENT_SESSION:
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-phoneme",
		.description = "Run a MIDlet JAR.",
	};
	const char *home;
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	/* MIDlet output reaches the session log line by line. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	app.ui = fplinux_ui_open(FPLINUX_PM_APP_ID, handle_event, &app);
	if (!app.ui) {
		perror("fplinux-phoneme: open UI");
		return EXIT_FAILURE;
	}
	fplinux_pm_lifecycle_init(&app.lifecycle, false);
	fplinux_ui_set_document_handler(app.ui, open_document, &app);
	fplinux_ui_set_close_handler(app.ui, prepare_close, &app);
	fplinux_ui_set_storage_handler(app.ui, storage_result, &app);
	app.defaults = fplinux_ui_view_settings_defaults();
	app.settings = app.defaults;
	app.prefs = fplinux_ui_prefs_open(FPLINUX_PM_APP_ID);
	if (app.prefs) {
		long long size =
			fplinux_ui_prefs_get_int(app.prefs, "canvas-size", 0);

		fplinux_ui_view_settings_load(&app.defaults, app.prefs,
					      &app.settings);
		fplinux_ui_recent_load(&app.recent, app.prefs);
		if (size >= 0 && size <= 6)
			app.default_size = (unsigned int)size;
		app.settings = app.defaults;
	}
	home = getenv("HOME");
	if (!home || home[0] != '/' ||
	    snprintf(app.home_path, sizeof(app.home_path), "%s/Files", home) >=
		    (int)sizeof(app.home_path) ||
	    !fplinux_ui_state_dir(FPLINUX_PM_APP_ID, app.state_root,
				  sizeof(app.state_root))) {
		result = -1;
		goto cleanup;
	}
	show_home(&app);
	result = fplinux_ui_run(app.ui);
cleanup:
	if (app.tick)
		lv_timer_delete(app.tick);
	app.tick = NULL;
	fplinux_ui_close(app.ui);
	app.ui = NULL;
	free(app.storage);
	fplinux_ui_prefs_close(app.program_prefs);
	fplinux_ui_prefs_close(app.prefs);
	fplinux_ui_recent_cleanup(&app.recent);
	if (app.jar_directory >= 0)
		close(app.jar_directory);
	fplinux_pm_suite_cleanup(&app.suite);
	return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
