/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"

#define FPLINUX_GPU_OUTPUT_WAIT_NS 2000000000ULL
#define GL_COLOR_LOGIC_OP 0x0bf2
#define GL_FRAMEBUFFER_SRGB 0x8db9
#define GL_ALPHA_TEST 0x0bc0
#define GL_POLYGON_MODE 0x0b40
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FILL 0x1b02

struct output_gl {
	PFNGLGETERRORPROC GetError;
	PFNGLGETINTEGERVPROC GetIntegerv;
	PFNGLGETBOOLEANVPROC GetBooleanv;
	PFNGLISENABLEDPROC IsEnabled;
	PFNGLENABLEPROC Enable;
	PFNGLDISABLEPROC Disable;
	PFNGLVIEWPORTPROC Viewport;
	PFNGLCOLORMASKPROC ColorMask;
	PFNGLACTIVETEXTUREPROC ActiveTexture;
	PFNGLBINDTEXTUREPROC BindTexture;
	PFNGLBINDVERTEXARRAYPROC BindVertexArray;
	PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
	PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
	PFNGLCREATESHADERPROC CreateShader;
	PFNGLSHADERSOURCEPROC ShaderSource;
	PFNGLCOMPILESHADERPROC CompileShader;
	PFNGLGETSHADERIVPROC GetShaderiv;
	PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
	PFNGLDELETESHADERPROC DeleteShader;
	PFNGLCREATEPROGRAMPROC CreateProgram;
	PFNGLATTACHSHADERPROC AttachShader;
	PFNGLLINKPROGRAMPROC LinkProgram;
	PFNGLGETPROGRAMIVPROC GetProgramiv;
	PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
	PFNGLDELETEPROGRAMPROC DeleteProgram;
	PFNGLUSEPROGRAMPROC UseProgram;
	PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
	PFNGLUNIFORM1IPROC Uniform1i;
	PFNGLDRAWARRAYSPROC DrawArrays;
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
	PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC ImageRenderbuffer;
	PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
	PFNGLDRAWBUFFERSPROC DrawBuffers;
	PFNGLFENCESYNCPROC FenceSync;
	PFNGLCLIENTWAITSYNCPROC ClientWaitSync;
	PFNGLDELETESYNCPROC DeleteSync;
	PFNGLFLUSHPROC Flush;
	void (*PolygonMode)(GLenum face, GLenum mode);
};

struct output_egl {
	PFNEGLGETCURRENTDISPLAYPROC GetCurrentDisplay;
	PFNEGLGETCURRENTCONTEXTPROC GetCurrentContext;
	PFNEGLQUERYSTRINGPROC QueryString;
	PFNEGLCREATEIMAGEKHRPROC CreateImage;
	PFNEGLDESTROYIMAGEKHRPROC DestroyImage;
	PFNEGLGETERRORPROC GetError;
};

static const GLenum raster_states[] = {
	GL_BLEND,
	GL_CULL_FACE,
	GL_DEPTH_TEST,
	GL_STENCIL_TEST,
	GL_SCISSOR_TEST,
	GL_DITHER,
	GL_RASTERIZER_DISCARD,
	GL_SAMPLE_ALPHA_TO_COVERAGE,
	GL_SAMPLE_COVERAGE,
	GL_COLOR_LOGIC_OP,
	GL_FRAMEBUFFER_SRGB,
	GL_ALPHA_TEST,
};

struct output_state {
	GLint framebuffer;
	GLint viewport[4];
	GLint program;
	GLint active_texture;
	GLint texture;
	GLint vertex_array;
	GLint polygon_mode[2];
	GLboolean color_mask[4];
	GLboolean enabled[sizeof(raster_states) / sizeof(raster_states[0])];
};

struct fplinux_gpu_output {
	struct fplinux_drm_session *session;
	struct fplinux_gpu *gpu;
	struct output_gl gl;
	struct output_egl egl;
	EGLDisplay display;
	EGLContext context;
	EGLImageKHR images[2];
	GLuint renderbuffers[2];
	GLuint framebuffers[2];
	GLuint program;
	GLuint vertex_array;
	GLint source_uniform;
	GLint height_uniform;
	GLint width_uniform;
	GLint scale_uniform;
	GLint clockwise_uniform;
	unsigned int source_width;
	unsigned int source_height;
	unsigned int scale;
	unsigned int page;
	size_t raster_state_count;
	int fd;
	int error_number;
	bool desktop;
	bool clockwise;
	bool borrowed;
	bool ready;
	bool cpu_access;
	bool failed;
	char error[256];
};

static bool set_error(struct fplinux_gpu_output *output, int error_number,
		      const char *format, ...)
{
	va_list arguments;

	if (!output->failed) {
		va_start(arguments, format);
		vsnprintf(output->error, sizeof(output->error), format,
			  arguments);
		va_end(arguments);
		output->error_number = error_number ? error_number : EIO;
		output->failed = true;
	}
	output->ready = false;
	errno = output->error_number;
	return false;
}

static bool gl_ok(struct fplinux_gpu_output *output, const char *operation)
{
	GLenum error = output->gl.GetError();

	return error == GL_NO_ERROR ||
	       set_error(output, EIO, "%s: GL error 0x%x", operation, error);
}

static bool has_extension(const char *extensions, const char *name)
{
	size_t size = strlen(name);
	const char *match = extensions;

	while (match && (match = strstr(match, name))) {
		if ((match == extensions || match[-1] == ' ') &&
		    (match[size] == '\0' || match[size] == ' '))
			return true;
		match += size;
	}
	return false;
}

static bool load_functions(struct fplinux_gpu_output *output)
{
#define LOAD(member, name)                                                  \
	do {                                                                \
		output->member = fplinux_gpu_get_proc(output->gpu, name);   \
		if (!output->member)                                        \
			return set_error(output, ENOTSUP, "%s unavailable", \
					 name);                             \
	} while (0)
#define LOAD_GL(member) LOAD(gl.member, "gl" #member)
	LOAD(egl.GetCurrentDisplay, "eglGetCurrentDisplay");
	LOAD(egl.GetCurrentContext, "eglGetCurrentContext");
	LOAD(egl.QueryString, "eglQueryString");
	LOAD(egl.CreateImage, "eglCreateImageKHR");
	LOAD(egl.DestroyImage, "eglDestroyImageKHR");
	LOAD(egl.GetError, "eglGetError");
	LOAD_GL(GetError);
	LOAD_GL(GetIntegerv);
	LOAD_GL(GetBooleanv);
	LOAD_GL(IsEnabled);
	LOAD_GL(Enable);
	LOAD_GL(Disable);
	LOAD_GL(Viewport);
	LOAD_GL(ColorMask);
	LOAD_GL(ActiveTexture);
	LOAD_GL(BindTexture);
	LOAD_GL(BindVertexArray);
	LOAD_GL(GenVertexArrays);
	LOAD_GL(DeleteVertexArrays);
	LOAD_GL(CreateShader);
	LOAD_GL(ShaderSource);
	LOAD_GL(CompileShader);
	LOAD_GL(GetShaderiv);
	LOAD_GL(GetShaderInfoLog);
	LOAD_GL(DeleteShader);
	LOAD_GL(CreateProgram);
	LOAD_GL(AttachShader);
	LOAD_GL(LinkProgram);
	LOAD_GL(GetProgramiv);
	LOAD_GL(GetProgramInfoLog);
	LOAD_GL(DeleteProgram);
	LOAD_GL(UseProgram);
	LOAD_GL(GetUniformLocation);
	LOAD_GL(Uniform1i);
	LOAD_GL(DrawArrays);
	LOAD_GL(GenRenderbuffers);
	LOAD_GL(BindRenderbuffer);
	LOAD(gl.ImageRenderbuffer, "glEGLImageTargetRenderbufferStorageOES");
	LOAD_GL(DeleteRenderbuffers);
	LOAD_GL(GenFramebuffers);
	LOAD_GL(BindFramebuffer);
	LOAD_GL(FramebufferRenderbuffer);
	LOAD_GL(CheckFramebufferStatus);
	LOAD_GL(DeleteFramebuffers);
	LOAD_GL(DrawBuffers);
	LOAD_GL(FenceSync);
	LOAD_GL(ClientWaitSync);
	LOAD_GL(DeleteSync);
	LOAD_GL(Flush);
	if (output->desktop)
		LOAD_GL(PolygonMode);
#undef LOAD_GL
#undef LOAD
	return true;
}

static bool validate_session(struct fplinux_gpu_output *output)
{
	struct fplinux_drm_session *session = output->session;
	size_t row_bytes;
	size_t page_bytes;
	unsigned int width;
	unsigned int height;

	if (!session || session->format != DRM_FORMAT_RGB565 ||
	    session->pages != 2 || !session->mapping || !session->width ||
	    !session->height || session->width > INT_MAX ||
	    session->height > INT_MAX || session->stride > INT_MAX ||
	    session->page_bytes > INT_MAX || session->shown_page >= 2)
		return set_error(output, EINVAL,
				 "invalid two-page RGB565 display");
	row_bytes = (size_t)session->width * sizeof(uint16_t);
	if (session->stride < row_bytes ||
	    session->stride > SIZE_MAX / session->height)
		return set_error(output, EOVERFLOW,
				 "RGB565 display pitch overflow");
	page_bytes = session->stride * session->height;
	if (session->page_bytes < page_bytes ||
	    session->page_bytes > session->size / 2)
		return set_error(output, EOVERFLOW,
				 "RGB565 display pages overflow");
	width = output->clockwise ? session->height : session->width;
	height = output->clockwise ? session->width : session->height;
	if (!output->source_width || !output->source_height ||
	    output->source_width > INT_MAX || output->source_height > INT_MAX ||
	    output->source_width % width || output->source_height % height ||
	    output->source_width / width != output->source_height / height)
		return set_error(output, EINVAL,
				 "source and display geometry mismatch");
	output->scale = output->source_width / width;
	return true;
}

static bool validate_context(struct fplinux_gpu_output *output)
{
	const char *version = fplinux_gpu_version(output->gpu);
	const char *extensions;
	unsigned int major;
	unsigned int minor;

	if (!version ||
	    (output->desktop ?
		     sscanf(version, "%u.%u", &major, &minor) :
		     sscanf(version, "OpenGL ES %u.%u", &major, &minor)) != 2 ||
	    major < 3 || (output->desktop && major == 3 && minor < 1))
		return set_error(output, ENOTSUP,
				 "GPU output requires GL 3.1 or GLES 3");
	output->display = output->egl.GetCurrentDisplay();
	output->context = output->egl.GetCurrentContext();
	if (output->display == EGL_NO_DISPLAY ||
	    output->context == EGL_NO_CONTEXT)
		return set_error(output, EINVAL,
				 "GPU output context is not current");
	extensions = output->egl.QueryString(output->display, EGL_EXTENSIONS);
	if (!has_extension(extensions, "EGL_EXT_image_dma_buf_import") ||
	    !has_extension(extensions,
			   "EGL_EXT_image_dma_buf_import_modifiers"))
		return set_error(output, ENOTSUP,
				 "linear DMA-BUF image import unavailable");
	return gl_ok(output, "GPU rendering before output setup");
}

static bool save_state(struct fplinux_gpu_output *output,
		       struct output_state *state)
{
	struct output_gl *gl = &output->gl;

	gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->framebuffer);
	gl->GetIntegerv(GL_VIEWPORT, state->viewport);
	gl->GetIntegerv(GL_CURRENT_PROGRAM, &state->program);
	gl->GetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
	gl->ActiveTexture(GL_TEXTURE0);
	gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture);
	gl->ActiveTexture((GLenum)state->active_texture);
	gl->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &state->vertex_array);
	gl->GetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
	for (size_t i = 0; i < output->raster_state_count; ++i)
		state->enabled[i] = gl->IsEnabled(raster_states[i]);
	if (output->desktop)
		gl->GetIntegerv(GL_POLYGON_MODE, state->polygon_mode);
	return gl_ok(output, "read GPU output state");
}

static bool restore_state(struct fplinux_gpu_output *output,
			  const struct output_state *state)
{
	struct output_gl *gl = &output->gl;

	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)state->framebuffer);
	gl->Viewport(state->viewport[0], state->viewport[1], state->viewport[2],
		     state->viewport[3]);
	gl->UseProgram((GLuint)state->program);
	gl->ActiveTexture(GL_TEXTURE0);
	gl->BindTexture(GL_TEXTURE_2D, (GLuint)state->texture);
	gl->ActiveTexture((GLenum)state->active_texture);
	gl->BindVertexArray((GLuint)state->vertex_array);
	gl->ColorMask(state->color_mask[0], state->color_mask[1],
		      state->color_mask[2], state->color_mask[3]);
	for (size_t i = 0; i < output->raster_state_count; ++i) {
		if (state->enabled[i])
			gl->Enable(raster_states[i]);
		else
			gl->Disable(raster_states[i]);
	}
	if (output->desktop) {
		gl->PolygonMode(GL_FRONT, (GLenum)state->polygon_mode[0]);
		gl->PolygonMode(GL_BACK, (GLenum)state->polygon_mode[1]);
	}
	return gl_ok(output, "restore GPU output state");
}

static GLuint compile_shader(struct fplinux_gpu_output *output, GLenum type,
			     const char *body)
{
	const char *sources[] = {
		output->desktop ?
			"#version 140\n" :
			"#version 300 es\nprecision highp float;\nprecision highp int;\n",
		body,
	};
	struct output_gl *gl = &output->gl;
	GLuint shader = gl->CreateShader(type);
	GLint compiled = GL_FALSE;
	char log[256] = { 0 };

	if (shader) {
		gl->ShaderSource(shader, 2, sources, NULL);
		gl->CompileShader(shader);
		gl->GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	}
	if (!compiled || !gl_ok(output, "compile GPU output shader")) {
		if (shader) {
			gl->GetShaderInfoLog(shader, sizeof(log), NULL, log);
			gl->DeleteShader(shader);
		}
		set_error(output, EIO, "compile GPU output shader: %s", log);
		return 0;
	}
	return shader;
}

static bool create_program(struct fplinux_gpu_output *output)
{
	static const char vertex[] =
		"void main() {\n"
		"  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
		"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment[] =
		"uniform sampler2D source;\n"
		"uniform int source_height;\n"
		"uniform int display_width;\n"
		"uniform int scale;\n"
		"uniform int clockwise;\n"
		"out vec4 color;\n"
		"void main() {\n"
		"  ivec2 d = ivec2(gl_FragCoord.xy);\n"
		"  ivec2 s = clockwise != 0 ? ivec2(d.y, display_width - 1 - d.x) : d;\n"
		"  s *= scale;\n"
		"  color = texelFetch(source, ivec2(s.x, source_height - 1 - s.y), 0);\n"
		"}\n";
	struct output_gl *gl = &output->gl;
	GLuint vertex_shader = compile_shader(output, GL_VERTEX_SHADER, vertex);
	GLuint fragment_shader = 0;
	GLint linked = GL_FALSE;
	char log[256] = { 0 };

	if (!vertex_shader)
		return false;
	fragment_shader = compile_shader(output, GL_FRAGMENT_SHADER, fragment);
	if (fragment_shader) {
		output->program = gl->CreateProgram();
		if (output->program) {
			gl->AttachShader(output->program, vertex_shader);
			gl->AttachShader(output->program, fragment_shader);
			gl->LinkProgram(output->program);
			gl->GetProgramiv(output->program, GL_LINK_STATUS,
					 &linked);
		}
	}
	gl->DeleteShader(vertex_shader);
	if (fragment_shader)
		gl->DeleteShader(fragment_shader);
	if (!linked || !gl_ok(output, "link GPU output program")) {
		if (output->program)
			gl->GetProgramInfoLog(output->program, sizeof(log),
					      NULL, log);
		return set_error(output, EIO, "link GPU output program: %s",
				 log);
	}
#define UNIFORM(member, name)                                                \
	do {                                                                 \
		output->member =                                             \
			gl->GetUniformLocation(output->program, name);       \
		if (output->member < 0)                                      \
			return set_error(output, EIO,                        \
					 "missing output uniform %s", name); \
	} while (0)
	UNIFORM(source_uniform, "source");
	UNIFORM(height_uniform, "source_height");
	UNIFORM(width_uniform, "display_width");
	UNIFORM(scale_uniform, "scale");
	UNIFORM(clockwise_uniform, "clockwise");
#undef UNIFORM
	gl->GenVertexArrays(1, &output->vertex_array);
	if (!gl_ok(output, "create GPU output vertex array"))
		return false;
	return output->vertex_array ||
	       set_error(output, EIO, "create GPU output vertex array failed");
}

static bool create_images(struct fplinux_gpu_output *output)
{
	struct fplinux_drm_session *session = output->session;
	struct output_gl *gl = &output->gl;
	const GLenum buffer = GL_COLOR_ATTACHMENT0;
	GLint framebuffer = 0;
	GLint renderbuffer = 0;
	bool ok = true;

	gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
	gl->GetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
	if (!gl_ok(output, "read GPU import bindings"))
		return false;
	for (unsigned int page = 0; page < 2 && ok; ++page) {
		const EGLint attributes[] = {
			EGL_WIDTH,
			(EGLint)session->width,
			EGL_HEIGHT,
			(EGLint)session->height,
			EGL_LINUX_DRM_FOURCC_EXT,
			DRM_FORMAT_RGB565,
			EGL_DMA_BUF_PLANE0_FD_EXT,
			output->fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,
			(EGLint)(page * session->page_bytes),
			EGL_DMA_BUF_PLANE0_PITCH_EXT,
			(EGLint)session->stride,
			EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
			0,
			EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
			0,
			EGL_NONE,
		};

		output->images[page] = output->egl.CreateImage(
			output->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
			NULL, attributes);
		if (output->images[page] == EGL_NO_IMAGE_KHR) {
			ok = set_error(output, EIO,
				       "import RGB565 page %u: EGL error 0x%x",
				       page, output->egl.GetError());
			break;
		}
		gl->GenRenderbuffers(1, &output->renderbuffers[page]);
		gl->BindRenderbuffer(GL_RENDERBUFFER,
				     output->renderbuffers[page]);
		gl->ImageRenderbuffer(GL_RENDERBUFFER, output->images[page]);
		gl->GenFramebuffers(1, &output->framebuffers[page]);
		gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER,
				    output->framebuffers[page]);
		gl->FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER,
					    GL_COLOR_ATTACHMENT0,
					    GL_RENDERBUFFER,
					    output->renderbuffers[page]);
		gl->DrawBuffers(1, &buffer);
		if (!gl_ok(output, "attach RGB565 output image"))
			ok = false;
		else if (!output->renderbuffers[page] ||
			 !output->framebuffers[page] ||
			 gl->CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) !=
				 GL_FRAMEBUFFER_COMPLETE)
			ok = set_error(output, EIO,
				       "RGB565 output page %u is incomplete",
				       page);
	}
	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)framebuffer);
	gl->BindRenderbuffer(GL_RENDERBUFFER, (GLuint)renderbuffer);
	return gl_ok(output, "restore GPU import bindings") && ok;
}

struct fplinux_gpu_output *
fplinux_gpu_output_open(struct fplinux_drm_session *session,
			enum fplinux_gpu_api api, unsigned int source_width,
			unsigned int source_height, bool clockwise, char *error,
			size_t error_size)
{
	struct fplinux_gpu_output *output = calloc(1, sizeof(*output));
	int saved_errno;

	if (error && error_size)
		error[0] = '\0';
	if (!output) {
		if (error && error_size)
			snprintf(error, error_size, "allocate GPU output: %s",
				 strerror(errno));
		return NULL;
	}
	output->session = session;
	output->source_width = source_width;
	output->source_height = source_height;
	output->clockwise = clockwise;
	output->desktop = api == FPLINUX_GPU_OPENGL2;
	output->raster_state_count =
		output->desktop ?
			sizeof(raster_states) / sizeof(raster_states[0]) :
			sizeof(raster_states) / sizeof(raster_states[0]) - 3;
	output->fd = -1;
	if (!validate_session(output))
		goto fail;
	if (!fplinux_drm_session_acquire_buffer(session, &output->fd)) {
		set_error(output, errno, "export display buffer: %s",
			  strerror(errno));
		goto fail;
	}
	output->borrowed = true;
	output->gpu = fplinux_gpu_open(api, source_width, source_height, NULL,
				       output->error, sizeof(output->error));
	if (!output->gpu) {
		output->failed = true;
		output->error_number = errno ? errno : EIO;
		goto fail;
	}
	if (!load_functions(output) || !validate_context(output) ||
	    !create_images(output) || !create_program(output))
		goto fail;
	return output;
fail:
	saved_errno = output->error_number ? output->error_number : EIO;
	if (error && error_size)
		snprintf(error, error_size, "%s", output->error);
	fplinux_gpu_output_close(output);
	errno = saved_errno;
	return NULL;
}

struct fplinux_gpu *
fplinux_gpu_output_gpu(const struct fplinux_gpu_output *output)
{
	return output ? output->gpu : NULL;
}

bool fplinux_gpu_output_render(struct fplinux_gpu_output *output,
			       unsigned int texture)
{
	struct output_state state;
	struct output_gl *gl;
	GLsync fence;
	GLenum result;
	bool ok;

	if (!output) {
		errno = EINVAL;
		return false;
	}
	if (output->failed)
		return set_error(output, output->error_number, "%s",
				 output->error);
	if (!texture || output->ready || output->cpu_access ||
	    !output->session->active)
		return set_error(output, EINVAL,
				 "GPU output cannot start this frame");
	if (output->egl.GetCurrentContext() != output->context ||
	    output->egl.GetCurrentDisplay() != output->display)
		return set_error(output, EINVAL,
				 "GPU output context is not current");
	if (!gl_ok(output, "GPU rendering before output"))
		return false;
	/* Submit independent scene work before waiting for the display buffer. */
	output->gl.Flush();
	if (!gl_ok(output, "submit GPU scene before output"))
		return false;
	if (!fplinux_drm_session_wait_present(output->session))
		return set_error(output, errno, "finish GPU presentation: %s",
				 strerror(errno));
	if (!save_state(output, &state))
		return false;
	gl = &output->gl;
	output->page = 1U - output->session->shown_page;
	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER,
			    output->framebuffers[output->page]);
	gl->Viewport(0, 0, (GLsizei)output->session->width,
		     (GLsizei)output->session->height);
	gl->UseProgram(output->program);
	gl->BindVertexArray(output->vertex_array);
	gl->ActiveTexture(GL_TEXTURE0);
	gl->BindTexture(GL_TEXTURE_2D, texture);
	gl->Uniform1i(output->source_uniform, 0);
	gl->Uniform1i(output->height_uniform, (GLint)output->source_height);
	gl->Uniform1i(output->width_uniform, (GLint)output->session->width);
	gl->Uniform1i(output->scale_uniform, (GLint)output->scale);
	gl->Uniform1i(output->clockwise_uniform, output->clockwise);
	gl->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	for (size_t i = 0; i < output->raster_state_count; ++i)
		gl->Disable(raster_states[i]);
	if (output->desktop)
		gl->PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	gl->DrawArrays(GL_TRIANGLES, 0, 3);
	ok = gl_ok(output, "resolve GPU output");
	if (!restore_state(output, &state) || !ok)
		return false;
	fence = gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	ok = gl_ok(output, "create GPU output fence");
	if (!fence || !ok) {
		if (fence)
			gl->DeleteSync(fence);
		return set_error(output, EIO, "create GPU output fence failed");
	}
	gl->Flush();
	result = gl->ClientWaitSync(fence, 0, FPLINUX_GPU_OUTPUT_WAIT_NS);
	ok = gl_ok(output, "wait GPU output fence");
	if (ok && result != GL_ALREADY_SIGNALED &&
	    result != GL_CONDITION_SATISFIED)
		ok = set_error(output,
			       result == GL_TIMEOUT_EXPIRED ? ETIMEDOUT : EIO,
			       "GPU output fence %s",
			       result == GL_TIMEOUT_EXPIRED ? "timed out" :
							      "wait failed");
	gl->DeleteSync(fence);
	if (!gl_ok(output, "delete GPU output fence") || !ok)
		return false;
	output->ready = true;
	return true;
}

static bool sync_cpu(struct fplinux_gpu_output *output, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = flags | DMA_BUF_SYNC_RW };
	int result;

	do {
		result = ioctl(output->fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (result < 0 && errno == EINTR);
	return result == 0;
}

bool fplinux_gpu_output_begin_cpu(struct fplinux_gpu_output *output,
				  void **pixels, size_t *stride)
{
	if (!output) {
		errno = EINVAL;
		return false;
	}
	if (output->failed)
		return set_error(output, output->error_number, "%s",
				 output->error);
	if (!pixels || !stride || !output->ready || output->cpu_access)
		return set_error(output, EINVAL,
				 "GPU output is not ready for CPU access");
	if (!fplinux_drm_session_wait_present(output->session))
		return set_error(output, errno, "finish GPU presentation: %s",
				 strerror(errno));
	if (!sync_cpu(output, DMA_BUF_SYNC_START))
		return set_error(output, errno, "start output CPU access: %s",
				 strerror(errno));
	output->cpu_access = true;
	*pixels = output->session->mapping +
		  output->page * output->session->page_bytes;
	*stride = output->session->stride;
	return true;
}

bool fplinux_gpu_output_end_cpu(struct fplinux_gpu_output *output)
{
	if (!output) {
		errno = EINVAL;
		return false;
	}
	if (output->failed)
		return set_error(output, output->error_number, "%s",
				 output->error);
	if (!output->cpu_access)
		return set_error(output, EINVAL,
				 "GPU output has no CPU access");
	if (!sync_cpu(output, DMA_BUF_SYNC_END))
		return set_error(output, errno, "end output CPU access: %s",
				 strerror(errno));
	output->cpu_access = false;
	return true;
}

bool fplinux_gpu_output_present(struct fplinux_gpu_output *output)
{
	if (!output) {
		errno = EINVAL;
		return false;
	}
	if (output->failed)
		return set_error(output, output->error_number, "%s",
				 output->error);
	if (!output->ready || output->cpu_access || !output->session->active)
		return set_error(output, EINVAL,
				 "GPU output is not ready for presentation");
	__sync_synchronize();
	if (!fplinux_drm_session_queue_present(output->session, output->page))
		return set_error(output, errno, "present GPU output: %s",
				 strerror(errno));
	output->ready = false;
	return true;
}

const char *fplinux_gpu_output_error(const struct fplinux_gpu_output *output)
{
	return output ? output->error : "GPU output unavailable";
}

bool fplinux_gpu_output_close(struct fplinux_gpu_output *output)
{
	int saved_errno = 0;
	bool current;

	if (!output)
		return true;
	if (output->borrowed &&
	    !fplinux_drm_session_wait_present(output->session))
		saved_errno = errno;
	if (output->cpu_access && !sync_cpu(output, DMA_BUF_SYNC_END) &&
	    !saved_errno)
		saved_errno = errno;
	current = output->gpu && output->egl.GetCurrentContext &&
		  output->egl.GetCurrentContext() == output->context;
	if (current) {
		for (unsigned int page = 0; page < 2; ++page) {
			if (output->framebuffers[page])
				output->gl.DeleteFramebuffers(
					1, &output->framebuffers[page]);
			if (output->renderbuffers[page])
				output->gl.DeleteRenderbuffers(
					1, &output->renderbuffers[page]);
		}
		if (output->vertex_array)
			output->gl.DeleteVertexArrays(1, &output->vertex_array);
		if (output->program)
			output->gl.DeleteProgram(output->program);
		if (output->gl.GetError &&
		    output->gl.GetError() != GL_NO_ERROR && !saved_errno)
			saved_errno = EIO;
	}
	for (unsigned int page = 0; page < 2; ++page)
		if (output->images[page] != EGL_NO_IMAGE_KHR &&
		    !output->egl.DestroyImage(output->display,
					      output->images[page]) &&
		    !saved_errno)
			saved_errno = EIO;
	if (output->fd >= 0 && close(output->fd) < 0 && !saved_errno)
		saved_errno = errno;
	fplinux_gpu_close(output->gpu);
	if (output->borrowed)
		fplinux_drm_session_release_buffer(output->session);
	free(output);
	if (saved_errno)
		errno = saved_errno;
	return saved_errno == 0;
}
