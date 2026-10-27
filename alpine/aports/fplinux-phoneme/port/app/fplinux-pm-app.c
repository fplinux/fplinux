// SPDX-License-Identifier: GPL-2.0-only
/*
 * phoneME: receive a JAR from Files and run its MIDlet in the in-process VM.
 * Nothing is installed and nothing is kept:
 * record stores live in a /tmp directory that is removed at exit.
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
#include "../canvas/fplinux-pm-canvas.h"
#include "../../vm/fplinux-pm-vm.h"

#include <errno.h>
#include <fcntl.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-ui.h>
#include <ftw.h>
#include <libintl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define _(text) gettext(text)
#define STORAGE_TEMPLATE "/tmp/fplinux-phoneme-XXXXXX"
/* A MIDlet icon is a small PNG; larger ones are not shown. */
#define ICON_MAX_BYTES (64u * 1024u)
#define MESSAGE_BYTES 1024

enum command {
	COMMAND_BACK = 1,
	COMMAND_CLOSE,
	COMMAND_CANCEL,
	COMMAND_CONFIRM_CLOSE,
};

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
	char storage[sizeof(STORAGE_TEMPLATE)];
	fplinux_pm_app_resize_fn resize;
	void *resize_context;
};

static struct pm_app app = { .jar_directory = -1 };

static const struct fplinux_ui_command back_command = {
	.id = COMMAND_BACK,
	.type = FPLINUX_UI_COMMAND_BACK,
};

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
	return app.ui && fplinux_ui_set_fullscreen(app.ui, fullscreen);
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

static int remove_entry(const char *path, const struct stat *status, int type,
			struct FTW *ftw)
{
	(void)status;
	(void)type;
	(void)ftw;
	if (remove(path))
		fprintf(stderr, "fplinux-phoneme: remove %s: %s\n", path,
			strerror(errno));
	return 0;
}

/* The VM's private directory for this run, or NULL without record stores. */
static const char *create_storage(struct pm_app *app)
{
	memcpy(app->storage, STORAGE_TEMPLATE, sizeof(app->storage));
	if (mkdtemp(app->storage))
		return app->storage;
	fprintf(stderr, "fplinux-phoneme: record store folder: %s\n",
		strerror(errno));
	app->storage[0] = '\0';
	return NULL;
}

/* Only the directory mkdtemp() made, without following links out of it. */
static void remove_storage(struct pm_app *app)
{
	if (!app->storage[0])
		return;
	if (nftw(app->storage, remove_entry, 8,
		 FTW_DEPTH | FTW_PHYS | FTW_MOUNT))
		fprintf(stderr, "fplinux-phoneme: remove %s: %s\n",
			app->storage, strerror(errno));
	app->storage[0] = '\0';
}

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

/*
 * The MIDlet has ended. notifyDestroyed() leaves for the catalogue, which
 * then sends CLOSE; a failure stays on screen until it is read.
 */
static void vm_finished(struct pm_app *app, bool started)
{
	char text[MESSAGE_BYTES];
	int code = fplinux_pm_vm_exit_code();

	set_busy(app, NULL);
	document_reply(app, FPLINUX_UI_DOCUMENT_ERROR, EIO);

	if (app->tick) {
		lv_timer_delete(app->tick);
		app->tick = NULL;
	}
	fprintf(stderr, "fplinux-phoneme: VM exited with status %d\n", code);
	if (started && !code) {
		fplinux_ui_back(app->ui);
		return;
	}
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
		.storage_dir = create_storage(app),
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
		fplinux_ui_form_append_string(form, _("Vendor"),
					      app->suite.vendor);
	if (app->suite.version[0])
		fplinux_ui_form_append_string(form, _("Version"),
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

/* Check the class before the VM starts, so a broken suite stays in the GUI. */
static int choose_midlet(struct pm_app *app, size_t index)
{
	const struct fplinux_pm_midlet *midlet = &app->suite.midlets[index];
	enum fplinux_pm_jar_status status = FPLINUX_PM_JAR_NO_ENTRY;
	char entry[PATH_MAX];
	unsigned char *data = NULL;
	size_t size = 0;

	set_busy(app, midlet->name);
	if (class_entry(midlet->class_name, entry, sizeof(entry)))
		status = fplinux_pm_jar_read_entry(app->jar_path, entry, 0,
						   &data, &size);
	free(data);
	if (status == FPLINUX_PM_JAR_OK || status == FPLINUX_PM_JAR_TOO_LARGE) {
		return start_midlet(app, midlet);
	}
	set_busy(app, NULL);
	return status == FPLINUX_PM_JAR_IO	  ? errno :
	       status == FPLINUX_PM_JAR_NO_MEMORY ? ENOMEM :
						    ENOEXEC;
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
			show_jar_problem(app, error == ENOMEM ?
						      FPLINUX_PM_JAR_NO_MEMORY :
						      FPLINUX_PM_JAR_FORMAT);
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
		fplinux_pm_suite_cleanup(&suite);
		close(descriptor);
		result.status = FPLINUX_UI_DOCUMENT_RESTART_REQUIRED;
		result.error = 0;
		return result;
	}
	while (fplinux_ui_pop(ui))
		;
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
	alert = fplinux_ui_alert_new(
		ui, FPLINUX_UI_ALERT_CONFIRMATION, _("Close MIDlet?"),
		_("Record stores are temporary and will be removed when phoneME closes."),
		0);
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

static void handle_event(struct fplinux_ui *ui,
			 const struct fplinux_ui_event *event, void *context)
{
	struct pm_app *app = context;
	struct fplinux_pm_visibility visibility;

	(void)ui;
	switch (event->type) {
	case FPLINUX_UI_EVENT_VISIBILITY:
	case FPLINUX_UI_EVENT_LIFECYCLE:
		visibility = fplinux_pm_lifecycle_visibility(
			&app->lifecycle,
			fplinux_ui_is_visible(ui) &&
				fplinux_ui_lifecycle(ui) ==
					FPLINUX_UI_FOREGROUND,
			fplinux_ui_now_ms());
		if (visibility.notify_vm)
			fplinux_pm_vm_set_visible(app->lifecycle.visible);
		if (!app->close_wait)
			apply(app, visibility.schedule, false);
		if (app->lifecycle.visible && app->loaded)
			set_busy(app, NULL);
		break;
	case FPLINUX_UI_EVENT_CLOSE:
		set_busy(app, NULL);
		close_vm(app);
		break;
	case FPLINUX_UI_EVENT_RESIZE:
		if (app->resize)
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
		.description =
			"Run a MIDlet JAR opened from Files. Record stores are temporary.",
	};
	struct fplinux_ui_screen *screen;
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
	fplinux_pm_lifecycle_init(&app.lifecycle,
				  fplinux_ui_is_visible(app.ui) &&
					  fplinux_ui_lifecycle(app.ui) ==
						  FPLINUX_UI_FOREGROUND);
	fplinux_ui_set_document_handler(app.ui, open_document, &app);
	fplinux_ui_set_close_handler(app.ui, prepare_close, &app);
	screen = fplinux_ui_form_new(app.ui, "phoneME");
	if (!screen) {
		fplinux_ui_close(app.ui);
		return EXIT_FAILURE;
	}
	fplinux_ui_form_append_string(screen, NULL,
				      _("Open a JAR file from Files."));
	fplinux_ui_screen_add_command(screen, &back_command);
	fplinux_ui_screen_set_listener(screen, leave_command, NULL, &app);
	fplinux_ui_push(app.ui, screen);
	result = fplinux_ui_run(app.ui);
	if (app.tick)
		lv_timer_delete(app.tick);
	app.tick = NULL;
	fplinux_ui_close(app.ui);
	app.ui = NULL;
	remove_storage(&app);
	if (app.jar_directory >= 0)
		close(app.jar_directory);
	fplinux_pm_suite_cleanup(&app.suite);
	return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
