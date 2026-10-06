/* SPDX-License-Identifier: GPL-2.0-only */
/* Observe the production adapter through fake engine and session boundaries. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fplinux-drm-session.h"
#include "fplinux-input-session.h"
#include "test-quake.h"

struct test_client cl;
struct test_button in_strafe;
cvar_t sensitivity, m_side, m_forward, m_yaw, m_pitch;
cvar_t cl_maxpitch, cl_minpitch;

static struct fplinux_input_event queue[64];
static size_t queue_head, queue_tail;
static bool down[K_LAST];
static unsigned int key_events;
static bool session_open, session_suspended;
static struct fplinux_drm_session display = { .active = true };

static void event(enum fplinux_input_event_type type, uint64_t device,
		  enum fplinux_input_source source, unsigned int code,
		  bool pressed)
{
	assert(queue_tail < sizeof(queue) / sizeof(queue[0]));
	queue[queue_tail++] = (struct fplinux_input_event){
		.type = type,
		.device_id = device,
		.source = source,
		.code = code,
		.pressed = pressed,
		.name = "test input",
	};
}

static void key(uint64_t device, enum fplinux_input_source source,
		unsigned int code, bool pressed)
{
	event(FPLINUX_INPUT_EVENT_KEY, device, source, code, pressed);
	IN_Commands();
}

bool fplinux_input_session_open(struct fplinux_input_session *session,
				unsigned int sources, char *error,
				size_t error_size)
{
	(void)session;
	(void)sources;
	(void)error;
	(void)error_size;
	session_open = true;
	return true;
}

bool fplinux_input_session_next(struct fplinux_input_session *session,
				struct fplinux_input_event *next)
{
	(void)session;
	if (queue_head == queue_tail) {
		queue_head = 0;
		queue_tail = 0;
		return false;
	}
	*next = queue[queue_head++];
	return true;
}

void fplinux_input_session_suspend(struct fplinux_input_session *session)
{
	(void)session;
	session_suspended = true;
}

bool fplinux_input_session_resume(struct fplinux_input_session *session,
				  char *error, size_t error_size)
{
	(void)session;
	(void)error;
	(void)error_size;
	session_suspended = false;
	return true;
}

void fplinux_input_session_close(struct fplinux_input_session *session)
{
	(void)session;
	session_open = false;
}

struct fplinux_drm_session *fplinux_quake_display_session(void)
{
	return &display;
}

bool fplinux_drm_session_set_active_handler(struct fplinux_drm_session *session,
					    fplinux_drm_active_handler handler,
					    void *data)
{
	return !handler || handler(session, session->active, data);
}

void Key_Event(knum_t code, qboolean pressed)
{
	down[code] = pressed;
	++key_events;
}

void Key_ClearAllStates(void)
{
	memset(down, 0, sizeof(down));
}

void Con_Printf(const char *format, ...)
{
	(void)format;
}

_Noreturn void Sys_Error(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	abort();
}

void Cvar_RegisterVariable(cvar_t *variable)
{
	(void)variable;
}

void V_StopPitchDrift(void)
{
}

int main(void)
{
	unsigned int before;

	IN_Init();
	assert(session_open && IN_HaveFocus());
	event(FPLINUX_INPUT_EVENT_DEVICE_ADDED, 1, FPLINUX_INPUT_SOURCE_KEYPAD,
	      0, false);
	event(FPLINUX_INPUT_EVENT_DEVICE_ADDED, 2,
	      FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false);
	IN_Commands();
	/* The same Linux arrow has the phone's rotated action only on a keypad. */
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 103, true);
	key(2, FPLINUX_INPUT_SOURCE_KEYBOARD, 103, true);
	assert(down[K_LEFTARROW] && down[K_UPARROW]);
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 103, false);
	assert(!down[K_LEFTARROW] && down[K_UPARROW]);
	key(2, FPLINUX_INPUT_SOURCE_KEYBOARD, 103, false);
	/* F13 is a softkey only on the phone. */
	before = key_events;
	key(2, FPLINUX_INPUT_SOURCE_KEYBOARD, 183, true);
	key(2, FPLINUX_INPUT_SOURCE_KEYBOARD, 183, false);
	assert(key_events == before);
	/* A shared action remains down until its last physical key releases it. */
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 183, true);
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 522, true);
	key(2, FPLINUX_INPUT_SOURCE_KEYBOARD, 57, true);
	assert(down[K_SPACE] && key_events == before + 1U);
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 183, false);
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 522, false);
	assert(down[K_SPACE]);
	key(2, FPLINUX_INPUT_SOURCE_KEYBOARD, 57, false);
	assert(!down[K_SPACE] && key_events == before + 2U);
	/* Releases left over after a focus reset cannot cancel a new owner. */
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 183, true);
	IN_ClearStates();
	key(2, FPLINUX_INPUT_SOURCE_KEYBOARD, 57, true);
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 183, false);
	assert(down[K_SPACE]);
	/* The input release is processed before the hotplug removal marker. */
	key(2, FPLINUX_INPUT_SOURCE_KEYBOARD, 57, false);
	event(FPLINUX_INPUT_EVENT_DEVICE_REMOVED, 2,
	      FPLINUX_INPUT_SOURCE_KEYBOARD, 0, false);
	IN_Commands();
	assert(!down[K_SPACE]);
	key(1, FPLINUX_INPUT_SOURCE_KEYPAD, 183, true);
	before = key_events;
	event(FPLINUX_INPUT_EVENT_KEY, 1, FPLINUX_INPUT_SOURCE_KEYPAD, 103,
	      true);
	IN_SetFocus(false);
	assert(session_suspended && !IN_HaveFocus() && !down[K_SPACE]);
	assert(key_events == before);
	IN_SetFocus(true);
	assert(!session_suspended && IN_HaveFocus());
	IN_Shutdown();
	assert(!session_open);
	return 0;
}
