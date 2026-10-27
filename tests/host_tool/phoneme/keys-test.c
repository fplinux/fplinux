/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <linux/input-event-codes.h>
#include <midpEvents.h>
#include <stdio.h>
#include <stdlib.h>

#include "fplinux-pm-canvas.h"
#include "fplinux-pm-vm.h"

struct lv_timer {
	lv_timer_cb_t callback;
	bool paused;
};

static lv_timer_t timer;
static MidpEvent events[8];
static size_t event_count;
int gForegroundDisplayId = 1;

struct fplinux_ui *fplinux_pm_canvas_ui(void)
{
	return (struct fplinux_ui *)&timer;
}

bool fplinux_pm_canvas_is_active(const struct fplinux_ui_screen *screen)
{
	return screen != NULL;
}

bool fplinux_ui_is_visible(const struct fplinux_ui *ui)
{
	(void)ui;
	return true;
}

enum fplinux_ui_lifecycle fplinux_ui_lifecycle(const struct fplinux_ui *ui)
{
	(void)ui;
	return FPLINUX_UI_FOREGROUND;
}

enum fplinux_pm_vm_state fplinux_pm_vm_get_state(void)
{
	return FPLINUX_PM_VM_RUNNING;
}

int GetEventQueueFreeCount(int isolate)
{
	assert(!isolate);
	return 32;
}

void StoreMIDPEventInVmThread(MidpEvent event, int isolate)
{
	assert(!isolate && event_count < 8);
	events[event_count++] = event;
}

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period,
			    void *context)
{
	(void)context;
	assert(period == 500);
	timer.callback = callback;
	return &timer;
}

void lv_timer_pause(lv_timer_t *clock)
{
	clock->paused = true;
}
void lv_timer_resume(lv_timer_t *clock)
{
	clock->paused = false;
}
void lv_timer_reset(lv_timer_t *clock)
{
	(void)clock;
}
void lv_timer_set_period(lv_timer_t *clock, uint32_t period)
{
	(void)clock;
	assert(period == 100 || period == 500);
}
void lv_timer_delete(lv_timer_t *clock)
{
	(void)clock;
}

int main(void)
{
	struct fplinux_ui_key phone = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = KEY_NUMERIC_2,
		.pressed = true,
	};
	struct fplinux_ui_key keyboard = {
		.source = FPLINUX_UI_KEY_KEYBOARD,
		.code = KEY_2,
		.unicode = '2',
		.pressed = true,
	};
	struct fplinux_ui_screen *canvas = (struct fplinux_ui_screen *)&timer;
	static const int actions[] = { 1, 1, 2, 3, 2 };

	fplinux_pm_canvas_key(NULL, canvas, &phone, NULL);
	fplinux_pm_canvas_key(NULL, canvas, &keyboard, NULL);
	/* A repeated physical press is not another MIDP_PRESSED. */
	fplinux_pm_canvas_key(NULL, canvas, &keyboard, NULL);
	fplinux_pm_keys_cancel(FPLINUX_UI_KEY_PHONE);
	assert(!timer.paused);
	phone.pressed = false;
	fplinux_pm_canvas_key(NULL, canvas, &phone, NULL);
	assert(event_count == 3);
	timer.callback(&timer);
	keyboard.pressed = false;
	fplinux_pm_canvas_key(NULL, canvas, &keyboard, NULL);
	assert(timer.paused && event_count == 5);
	for (size_t i = 0; i < 5; ++i) {
		assert(events[i].type == 1 && events[i].action == actions[i]);
		assert(events[i].character == '2' && events[i].display == 1);
	}
	fplinux_pm_keys_cleanup();
	puts("keys: seat cancellation preserves other held input");
	return 0;
}
