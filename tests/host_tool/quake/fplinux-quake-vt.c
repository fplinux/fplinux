/* SPDX-License-Identifier: GPL-2.0-only */
/* Real event pump and signalfd dispatch; engine and device setup are fakes. */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"
#include "fplinux-quake-internal.h"
#include "test-quake-video.h"

#ifdef FPLINUX_TEST_QUAKE_GPU
#define FPLINUX_QUAKE_GL_DISPATCH_IMPLEMENTATION
#include "fplinux-gpu-output.h"
#include "fplinux-quake-gl.h"
#include "fplinux-quake-stats.h"
#endif

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

#ifdef FPLINUX_TEST_QUAKE_GPU
unsigned int com_argc;
const char **com_argv;

struct fplinux_gpu {
	struct fplinux_drm_session *session;
};

struct fplinux_gpu_output {
	struct fplinux_gpu gpu;
};

static GLenum fake_get_error(void)
{
	return GL_NO_ERROR;
}

__typeof__(&glGetError) fplinux_quake_glGetError = fake_get_error;

bool fplinux_quake_stats_enabled(void)
{
	return false;
}

void fplinux_quake_stats_gpu_sample(uint64_t elapsed_ns)
{
	(void)elapsed_ns;
	Sys_Error("event pump unexpectedly reported GPU timing");
}

static const GLubyte *forbidden_get_string(GLenum name)
{
	(void)name;
	Sys_Error("disabled statistics unexpectedly probed GL support");
}

__typeof__(&glGetString) fplinux_quake_glGetString = forbidden_get_string;

qboolean GL_ExtensionCheck_String(const char *extension, const char *search)
{
	(void)extension;
	(void)search;
	Sys_Error("disabled statistics unexpectedly checked a GL extension");
}

static void fake_get_integer(GLenum name, GLint *value)
{
	(void)name;
	*value = 0;
}

static void fake_generate_textures(GLsizei count, GLuint *textures)
{
	for (GLsizei i = 0; i < count; ++i)
		textures[i] = (GLuint)i + 1;
}

static void fake_bind_texture(GLenum target, GLuint texture)
{
	(void)target;
	(void)texture;
}

static void fake_texture_image(GLenum target, GLint level,
			       GLint internal_format, GLsizei width,
			       GLsizei height, GLint border, GLenum format,
			       GLenum type, const void *pixels)
{
	(void)target;
	(void)level;
	(void)internal_format;
	(void)width;
	(void)height;
	(void)border;
	(void)format;
	(void)type;
	(void)pixels;
}

static void fake_texture_parameter(GLenum target, GLenum name, GLint value)
{
	(void)target;
	(void)name;
	(void)value;
}

static void fake_delete_textures(GLsizei count, const GLuint *textures)
{
	(void)count;
	(void)textures;
}

static void forbidden_frame_copy(GLenum target, GLint level, GLint xoffset,
				 GLint yoffset, GLint x, GLint y, GLsizei width,
				 GLsizei height)
{
	(void)target;
	(void)level;
	(void)xoffset;
	(void)yoffset;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	Sys_Error("event pump unexpectedly copied a frame");
}

__typeof__(&glGetIntegerv) fplinux_quake_glGetIntegerv = fake_get_integer;
__typeof__(&glGenTextures) fplinux_quake_glGenTextures = fake_generate_textures;
__typeof__(&glBindTexture) fplinux_quake_glBindTexture = fake_bind_texture;
__typeof__(&glTexImage2D) fplinux_quake_glTexImage2D = fake_texture_image;
__typeof__(&glTexParameteri) fplinux_quake_glTexParameteri =
	fake_texture_parameter;
__typeof__(&glDeleteTextures) fplinux_quake_glDeleteTextures =
	fake_delete_textures;
__typeof__(&glCopyTexSubImage2D) fplinux_quake_glCopyTexSubImage2D =
	forbidden_frame_copy;

void fplinux_quake_gl_draw_buffer(GLenum mode)
{
	(void)mode;
}

void fplinux_quake_gl_read_buffer(GLenum mode)
{
	(void)mode;
}

bool fplinux_quake_gl_load(struct fplinux_gpu *gpu, char *error, size_t size)
{
	(void)gpu;
	(void)error;
	(void)size;
	return true;
}

/* The fake output owns its context and borrows the already-open display. */
struct fplinux_gpu_output *
fplinux_gpu_output_open(struct fplinux_drm_session *session,
			enum fplinux_gpu_api api, unsigned int width,
			unsigned int height, bool clockwise, char *error,
			size_t size)
{
	struct fplinux_gpu_output *output = calloc(1, sizeof(*output));

	(void)api;
	(void)width;
	(void)height;
	(void)clockwise;
	if (error && size)
		snprintf(error, size, "%s",
			 output ? "" : "allocate fake output");
	if (output)
		output->gpu.session = session;
	return output;
}

struct fplinux_gpu *
fplinux_gpu_output_gpu(const struct fplinux_gpu_output *output)
{
	return output ? (struct fplinux_gpu *)&output->gpu : NULL;
}

bool fplinux_gpu_output_render(struct fplinux_gpu_output *output,
			       unsigned int texture)
{
	(void)output;
	(void)texture;
	Sys_Error("event pump unexpectedly rendered a frame");
}

bool fplinux_gpu_output_present(struct fplinux_gpu_output *output)
{
	(void)output;
	Sys_Error("event pump unexpectedly presented a frame");
}

const char *fplinux_gpu_output_error(const struct fplinux_gpu_output *output)
{
	(void)output;
	return "unexpected GPU output failure";
}

bool fplinux_gpu_output_close(struct fplinux_gpu_output *output)
{
	free(output);
	return true;
}

void *fplinux_gpu_get_proc(struct fplinux_gpu *gpu, const char *name)
{
	(void)gpu;
	(void)name;
	return NULL;
}

const char *fplinux_gpu_device(const struct fplinux_gpu *gpu)
{
	(void)gpu;
	return "fake device";
}

const char *fplinux_gpu_renderer(const struct fplinux_gpu *gpu)
{
	(void)gpu;
	return "fake renderer";
}

const char *fplinux_gpu_version(const struct fplinux_gpu *gpu)
{
	(void)gpu;
	return "2.1 fake";
}

int COM_CheckParm(const char *argument)
{
	(void)argument;
	return 0;
}

void GL_Init(void)
{
}

void GL_Shutdown(void)
{
}

void QPic32_InitPalettes(const byte *palette)
{
	(void)palette;
}
#endif

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
	session->present_fence = -1;
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
