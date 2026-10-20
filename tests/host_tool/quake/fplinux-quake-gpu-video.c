/* SPDX-License-Identifier: GPL-2.0-only */
/* Linked video adapter with controlled engine, GL and GPU-output boundaries. */
#define _GNU_SOURCE
#define FPLINUX_QUAKE_GL_DISPATCH_IMPLEMENTATION

#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"
#include "fplinux-gpu.h"
#include "fplinux-quake-gl.h"
#include "fplinux-quake-internal.h"
#include "fplinux-quake-stats.h"
#include "glquake.h"
#include "test-quake-video.h"

#include <GL/glext.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int colormap[4096];
static const char *scenario;
static bool output_live;
static bool gpu_live;
static bool display_live;
static bool engine_texture_live;
static bool transfer_texture_live;
static bool transfer_texture_allocated;
static bool output_frame_ready;
static unsigned int gpu_width;
static unsigned int gpu_height;
static unsigned int input_calls;
static unsigned int presented_frames;
static bool renderer_entered;
static bool host_shutdown_active;
static bool host_shutdown_complete;
static bool stats_enabled;
static bool query_live;
static bool query_active;
static bool query_pending;
static bool query_completed;
static unsigned int timer_extension_checks;
static unsigned int timer_proc_lookups;
static unsigned int timer_counter_checks;
static unsigned int timer_queries;
static unsigned int timer_begins;
static unsigned int timer_ends;
static unsigned int timer_results;
static unsigned int timer_deletes;
static unsigned int timer_samples;
static uint64_t timer_elapsed_ns;
static GLenum pending_gl_error;
static GLenum draw_modes[4];
static GLenum read_modes[4];
static size_t draw_count;
static size_t read_count;
static GLclampd depth_near;
static GLclampd depth_far;
static GLenum active_texture = GL_TEXTURE0;
static GLuint texture_bindings[3];
static GLenum read_buffer = GL_FRONT;
static GLenum draw_buffer = GL_FRONT;
static uint32_t pbuffer_colors[4];
static uint32_t transfer_colors[4];
static const uint32_t scene_colors[3][4] = {
	{ 0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff },
	{ 0x00ffffff, 0xff00ffff, 0xffff00ff, 0x000000ff },
	{ 0x010203ff, 0x040506ff, 0x070809ff, 0x0a0b0cff },
};

byte *host_colormap = (byte *)colormap;
unsigned int com_argc;
const char **com_argv;
qvidmode_t vid_windowed_mode;
qvidmode_t *vid_modelist;
int vid_nummodes;
const qvidmode_t *vid_currentmode;
qboolean vsync_available;
qboolean adaptive_vsync_available;
void (*vid_menudrawfn)(void);
void (*vid_menukeyfn)(knum_t key);

static void require(bool valid, const char *message)
{
	if (!valid) {
		fprintf(stderr, "harness: %s\n", message);
		exit(99);
	}
}

static bool is_scenario(const char *name)
{
	return !strcmp(scenario, name);
}

bool fplinux_quake_stats_enabled(void)
{
	return stats_enabled;
}

void fplinux_quake_stats_gpu_sample(uint64_t elapsed_ns)
{
	require(!query_pending && query_completed && output_frame_ready,
		"submit timing only after the output completed the scene query");
	require(elapsed_ns == UINT64_C(18000000),
		"submit the driver's elapsed nanoseconds without conversion");
	++timer_samples;
	timer_elapsed_ns = elapsed_ns;
}

qboolean GL_ExtensionCheck_String(const char *extension, const char *search)
{
	require(!strcmp(extension, "GL_ARB_timer_query"),
		"timer support uses the upstream extension checker");
	return !strcmp(search, "GL_ARB_timer_query");
}

static void require_engine_state(void)
{
	require(active_texture == GL_TEXTURE2 && texture_bindings[0] == 41 &&
			texture_bindings[2] == 77,
		"presentation preserves the engine texture unit and bindings");
	require(read_buffer == GL_FRONT_LEFT && draw_buffer == GL_FRONT,
		"presentation preserves engine pbuffer buffer selection");
}

int COM_CheckParm(const char *argument)
{
	unsigned int index;

	for (index = 1; index < com_argc; ++index)
		if (!strcmp(com_argv[index], argument))
			return index;
	return 0;
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
	if (is_scenario("small-portrait"))
		require(mode->width == 320 && mode->height == 256,
			"small display exposes the scaled engine mode");
	else
		require(mode->width == 320 && mode->height == 240,
			"native display exposes a landscape engine mode");
	require(mode->bpp == 16 && mode->resolution.scale == 1,
		"preserve engine mode depth and resolution scale");
	memset(definition, 0, sizeof(*definition));
}

void VID_MenuDraw(void)
{
}

void VID_MenuKey(knum_t key)
{
	(void)key;
}

void IN_Commands(void)
{
	++input_calls;
}

void QPic32_InitPalettes(const byte *palette)
{
	(void)palette;
}

void GL_Init(void)
{
	renderer_entered = true;
	engine_texture_live = true;
	active_texture = GL_TEXTURE2;
	texture_bindings[0] = 41;
	texture_bindings[2] = 77;
	read_buffer = GL_FRONT_LEFT;
	memcpy(pbuffer_colors, scene_colors[0], sizeof(pbuffer_colors));
	if (is_scenario("init-gl-error"))
		pending_gl_error = GL_INVALID_OPERATION;
}

void GL_Shutdown(void)
{
	require(gpu_live && output_live,
		"renderer shutdown needs its live borrowed context");
	require_engine_state();
	require(!query_live, "release timer queries before renderer shutdown");
	engine_texture_live = false;
}

void Con_Printf(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}

static void print_resources(void)
{
	struct sigaction action;

	require(sigaction(SIGTERM, NULL, &action) == 0, "read SIGTERM handler");
	printf("resources: output=%d gpu=%d display=%d input=%u signal_default=%d\n",
	       output_live, gpu_live, display_live, input_calls,
	       action.sa_handler == SIG_DFL);
	printf("gpu-timing: extensions=%u procs=%u counters=%u queries=%u "
	       "begins=%u ends=%u results=%u deletes=%u samples=%u ns=%" PRIu64
	       "\n",
	       timer_extension_checks, timer_proc_lookups, timer_counter_checks,
	       timer_queries, timer_begins, timer_ends, timer_results,
	       timer_deletes, timer_samples, timer_elapsed_ns);
}

static void shutdown_host(void)
{
	require(!host_shutdown_active, "host shutdown cannot recurse");
	host_shutdown_active = true;
	VID_Shutdown();
	/* Model engine cleanup that must finish after video releases its output. */
	require(!output_live && !gpu_live && !display_live,
		"video resources are released before host shutdown completes");
	host_shutdown_complete = true;
	host_shutdown_active = false;
}

_Noreturn void Sys_Quit(void)
{
	shutdown_host();
	puts("quit");
	print_resources();
	printf("host-shutdown-complete: %d\n", host_shutdown_complete);
	exit(fplinux_quake_video_exit_status());
}

_Noreturn void Sys_Error(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	shutdown_host();
	print_resources();
	exit(1);
}

bool fplinux_drm_session_open(struct fplinux_drm_session *session,
			      const char *drm, const char *tty, uint32_t format,
			      char *error, size_t size)
{
	(void)drm;
	(void)tty;
	(void)error;
	(void)size;
	require(format == DRM_FORMAT_RGB565, "display format is RGB565");
	memset(session, 0, sizeof(*session));
	session->present_fence = -1;
	session->width = is_scenario("landscape") ? 320 : 240;
	session->height = is_scenario("landscape") ? 240 : 320;
	if (is_scenario("small-portrait")) {
		session->width = 128;
		session->height = 160;
	}
	session->stride = session->width * 2;
	session->page_bytes = session->stride * session->height;
	session->pages = 2;
	session->active = true;
	display_live = true;
	return true;
}

bool fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	(void)session;
	require(!gpu_live && !output_live,
		"release output and owned GPU before restoring display");
	display_live = false;
	if (is_scenario("drm-close-fail") || is_scenario("cleanup-both-fail") ||
	    is_scenario("quit-drm-close-fail") ||
	    is_scenario("quit-both-close-fail")) {
		errno = EIO;
		return false;
	}
	return true;
}

bool fplinux_drm_session_dispatch(struct fplinux_drm_session *session)
{
	(void)session;
	require(!output_frame_ready,
		"VT dispatch cannot interrupt an acquired output frame");
	require(!query_active, "finish scene timing before VT dispatch");
	if (is_scenario("dispatch-fail") && renderer_entered) {
		errno = EIO;
		return false;
	}
	return true;
}

struct fplinux_gpu_output *
fplinux_gpu_output_open(struct fplinux_drm_session *session,
			enum fplinux_gpu_api api, unsigned int width,
			unsigned int height, bool clockwise, char *error,
			size_t size)
{
	require(session == fplinux_quake_display_session() && display_live,
		"output borrows the adapter's live display session");
	require(api == FPLINUX_GPU_OPENGL2,
		"output requests native desktop OpenGL");
	if (is_scenario("small-portrait"))
		require(width == 320 && height == 256 && clockwise,
			"128x160 output scales the landscape scene twofold");
	else
		require(width == 320 && height == 240 &&
				clockwise == !is_scenario("landscape"),
			"native outputs share landscape scene geometry and rotation");
	if (is_scenario("signal-init"))
		raise(SIGTERM);
	if (is_scenario("gpu-fail") || is_scenario("signal-init") ||
	    is_scenario("drm-close-fail")) {
		snprintf(error, size, "no hardware OpenGL context");
		return NULL;
	}
	output_live = true;
	gpu_live = true;
	gpu_width = width;
	gpu_height = height;
	return (struct fplinux_gpu_output *)&output_live;
}

struct fplinux_gpu *
fplinux_gpu_output_gpu(const struct fplinux_gpu_output *output)
{
	require(output && output_live, "borrow GPU from a live output");
	return (struct fplinux_gpu *)&gpu_live;
}

bool fplinux_gpu_output_render(struct fplinux_gpu_output *output,
			       unsigned int texture)
{
	require(output && output_live && gpu_live && display_live,
		"render through a live output and borrowed display");
	require_engine_state();
	require(!query_active, "GPU output is outside the scene timing query");
	require(texture == 9 && transfer_texture_live &&
			transfer_texture_allocated,
		"output consumes an owned RGBA8 transfer texture");
	require(!memcmp(transfer_colors, scene_colors[presented_frames],
			sizeof(transfer_colors)),
		"output receives the latest pbuffer contents");
	if (is_scenario("runtime-render-error") ||
	    (is_scenario("timer-render-error") && query_pending))
		return false;
	output_frame_ready = true;
	if (query_pending)
		query_completed = true;
	return true;
}

bool fplinux_gpu_output_present(struct fplinux_gpu_output *output)
{
	require(output && output_live && output_frame_ready,
		"only a rendered output frame may be presented");
	require_engine_state();
	if (is_scenario("runtime-present-error"))
		return false;
	output_frame_ready = false;
	++presented_frames;
	return true;
}

const char *fplinux_gpu_output_error(const struct fplinux_gpu_output *output)
{
	require(output && output_live, "read errors before destroying output");
	return is_scenario("runtime-present-error") ? "present failure" :
						      "render failure";
}

bool fplinux_gpu_output_close(struct fplinux_gpu_output *output)
{
	if (!output)
		return true;
	require(display_live && output_live && gpu_live,
		"display outlives its output and owned GPU");
	require(!engine_texture_live && !transfer_texture_live,
		"delete engine and transfer resources before closing their context");
	require(!query_live && !query_active,
		"timer resources do not outlive their context");
	output_frame_ready = false;
	output_live = false;
	gpu_live = false;
	if (is_scenario("output-close-fail") ||
	    is_scenario("cleanup-both-fail") ||
	    is_scenario("quit-output-close-fail") ||
	    is_scenario("quit-both-close-fail")) {
		if (is_scenario("quit-output-close-fail") ||
		    is_scenario("quit-both-close-fail"))
			require(presented_frames == 2,
				"final output completion fails after rendering succeeds");
		errno = EIO;
		if (is_scenario("cleanup-both-fail") ||
		    is_scenario("quit-both-close-fail"))
			errno = EBUSY;
		return false;
	}
	return true;
}

static void unused_gl_proc(void)
{
	require(false, "unexpected fake GL call");
}

static GLenum fake_get_error(void)
{
	GLenum error = pending_gl_error;

	pending_gl_error = GL_NO_ERROR;
	return error;
}

static void fake_draw_buffer(GLenum mode)
{
	if (is_scenario("dispatch")) {
		require(draw_count < 4, "draw mode capacity");
		draw_modes[draw_count++] = mode;
	}
	draw_buffer = mode;
}

static void fake_read_buffer(GLenum mode)
{
	if (is_scenario("dispatch")) {
		require(read_count < 4, "read mode capacity");
		read_modes[read_count++] = mode;
	}
	read_buffer = mode;
}

static void fake_get_integer(GLenum name, GLint *value)
{
	if (name == GL_TEXTURE_BINDING_2D)
		*value = texture_bindings[active_texture - GL_TEXTURE0];
	else if (name == GL_READ_BUFFER)
		*value = read_buffer;
	else
		require(false, "unexpected GL state query");
}

static void fake_gen_textures(GLsizei count, GLuint *textures)
{
	require(count == 1 && gpu_live && !transfer_texture_live,
		"allocate a transfer texture inside its context");
	textures[0] = 9;
	transfer_texture_live = true;
}

static void fake_bind_texture(GLenum target, GLuint texture)
{
	require(target == GL_TEXTURE_2D,
		"transfer uses a two-dimensional texture");
	texture_bindings[active_texture - GL_TEXTURE0] = texture;
}

static void fake_tex_image(GLenum target, GLint level, GLint internal_format,
			   GLsizei width, GLsizei height, GLint border,
			   GLenum format, GLenum type, const GLvoid *pixels)
{
	require(target == GL_TEXTURE_2D && level == 0 && border == 0 &&
			internal_format == GL_RGBA8 && format == GL_RGBA &&
			type == GL_UNSIGNED_BYTE && pixels == NULL,
		"allocate RGBA8 texture storage without a CPU transfer buffer");
	require(width == (int)gpu_width && height == (int)gpu_height &&
			texture_bindings[active_texture - GL_TEXTURE0] == 9,
		"transfer texture covers the complete engine pbuffer");
	transfer_texture_allocated = true;
	if (is_scenario("transfer-init-error") ||
	    is_scenario("output-close-fail") ||
	    is_scenario("cleanup-both-fail"))
		pending_gl_error = GL_OUT_OF_MEMORY;
}

static void fake_tex_parameter(GLenum target, GLenum name, GLint value)
{
	require(target == GL_TEXTURE_2D &&
			texture_bindings[active_texture - GL_TEXTURE0] == 9,
		"configure only the owned transfer texture");
	if (name == GL_TEXTURE_MIN_FILTER || name == GL_TEXTURE_MAG_FILTER)
		require(value == GL_NEAREST,
			"transfer texture keeps nearest sampling");
	else
		require((name == GL_TEXTURE_WRAP_S ||
			 name == GL_TEXTURE_WRAP_T) &&
				value == GL_CLAMP_TO_EDGE,
			"transfer texture clamps both edges");
}

static void fake_copy_texture(GLenum target, GLint level, GLint xoffset,
			      GLint yoffset, GLint x, GLint y, GLsizei width,
			      GLsizei height)
{
	require(gpu_live && target == GL_TEXTURE_2D && level == 0 &&
			xoffset == 0 && yoffset == 0 && x == 0 && y == 0 &&
			width == (int)gpu_width && height == (int)gpu_height,
		"copy the whole default pbuffer without cropping or CPU readback");
	require(texture_bindings[active_texture - GL_TEXTURE0] == 9 &&
			read_buffer == GL_FRONT && draw_buffer == GL_FRONT,
		"copy pbuffer front contents to the transfer texture");
	require(!query_active, "pbuffer transfer is outside scene GPU timing");
	memcpy(transfer_colors, pbuffer_colors, sizeof(transfer_colors));
	if (is_scenario("runtime-copy-error"))
		pending_gl_error = GL_INVALID_OPERATION;
}

static void fake_delete_textures(GLsizei count, const GLuint *textures)
{
	require(gpu_live && output_live && count == 1 && textures[0] == 9 &&
			transfer_texture_live,
		"delete transfer storage before closing its context");
	transfer_texture_live = false;
}

static void fake_read_pixels(GLint x, GLint y, GLsizei width, GLsizei height,
			     GLenum format, GLenum type, GLvoid *pixels)
{
	require(is_scenario("engine-readback") && x == 0 && y == 0 &&
			width == 1 && height == 1 && format == GL_RGBA &&
			type == GL_UNSIGNED_BYTE,
		"only an explicit engine screenshot may read pixels");
	require_engine_state();
	memcpy(pixels, pbuffer_colors, sizeof(pbuffer_colors[0]));
}

static void fake_depth_range(GLclampd near_value, GLclampd far_value)
{
	depth_near = near_value;
	depth_far = far_value;
}

static const GLubyte *fake_get_string(GLenum name)
{
	if (name == GL_EXTENSIONS) {
		require(stats_enabled,
			"disabled statistics never probe GL support");
		++timer_extension_checks;
		return (const GLubyte *)(is_scenario("timer-unsupported") ?
						 "" :
						 "GL_ARB_timer_query");
	}
	require(name == GL_VERSION, "typed string query argument");
	return (const GLubyte *)"2.1 fake";
}

static void fake_get_query(GLenum target, GLenum name, GLint *value)
{
	require(target == GL_TIME_ELAPSED && name == GL_QUERY_COUNTER_BITS,
		"probe only elapsed-time counter support");
	++timer_counter_checks;
	*value = is_scenario("timer-no-counter") ? 0 : 64;
}

static void fake_gen_queries(GLsizei count, GLuint *queries)
{
	require(gpu_live && output_live && count == 1 && !query_live,
		"allocate one timer query inside its context");
	queries[0] = 23;
	query_live = true;
	++timer_queries;
}

static void fake_begin_query(GLenum target, GLuint query)
{
	require(query_live && query == 23 && target == GL_TIME_ELAPSED &&
			!query_active && !query_pending,
		"reuse the scene query only after its completed result was read");
	++timer_begins;
	if (is_scenario("timer-begin-oom")) {
		pending_gl_error = GL_OUT_OF_MEMORY;
		return;
	}
	query_active = true;
}

static void fake_end_query(GLenum target)
{
	require(query_live && query_active && target == GL_TIME_ELAPSED,
		"each scene query end balances a live begin");
	query_active = false;
	query_pending = true;
	query_completed = false;
	++timer_ends;
}

static void fake_get_query_result(GLuint query, GLenum name, GLuint64 *value)
{
	require(query_live && query == 23 && name == GL_QUERY_RESULT &&
			query_pending && query_completed && output_frame_ready,
		"read query results only after successful output completion");
	*value = UINT64_C(18000000);
	query_pending = false;
	++timer_results;
	if (is_scenario("timer-query-error"))
		pending_gl_error = GL_INVALID_OPERATION;
}

static void fake_delete_queries(GLsizei count, const GLuint *queries)
{
	require(gpu_live && output_live && count == 1 && queries[0] == 23 &&
			query_live && !query_active,
		"delete the owned query once after balancing any active begin");
	query_live = false;
	query_pending = false;
	++timer_deletes;
}

void *fplinux_gpu_get_proc(struct fplinux_gpu *context, const char *name)
{
	(void)context;
	if (is_scenario("missing-proc") && !strcmp(name, "glMatrixMode"))
		return NULL;
#define GL_PROC(symbol, function)   \
	if (!strcmp(name, #symbol)) \
		return function;
	GL_PROC(glGetError, fake_get_error)
	GL_PROC(glDrawBuffer, fake_draw_buffer)
	GL_PROC(glReadBuffer, fake_read_buffer)
	GL_PROC(glGetIntegerv, fake_get_integer)
	GL_PROC(glGenTextures, fake_gen_textures)
	GL_PROC(glBindTexture, fake_bind_texture)
	GL_PROC(glTexImage2D, fake_tex_image)
	GL_PROC(glTexParameteri, fake_tex_parameter)
	GL_PROC(glCopyTexSubImage2D, fake_copy_texture)
	GL_PROC(glDeleteTextures, fake_delete_textures)
	GL_PROC(glReadPixels, fake_read_pixels)
	GL_PROC(glDepthRange, fake_depth_range)
	GL_PROC(glGetString, fake_get_string)
#undef GL_PROC
#define TIMER_PROC(symbol, function)  \
	if (!strcmp(name, #symbol)) { \
		++timer_proc_lookups; \
		return function;      \
	}
	TIMER_PROC(glGetQueryiv, fake_get_query)
	TIMER_PROC(glGenQueries, fake_gen_queries)
	TIMER_PROC(glBeginQuery, fake_begin_query)
	TIMER_PROC(glEndQuery, fake_end_query)
	TIMER_PROC(glDeleteQueries, fake_delete_queries)
	if (is_scenario("timer-missing-proc") &&
	    !strcmp(name, "glGetQueryObjectui64v")) {
		++timer_proc_lookups;
		return NULL;
	}
	TIMER_PROC(glGetQueryObjectui64v, fake_get_query_result)
#undef TIMER_PROC
	return unused_gl_proc;
}

const char *fplinux_gpu_device(const struct fplinux_gpu *context)
{
	(void)context;
	return "/dev/dri/renderD-test";
}

const char *fplinux_gpu_renderer(const struct fplinux_gpu *context)
{
	(void)context;
	return "hardware-test";
}

const char *fplinux_gpu_version(const struct fplinux_gpu *context)
{
	(void)context;
	return "2.1 test";
}

int __wrap_execv(const char *path, char *const arguments[])
{
	unsigned int index;

	require(!strcmp(path, "/usr/bin/tyr-quake"),
		"software fallback engine");
	require(!gpu_live && !output_live && !display_live && input_calls == 0,
		"release output and display before software starts or input opens");
	require(renderer_entered == is_scenario("init-gl-error"),
		"failed core setup does not enter the renderer");
	require(!strcmp(arguments[0], "/usr/bin/tyr-quake"),
		"software argv[0]");
	require(!strcmp(arguments[1], scenario) &&
			!strcmp(arguments[2], "-heapsize") &&
			!strcmp(arguments[3], "16384") &&
			!strcmp(arguments[4], "+mlook") && arguments[5] == NULL,
		"preserve engine arguments and remove the private fallback flag");
	for (index = 0; arguments[index]; ++index)
		printf("fallback-argument: %s\n", arguments[index]);
	print_resources();
	puts("software-fallback");
	exit(37);
}

static void test_dispatch(void)
{
	char error[160];

	require(fplinux_quake_gl_load(NULL, error, sizeof(error)),
		"resolve GL calls");
	fplinux_quake_gl_draw_buffer(GL_BACK);
	fplinux_quake_gl_draw_buffer(GL_FRONT);
	fplinux_quake_gl_draw_buffer(GL_LEFT);
	fplinux_quake_gl_read_buffer(GL_BACK);
	fplinux_quake_gl_read_buffer(GL_FRONT);
	fplinux_quake_gl_read_buffer(GL_RIGHT);
	fplinux_quake_glDepthRange(0.25, 0.75);
	require(draw_count == 3 && draw_modes[0] == GL_FRONT &&
			draw_modes[1] == GL_FRONT && draw_modes[2] == GL_LEFT,
		"adapt the two color buffers without suppressing invalid modes");
	require(read_count == 3 && read_modes[0] == GL_FRONT &&
			read_modes[1] == GL_FRONT && read_modes[2] == GL_RIGHT,
		"adapt read buffers with the same pbuffer contract");
	require(depth_near == 0.25 && depth_far == 0.75,
		"preserve desktop GL double arguments");
	require(!strcmp((const char *)fplinux_quake_glGetString(GL_VERSION),
			"2.1 fake"),
		"preserve GL pointer return types");
}

static void check_engine_readback(void)
{
	uint32_t color;

	fplinux_quake_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
				   &color);
	require(color == 0xff0000ff,
		"engine screenshot reads the pbuffer outside a begin/end pair");
}

static void test_timer_lifetime(void)
{
	int x, y, width, height;

	GL_BeginRendering(&x, &y, &width, &height);
	if (is_scenario("timer-close-active"))
		Sys_Quit();
	fplinux_quake_display_session()->active = false;
	GL_EndRendering();
	require(!query_active && query_pending && !timer_results &&
			!timer_samples && !presented_frames,
		"losing the VT balances timing without reading an incomplete query");
	if (is_scenario("timer-close-pending"))
		Sys_Quit();
	GL_BeginRendering(&x, &y, &width, &height);
	GL_EndRendering();
	require(timer_begins == 1 && !timer_results && !timer_samples,
		"inactive frames retain their pending query without waiting");
	fplinux_quake_display_session()->active = true;
	GL_EndRendering();
	require(timer_results == 1 && timer_samples == 1 &&
			presented_frames == 1,
		"a resumed output completes and reports the retained scene query");
	GL_BeginRendering(&x, &y, &width, &height);
	memcpy(pbuffer_colors, scene_colors[1], sizeof(pbuffer_colors));
	GL_EndRendering();
	require(timer_begins == 2 && timer_samples == 2 &&
			presented_frames == 2,
		"the completed query can measure the next active frame");
	Sys_Quit();
}

static void test_timer_toggle(void)
{
	int x, y, width, height;

	GL_BeginRendering(&x, &y, &width, &height);
	GL_EndRendering();
	require(!timer_extension_checks && !timer_proc_lookups &&
			!timer_queries,
		"disabled startup and frames do not probe timer support");
	stats_enabled = true;
	GL_BeginRendering(&x, &y, &width, &height);
	memcpy(pbuffer_colors, scene_colors[1], sizeof(pbuffer_colors));
	GL_EndRendering();
	require(timer_samples == 1, "enabling statistics starts GPU timing");
	stats_enabled = false;
	GL_BeginRendering(&x, &y, &width, &height);
	memcpy(pbuffer_colors, scene_colors[2], sizeof(pbuffer_colors));
	GL_EndRendering();
	require(timer_begins == 1 && timer_ends == 1 && timer_samples == 1 &&
			presented_frames == 3,
		"disabling statistics stops queries while rendering continues");
	Sys_Quit();
}

static void test_timer_begin_oom(void)
{
	int x, y, width, height;
	unsigned int frame;

	for (frame = 0; frame < 2; ++frame) {
		GL_BeginRendering(&x, &y, &width, &height);
		memcpy(pbuffer_colors, scene_colors[frame],
		       sizeof(pbuffer_colors));
		GL_EndRendering();
	}
	require(presented_frames == 2 && timer_queries == 1 &&
			timer_begins == 1 && !timer_ends && !timer_results &&
			!timer_samples,
		"timer allocation failure does not stop rendering or retry queries");
	Sys_Quit();
}

int main(int argc, char **argv)
{
	byte palette[768] = { 0 };
	int x, y, width, height;

	if (argc < 2)
		return 2;
	scenario = argv[1];
	stats_enabled = !strncmp(scenario, "timer-", 6) &&
			!is_scenario("timer-toggle");
	com_argc = argc;
	com_argv = (const char **)argv;
	if (is_scenario("dispatch")) {
		test_dispatch();
		return 0;
	}
	VID_Init(palette);
	if (is_scenario("timer-begin-oom"))
		test_timer_begin_oom();
	if (is_scenario("timer-toggle"))
		test_timer_toggle();
	if (is_scenario("timer-vt") || is_scenario("timer-close-active") ||
	    is_scenario("timer-close-pending"))
		test_timer_lifetime();
	if (is_scenario("signal-runtime")) {
		raise(SIGTERM);
		VID_ProcessEvents();
		return 99;
	}
	if (is_scenario("runtime-gl-error"))
		pending_gl_error = GL_INVALID_OPERATION;
	if (is_scenario("inactive"))
		fplinux_quake_display_session()->active = false;
	if (is_scenario("engine-readback"))
		check_engine_readback();
	/* Initialization and loading may finish a frame without BeginRendering. */
	GL_EndRendering();
	require_engine_state();
	if (is_scenario("engine-readback"))
		check_engine_readback();
	if (!is_scenario("inactive")) {
		if (is_scenario("timer-preexisting-error"))
			pending_gl_error = GL_INVALID_OPERATION;
		GL_BeginRendering(&x, &y, &width, &height);
		require(x == 0 && y == 0 && width == (int)gpu_width &&
				height == (int)gpu_height,
			"begin rendering exposes the complete existing pbuffer");
		memcpy(pbuffer_colors, scene_colors[1], sizeof(pbuffer_colors));
		if (is_scenario("timer-scene-error"))
			pending_gl_error = GL_INVALID_OPERATION;
		GL_EndRendering();
	}
	require(presented_frames == (is_scenario("inactive") ? 0U : 2U),
		"present complete frames only while the display is active");
	Sys_Quit();
}
