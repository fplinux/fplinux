/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <kni.h>
#include <midpEvents.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-ui-file-picker.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-volumes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fplinux-pm-app.h"
#include "fplinux-pm-vm.h"

struct fplinux_ui {
	fplinux_ui_event_cb event;
	fplinux_ui_document_cb document;
	fplinux_ui_close_cb close;
	void *context;
	void *document_context;
	void *close_context;
	bool visible;
	enum fplinux_ui_lifecycle lifecycle;
};

struct fplinux_ui_screen {
	bool alert;
	char title[256];
	char text[512];
	char status[128];
	char rows[16][128];
	char details[16][128];
	bool icons[16];
	bool selected[16];
	size_t row_count;
	size_t focused;
	struct fplinux_ui_command commands[4];
	size_t command_count;
	fplinux_ui_command_cb command;
	fplinux_ui_change_cb change;
	void *context;
	void *data;
};

struct lv_timer {
	lv_timer_cb_t callback;
	void *context;
	bool paused;
};

static struct fplinux_ui runtime = {
	.visible = true,
	.lifecycle = FPLINUX_UI_FOREGROUND,
};
static struct fplinux_ui_screen *stack[16];
static size_t depth;
static lv_timer_t *tick;
static enum fplinux_pm_vm_state vm_state;
static bool vm_exits;
static int vm_exit_status;
static unsigned int starts, destroys, hide_events, show_events, replies;
static unsigned int busy_notifications, quit_requests, back_requests;
static bool busy, fail_file_sync, fail_directory_sync;
static unsigned int resize_notifications;
static int navigation_error;
static struct fplinux_ui_document_result reply;
static char running_jar[4096], running_class[256];
static const char *scenario;
static fplinux_ui_file_picker_cb picker_callback;
static void *picker_context;
static char requested_path[4096];
static fplinux_ui_storage_cb storage_callback;
static void *storage_context;
static const int system_glyph;

int __real_fsync(int fd);

int __wrap_fsync(int fd)
{
	struct stat status;

	assert(fstat(fd, &status) == 0);
	if ((fail_file_sync && S_ISREG(status.st_mode)) ||
	    (fail_directory_sync && S_ISDIR(status.st_mode))) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

const char *fplinux_ui_storage_error(struct fplinux_storage_result result)
{
	assert(result.error == EIO);
	return "Storage input/output error";
}

static void resized(void *context)
{
	assert(context == &resize_notifications);
	++resize_notifications;
}

jint fplinux_pm_test_parameters[5];

bool Java_javax_microedition_lcdui_LFNative_attach0(void);
bool Java_javax_microedition_lcdui_LFNative_alive0(void);
bool Java_javax_microedition_lcdui_LFNative_replace0(void);
bool Java_javax_microedition_lcdui_LFNative_push0(void);
jint Java_javax_microedition_lcdui_LFNative_current0(void);

static struct fplinux_ui_screen *new_screen(const char *title)
{
	struct fplinux_ui_screen *screen = calloc(1, sizeof(*screen));

	assert(screen);
	snprintf(screen->title, sizeof(screen->title), "%s",
		 title ? title : "");
	return screen;
}

bool __wrap_fplinux_ui_state_dir(const char *id, char *path, size_t capacity)
{
	const char *root = getenv("FPLINUX_PM_TEST_ROOT");

	(void)id;
	return root && snprintf(path, capacity, "%s", root) < (int)capacity;
}

struct fplinux_ui_prefs *__wrap_fplinux_ui_prefs_open(const char *id)
{
	char path[4096];

	return __wrap_fplinux_ui_state_dir(id, path, sizeof(path)) ?
		       fplinux_ui_prefs_open_directory(path) :
		       NULL;
}

void fplinux_ui_set_storage_handler(struct fplinux_ui *ui,
				    fplinux_ui_storage_cb callback,
				    void *context)
{
	(void)ui;
	storage_callback = callback;
	storage_context = context;
}

uint32_t fplinux_ui_storage_request(struct fplinux_ui *ui,
				    enum fplinux_volume_operation operation,
				    uint32_t volume_id)
{
	(void)ui;
	assert(operation == FPLINUX_VOLUME_REFRESH && !volume_id);
	return scenario && !strncmp(scenario, "storage-", 8) ? 42 : 0;
}

struct fplinux_ui_file_picker *fplinux_ui_file_picker_open(
	struct fplinux_ui *ui,
	const struct fplinux_ui_file_picker_options *options,
	fplinux_ui_file_picker_cb callback, void *context)
{
	(void)ui;
	assert(options->mode == FPLINUX_UI_FILE_PICKER_FILE);
	assert(!strcmp(options->extensions[0], "jar"));
	picker_callback = callback;
	picker_context = context;
	return (struct fplinux_ui_file_picker *)&runtime;
}

void fplinux_ui_file_picker_close(struct fplinux_ui_file_picker *picker)
{
	(void)picker;
}

void fplinux_ui_file_picker_storage_result(
	struct fplinux_ui_file_picker *picker,
	const struct fplinux_volume_result *result)
{
	(void)picker;
	(void)result;
}

uint32_t fplinux_ui_open_file(struct fplinux_ui *ui, const char *path)
{
	(void)ui;
	snprintf(requested_path, sizeof(requested_path), "%s", path);
	return 77;
}

void fplinux_pm_canvas_view_changed(void)
{
}

struct fplinux_ui *fplinux_ui_open(const char *id, fplinux_ui_event_cb callback,
				   void *context)
{
	assert(!strcmp(id, "org.fplinux.phoneme"));
	runtime.event = callback;
	runtime.context = context;
	scenario = getenv("FPLINUX_PM_TEST_SCENARIO");
	return &runtime;
}

void fplinux_ui_set_document_handler(struct fplinux_ui *ui,
				     fplinux_ui_document_cb callback,
				     void *context)
{
	ui->document = callback;
	ui->document_context = context;
}

void fplinux_ui_set_close_handler(struct fplinux_ui *ui,
				  fplinux_ui_close_cb callback, void *context)
{
	ui->close = callback;
	ui->close_context = context;
}

bool fplinux_ui_document_reply(struct fplinux_ui *ui, uint32_t id,
			       enum fplinux_ui_document_status status,
			       int error)
{
	(void)ui;
	reply = (struct fplinux_ui_document_result){ id, status, error };
	++replies;
	return true;
}

bool fplinux_ui_close_reply(struct fplinux_ui *ui,
			    enum fplinux_ui_close_response response)
{
	(void)ui;
	(void)response;
	return true;
}

bool fplinux_ui_send(struct fplinux_ui *ui,
		     const struct fplinux_ui_session_message *message)
{
	(void)ui;
	assert(message->type == FPLINUX_UI_SESSION_BUSY);
	++busy_notifications;
	busy = message->enabled;
	return true;
}

void fplinux_ui_close(struct fplinux_ui *ui)
{
	(void)ui;
	while (depth)
		free(stack[--depth]);
}

bool fplinux_ui_is_visible(const struct fplinux_ui *ui)
{
	return ui->visible;
}

enum fplinux_ui_lifecycle fplinux_ui_lifecycle(const struct fplinux_ui *ui)
{
	return ui->lifecycle;
}

bool fplinux_ui_set_fullscreen(struct fplinux_ui *ui, bool full)
{
	(void)ui;
	(void)full;
	return true;
}

uint64_t fplinux_ui_now_ms(void)
{
	return 1000;
}

void fplinux_ui_quit(struct fplinux_ui *ui)
{
	(void)ui;
	++quit_requests;
}

bool fplinux_ui_back(struct fplinux_ui *ui)
{
	(void)ui;
	return true;
}

bool fplinux_ui_back_after_document(struct fplinux_ui *ui)
{
	(void)ui;
	++back_requests;
	return true;
}

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	(void)ui;
	if (depth < 2)
		return false;
	free(stack[--depth]);
	return true;
}

bool fplinux_ui_pop_to(struct fplinux_ui *ui, struct fplinux_ui_screen *target)
{
	while (depth && stack[depth - 1] != target)
		assert(fplinux_ui_pop(ui));
	return depth != 0;
}

void fplinux_ui_go_back(struct fplinux_ui *ui)
{
	fplinux_ui_pop(ui);
}

bool fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	(void)ui;
	if (navigation_error) {
		errno = navigation_error;
		return false;
	}
	assert(depth < 16);
	stack[depth++] = screen;
	return true;
}

bool fplinux_ui_replace(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	if (navigation_error) {
		errno = navigation_error;
		return false;
	}
	if (depth)
		free(stack[--depth]);
	return fplinux_ui_push(ui, screen);
}

struct fplinux_ui_screen *fplinux_ui_current(const struct fplinux_ui *ui)
{
	(void)ui;
	return depth ? stack[depth - 1] : NULL;
}

void fplinux_ui_screen_set_data(struct fplinux_ui_screen *screen, void *data)
{
	screen->data = data;
}

void *fplinux_ui_screen_get_data(const struct fplinux_ui_screen *screen)
{
	return screen->data;
}

bool fplinux_ui_screen_set_title(struct fplinux_ui_screen *screen,
				 const char *title)
{
	snprintf(screen->title, sizeof(screen->title), "%s",
		 title ? title : "");
	return true;
}

bool fplinux_ui_screen_set_header_status(struct fplinux_ui_screen *screen,
					 const char *full, const char *compact)
{
	(void)compact;
	snprintf(screen->status, sizeof(screen->status), "%s",
		 full ? full : "");
	return true;
}

void fplinux_ui_screen_remove_command(struct fplinux_ui_screen *screen, int id)
{
	for (size_t i = 0; i < screen->command_count; ++i) {
		if (screen->commands[i].id == id) {
			--screen->command_count;
			memmove(&screen->commands[i], &screen->commands[i + 1],
				(screen->command_count - i) *
					sizeof(screen->commands[0]));
			return;
		}
	}
}

bool fplinux_ui_screen_get_content_size(const struct fplinux_ui_screen *screen,
					int *width, int *height)
{
	(void)screen;
	(void)width;
	(void)height;
	abort();
}

size_t
fplinux_ui_screen_get_command_item(const struct fplinux_ui_screen *screen)
{
	(void)screen;
	return SIZE_MAX;
}

lv_obj_t *fplinux_ui_canvas_object(struct fplinux_ui_screen *screen)
{
	(void)screen;
	return NULL;
}

struct fplinux_ui_screen *fplinux_ui_form_new(struct fplinux_ui *ui,
					      const char *title)
{
	(void)ui;
	return new_screen(title);
}

size_t fplinux_ui_form_append_string(struct fplinux_ui_screen *form,
				     const char *label, const char *text)
{
	(void)label;
	snprintf(form->text, sizeof(form->text), "%s", text);
	return 0;
}

size_t fplinux_ui_form_append_property(struct fplinux_ui_screen *form,
				       const char *label, const char *text)
{
	return fplinux_ui_form_append_string(form, label, text);
}

size_t fplinux_ui_form_append_image(struct fplinux_ui_screen *form,
				    const char *label, const void *image)
{
	(void)form;
	(void)label;
	(void)image;
	return 0;
}

struct fplinux_ui_screen *
fplinux_ui_alert_new(struct fplinux_ui *ui, enum fplinux_ui_alert_kind kind,
		     const char *title, const char *text, unsigned int timeout)
{
	struct fplinux_ui_screen *screen = new_screen(title);

	(void)ui;
	(void)kind;
	(void)timeout;
	screen->alert = true;
	snprintf(screen->text, sizeof(screen->text), "%s", text);
	return screen;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command)
{
	assert(screen->command_count < 4);
	screen->commands[screen->command_count++] = *command;
	return true;
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *screen,
				    fplinux_ui_command_cb callback,
				    fplinux_ui_change_cb change, void *context)
{
	screen->command = callback;
	screen->change = change;
	screen->context = context;
}

struct fplinux_ui_screen *fplinux_ui_list_new(struct fplinux_ui *ui,
					      const char *title,
					      enum fplinux_ui_list_kind kind)
{
	(void)ui;
	assert(kind == FPLINUX_UI_LIST_IMPLICIT ||
	       kind == FPLINUX_UI_LIST_EXCLUSIVE);
	return new_screen(title);
}

size_t fplinux_ui_list_append(struct fplinux_ui_screen *list, const char *text,
			      const char *detail)
{
	size_t index = list->row_count++;

	assert(index < 16);
	snprintf(list->rows[index], sizeof(list->rows[index]), "%s",
		 text ? text : "");
	snprintf(list->details[index], sizeof(list->details[index]), "%s",
		 detail ? detail : "");
	return index;
}

size_t fplinux_ui_list_get_focused(const struct fplinux_ui_screen *list)
{
	return list->focused;
}

bool fplinux_ui_list_set_select_label(struct fplinux_ui_screen *list,
				      const char *label)
{
	(void)list;
	assert(!strcmp(label, "Start"));
	return true;
}

bool fplinux_ui_list_set_icon(struct fplinux_ui_screen *list, size_t index,
			      const void *source,
			      enum fplinux_ui_list_icon_kind kind)
{
	const lv_image_dsc_t *image = source;

	assert(index < list->row_count);
	if (source != &system_glyph) {
		assert(image->header.w == 1 && image->header.h == 1);
		assert(image->data_size >= 24 && image->data[0] == 0x89);
	}
	list->icons[index] = true;
	assert(kind == FPLINUX_UI_LIST_ICON_IMAGE);
	return true;
}

const void *fplinux_ui_glyph(struct fplinux_ui *ui, const char *name,
			     enum fplinux_ui_glyph_size size)
{
	(void)ui;
	assert(name && size == FPLINUX_UI_GLYPH_ROW);
	return &system_glyph;
}

bool fplinux_ui_list_set_trailing_value(struct fplinux_ui_screen *list,
					size_t index, const char *value)
{
	assert(index < list->row_count);
	snprintf(list->details[index], sizeof(list->details[index]), "%s",
		 value);
	return true;
}

/* Only navigation is under test; unused widget calls fail explicitly. */
size_t fplinux_ui_list_insert(struct fplinux_ui_screen *list, size_t index,
			      const char *text, const char *detail)
{
	(void)list;
	(void)index;
	(void)text;
	(void)detail;
	abort();
}

bool fplinux_ui_list_delete(struct fplinux_ui_screen *list, size_t index)
{
	assert(index < list->row_count);
	--list->row_count;
	memmove(&list->rows[index], &list->rows[index + 1],
		(list->row_count - index) * sizeof(list->rows[0]));
	memmove(&list->details[index], &list->details[index + 1],
		(list->row_count - index) * sizeof(list->details[0]));
	memmove(&list->icons[index], &list->icons[index + 1],
		(list->row_count - index) * sizeof(list->icons[0]));
	return true;
}

void fplinux_ui_list_delete_all(struct fplinux_ui_screen *list)
{
	(void)list;
	abort();
}

bool fplinux_ui_list_set(struct fplinux_ui_screen *list, size_t index,
			 const char *text, const char *detail)
{
	assert(index < list->row_count);
	snprintf(list->rows[index], sizeof(list->rows[index]), "%s", text);
	snprintf(list->details[index], sizeof(list->details[index]), "%s",
		 detail ? detail : "");
	return true;
}

size_t fplinux_ui_list_size(const struct fplinux_ui_screen *list)
{
	return list->row_count;
}

void fplinux_ui_list_set_focused(struct fplinux_ui_screen *list, size_t index)
{
	list->focused = index;
}

bool fplinux_ui_list_is_selected(const struct fplinux_ui_screen *list,
				 size_t index)
{
	assert(index < list->row_count);
	return list->selected[index];
}

void fplinux_ui_list_set_selected(struct fplinux_ui_screen *list, size_t index,
				  bool selected)
{
	assert(index < list->row_count);
	list->selected[index] = selected;
}

bool fplinux_ui_alert_set_text(struct fplinux_ui_screen *alert,
			       const char *text)
{
	(void)alert;
	(void)text;
	abort();
}

bool fplinux_ui_alert_set_progress(struct fplinux_ui_screen *alert, int value,
				   int maximum)
{
	(void)alert;
	(void)value;
	(void)maximum;
	abort();
}

void StoreMIDPEventInVmThread(MidpEvent event, int isolate)
{
	(void)event;
	(void)isolate;
	abort();
}

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period,
			    void *context)
{
	(void)period;
	assert(!tick);
	tick = calloc(1, sizeof(*tick));
	assert(tick);
	tick->callback = callback;
	tick->context = context;
	return tick;
}

void lv_timer_pause(lv_timer_t *timer)
{
	timer->paused = true;
}
void lv_timer_resume(lv_timer_t *timer)
{
	timer->paused = false;
}
void lv_timer_ready(lv_timer_t *timer)
{
	(void)timer;
}
void lv_timer_reset(lv_timer_t *timer)
{
	(void)timer;
}
void lv_timer_set_period(lv_timer_t *timer, uint32_t period)
{
	(void)timer;
	(void)period;
}
void *lv_timer_get_user_data(lv_timer_t *timer)
{
	return timer->context;
}
void lv_refr_now(void *display)
{
	(void)display;
}
void lv_timer_delete(lv_timer_t *timer)
{
	assert(timer == tick);
	tick = NULL;
	free(timer);
}

bool fplinux_pm_vm_start(const struct fplinux_pm_vm_config *config)
{
	struct stat directory;

	assert(vm_state == FPLINUX_PM_VM_IDLE);
	assert(config->storage_dir && !stat(config->storage_dir, &directory));
	assert(S_ISDIR(directory.st_mode) &&
	       (directory.st_mode & 0777) == 0700);
	printf("storage %s\n", config->storage_dir);
	if (!strcmp(scenario, "recent-remove-last")) {
		char path[4096];
		FILE *file;

		assert(snprintf(path, sizeof(path), "%s/record",
				config->storage_dir) < (int)sizeof(path));
		file = fopen(path, "w");
		assert(file && fputs("saved data", file) >= 0);
		assert(fclose(file) == 0);
	}
	++starts;
	snprintf(running_jar, sizeof(running_jar), "%s", config->jar_path);
	snprintf(running_class, sizeof(running_class), "%s",
		 config->midlet_class);
	vm_state = !strcmp(scenario, "start-failure") ? FPLINUX_PM_VM_EXITED :
							FPLINUX_PM_VM_RUNNING;
	return vm_state == FPLINUX_PM_VM_RUNNING;
}

int fplinux_pm_vm_slice(void)
{
	if (vm_exits || vm_state == FPLINUX_PM_VM_DESTROYING) {
		vm_state = FPLINUX_PM_VM_EXITED;
		return FPLINUX_PM_VM_SLICE_EXITED;
	}
	return FPLINUX_PM_VM_SLICE_IDLE;
}

void fplinux_pm_vm_request_destroy(void)
{
	++destroys;
	vm_state = FPLINUX_PM_VM_DESTROYING;
}

void fplinux_pm_vm_set_visible(bool visible)
{
	if (visible)
		++show_events;
	else
		++hide_events;
}

void fplinux_pm_device_visibility(bool visible)
{
	(void)visible;
}

void fplinux_pm_device_session(const struct fplinux_ui_session_message *reply)
{
	(void)reply;
}

void fplinux_pm_files_volumes(const struct fplinux_volume_snapshot *snapshot)
{
	(void)snapshot;
}

enum fplinux_pm_vm_state fplinux_pm_vm_get_state(void)
{
	return vm_state;
}
int fplinux_pm_vm_exit_code(void)
{
	return vm_exit_status;
}

void fplinux_pm_keys_cancel(enum fplinux_ui_key_source source)
{
	(void)source;
}

static void advance(void)
{
	assert(tick && !tick->paused);
	tick->callback(tick);
}

static void send_event(enum fplinux_ui_event_type type)
{
	struct fplinux_ui_event event = { .type = type };

	runtime.event(&runtime, &event, runtime.context);
}

static struct fplinux_ui_document_result document(uint32_t id, const char *path)
{
	struct fplinux_ui_document_request request = { id, path };

	return runtime.document(&runtime, &request, runtime.document_context);
}

static void command(enum fplinux_ui_command_type type)
{
	struct fplinux_ui_screen *screen = fplinux_ui_current(&runtime);

	for (size_t i = 0; i < screen->command_count; ++i)
		if (screen->commands[i].type == type) {
			bool alert = screen->alert;

			/* Alert leaves the stack before its listener, and stays readable. */
			if (alert)
				--depth;
			screen->command(&runtime, screen,
					screen->commands[i].id,
					screen->context);
			if (alert) {
				bool stacked = false;

				for (size_t j = 0; j < depth; ++j)
					if (stack[j] == screen)
						stacked = true;
				if (!stacked)
					free(screen);
			}
			return;
		}
	abort();
}

static bool has_remove(const struct fplinux_ui_screen *screen)
{
	for (size_t i = 0; i < screen->command_count; ++i)
		if (screen->commands[i].label &&
		    !strcmp(screen->commands[i].label, "Remove"))
			return true;
	return false;
}

static void attach_peer(struct fplinux_ui_screen *screen, int serial)
{
	fplinux_pm_test_parameters[0] = (jint)(intptr_t)screen;
	fplinux_pm_test_parameters[1] = serial;
	fplinux_pm_test_parameters[2] = screen->alert;
	assert(Java_javax_microedition_lcdui_LFNative_attach0());
}

static void select_row(size_t row)
{
	struct fplinux_ui_screen *screen = fplinux_ui_current(&runtime);

	assert(row < screen->row_count);
	screen->focused = row;
	screen->command(&runtime, screen, FPLINUX_UI_SELECT_COMMAND,
			screen->context);
}

static void choose_setting(size_t row, size_t value)
{
	struct fplinux_ui_screen *choice;

	select_row(row);
	choice = fplinux_ui_current(&runtime);
	assert(value < choice->row_count);
	choice->focused = value;
	assert(choice->change);
	choice->change(&runtime, choice, value, choice->context);
}

static void open_global_settings(void)
{
	select_row(3);
	assert(!strcmp(fplinux_ui_current(&runtime)->rows[1], "All programs"));
	select_row(1);
	assert(!strcmp(fplinux_ui_current(&runtime)->title, "All programs"));
}

static bool peer_alive(struct fplinux_ui_screen *screen, int serial)
{
	fplinux_pm_test_parameters[0] = (jint)(intptr_t)screen;
	fplinux_pm_test_parameters[1] = serial;
	return Java_javax_microedition_lcdui_LFNative_alive0();
}

static void refused_navigation(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *old = fplinux_ui_current(ui);
	struct fplinux_ui_screen *next =
		fplinux_ui_form_new(ui, "Next MIDlet screen");
	size_t old_depth = depth;
	unsigned int old_notifications = busy_notifications;
	bool replace = !strncmp(scenario, "replace-", 8);
	bool (*navigate)(void) =
		replace ? Java_javax_microedition_lcdui_LFNative_replace0 :
			  Java_javax_microedition_lcdui_LFNative_push0;
	char old_title[sizeof(old->title)];

	snprintf(old_title, sizeof(old_title), "%s", old->title);
	attach_peer(old, 41);
	attach_peer(next, 42);
	navigation_error = strstr(scenario, "busy") ? EBUSY : ENOENT;
	fplinux_pm_test_parameters[0] = (jint)(intptr_t)next;
	errno = 0;
	assert(!navigate() && errno == navigation_error);
	assert(Java_javax_microedition_lcdui_LFNative_current0() ==
	       (jint)(intptr_t)old);
	assert(depth == old_depth && !strcmp(old->title, old_title));
	assert(peer_alive(old, 41) && peer_alive(next, 42));
	assert(!replies && busy_notifications == old_notifications);
	assert(!destroys && !quit_requests && starts == 1);

	navigation_error = 0;
	fplinux_pm_test_parameters[0] = (jint)(intptr_t)next;
	assert(navigate());
	assert(Java_javax_microedition_lcdui_LFNative_current0() ==
	       (jint)(intptr_t)next);
	assert(depth == old_depth + (replace ? 0 : 1));
	assert(peer_alive(old, 41) == !replace && peer_alive(next, 42));
	assert(replies == 1 && reply.request_id == 17 &&
	       reply.status == FPLINUX_UI_DOCUMENT_ACCEPTED);
	assert(busy_notifications == old_notifications + 1);
	assert(!destroys && !quit_requests && starts == 1);
	printf("refused %s preserves peer and pending document; retry accepts\n",
	       replace ? "replace" : "push");
}

int fplinux_ui_run(struct fplinux_ui *ui)
{
	const char *jar = getenv("FPLINUX_PM_TEST_JAR");
	struct fplinux_ui_document_result result;
	struct fplinux_ui_screen *screen;
	struct fplinux_ui_screen *game;
	struct fplinux_ui_screen *suite;
	size_t game_depth;
	assert(ui->document);
	if (!scenario || !strcmp(scenario, "startup")) {
		assert(stack[0]->row_count == 3);
		assert(!strcmp(stack[0]->rows[0], "Open JAR"));
		assert(!strcmp(stack[0]->rows[1], "Recent"));
		assert(!strcmp(stack[0]->rows[2], "Settings"));
		puts("home offers JAR, recent and settings; no VM started");
		return 0;
	}
	if (!strcmp(scenario, "recent-empty")) {
		screen = fplinux_ui_current(ui);
		screen->focused = 1;
		screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND,
				screen->context);
		screen = fplinux_ui_current(ui);
		assert(!strcmp(screen->title, "Recent") && !screen->row_count);
		assert(!has_remove(screen));
		puts("empty Recent has no Remove action");
		return 0;
	}
	if (!strcmp(scenario, "storage-picker")) {
		struct fplinux_volume_result volume = {
			.request_id = 42,
			.snapshot.home_persistent = true,
		};

		screen = fplinux_ui_current(ui);
		assert(!screen->details[0][0]);
		screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND,
				screen->context);
		assert(!picker_callback);
		assert(storage_callback);
		storage_callback(ui, &volume, storage_context);
		assert(!screen->details[0][0] && picker_callback);
		puts("picker waits for the storage snapshot");
		return 0;
	}
	if (!strcmp(scenario, "picker-single")) {
		screen = fplinux_ui_current(ui);
		screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND,
				screen->context);
		assert(picker_callback);
		picker_callback((struct fplinux_ui_file_picker *)&runtime,
				FPLINUX_UI_FILE_PICKER_SELECTED, jar, 0,
				picker_context);
		assert(!strcmp(requested_path, jar));
	}
	result = document(17, jar);
	if (!strcmp(scenario, "invalid")) {
		assert(result.status == FPLINUX_UI_DOCUMENT_ERROR &&
		       result.error);
		assert(!starts && !replies);
		printf("document error %d; no VM started\n", result.error);
		return 0;
	}
	assert(result.status == FPLINUX_UI_DOCUMENT_WAIT && !result.error);
	assert(!starts && !replies);
	if (!strncmp(scenario, "chooser", 7)) {
		screen = fplinux_ui_current(ui);
		assert(screen->row_count == 2);
		assert(!strcmp(screen->rows[0], "First"));
		assert(!strcmp(screen->rows[1], "Second"));
		if (!strcmp(scenario, "chooser-cancel")) {
			command(FPLINUX_UI_COMMAND_BACK);
			assert(reply.request_id == 17 &&
			       reply.status == FPLINUX_UI_DOCUMENT_CANCELLED);
			assert(!starts);
			puts("chooser cancelled; no VM started");
			return 0;
		}
		screen->focused = 1;
		screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND,
				screen->context);
		if (!strcmp(scenario, "chooser-rms-failure")) {
			assert(replies == 1 &&
			       reply.status == FPLINUX_UI_DOCUMENT_ERROR);
			assert(reply.error == ENOTDIR && !busy && !starts);
			screen = fplinux_ui_current(ui);
			assert(screen->alert &&
			       !strcmp(screen->title, "Cannot start"));
			assert(!strcmp(screen->text, strerror(ENOTDIR)));
			puts("RMS failure clears busy and reports error without starting VM");
			return 0;
		}
	}
	advance();
	assert(!starts && !replies);
	if (!strcmp(scenario, "start-failure"))
		vm_exit_status = 1;
	advance();
	assert(starts == 1);
	if (!strcmp(scenario, "start-failure")) {
		assert(replies == 1 &&
		       reply.status == FPLINUX_UI_DOCUMENT_ERROR);
		assert(reply.request_id == 17 && reply.error == EIO);
		assert(!busy && !back_requests);
		puts("failed VM start reports error before acceptance");
		return 0;
	}
	assert(!replies);
	if (!strcmp(scenario, "early-exit") ||
	    !strcmp(scenario, "early-error")) {
		vm_exits = true;
		vm_exit_status = !strcmp(scenario, "early-error") ? 2 : 0;
		screen = fplinux_ui_current(ui);
		advance();
		assert(replies == 1 && reply.request_id == 17 && !busy &&
		       !tick);
		if (vm_exit_status) {
			assert(reply.status == FPLINUX_UI_DOCUMENT_ERROR &&
			       reply.error == EIO && !back_requests);
			screen = fplinux_ui_current(ui);
			assert(screen->alert &&
			       !strcmp(screen->title, "MIDlet stopped"));
			puts("MIDlet startup error reports document failure");
		} else {
			assert(reply.status == FPLINUX_UI_DOCUMENT_ACCEPTED &&
			       !reply.error && back_requests == 1);
			assert(fplinux_ui_current(ui) == screen);
			puts("successful MIDlet exit accepts document before the first screen");
		}
		return 0;
	}
	if (!strncmp(scenario, "replace-", 8) ||
	    !strncmp(scenario, "push-", 5)) {
		refused_navigation(ui);
		return 0;
	}
	/* The VM boundary presents a MIDlet screen over the production suite. */
	suite = stack[0];
	game = fplinux_ui_form_new(ui, "Running MIDlet");
	fplinux_ui_form_append_string(game, NULL, "Score: 42");
	fplinux_ui_replace(ui, game);
	game_depth = depth;
	if (!strcmp(scenario, "save-unpublished"))
		fail_file_sync = true;
	else if (!strcmp(scenario, "save-unsynced"))
		fail_directory_sync = true;
	fplinux_pm_app_loaded();
	assert(replies == 1 && reply.request_id == 17 &&
	       reply.status == FPLINUX_UI_DOCUMENT_ACCEPTED);
	if (fail_file_sync || fail_directory_sync) {
		screen = fplinux_ui_current(ui);
		assert(screen->alert);
		assert(!strcmp(screen->title, fail_file_sync ?
						      "Cannot save" :
						      "Saving not confirmed"));
		assert(!strcmp(screen->text, "Storage input/output error"));
		puts(fail_file_sync ?
			     "unpublished save reports failure" :
			     "published save reports unconfirmed durability");
		return 0;
	}
	if (!strcmp(scenario, "chooser-start"))
		assert(!strcmp(running_class, "example.Second"));
	if (!strncmp(scenario, "settings-save-", 14)) {
		struct fplinux_ui_event activation = {
			.type = FPLINUX_UI_EVENT_ACTIVATION,
			.activation = FPLINUX_UI_ACTIVATION_OPEN,
		};
		struct fplinux_ui_view_settings settings;
		struct fplinux_ui_prefs *disk;
		bool unpublished =
			!strcmp(scenario, "settings-save-unpublished");
		const char *stored;

		runtime.event(ui, &activation, runtime.context);
		open_global_settings();
		fail_file_sync = unpublished;
		fail_directory_sync = !unpublished;
		choose_setting(0, 2);
		assert(fplinux_ui_current(ui)->alert);
		assert(fplinux_ui_pop(ui));
		choose_setting(1, 1);
		assert(fplinux_ui_current(ui)->alert);
		assert(fplinux_ui_pop(ui));
		choose_setting(2, 3);
		screen = fplinux_ui_current(ui);
		assert(screen->alert &&
		       !strcmp(screen->title, unpublished ?
						      "Cannot save" :
						      "Saving not confirmed"));
		assert(!strcmp(screen->text, "Storage input/output error"));
		settings = fplinux_pm_app_view_settings();
		assert(settings.scale == FPLINUX_RGB565_INTEGER &&
		       settings.rotation == FPLINUX_RGB565_ROTATE_90);
		assert(fplinux_pm_app_canvas_size() == 0);
		disk = fplinux_ui_prefs_open_directory(
			getenv("FPLINUX_PM_TEST_ROOT"));
		assert(disk);
		stored = fplinux_ui_prefs_get(disk, "view-settings", NULL);
		if (unpublished)
			assert(!stored);
		else
			assert(stored &&
			       !strcmp(stored,
				       "{\"scale\":\"integer\",\"rotation\":\"90\"}"));
		fplinux_ui_prefs_close(disk);
		assert(fplinux_ui_pop(ui));
		screen = fplinux_ui_current(ui);
		assert(!strcmp(screen->status, "Unsaved"));
		assert(!strcmp(screen->details[0], "Integer") &&
		       !strcmp(screen->details[1], "90°") &&
		       !strcmp(screen->details[2], "176x208"));
		fail_file_sync = fail_directory_sync = false;
		choose_setting(2, 3);
		assert(!fplinux_ui_current(ui)->status[0]);
		disk = fplinux_ui_prefs_open_directory(
			getenv("FPLINUX_PM_TEST_ROOT"));
		assert(disk &&
		       fplinux_ui_prefs_get_int(disk, "canvas-size", 0) == 3);
		stored = fplinux_ui_prefs_get(disk, "view-settings", NULL);
		assert(stored &&
		       !strcmp(stored,
			       "{\"scale\":\"integer\",\"rotation\":\"90\"}"));
		fplinux_ui_prefs_close(disk);
		command(FPLINUX_UI_COMMAND_BACK);
		command(FPLINUX_UI_COMMAND_BACK);
		screen = fplinux_ui_current(ui);
		screen->focused = 2;
		screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND,
				screen->context);
		screen = fplinux_ui_current(ui);
		assert(!strcmp(screen->title, "Recent") &&
		       screen->row_count == 1);
		command(FPLINUX_UI_COMMAND_SCREEN);
		assert(!screen->row_count);
		command(FPLINUX_UI_COMMAND_BACK);
		open_global_settings();
		screen = fplinux_ui_current(ui);
		assert(!strcmp(screen->details[0], "Integer") &&
		       !strcmp(screen->details[1], "90°") &&
		       !strcmp(screen->details[2], "176x208"));
		puts("settings remain consistent after save warning and Recent save");
		return 0;
	}
	if (!strcmp(scenario, "recent-remove-last")) {
		struct fplinux_ui_event activation = {
			.type = FPLINUX_UI_EVENT_ACTIVATION,
			.activation = FPLINUX_UI_ACTIVATION_OPEN,
		};

		runtime.event(ui, &activation, runtime.context);
		screen = fplinux_ui_current(ui);
		assert(!strcmp(screen->rows[2], "Recent"));
		screen->focused = 2;
		screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND,
				screen->context);
		screen = fplinux_ui_current(ui);
		assert(!strcmp(screen->title, "Recent") &&
		       screen->row_count == 1);
		assert(has_remove(screen));
		command(FPLINUX_UI_COMMAND_SCREEN);
		assert(!screen->row_count && !has_remove(screen));
		assert(starts == 1 && !destroys);
		puts("removing last Recent entry removes its action");
		return 0;
	}
	if (!strcmp(scenario, "task-close")) {
		enum fplinux_ui_close_response response;

		ui->lifecycle = FPLINUX_UI_BACKGROUND;
		send_event(FPLINUX_UI_EVENT_LIFECYCLE);
		advance();
		response = ui->close ? ui->close(ui, ui->close_context) :
				       FPLINUX_UI_CLOSE_READY;
		assert(response == FPLINUX_UI_CLOSE_READY && !destroys);
		assert(depth == game_depth && fplinux_ui_current(ui) == game);
		puts("confirmed task close needs no second decision");
	} else if (!strcmp(scenario, "home-resume")) {
		ui->lifecycle = FPLINUX_UI_BACKGROUND;
		send_event(FPLINUX_UI_EVENT_LIFECYCLE);
		advance();
		assert(tick->paused && hide_events == 1);
		ui->lifecycle = FPLINUX_UI_FOREGROUND;
		send_event(FPLINUX_UI_EVENT_LIFECYCLE);
		assert(!tick->paused && show_events == 1 && starts == 1);
		puts("Home freezes VM; Resume continues same VM");
	} else if (!strcmp(scenario, "tile-continue") ||
		   !strcmp(scenario, "tile-icon") ||
		   !strcmp(scenario, "tile-resize") ||
		   !strcmp(scenario, "tile-picker-cancel")) {
		struct fplinux_ui_event activation = {
			.type = FPLINUX_UI_EVENT_ACTIVATION,
			.activation = FPLINUX_UI_ACTIVATION_OPEN,
		};

		runtime.event(ui, &activation, runtime.context);
		screen = fplinux_ui_current(ui);
		assert(!strcmp(screen->rows[0], "Continue"));
		assert(!strcmp(screen->rows[1], "Open JAR"));
		if (!strcmp(scenario, "tile-icon"))
			assert(screen->icons[0]);
		if (!strcmp(scenario, "tile-resize")) {
			fplinux_pm_app_set_resize_handler(
				resized, &resize_notifications);
			send_event(FPLINUX_UI_EVENT_RESIZE);
			assert(!resize_notifications);
		}
		advance();
		assert(tick->paused && hide_events == 1);
		if (!strcmp(scenario, "tile-picker-cancel")) {
			screen->focused = 1;
			screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND,
					screen->context);
			assert(picker_callback);
			picker_callback(
				(struct fplinux_ui_file_picker *)&runtime,
				FPLINUX_UI_FILE_PICKER_CANCELLED, NULL, 0,
				picker_context);
			assert(!destroys && starts == 1 &&
			       fplinux_ui_current(ui) == screen);
			screen->focused = 0;
		}
		screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND,
				screen->context);
		assert(fplinux_ui_current(ui) == game);
		assert(!tick->paused && show_events == 1 && starts == 1);
		if (!strcmp(scenario, "tile-resize")) {
			assert(resize_notifications == 1);
			fplinux_pm_app_set_resize_handler(NULL, NULL);
			puts("Continue resynchronizes the guest surface after home resize");
			return 0;
		}
		if (!strcmp(scenario, "tile-icon")) {
			puts("Continue shows the running MIDlet icon");
			return 0;
		}
		puts(!strcmp(scenario, "tile-picker-cancel") ?
			     "picker cancellation preserves VM; Continue resumes it" :
			     "tile opens home; Continue resumes same VM");
	} else if (!strncmp(scenario, "restart-", 8)) {
		result = document(18, getenv("FPLINUX_PM_TEST_SECOND_JAR"));
		if (!strncmp(scenario, "restart-missing-class", 21)) {
			assert(result.status == FPLINUX_UI_DOCUMENT_ERROR &&
			       result.error == ENOEXEC);
			assert(starts == 1 && !destroys && !tick->paused &&
			       fplinux_ui_current(ui) == game);
			puts("missing MIDlet class preserves current VM");
			return 0;
		}
		if (!strcmp(scenario, "restart-invalid")) {
			assert(result.status == FPLINUX_UI_DOCUMENT_ERROR);
			assert(starts == 1 && !destroys &&
			       !strcmp(running_jar, jar));
			puts("invalid replacement preserves current VM");
			return 0;
		}
		assert(result.status == FPLINUX_UI_DOCUMENT_WAIT);
		assert(tick->paused);
		assert(depth == game_depth + 1);
		if (!strcmp(scenario, "restart-cancel")) {
			command(FPLINUX_UI_COMMAND_CANCEL);
			assert(reply.request_id == 18 &&
			       reply.status == FPLINUX_UI_DOCUMENT_CANCELLED &&
			       !tick->paused);
			assert(starts == 1 && !destroys &&
			       !strcmp(running_jar, jar));
			assert(depth == game_depth &&
			       fplinux_ui_current(ui) == game);
			assert(stack[0] == suite &&
			       !strcmp(suite->title, "First"));
			assert(!strcmp(game->title, "Running MIDlet"));
			assert(!strcmp(game->text, "Score: 42"));
			puts("one close confirmation; Cancel preserves current VM");
			return 0;
		}
		command(FPLINUX_UI_COMMAND_OK);
		assert(reply.request_id == 18 &&
		       reply.status == FPLINUX_UI_DOCUMENT_RESTART_REQUIRED);
		puts("one close confirmation; ready for process restart");
	} else if (!strcmp(scenario, "picker-single")) {
		puts("picker routes JAR to the document handler; one VM started");
	} else {
		puts("first MIDlet screen accepts document; one VM started");
	}
	send_event(FPLINUX_UI_EVENT_CLOSE);
	assert(destroys == 1 && vm_state == FPLINUX_PM_VM_EXITED);
	return 0;
}
