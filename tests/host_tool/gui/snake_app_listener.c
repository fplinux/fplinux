/* SPDX-License-Identifier: GPL-2.0-only */
/* Host UI, drawing, timer and in-memory preferences double for the app. */
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>

#include <assert.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lv_obj {
	lv_area_t area;
	char text[256];
	bool hidden;
};

struct lv_event {
	void *context;
	lv_layer_t layer;
};

struct lv_timer {
	lv_timer_cb_t callback;
	void *context;
	unsigned int period;
	bool paused;
	bool deleted;
};

struct fplinux_ui {
	fplinux_ui_event_cb callback;
	void *context;
	int width;
	int height;
	bool kept_on;
	bool closed;
	struct fplinux_ui_screen *current;
};

struct fplinux_ui_screen {
	struct fplinux_ui_screen *below;
	fplinux_ui_command_cb listener;
	fplinux_ui_screen_key_filter_cb filter;
	fplinux_ui_canvas_key_cb direction;
	void *context;
	int play;
	int back;
	char text[256];
	char title[64];
};

struct fplinux_ui_prefs {
	long long last;
	long long best;
	long long saved_last;
	long long saved_best;
	bool closed;
};

static struct fplinux_ui ui;
static struct fplinux_ui_prefs prefs = { 17, 20, 17, 20, false };
static struct fplinux_ui_metrics metrics;
static const lv_font_t font = { .line_height = 12 };
static const struct fplinux_ui_theme theme = {
	.font = &font,
	.foreground = 1,
	.focus = 2,
	.danger = 3,
};
static struct lv_obj objects[16];
static size_t object_count;
static struct lv_obj screen;
static struct fplinux_ui_screen canvas;
static struct lv_obj *board;
static lv_event_cb_t draw_callback;
static struct lv_event draw_event;
static struct lv_timer game_timer;
static lv_area_t head;
static unsigned int drawn_cells;
static char centre_key[32];
static const char *scenario;
static int expected_width;
static int expected_height;
static int expected_x;
static int expected_y;
static bool home_requested;
static bool back_requested;
static bool preferences_opened;

int fplinux_snake_app_main(int argc, char **argv);

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
	assert(parent);
	assert(object_count < 16);
	return &objects[object_count++];
}

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
	return lv_obj_create(parent);
}

void lv_obj_set_pos(lv_obj_t *object, int x, int y)
{
	object->area.x1 = x;
	object->area.y1 = y;
}

void lv_obj_set_size(lv_obj_t *object, int width, int height)
{
	object->area.x2 = object->area.x1 + width - 1;
	object->area.y2 = object->area.y1 + height - 1;
}

void lv_obj_set_width(lv_obj_t *object, int width)
{
	object->area.x2 = object->area.x1 + width - 1;
}

void lv_obj_set_height(lv_obj_t *object, int height)
{
	object->area.y2 = object->area.y1 + height - 1;
}

void lv_obj_align(lv_obj_t *object, int alignment, int x, int y)
{
	(void)alignment;
	lv_obj_set_pos(object, x, y);
}

void lv_obj_set_hidden(lv_obj_t *object, bool hidden)
{
	object->hidden = hidden;
}

void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback, int type,
			 void *context)
{
	assert(type == LV_EVENT_DRAW_MAIN);
	board = object;
	draw_callback = callback;
	draw_event.context = context;
}

void lv_obj_get_coords(lv_obj_t *object, lv_area_t *area)
{
	*area = object->area;
}

int lv_obj_get_width(const lv_obj_t *object)
{
	return object->area.x2 - object->area.x1 + 1;
}

int lv_obj_get_height(const lv_obj_t *object)
{
	return object->area.y2 - object->area.y1 + 1;
}

void lv_obj_update_layout(lv_obj_t *object)
{
	assert(object == &screen);
}

void lv_label_set_text(lv_obj_t *object, const char *text)
{
	assert(strlen(text) < sizeof(object->text));
	strcpy(object->text, text);
}

void *lv_event_get_user_data(lv_event_t *event)
{
	return event->context;
}

lv_layer_t *lv_event_get_layer(lv_event_t *event)
{
	return &event->layer;
}

void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *rectangle)
{
	memset(rectangle, 0, sizeof(*rectangle));
}

void lv_draw_rect(lv_layer_t *layer, const lv_draw_rect_dsc_t *rectangle,
		  const lv_area_t *area)
{
	(void)layer;
	assert(area->x1 >= board->area.x1 && area->y1 >= board->area.y1);
	assert(area->x2 <= board->area.x2 && area->y2 <= board->area.y2);
	assert(area->x1 <= area->x2 && area->y1 <= area->y2);
	if (rectangle->bg_color == 1)
		head = *area;
	drawn_cells++;
}

void lv_text_get_size(lv_point_t *size, const char *text,
		      const lv_font_t *text_font, int letter_space,
		      int line_space, int width, int flags)
{
	(void)letter_space;
	(void)line_space;
	(void)width;
	(void)flags;
	size->x = (int)strlen(text) * 6;
	size->y = text_font->line_height;
}

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, unsigned int period,
			    void *context)
{
	game_timer.callback = callback;
	game_timer.period = period;
	game_timer.context = context;
	return &game_timer;
}

void lv_timer_pause(lv_timer_t *timer)
{
	timer->paused = true;
}

void lv_timer_resume(lv_timer_t *timer)
{
	timer->paused = false;
}

void lv_timer_reset(lv_timer_t *timer)
{
	assert(timer == &game_timer);
}

void lv_timer_delete(lv_timer_t *timer)
{
	timer->deleted = true;
}

void *lv_timer_get_user_data(lv_timer_t *timer)
{
	return timer->context;
}

struct fplinux_ui *fplinux_ui_open(const char *id, fplinux_ui_event_cb callback,
				   void *context)
{
	assert(strcmp(id, "org.fplinux.snake") == 0);
	ui.callback = callback;
	ui.context = context;
	if (!strcmp(scenario, "close-during-open")) {
		const struct fplinux_ui_event close = {
			.type = FPLINUX_UI_EVENT_CLOSE,
		};

		prefs.saved_last = -1;
		prefs.saved_best = -1;
		callback(&ui, &close, context);
		assert(prefs.saved_last == 17 && prefs.saved_best == 20);
		assert(!object_count && !game_timer.callback && !ui.kept_on);
	}
	return &ui;
}

void fplinux_ui_close(struct fplinux_ui *instance)
{
	if (instance->current && instance->current != &canvas)
		free(instance->current);
	instance->closed = true;
}

lv_obj_t *fplinux_ui_screen(struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return &screen;
}

int fplinux_ui_width(const struct fplinux_ui *instance)
{
	return instance->width;
}

struct fplinux_ui_screen *fplinux_ui_canvas_new(struct fplinux_ui *instance,
						const char *title)
{
	assert(instance == &ui && !title);
	lv_obj_set_size(&screen, ui.width, ui.height);
	return &canvas;
}

lv_obj_t *fplinux_ui_canvas_object(struct fplinux_ui_screen *instance)
{
	assert(instance == &canvas);
	return &screen;
}

void fplinux_ui_canvas_set_key_handler(struct fplinux_ui_screen *instance,
				       fplinux_ui_canvas_key_cb handler,
				       void *context)
{
	assert(instance == &canvas);
	instance->direction = handler;
	instance->context = context;
}

void fplinux_ui_screen_set_key_filter(struct fplinux_ui_screen *instance,
				      fplinux_ui_screen_key_filter_cb filter,
				      void *context)
{
	instance->filter = filter;
	instance->context = context;
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *instance,
				    fplinux_ui_command_cb listener,
				    fplinux_ui_change_cb changed, void *context)
{
	assert(!changed);
	instance->listener = listener;
	instance->context = context;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *instance,
				   const struct fplinux_ui_command *command)
{
	if (command->type == FPLINUX_UI_COMMAND_OK)
		instance->play = command->id;
	else {
		assert(command->type == FPLINUX_UI_COMMAND_BACK);
		instance->back = command->id;
	}
	return true;
}

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *instance,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms)
{
	struct fplinux_ui_screen *alert = calloc(1, sizeof(*alert));

	assert(instance == &ui && kind == FPLINUX_UI_ALERT_INFO && !timeout_ms);
	assert(alert);
	snprintf(alert->title, sizeof(alert->title), "%s", title);
	snprintf(alert->text, sizeof(alert->text), "%s", text);
	return alert;
}

bool fplinux_ui_alert_set_text(struct fplinux_ui_screen *alert,
			       const char *text)
{
	snprintf(alert->text, sizeof(alert->text), "%s", text);
	return true;
}

void fplinux_ui_push(struct fplinux_ui *instance,
		     struct fplinux_ui_screen *pushed)
{
	pushed->below = instance->current;
	instance->current = pushed;
}

bool fplinux_ui_pop(struct fplinux_ui *instance)
{
	struct fplinux_ui_screen *popped = instance->current;

	assert(popped && popped != &canvas && popped->below == &canvas);
	instance->current = popped->below;
	free(popped);
	return true;
}

void fplinux_ui_go_back(struct fplinux_ui *instance)
{
	assert(instance->current == &canvas);
	fplinux_ui_back(instance);
}

int fplinux_ui_height(const struct fplinux_ui *instance)
{
	return instance->height;
}

const struct fplinux_ui_theme *
fplinux_ui_theme(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return &theme;
}

const struct fplinux_ui_metrics *
fplinux_ui_metrics(const struct fplinux_ui *instance)
{
	assert(instance == &ui);
	return &metrics;
}

uint64_t fplinux_ui_now_ms(void)
{
	return 1;
}

lv_obj_t *fplinux_ui_softkey_bar_create(struct fplinux_ui *instance,
					lv_obj_t *parent)
{
	assert(instance == &ui);
	return lv_obj_create(parent);
}

bool fplinux_ui_softkey_bar_set(struct fplinux_ui *instance, lv_obj_t *bar,
				const char *left, const char *centre,
				const char *right)
{
	assert(instance == &ui && bar);
	assert(strcmp(left, "") == 0 && strcmp(right, "Back") == 0);
	assert(strlen(centre) < sizeof(centre_key));
	strcpy(centre_key, centre);
	return true;
}

static void send_event(enum fplinux_ui_event_type type, bool visible,
		       unsigned int key, bool pressed)
{
	struct fplinux_ui_event event = {
		.type = type,
		.visible = visible,
		.key = { .code = key, .pressed = pressed },
	};

	ui.callback(&ui, &event, ui.context);
}

bool fplinux_ui_back(struct fplinux_ui *instance)
{
	assert(instance == &ui);
	back_requested = true;
	send_event(FPLINUX_UI_EVENT_CLOSE, false, 0, false);
	return true;
}

bool fplinux_ui_home(struct fplinux_ui *instance)
{
	assert(instance == &ui);
	home_requested = true;
	/* The manager hides a backgrounded surface without closing the app. */
	send_event(FPLINUX_UI_EVENT_VISIBILITY, false, 0, false);
	return true;
}

bool fplinux_ui_keep_screen_on(struct fplinux_ui *instance, bool enabled)
{
	instance->kept_on = enabled;
	return true;
}

bool fplinux_ui_is_softkey(const struct fplinux_ui_key *key, bool left)
{
	return key->source == FPLINUX_UI_KEY_PHONE &&
	       key->code == (left ? KEY_F13 : KEY_F14);
}

struct fplinux_ui_prefs *fplinux_ui_prefs_open(const char *id)
{
	assert(strcmp(id, "org.fplinux.snake") == 0);
	preferences_opened = true;
	return &prefs;
}

void fplinux_ui_prefs_close(struct fplinux_ui_prefs *instance)
{
	instance->closed = true;
}

long long fplinux_ui_prefs_get_int(const struct fplinux_ui_prefs *instance,
				   const char *key, long long fallback)
{
	(void)fallback;
	assert(instance == &prefs);
	return strcmp(key, "last_score") == 0 ? prefs.last : prefs.best;
}

bool fplinux_ui_prefs_set_int(struct fplinux_ui_prefs *instance,
			      const char *key, long long value)
{
	assert(instance == &prefs);
	if (strcmp(key, "last_score") == 0)
		prefs.last = value;
	else {
		assert(strcmp(key, "best") == 0);
		prefs.best = value;
	}
	return true;
}

struct fplinux_storage_result
fplinux_ui_prefs_save(struct fplinux_ui_prefs *instance)
{
	if (!strcmp(scenario, "save-failure")) {
		return (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_UNPUBLISHED, .error = ENOSPC
		};
	}
	instance->saved_last = instance->last;
	instance->saved_best = instance->best;
	return (struct fplinux_storage_result){
		.state = FPLINUX_STORAGE_DURABLE
	};
}

const char *fplinux_ui_storage_error(struct fplinux_storage_result result)
{
	assert(result.state == FPLINUX_STORAGE_UNPUBLISHED &&
	       result.error == ENOSPC);
	return "Not enough storage space";
}

static void draw(void)
{
	drawn_cells = 0;
	draw_callback(&draw_event);
}

static void pulse(void)
{
	if (!game_timer.paused)
		game_timer.callback(&game_timer);
	draw();
}

static void key(unsigned int code)
{
	struct fplinux_ui_screen *current = ui.current;
	struct fplinux_ui_key key = { .code = code, .pressed = true };
	int action;

	if (code == KEY_HOME) {
		fplinux_ui_home(&ui);
		return;
	}
	if (current->filter &&
	    current->filter(&ui, current, &key, current->context))
		return;
	if (code == KEY_OK || code == KEY_ENTER || code == KEY_KPENTER)
		action = current->play;
	else if (code == KEY_F14 || code == KEY_ESC)
		action = current->back;
	else {
		if (current->direction)
			current->direction(&ui, current, &key,
					   current->context);
		return;
	}
	/* Remove an Alert before its listener, as the screen API requires. */
	if (current != &canvas)
		ui.current = current->below;
	current->listener(&ui, current, action, current->context);
	if (current != &canvas)
		free(current);
}

static void test_listener(void)
{
	lv_area_t initial_head;
	lv_area_t paused_head;
	long long played_score;
	unsigned int paused_cells;

	assert(game_timer.paused && !ui.kept_on);
	assert(strcmp(centre_key, "Play") == 0);
	initial_head = head;
	send_event(FPLINUX_UI_EVENT_KEY, true, KEY_OK, false);
	assert(game_timer.paused);
	key(KEY_OK);
	assert(!game_timer.paused && ui.kept_on);
	assert(strcmp(centre_key, "Pause") == 0);
	pulse();
	assert(head.x1 > initial_head.x1 && head.y1 == initial_head.y1);
	/* Food may be on the first step; both placements are valid. */
	assert(drawn_cells == 4 || drawn_cells == 5);
	played_score = drawn_cells == 5 ? 1 : 0;
	/* A running tick does not persist a new score. */
	assert(prefs.saved_last == 17 && prefs.saved_best == 20);
	send_event(FPLINUX_UI_EVENT_VISIBILITY, false, 0, false);
	assert(game_timer.paused && !ui.kept_on);
	assert(prefs.saved_last == played_score && prefs.saved_best == 20);
	paused_head = head;
	paused_cells = drawn_cells;
	pulse();
	assert(head.x1 == paused_head.x1 && head.y1 == paused_head.y1);
	assert(drawn_cells == paused_cells);
	send_event(FPLINUX_UI_EVENT_VISIBILITY, true, 0, false);
	assert(game_timer.paused);
	key(KEY_ENTER);
	assert(!game_timer.paused);
	key(KEY_HOME);
	assert(home_requested && game_timer.paused && !ui.closed);
	send_event(FPLINUX_UI_EVENT_VISIBILITY, true, 0, false);
	assert(game_timer.paused);
	key(KEY_F14);
	assert(back_requested);
	assert(game_timer.paused && !ui.kept_on);
	assert(prefs.saved_last == played_score && prefs.saved_best == 20);
}

static void activate(enum fplinux_ui_activation activation)
{
	const struct fplinux_ui_event event = {
		.type = FPLINUX_UI_EVENT_ACTIVATION,
		.activation = activation,
	};

	ui.callback(&ui, &event, ui.context);
}

static void test_activation(void)
{
	lv_area_t running_head;
	unsigned int running_cells;
	struct fplinux_ui_screen *paused;

	key(KEY_OK);
	pulse();
	assert(!game_timer.paused && ui.kept_on);
	activate(FPLINUX_UI_ACTIVATION_RESUME);
	assert(!game_timer.paused && ui.current == &canvas);
	running_head = head;
	running_cells = drawn_cells;
	activate(FPLINUX_UI_ACTIVATION_OPEN);
	assert(game_timer.paused && !ui.kept_on);
	assert(ui.current != &canvas && !strcmp(ui.current->title, "Paused"));
	assert(!strcmp(centre_key, "Resume"));
	paused = ui.current;
	activate(FPLINUX_UI_ACTIVATION_OPEN);
	activate(FPLINUX_UI_ACTIVATION_RESUME);
	assert(ui.current == paused);
	pulse();
	assert(!memcmp(&head, &running_head, sizeof(head)));
	assert(drawn_cells == running_cells);
	assert(prefs.saved_last == 17 && prefs.saved_best == 20);
	key(KEY_OK);
	assert(!game_timer.paused);
	for (unsigned int index = 0; index < 40 && !game_timer.paused; ++index)
		pulse();
	assert(game_timer.paused && ui.current != &canvas);
	assert(!strcmp(ui.current->title, "Game over"));
	paused = ui.current;
	running_head = head;
	running_cells = drawn_cells;
	activate(FPLINUX_UI_ACTIVATION_OPEN);
	activate(FPLINUX_UI_ACTIVATION_RESUME);
	assert(ui.current == paused && !strcmp(ui.current->title, "Game over"));
	assert(!strcmp(centre_key, "Restart"));
	pulse();
	assert(!memcmp(&head, &running_head, sizeof(head)));
	assert(drawn_cells == running_cells);
}

int fplinux_ui_run(struct fplinux_ui *instance)
{
	if (!strcmp(scenario, "close-during-open")) {
		assert(instance == &ui && !board && !ui.current);
		assert(!game_timer.callback && !ui.kept_on);
		assert(prefs.saved_last == 17 && prefs.saved_best == 20);
		return 0;
	}
	assert(instance == &ui && board);
	assert(board->area.x1 == expected_x && board->area.y1 == expected_y);
	assert(board->area.x2 - board->area.x1 + 1 == expected_width);
	assert(board->area.y2 - board->area.y1 + 1 == expected_height);
	assert(board->area.x2 < ui.width);
	assert(board->area.y1 >= metrics.status_height);
	assert(board->area.y2 < ui.height - metrics.softkey_height);
	assert(game_timer.period == 125);
	draw();
	assert(drawn_cells == 4);
	if (strcmp(scenario, "listener") == 0)
		test_listener();
	else if (!strcmp(scenario, "activation"))
		test_activation();
	else if (strcmp(scenario, "close-playing") == 0) {
		key(KEY_OK);
		assert(!game_timer.paused && ui.kept_on);
		send_event(FPLINUX_UI_EVENT_CLOSE, false, 0, false);
		assert(game_timer.paused && !ui.kept_on);
	} else if (!strcmp(scenario, "save-failure")) {
		key(KEY_OK);
		assert(!game_timer.paused);
		key(KEY_HOME);
		assert(home_requested && game_timer.paused && !ui.closed);
		assert(prefs.saved_last == 17 && prefs.saved_best == 20);
		send_event(FPLINUX_UI_EVENT_VISIBILITY, true, 0, false);
		assert(ui.current != &canvas &&
		       strstr(ui.current->text, "Scores not saved"));
		key(KEY_OK);
		assert(!game_timer.paused && ui.kept_on);
		key(KEY_F14);
		assert(back_requested && game_timer.paused);
	} else {
		assert(strcmp(scenario, "geometry") == 0);
		send_event(FPLINUX_UI_EVENT_CLOSE, false, 0, false);
		assert(prefs.saved_last == 17 && prefs.saved_best == 20);
	}
	return 0;
}

int main(int argc, char **argv)
{
	char *application_argv[] = { "fplinux-snake", NULL };
	int result;

	if (argc > 1 && !strcmp(argv[1], "cli")) {
		result = fplinux_snake_app_main(argc - 1, argv + 1);
		assert(!preferences_opened && !ui.callback);
		return result;
	}
	assert(argc == 10);
	scenario = argv[1];
	ui.width = atoi(argv[2]);
	ui.height = atoi(argv[3]);
	metrics.status_height = atoi(argv[4]);
	metrics.softkey_height = atoi(argv[5]);
	metrics.spacing = 2;
	metrics.margin = 3;
	expected_x = atoi(argv[6]);
	expected_y = atoi(argv[7]);
	expected_width = atoi(argv[8]);
	expected_height = atoi(argv[9]);
	assert(fplinux_snake_app_main(1, application_argv) == 0);
	assert(ui.closed && prefs.closed);
	assert(game_timer.deleted ==
	       (strcmp(scenario, "close-during-open") != 0));
	puts("snake-app-listener: all checks passed");
	return 0;
}
