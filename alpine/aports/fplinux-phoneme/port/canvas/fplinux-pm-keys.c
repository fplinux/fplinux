// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme Canvas keys: the key handler of every Canvas peer turns
 * runtime keys into MIDP key events (press, release, and repeats from a
 * timer, because the session does not repeat keys). The runtime gives a
 * Canvas peer only the keys its commands leave: the soft keys arrive here
 * when the Canvas has no command for them, always in full screen, where its
 * LF adds no commands; OK always arrives, as the LF never adds OK commands.
 * Events are queued from the runtime callback,
 * outside a VM slice, and wake the VM.
 */
#include <stddef.h>
#include <string.h>

#include <kni.h>
#include <midpEvents.h>

#include "fplinux-pm-canvas-core.h"
#include "fplinux-pm-canvas.h"
#include "fplinux-pm-vm.h"

/* com.sun.midp.lcdui.EventConstants.REPEATED; midpEvents.h lacks it. */
#define KEY_REPEATED 3
/* Nokia-like: repeats start after half a second, then ten a second. */
#define REPEAT_DELAY_MS 500u
#define REPEAT_PERIOD_MS 100u
/* Repeats wait while the VM has not taken earlier events yet. */
#define REPEAT_QUEUE_RESERVE 8
#define HELD_MAX 8

extern int gForegroundDisplayId;

/* A key pressed on a Canvas, so its release sends the same MIDP code. */
struct held_key {
	bool keyboard;
	unsigned int code;
	int midp;
};

static struct {
	struct held_key held[HELD_MAX];
	size_t held_next;
	lv_timer_t *timer;
	const struct fplinux_ui_screen *screen;
	int midp;
	bool keyboard;
	unsigned int code;
} keys;

/* The event queue exists only while the VM runs (a Canvas may outlive it). */
static bool vm_takes_events(void)
{
	enum fplinux_pm_vm_state state = fplinux_pm_vm_get_state();

	return state == FPLINUX_PM_VM_RUNNING ||
	       state == FPLINUX_PM_VM_DESTROYING;
}

static void post_key(int action, int midp)
{
	MidpEvent event;

	if (!vm_takes_events())
		return;
	MIDP_EVENT_INITIALIZE(event);
	event.type = MIDP_KEY_EVENT;
	event.ACTION = action;
	event.CHR = midp;
	event.DISPLAY = gForegroundDisplayId;
	StoreMIDPEventInVmThread(event, 0);
}

void fplinux_pm_vm_post_key(int key_code, int action)
{
	if (key_code && action >= FPLINUX_PM_VM_KEY_PRESSED &&
	    action <= FPLINUX_PM_VM_KEY_REPEATED)
		post_key(action, key_code);
}

void fplinux_pm_keys_stop(void)
{
	keys.screen = NULL;
	if (keys.timer)
		lv_timer_pause(keys.timer);
}

void fplinux_pm_keys_cleanup(void)
{
	fplinux_pm_keys_stop();
	if (keys.timer)
		lv_timer_delete(keys.timer);
	keys.timer = NULL;
	memset(keys.held, 0, sizeof(keys.held));
}

void fplinux_pm_keys_cancel(enum fplinux_ui_key_source source)
{
	bool keyboard = source == FPLINUX_UI_KEY_KEYBOARD;

	for (size_t i = 0; i < HELD_MAX; ++i) {
		struct held_key *held = &keys.held[i];

		if (!held->midp || held->keyboard != keyboard)
			continue;
		if (keys.screen && keys.keyboard == keyboard &&
		    keys.code == held->code)
			fplinux_pm_keys_stop();
		post_key(MIDP_RELEASED, held->midp);
		held->midp = 0;
	}
}

static void repeat(lv_timer_t *timer)
{
	struct fplinux_ui *ui = fplinux_pm_canvas_ui();

	/* Hidden, or the Canvas went away: its release may never come. */
	if (!ui || !fplinux_ui_is_visible(ui) ||
	    fplinux_ui_lifecycle(ui) != FPLINUX_UI_FOREGROUND ||
	    !fplinux_pm_canvas_is_active(keys.screen)) {
		fplinux_pm_keys_stop();
		return;
	}
	lv_timer_set_period(timer, REPEAT_PERIOD_MS);
	if (GetEventQueueFreeCount(0) > REPEAT_QUEUE_RESERVE)
		post_key(KEY_REPEATED, keys.midp);
}

static void start_repeat(const struct fplinux_ui_screen *screen,
			 const struct held_key *held)
{
	if (!keys.timer) {
		keys.timer = lv_timer_create(repeat, REPEAT_DELAY_MS, NULL);
		if (!keys.timer)
			return;
	}
	keys.screen = screen;
	keys.midp = held->midp;
	keys.keyboard = held->keyboard;
	keys.code = held->code;
	lv_timer_set_period(keys.timer, REPEAT_DELAY_MS);
	lv_timer_reset(keys.timer);
	lv_timer_resume(keys.timer);
}

static struct held_key *find_held(bool keyboard, unsigned int code)
{
	for (size_t i = 0; i < HELD_MAX; ++i)
		if (keys.held[i].midp && keys.held[i].keyboard == keyboard &&
		    keys.held[i].code == code)
			return &keys.held[i];
	return NULL;
}

static void press(const struct fplinux_ui_screen *screen,
		  const struct fplinux_ui_key *key, bool keyboard)
{
	struct held_key *held = find_held(keyboard, key->code);
	int midp = fplinux_pm_key_code(keyboard, key->code, key->unicode);

	if (!midp || held)
		return;
	for (size_t i = 0; !held && i < HELD_MAX; ++i)
		if (!keys.held[i].midp)
			held = &keys.held[i];
	if (!held) {
		/* More keys held than slots: the oldest slot gives way. */
		held = &keys.held[keys.held_next];
		keys.held_next = (keys.held_next + 1) % HELD_MAX;
	}
	*held = (struct held_key){ keyboard, key->code, midp };
	post_key(MIDP_PRESSED, midp);
	start_repeat(screen, held);
}

static void release(const struct fplinux_ui_key *key, bool keyboard)
{
	struct held_key *held = find_held(keyboard, key->code);
	int midp = held ? held->midp : 0;

	if (held)
		held->midp = 0;
	if (!midp)
		return;
	if (keys.screen && keys.keyboard == keyboard && keys.code == key->code)
		fplinux_pm_keys_stop();
	post_key(MIDP_RELEASED, midp);
}

void fplinux_pm_canvas_key(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *canvas,
			   const struct fplinux_ui_key *key, void *context)
{
	bool keyboard = key->source == FPLINUX_UI_KEY_KEYBOARD;

	(void)ui;
	(void)context;
	if (key->pressed)
		press(canvas, key, keyboard);
	else
		release(key, keyboard);
}
