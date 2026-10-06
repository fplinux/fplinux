/* SPDX-License-Identifier: GPL-2.0-only */
/* Real event pump and signalfd dispatch; engine and device setup are fakes. */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"
#include "fplinux-quake-internal.h"
#include "test-quake-video.h"

#include <errno.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

static int colormap[4096];
static byte display_memory[240 * 320 * 4];
static void *renderer_memory;
static bool input_active;
static unsigned int accepted_input;

byte *host_colormap = (byte *)colormap;
short *d_pzbuffer;
byte *r_warpbuffer;
qvidmode_t vid_windowed_mode;
qvidmode_t *vid_modelist;
int vid_nummodes;
const qvidmode_t *vid_currentmode;
qboolean vsync_available;
qboolean adaptive_vsync_available;
void (*vid_menudrawfn)(void);
void (*vid_menukeyfn)(knum_t key);

/* Engine allocation and setup do not render or execute game logic. */
int Hunk_HighMark(void)
{
	return 0;
}

void *Hunk_HighAllocName(int size, const char *name)
{
	(void)name;
	renderer_memory = calloc(1, (size_t)size);
	return renderer_memory;
}

void Hunk_FreeToHighMark(int mark)
{
	(void)mark;
	free(renderer_memory);
}

int D_SurfaceCacheForRes(int width, int height)
{
	(void)width;
	(void)height;
	return 4096;
}

void D_InitCaches(void *buffer, int size)
{
	(void)buffer;
	(void)size;
}

void R_AllocSurfEdges(qboolean nostack)
{
	(void)nostack;
}

void Cvar_SetValue(const char *name, float value)
{
	(void)name;
	(void)value;
}

int LittleLong(int value)
{
	return value;
}

void VID_Mode_SetupViddef(const qvidmode_t *mode, viddef_t *definition)
{
	(void)mode;
	memset(definition, 0, sizeof(*definition));
}

void VID_MenuDraw(void)
{
}

void VID_MenuKey(knum_t key)
{
	(void)key;
}

void Con_Printf(const char *format, ...)
{
	(void)format;
}

_Noreturn void Sys_Quit(void)
{
	exit(2);
}

_Noreturn void Sys_Error(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	exit(2);
}

/* Only acquisition/close and kernel device operations are substituted. */
bool __wrap_fplinux_drm_session_open(struct fplinux_drm_session *session,
				     const char *drm, const char *tty,
				     uint32_t format, char *error, size_t size)
{
	(void)drm;
	(void)tty;
	(void)format;
	(void)error;
	(void)size;
	memset(session, 0, sizeof(*session));
	session->width = 240;
	session->height = 320;
	session->stride = 480;
	session->page_bytes = 153600;
	session->pages = 2;
	session->active = true;
	session->mapping = display_memory;
	return true;
}

bool __wrap_fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	(void)session;
	return true;
}

int __wrap_drmDropMaster(int descriptor)
{
	(void)descriptor;
	return 0;
}

int __wrap_drmSetMaster(int descriptor)
{
	(void)descriptor;
	return 0;
}

int __wrap_ioctl(int descriptor, unsigned long request, ...)
{
	(void)descriptor;
	if (request == VT_RELDISP)
		return 0;
	errno = ENOTTY;
	return -1;
}

static bool active_changed(struct fplinux_drm_session *session, bool active,
			   void *data)
{
	(void)session;
	(void)data;
	input_active = active;
	return true;
}

void IN_Commands(void)
{
	/* A game action may be consumed only while this input owner is active. */
	if (input_active)
		++accepted_input;
}

int main(void)
{
	struct fplinux_drm_session *display = fplinux_quake_display_session();
	byte palette[256 * 3] = { 0 };
	sigset_t signals, previous;
	bool released;
	bool acquired;

	VID_Init(palette);
	sigemptyset(&signals);
	sigaddset(&signals, SIGRTMIN + 4);
	sigaddset(&signals, SIGRTMIN + 5);
	if (sigprocmask(SIG_BLOCK, &signals, &previous) < 0)
		return 2;
	display->signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
	if (display->signal_fd < 0)
		return 2;
	display->drm = -1;
	display->tty = -1;
	if (!fplinux_drm_session_set_active_handler(display, active_changed,
						    NULL))
		return 2;
	if (raise(SIGRTMIN + 4))
		return 2;
	/* A modal dialog pumps events without publishing another frame. */
	Sys_SendKeyEvents();
	released = !display->active && !input_active && accepted_input == 0;
	printf("release: display_active=%d input_active=%d accepted_input=%u\n",
	       display->active, input_active, accepted_input);
	if (raise(SIGRTMIN + 5))
		return 2;
	Sys_SendKeyEvents();
	acquired = display->active && input_active && accepted_input == 1;
	printf("acquire: display_active=%d input_active=%d accepted_input=%u\n",
	       display->active, input_active, accepted_input);
	/* Drain pending signals even on a failed expectation before unblocking. */
	fplinux_drm_session_dispatch(display);
	close(display->signal_fd);
	sigprocmask(SIG_SETMASK, &previous, NULL);
	VID_Shutdown();
	return released && acquired ? 0 : 1;
}
