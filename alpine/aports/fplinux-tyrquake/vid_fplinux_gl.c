// SPDX-License-Identifier: GPL-2.0-or-later
/* Native OpenGL rendering with GPU-backed RGB565 DRM presentation. */
/* fplinux-check: package-embedded */

#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"
#include "fplinux-gpu.h"
#include "fplinux-quake-gl.h"
#include "fplinux-quake-internal.h"
#include "fplinux-quake-stats.h"

#include <GL/glext.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#undef K_SCROLLLOCK
#undef K_NUMLOCK
#undef K_CAPSLOCK

#include "common.h"
#include "console.h"
#include "glquake.h"
#include "input.h"
#include "quakedef.h"
#include "screen.h"
#include "sys.h"
#include "vid.h"

#ifdef NQ_HACK
#include "host.h"
#endif

#define FPLINUX_QUAKE_VIDEO_BITS_PER_PIXEL 16

viddef_t vid;
float gldepthmin, gldepthmax;
void (*VID_SetGammaRamp)(unsigned short ramp[3][256]);

static struct fplinux_drm_session display;
static struct fplinux_gpu_output *output;
static struct fplinux_gpu *gpu;
static GLuint transfer_texture;
static unsigned int output_width;
static unsigned int output_height;
static unsigned int render_width;
static unsigned int render_height;
static unsigned int render_scale;
static qboolean framebuffer_rotated;
static qboolean display_opened;
static qboolean gl_initialized;
static int video_exit_status;
static volatile sig_atomic_t exit_signal;
static const int handled_signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
static struct sigaction previous_actions[4];
static size_t installed_signals;
static qvidmode_t fixed_mode;

static struct {
	PFNGLDELETEQUERIESPROC delete_queries;
	PFNGLBEGINQUERYPROC begin_query;
	PFNGLENDQUERYPROC end_query;
	PFNGLGETQUERYOBJECTUI64VPROC get_result;
	GLuint query;
	qboolean checked;
	qboolean active;
	qboolean pending;
} gpu_timer;

static void check_render_error(void)
{
	GLenum gl_error = glGetError();

	if (gl_error != GL_NO_ERROR)
		Sys_Error("FPLinux GPU: rendering failed: 0x%x", gl_error);
}

static qboolean timer_call_ok(void)
{
	qboolean ok = true;

	while (glGetError() != GL_NO_ERROR)
		ok = false;
	return ok;
}

static void discard_gpu_timer(void)
{
	if (gpu_timer.query)
		gpu_timer.delete_queries(1, &gpu_timer.query);
	gpu_timer.query = 0;
	gpu_timer.active = false;
	gpu_timer.pending = false;
	timer_call_ok();
}

static void open_gpu_timer(void)
{
	PFNGLGENQUERIESPROC gen_queries;
	PFNGLGETQUERYIVPROC get_query;
	const char *extensions;
	GLint counter_bits = 0;

	gpu_timer.checked = true;
	extensions = (const char *)glGetString(GL_EXTENSIONS);
	if (!timer_call_ok() || !extensions ||
	    !GL_ExtensionCheck_String("GL_ARB_timer_query", extensions))
		return;
	gen_queries =
		(PFNGLGENQUERIESPROC)fplinux_gpu_get_proc(gpu, "glGenQueries");
	get_query =
		(PFNGLGETQUERYIVPROC)fplinux_gpu_get_proc(gpu, "glGetQueryiv");
	gpu_timer.delete_queries = (PFNGLDELETEQUERIESPROC)fplinux_gpu_get_proc(
		gpu, "glDeleteQueries");
	gpu_timer.begin_query =
		(PFNGLBEGINQUERYPROC)fplinux_gpu_get_proc(gpu, "glBeginQuery");
	gpu_timer.end_query =
		(PFNGLENDQUERYPROC)fplinux_gpu_get_proc(gpu, "glEndQuery");
	gpu_timer.get_result =
		(PFNGLGETQUERYOBJECTUI64VPROC)fplinux_gpu_get_proc(
			gpu, "glGetQueryObjectui64v");
	if (!gen_queries || !get_query || !gpu_timer.delete_queries ||
	    !gpu_timer.begin_query || !gpu_timer.end_query ||
	    !gpu_timer.get_result)
		return;
	get_query(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &counter_bits);
	if (!timer_call_ok() || counter_bits <= 0)
		return;
	gen_queries(1, &gpu_timer.query);
	if (!timer_call_ok())
		discard_gpu_timer();
}

static void end_gpu_timer(void)
{
	if (!gpu_timer.active)
		return;
	gpu_timer.end_query(GL_TIME_ELAPSED);
	gpu_timer.active = false;
	gpu_timer.pending = true;
}

static void close_gpu_timer(void)
{
	end_gpu_timer();
	if (gpu_timer.query)
		gpu_timer.delete_queries(1, &gpu_timer.query);
	memset(&gpu_timer, 0, sizeof(gpu_timer));
}

static void sample_gpu_timer(void)
{
	GLuint64 elapsed_ns;

	if (!gpu_timer.pending)
		return;
	/* The output's completed fence also covers this scene query. */
	gpu_timer.get_result(gpu_timer.query, GL_QUERY_RESULT, &elapsed_ns);
	if (!timer_call_ok()) {
		discard_gpu_timer();
		return;
	}
	gpu_timer.pending = false;
	fplinux_quake_stats_gpu_sample(elapsed_ns);
}

static void request_clean_exit(int signal_number)
{
	exit_signal = signal_number;
}

static qboolean install_signal_handlers(char *error, size_t size)
{
	struct sigaction action;
	size_t index;

	memset(&action, 0, sizeof(action));
	action.sa_handler = request_clean_exit;
	sigemptyset(&action.sa_mask);
	for (index = 0;
	     index < sizeof(handled_signals) / sizeof(handled_signals[0]);
	     ++index) {
		if (sigaction(handled_signals[index], &action,
			      &previous_actions[index]) < 0) {
			snprintf(error, size, "sigaction: %s", strerror(errno));
			return false;
		}
		++installed_signals;
	}
	return true;
}

static void restore_signal_handlers(void)
{
	while (installed_signals) {
		size_t index = --installed_signals;

		sigaction(handled_signals[index], &previous_actions[index],
			  NULL);
	}
	exit_signal = 0;
}

struct fplinux_drm_session *fplinux_quake_display_session(void)
{
	return &display;
}

static qboolean close_video(void)
{
	qboolean restored = true;
	int saved_errno = 0;

	close_gpu_timer();
	if (gl_initialized) {
		GL_Shutdown();
		gl_initialized = false;
	}
	if (transfer_texture) {
		glDeleteTextures(1, &transfer_texture);
		transfer_texture = 0;
	}
	if (!fplinux_gpu_output_close(output)) {
		restored = false;
		saved_errno = errno;
	}
	output = NULL;
	gpu = NULL;
	if (display_opened) {
		if (!fplinux_drm_session_close(&display)) {
			restored = false;
			if (!saved_errno)
				saved_errno = errno;
		}
		display_opened = false;
	}
	restore_signal_handlers();
	if (!restored)
		errno = saved_errno;
	return restored;
}

static void fail_initialization(const char *error)
{
	qboolean fallback = COM_CheckParm(FPLINUX_QUAKE_FALLBACK_ARGUMENT) != 0;
	int signal_number = exit_signal;
	char **arguments;
	unsigned int index;
	unsigned int count;
	int saved_errno;

	/* Input has not been initialized yet; release the borrowed display first. */
	if (!close_video())
		Sys_Error("FPLinux GPU: cannot restore display: %s",
			  strerror(errno));
	if (signal_number)
		Sys_Quit();
	if (!fallback)
		Sys_Error("FPLinux GPU: %s", error);

	arguments = calloc((size_t)com_argc + 1, sizeof(*arguments));
	if (!arguments)
		Sys_Error("FPLinux GPU: cannot prepare software fallback");
	arguments[0] = (char *)FPLINUX_QUAKE_SOFTWARE_ENGINE;
	count = 1;
	for (index = 1; index < com_argc; ++index)
		if (strcmp(com_argv[index], FPLINUX_QUAKE_FALLBACK_ARGUMENT))
			arguments[count++] = (char *)com_argv[index];

	fprintf(stderr,
		"quake: GPU initialization failed: %s; "
		"using software renderer\n",
		error);
	fflush(NULL);
	execv(FPLINUX_QUAKE_SOFTWARE_ENGINE, arguments);
	saved_errno = errno;
	free(arguments);
	Sys_Error("FPLinux GPU: cannot execute software renderer: %s",
		  strerror(saved_errno));
}

static void choose_render_geometry(void)
{
	framebuffer_rotated = display.height > display.width;
	output_width = framebuffer_rotated ? display.height : display.width;
	output_height = framebuffer_rotated ? display.width : display.height;
	render_scale = 1;
	while (output_width * render_scale < MINWIDTH ||
	       output_height * render_scale < MINHEIGHT ||
	       (output_width * render_scale) % 8 != 0)
		++render_scale;
	render_width = output_width * render_scale;
	render_height = output_height * render_scale;
}

static qboolean open_video(char *error, size_t size)
{
	GLenum gl_error;
	GLint texture;

	if (!install_signal_handlers(error, size))
		return false;
	if (!fplinux_drm_session_open(&display, NULL, NULL, DRM_FORMAT_RGB565,
				      error, size))
		return false;
	display_opened = true;
	choose_render_geometry();
	output = fplinux_gpu_output_open(&display, FPLINUX_GPU_OPENGL2,
					 render_width, render_height,
					 framebuffer_rotated, error, size);
	if (!output)
		return false;
	gpu = fplinux_gpu_output_gpu(output);
	if (!fplinux_quake_gl_load(gpu, error, size))
		return false;

	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glGenTextures(1, &transfer_texture);
	glBindTexture(GL_TEXTURE_2D, transfer_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, render_width, render_height, 0,
		     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, texture);
	glDrawBuffer(GL_FRONT);
	glReadBuffer(GL_FRONT);
	gl_error = glGetError();
	if (gl_error != GL_NO_ERROR) {
		snprintf(error, size, "OpenGL pbuffer setup failed: 0x%x",
			 gl_error);
		return false;
	}
	return true;
}

void *GL_GetProcAddress(const char *name)
{
	return fplinux_gpu_get_proc(gpu, name);
}

void VID_GetDesktopRect(vrect_t *rect)
{
	rect->x = rect->y = 0;
	rect->width = render_width;
	rect->height = render_height;
}

void VID_SetPalette(const byte *palette)
{
	QPic32_InitPalettes(palette);
}

void VID_ShiftPalette(const byte *palette)
{
	/* The OpenGL renderer draws color shifts with its scene blend. */
	(void)palette;
}

void VID_InitColormap(const byte *palette)
{
	(void)palette;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
}

void VID_Init(const byte *palette)
{
	char error[192];
	GLenum gl_error;

	if (!open_video(error, sizeof(error)))
		fail_initialization(error);

	fixed_mode.width = render_width;
	fixed_mode.height = render_height;
	fixed_mode.bpp = FPLINUX_QUAKE_VIDEO_BITS_PER_PIXEL;
	fixed_mode.refresh = 0;
	fixed_mode.min_scale = 1;
	fixed_mode.resolution.scale = 1;
	fixed_mode.resolution.width = render_width;
	fixed_mode.resolution.height = render_height;
	vid_modelist = &fixed_mode;
	vid_nummodes = 1;
	vid_windowed_mode = fixed_mode;
	vid_currentmode = &fixed_mode;
	VID_Mode_SetupViddef(&fixed_mode, &vid);
	vid.aspect = 1.0;
	vid.numpages = 0;
	vid.recalc_refdef = 1;
	VID_InitColormap(palette);
	VID_SetPalette(palette);
	GL_Init();
	gl_initialized = true;
	gl_error = glGetError();
	if (gl_error != GL_NO_ERROR) {
		snprintf(error, sizeof(error),
			 "OpenGL initialization failed: 0x%x", gl_error);
		fail_initialization(error);
	}

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;
	vsync_available = false;
	adaptive_vsync_available = false;
	Cvar_SetValue("vid_fullscreen", 1);
	Cvar_SetValue("vid_width", render_width);
	Cvar_SetValue("vid_height", render_height);
	Cvar_SetValue("vid_bpp", FPLINUX_QUAKE_VIDEO_BITS_PER_PIXEL);
	Cvar_SetValue("vid_refreshrate", fixed_mode.refresh);

	Con_Printf("Quake renderer: gpu; %s; OpenGL %s; %s\n",
		   fplinux_gpu_renderer(gpu), fplinux_gpu_version(gpu),
		   fplinux_gpu_device(gpu));
	Con_Printf("FPLinux video: GPU render %ux%u -> DMA-BUF -> "
		   "DRM %ux%u RGB565, %s, %ux downscale\n",
		   render_width, render_height, display.width, display.height,
		   framebuffer_rotated ? "clockwise" : "native orientation",
		   render_scale);
}

void VID_Shutdown(void)
{
	if (!close_video()) {
		video_exit_status = EXIT_FAILURE;
		Con_Printf("FPLinux video: DRM close: %s\n", strerror(errno));
	}
}

int fplinux_quake_video_exit_status(void)
{
	return video_exit_status;
}

void GL_BeginRendering(int *x, int *y, int *width, int *height)
{
	*x = *y = 0;
	*width = render_width;
	*height = render_height;
	if (!fplinux_quake_stats_enabled())
		return;
	check_render_error();
	if (!gpu_timer.checked)
		open_gpu_timer();
	if (gpu_timer.query && !gpu_timer.pending && !gpu_timer.active) {
		gpu_timer.begin_query(GL_TIME_ELAPSED, gpu_timer.query);
		if (timer_call_ok())
			gpu_timer.active = true;
		else
			discard_gpu_timer();
	}
}

static void copy_frame(void)
{
	GLint texture;
	GLint read_buffer;

	/* Keep the engine's cached texture binding valid on its current unit. */
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glGetIntegerv(GL_READ_BUFFER, &read_buffer);
	glReadBuffer(GL_FRONT);
	glBindTexture(GL_TEXTURE_2D, transfer_texture);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, render_width,
			    render_height);
	glBindTexture(GL_TEXTURE_2D, texture);
	glReadBuffer(read_buffer);
}

void GL_EndRendering(void)
{
	GLenum gl_error;

	if (gpu_timer.active) {
		check_render_error();
		end_gpu_timer();
		if (!timer_call_ok())
			discard_gpu_timer();
	}
	if (!fplinux_drm_session_dispatch(&display))
		Sys_Error("FPLinux video: VT dispatch: %s", strerror(errno));
	if (!display.active)
		return;
	gl_error = glGetError();
	if (gl_error != GL_NO_ERROR)
		Sys_Error("FPLinux GPU: rendering failed: 0x%x", gl_error);
	copy_frame();
	gl_error = glGetError();
	if (gl_error != GL_NO_ERROR)
		Sys_Error("FPLinux GPU: frame copy failed: 0x%x", gl_error);
	if (!fplinux_gpu_output_render(output, transfer_texture))
		Sys_Error("FPLinux GPU: output render: %s",
			  fplinux_gpu_output_error(output));
	sample_gpu_timer();
	if (!fplinux_gpu_output_present(output))
		Sys_Error("FPLinux GPU: output present: %s",
			  fplinux_gpu_output_error(output));
}

void VID_ProcessEvents(void)
{
	if (exit_signal) {
		int signal_number = exit_signal;

		exit_signal = 0;
		Con_Printf("FPLinux video: signal %d requested clean exit\n",
			   signal_number);
		Sys_Quit();
	}
	if (display_opened && !fplinux_drm_session_dispatch(&display))
		Sys_Error("FPLinux video: VT dispatch: %s", strerror(errno));
	IN_Commands();
}

void Sys_SendKeyEvents(void)
{
	VID_ProcessEvents();
}

qboolean VID_CheckAdequateMem(int width, int height)
{
	return width == (int)render_width && height == (int)render_height;
}

qboolean VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
	if (mode->width != (int)render_width ||
	    mode->height != (int)render_height ||
	    mode->bpp != FPLINUX_QUAKE_VIDEO_BITS_PER_PIXEL)
		return false;
	vid_currentmode = mode;
	VID_SetPalette(palette);
	return true;
}

void VID_Update(vrect_t *rects)
{
	(void)rects;
}

void D_BeginDirectRect(int x, int y, const byte *bitmap, int width, int height)
{
	(void)x;
	(void)y;
	(void)bitmap;
	(void)width;
	(void)height;
}

void D_EndDirectRect(int x, int y, int width, int height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

void VID_SetDefaultMode(void)
{
}

void VID_LockBuffer(void)
{
}

void VID_UnlockBuffer(void)
{
}

void VID_AddCommands(void)
{
}

void VID_RegisterVariables(void)
{
}

qboolean window_visible(void)
{
	return true;
}
