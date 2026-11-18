/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <fplinux/fplinux-ui.h>

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *application_context;
static lv_timer_t *child_timer;
static lv_timer_cb_t child_callback;

static void record_tick(char state)
{
	const char *path = getenv("FPLINUX_MRE_TICK_RECORD");
	FILE *output;

	if (!path)
		return;
	output = fopen(path, "a");
	assert(output);
	fprintf(output, "%c\n", state);
	assert(fclose(output) == 0);
}

struct fplinux_ui *__real_fplinux_ui_open(const char *id,
					  fplinux_ui_event_cb callback,
					  void *context);

struct fplinux_ui *__wrap_fplinux_ui_open(const char *id,
					  fplinux_ui_event_cb callback,
					  void *context)
{
	application_context = context;
	return __real_fplinux_ui_open(id, callback, context);
}

static void observed_child_tick(lv_timer_t *timer)
{
	record_tick('T');
	child_callback(timer);
}

lv_timer_t *__real_lv_timer_create(lv_timer_cb_t callback, uint32_t period,
				   void *context);

lv_timer_t *__wrap_lv_timer_create(lv_timer_cb_t callback, uint32_t period,
				   void *context)
{
	if (context != application_context || period != 25)
		return __real_lv_timer_create(callback, period, context);
	assert(!child_timer);
	child_callback = callback;
	child_timer =
		__real_lv_timer_create(observed_child_tick, period, context);
	return child_timer;
}

void __real_lv_timer_pause(lv_timer_t *timer);
void __real_lv_timer_resume(lv_timer_t *timer);

void __wrap_lv_timer_pause(lv_timer_t *timer)
{
	if (timer == child_timer && !lv_timer_get_paused(timer))
		record_tick('P');
	__real_lv_timer_pause(timer);
}

void __wrap_lv_timer_resume(lv_timer_t *timer)
{
	if (timer == child_timer && lv_timer_get_paused(timer))
		record_tick('R');
	__real_lv_timer_resume(timer);
}

/* Replace only the unavailable ARM execution boundary. The application, IPC,
 * sealed snapshot, broker, UI objects and Wayland presentation remain real.
 */
int __wrap_execl(const char *path, const char *first, ...)
{
	va_list arguments;
	char *child_arguments[4];
	const char *engine = getenv("FPLINUX_MRE_TEST_ENGINE");

	assert(!strcmp(path, "/usr/libexec/fplinux/mre-engine") && engine);
	va_start(arguments, first);
	child_arguments[0] = (char *)first;
	child_arguments[1] = va_arg(arguments, char *);
	child_arguments[2] = va_arg(arguments, char *);
	child_arguments[3] = va_arg(arguments, char *);
	va_end(arguments);
	assert(!child_arguments[3]);
	return execv(engine, child_arguments);
}
