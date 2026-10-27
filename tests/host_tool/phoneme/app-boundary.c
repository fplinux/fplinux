/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <kni.h>
#include <midpEvents.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-session.h>
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
	char rows[4][128];
	size_t row_count;
	size_t focused;
	struct fplinux_ui_command commands[4];
	size_t command_count;
	fplinux_ui_command_cb command;
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
static unsigned int starts, destroys, hide_events, show_events, replies;
static unsigned int busy_notifications, quit_requests;
static int navigation_error;
static enum fplinux_ui_close_response close_response;
static struct fplinux_ui_document_result reply;
static char running_jar[4096], running_class[256];
static const char *scenario;

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
	close_response = response;
	return true;
}

bool fplinux_ui_send(struct fplinux_ui *ui,
		     const struct fplinux_ui_session_message *message)
{
	(void)ui;
	assert(message->type == FPLINUX_UI_SESSION_BUSY);
	++busy_notifications;
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

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	(void)ui;
	if (depth < 2)
		return false;
	free(stack[--depth]);
	return true;
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

void fplinux_ui_screen_remove_command(struct fplinux_ui_screen *screen, int id)
{
	(void)screen;
	(void)id;
	abort();
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
	(void)change;
	screen->command = callback;
	screen->context = context;
}

struct fplinux_ui_screen *fplinux_ui_list_new(struct fplinux_ui *ui,
					      const char *title,
					      enum fplinux_ui_list_kind kind)
{
	(void)ui;
	assert(kind == FPLINUX_UI_LIST_IMPLICIT);
	return new_screen(title);
}

size_t fplinux_ui_list_append(struct fplinux_ui_screen *list, const char *text,
			      const char *detail)
{
	size_t index = list->row_count++;

	(void)detail;
	assert(index < 4);
	snprintf(list->rows[index], sizeof(list->rows[index]), "%s", text);
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
	(void)list;
	(void)index;
	(void)source;
	assert(kind == FPLINUX_UI_LIST_ICON_IMAGE);
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
	(void)list;
	(void)index;
	abort();
}

void fplinux_ui_list_delete_all(struct fplinux_ui_screen *list)
{
	(void)list;
	abort();
}

bool fplinux_ui_list_set(struct fplinux_ui_screen *list, size_t index,
			 const char *text, const char *detail)
{
	(void)list;
	(void)index;
	(void)text;
	(void)detail;
	abort();
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
	(void)list;
	(void)index;
	abort();
}

void fplinux_ui_list_set_selected(struct fplinux_ui_screen *list, size_t index,
				  bool selected)
{
	(void)list;
	(void)index;
	(void)selected;
	abort();
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
	if (vm_state == FPLINUX_PM_VM_DESTROYING) {
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

enum fplinux_pm_vm_state fplinux_pm_vm_get_state(void)
{
	return vm_state;
}
int fplinux_pm_vm_exit_code(void)
{
	return !strcmp(scenario, "start-failure") ? 1 : 0;
}

void fplinux_pm_keys_cancel(enum fplinux_ui_key_source source)
{
	assert(source == FPLINUX_UI_KEY_KEYBOARD);
	puts("input cancelled keyboard");
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

static void attach_peer(struct fplinux_ui_screen *screen, int serial)
{
	fplinux_pm_test_parameters[0] = (jint)(intptr_t)screen;
	fplinux_pm_test_parameters[1] = serial;
	fplinux_pm_test_parameters[2] = screen->alert;
	assert(Java_javax_microedition_lcdui_LFNative_attach0());
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
	size_t old_depth;

	assert(ui->document && ui->close);
	if (!scenario || !strcmp(scenario, "startup")) {
		assert(!strcmp(stack[0]->text, "Open a JAR file from Files."));
		puts("waiting for Files; no VM started");
		return 0;
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
	}
	advance();
	assert(!starts && !replies);
	advance();
	assert(starts == 1);
	if (!strcmp(scenario, "start-failure")) {
		assert(replies == 1 &&
		       reply.status == FPLINUX_UI_DOCUMENT_ERROR);
		puts("failed VM start reports error before acceptance");
		return 0;
	}
	assert(!replies);
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
	fplinux_pm_app_loaded();
	assert(replies == 1 && reply.request_id == 17 &&
	       reply.status == FPLINUX_UI_DOCUMENT_ACCEPTED);
	if (!strcmp(scenario, "chooser-start"))
		assert(!strcmp(running_class, "example.Second"));
	if (!strcmp(scenario, "home-resume")) {
		ui->lifecycle = FPLINUX_UI_BACKGROUND;
		send_event(FPLINUX_UI_EVENT_LIFECYCLE);
		advance();
		assert(tick->paused && hide_events == 1);
		ui->lifecycle = FPLINUX_UI_FOREGROUND;
		send_event(FPLINUX_UI_EVENT_LIFECYCLE);
		assert(!tick->paused && show_events == 1 && starts == 1);
		puts("Home freezes VM; Resume continues same VM");
	} else if (!strncmp(scenario, "restart-", 8)) {
		result = document(18, getenv("FPLINUX_PM_TEST_SECOND_JAR"));
		if (!strcmp(scenario, "restart-invalid")) {
			assert(result.status == FPLINUX_UI_DOCUMENT_ERROR);
			assert(starts == 1 && !destroys &&
			       !strcmp(running_jar, jar));
			puts("invalid replacement preserves current VM");
			return 0;
		}
		assert(result.status == FPLINUX_UI_DOCUMENT_RESTART_REQUIRED);
		assert(ui->close(ui, ui->close_context) ==
		       FPLINUX_UI_CLOSE_WAIT);
		assert(tick->paused);
		old_depth = depth;
		assert(ui->close(ui, ui->close_context) ==
		       FPLINUX_UI_CLOSE_WAIT);
		assert(depth == old_depth);
		if (!strcmp(scenario, "restart-cancel")) {
			command(FPLINUX_UI_COMMAND_CANCEL);
			assert(close_response == FPLINUX_UI_CLOSE_CANCEL &&
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
		assert(close_response == FPLINUX_UI_CLOSE_READY);
		puts("one close confirmation; ready for process restart");
	} else {
		puts("first MIDlet screen accepts document; one VM started");
	}
	send_event(FPLINUX_UI_EVENT_CLOSE);
	assert(destroys == 1 && vm_state == FPLINUX_PM_VM_EXITED);
	return 0;
}
