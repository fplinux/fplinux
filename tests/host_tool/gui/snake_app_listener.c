/* SPDX-License-Identifier: GPL-2.0-only */
/* Host UI, drawing, timer and in-memory preferences double for the app. */
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-prefs.h>

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
	send_event(FPLINUX_UI_EVENT_KEY, true, code, true);
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

int fplinux_ui_run(struct fplinux_ui *instance)
{
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
	else if (strcmp(scenario, "close-during-open") == 0) {
		assert(game_timer.paused && !ui.kept_on);
		assert(prefs.saved_last == 17 && prefs.saved_best == 20);
	} else if (strcmp(scenario, "close-playing") == 0) {
		key(KEY_OK);
		assert(!game_timer.paused && ui.kept_on);
		send_event(FPLINUX_UI_EVENT_CLOSE, false, 0, false);
		assert(game_timer.paused && !ui.kept_on);
	} else if (!strcmp(scenario, "save-failure")) {
		bool notice = false;

		key(KEY_OK);
		assert(!game_timer.paused);
		key(KEY_HOME);
		assert(home_requested && game_timer.paused && !ui.closed);
		assert(prefs.saved_last == 17 && prefs.saved_best == 20);
		send_event(FPLINUX_UI_EVENT_VISIBILITY, true, 0, false);
		for (size_t index = 0; index < object_count; ++index)
			if (!objects[index].hidden &&
			    strstr(objects[index].text, "Scores not saved"))
				notice = true;
		assert(notice);
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
	assert(ui.closed && prefs.closed && game_timer.deleted);
	puts("snake-app-listener: all checks passed");
	return 0;
}
