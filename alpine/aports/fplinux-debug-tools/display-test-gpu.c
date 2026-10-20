/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "display-test-gpu.h"

#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"
#include "fplinux-gpu.h"

#include <GLES3/gl3.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAY_TEST_GPU_SHADER_DIRECTORY "/usr/share/fplinux/display-test"

struct display_test_gpu_gl {
	GLuint (*CreateShader)(GLenum type);
	void (*ShaderSource)(GLuint shader, GLsizei count,
			     const GLchar *const *strings,
			     const GLint *lengths);
	void (*CompileShader)(GLuint shader);
	void (*GetShaderiv)(GLuint shader, GLenum parameter, GLint *value);
	void (*GetShaderInfoLog)(GLuint shader, GLsizei size, GLsizei *length,
				 GLchar *log);
	void (*DeleteShader)(GLuint shader);
	GLuint (*CreateProgram)(void);
	void (*AttachShader)(GLuint program, GLuint shader);
	void (*LinkProgram)(GLuint program);
	void (*GetProgramiv)(GLuint program, GLenum parameter, GLint *value);
	void (*GetProgramInfoLog)(GLuint program, GLsizei size, GLsizei *length,
				  GLchar *log);
	void (*DeleteProgram)(GLuint program);
	void (*UseProgram)(GLuint program);
	GLint (*GetUniformLocation)(GLuint program, const GLchar *name);
	void (*Uniform1i)(GLint location, GLint value);
	void (*Uniform1ui)(GLint location, GLuint value);
	void (*GenVertexArrays)(GLsizei count, GLuint *arrays);
	void (*BindVertexArray)(GLuint array);
	void (*DeleteVertexArrays)(GLsizei count, const GLuint *arrays);
	void (*GenTextures)(GLsizei count, GLuint *textures);
	void (*BindTexture)(GLenum target, GLuint texture);
	void (*TexParameteri)(GLenum target, GLenum parameter, GLint value);
	void (*TexImage2D)(GLenum target, GLint level, GLint internal_format,
			   GLsizei width, GLsizei height, GLint border,
			   GLenum format, GLenum type, const void *pixels);
	void (*DeleteTextures)(GLsizei count, const GLuint *textures);
	void (*GenFramebuffers)(GLsizei count, GLuint *framebuffers);
	void (*BindFramebuffer)(GLenum target, GLuint framebuffer);
	void (*FramebufferTexture2D)(GLenum target, GLenum attachment,
				     GLenum texture_target, GLuint texture,
				     GLint level);
	GLenum (*CheckFramebufferStatus)(GLenum target);
	void (*DeleteFramebuffers)(GLsizei count, const GLuint *framebuffers);
	void (*Viewport)(GLint x, GLint y, GLsizei width, GLsizei height);
	void (*Disable)(GLenum capability);
	void (*DrawArrays)(GLenum mode, GLint first, GLsizei count);
	GLenum (*GetError)(void);
};

struct fplinux_display_test_gpu {
	struct fplinux_drm_session *session;
	struct fplinux_gpu_output *output;
	struct fplinux_gpu *gpu;
	struct display_test_gpu_gl gl;
	GLuint program;
	GLuint vertex_array;
	GLuint texture;
	GLuint framebuffer;
	GLint pattern_uniform;
	GLint frame_uniform;
	GLint width_uniform;
	GLint height_uniform;
	unsigned int width;
	unsigned int height;
	bool drawn;
	char error[256];
};

static bool set_error(struct fplinux_display_test_gpu *renderer,
		      int error_number, const char *message)
{
	if (!renderer->error[0])
		snprintf(renderer->error, sizeof(renderer->error), "%s",
			 message);
	errno = error_number ? error_number : EIO;
	return false;
}

static bool load_gl(struct fplinux_display_test_gpu *renderer)
{
#define LOAD_GL(member)                                                        \
	do {                                                                   \
		void *address =                                                \
			fplinux_gpu_get_proc(renderer->gpu, "gl" #member);     \
		if (!address)                                                  \
			return set_error(renderer, ENOTSUP,                    \
					 "GLES3 entry point gl" #member        \
					 " is unavailable");                   \
		_Static_assert(sizeof(renderer->gl.member) == sizeof(address), \
			       "GLES procedure pointer size");                 \
		memcpy(&renderer->gl.member, &address, sizeof(address));       \
	} while (0)
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
	LOAD_GL(Uniform1ui);
	LOAD_GL(GenVertexArrays);
	LOAD_GL(BindVertexArray);
	LOAD_GL(DeleteVertexArrays);
	LOAD_GL(GenTextures);
	LOAD_GL(BindTexture);
	LOAD_GL(TexParameteri);
	LOAD_GL(TexImage2D);
	LOAD_GL(DeleteTextures);
	LOAD_GL(GenFramebuffers);
	LOAD_GL(BindFramebuffer);
	LOAD_GL(FramebufferTexture2D);
	LOAD_GL(CheckFramebufferStatus);
	LOAD_GL(DeleteFramebuffers);
	LOAD_GL(Viewport);
	LOAD_GL(Disable);
	LOAD_GL(DrawArrays);
	LOAD_GL(GetError);
#undef LOAD_GL
	return true;
}

static char *read_shader(const char *name)
{
	char path[PATH_MAX];
	FILE *file;
	long size;
	char *source;

	if (snprintf(path, sizeof(path), "%s/%s",
		     DISPLAY_TEST_GPU_SHADER_DIRECTORY,
		     name) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	file = fopen(path, "rb");
	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 ||
	    fseek(file, 0, SEEK_SET) || (unsigned long)size > SIZE_MAX - 1) {
		fclose(file);
		errno = EIO;
		return NULL;
	}
	source = malloc((size_t)size + 1);
	if (!source) {
		fclose(file);
		return NULL;
	}
	if (fread(source, 1, (size_t)size, file) != (size_t)size) {
		free(source);
		fclose(file);
		errno = EIO;
		return NULL;
	}
	source[size] = '\0';
	fclose(file);
	return source;
}

static GLuint compile_shader(struct fplinux_display_test_gpu *renderer,
			     GLenum type, const char *source, const char *name)
{
	GLuint shader = renderer->gl.CreateShader(type);
	GLint compiled = GL_FALSE;
	char log[192] = { 0 };
	const GLchar *sources[] = { source };

	if (shader) {
		renderer->gl.ShaderSource(shader, 1, sources, NULL);
		renderer->gl.CompileShader(shader);
		renderer->gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	}
	if (!shader || !compiled || renderer->gl.GetError() != GL_NO_ERROR) {
		if (shader) {
			renderer->gl.GetShaderInfoLog(shader, sizeof(log), NULL,
						      log);
			renderer->gl.DeleteShader(shader);
		}
		snprintf(renderer->error, sizeof(renderer->error),
			 "compile %s shader failed: %s", name,
			 log[0] ? log : "no compiler log");
		errno = EIO;
		return 0;
	}
	return shader;
}

static bool create_scene(struct fplinux_display_test_gpu *renderer)
{
	char *vertex_source = read_shader("display-test-gpu.vert");
	char *fragment_source = read_shader("display-test-gpu.frag");
	GLuint vertex_shader = 0;
	GLuint fragment_shader = 0;
	GLint linked = GL_FALSE;
	char log[192] = { 0 };

	if (!vertex_source || !fragment_source) {
		snprintf(renderer->error, sizeof(renderer->error),
			 "read display-test GPU shaders: %s", strerror(errno));
		free(vertex_source);
		free(fragment_source);
		return false;
	}
	vertex_shader = compile_shader(renderer, GL_VERTEX_SHADER,
				       vertex_source, "vertex");
	fragment_shader = vertex_shader ?
				  compile_shader(renderer, GL_FRAGMENT_SHADER,
						 fragment_source, "fragment") :
				  0;
	free(vertex_source);
	free(fragment_source);
	if (!fragment_shader) {
		if (vertex_shader)
			renderer->gl.DeleteShader(vertex_shader);
		return false;
	}
	if (fragment_shader) {
		renderer->program = renderer->gl.CreateProgram();
		if (renderer->program) {
			renderer->gl.AttachShader(renderer->program,
						  vertex_shader);
			renderer->gl.AttachShader(renderer->program,
						  fragment_shader);
			renderer->gl.LinkProgram(renderer->program);
			renderer->gl.GetProgramiv(renderer->program,
						  GL_LINK_STATUS, &linked);
		}
	}
	if (vertex_shader)
		renderer->gl.DeleteShader(vertex_shader);
	if (fragment_shader)
		renderer->gl.DeleteShader(fragment_shader);
	if (!linked || renderer->gl.GetError() != GL_NO_ERROR) {
		if (renderer->program)
			renderer->gl.GetProgramInfoLog(renderer->program,
						       sizeof(log), NULL, log);
		snprintf(renderer->error, sizeof(renderer->error),
			 "link display-test GPU program failed: %s",
			 log[0] ? log : "no linker log");
		errno = EIO;
		return false;
	}
#define GET_UNIFORM(member, name)                                           \
	do {                                                                \
		renderer->member = renderer->gl.GetUniformLocation(         \
			renderer->program, name);                           \
		if (renderer->member < 0) {                                 \
			snprintf(renderer->error, sizeof(renderer->error),  \
				 "GPU shader is missing uniform %s", name); \
			errno = EIO;                                        \
			return false;                                       \
		}                                                           \
	} while (0)
	GET_UNIFORM(pattern_uniform, "pattern");
	GET_UNIFORM(frame_uniform, "frame_id");
	GET_UNIFORM(width_uniform, "image_width");
	GET_UNIFORM(height_uniform, "image_height");
#undef GET_UNIFORM
	renderer->gl.GenVertexArrays(1, &renderer->vertex_array);
	renderer->gl.GenTextures(1, &renderer->texture);
	renderer->gl.BindTexture(GL_TEXTURE_2D, renderer->texture);
	renderer->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				   GL_NEAREST);
	renderer->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
				   GL_NEAREST);
	renderer->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
				   GL_CLAMP_TO_EDGE);
	renderer->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
				   GL_CLAMP_TO_EDGE);
	renderer->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
				(GLsizei)renderer->width,
				(GLsizei)renderer->height, 0, GL_RGBA,
				GL_UNSIGNED_BYTE, NULL);
	renderer->gl.GenFramebuffers(1, &renderer->framebuffer);
	renderer->gl.BindFramebuffer(GL_FRAMEBUFFER, renderer->framebuffer);
	renderer->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					  GL_TEXTURE_2D, renderer->texture, 0);
	if (!renderer->vertex_array || !renderer->texture ||
	    !renderer->framebuffer ||
	    renderer->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) !=
		    GL_FRAMEBUFFER_COMPLETE ||
	    renderer->gl.GetError() != GL_NO_ERROR) {
		snprintf(renderer->error, sizeof(renderer->error),
			 "create display-test RGBA8 framebuffer failed");
		errno = EIO;
		return false;
	}
	return true;
}

struct fplinux_display_test_gpu *
fplinux_display_test_gpu_open(struct fplinux_drm_session *session, char *error,
			      size_t error_size)
{
	struct fplinux_display_test_gpu *renderer =
		calloc(1, sizeof(*renderer));
	int saved_errno;

	if (error && error_size)
		error[0] = '\0';
	if (!renderer) {
		if (error && error_size)
			snprintf(error, error_size,
				 "allocate display-test GPU: %s",
				 strerror(errno));
		return NULL;
	}
	renderer->session = session;
	if (!session || !session->width || !session->height ||
	    session->width > INT_MAX || session->height > INT_MAX) {
		snprintf(renderer->error, sizeof(renderer->error),
			 "invalid display-test dimensions");
		errno = EINVAL;
		goto fail;
	}
	renderer->width = session->width;
	renderer->height = session->height;
	renderer->output = fplinux_gpu_output_open(
		session, FPLINUX_GPU_GLES3, renderer->width, renderer->height,
		false, renderer->error, sizeof(renderer->error));
	if (!renderer->output)
		goto fail;
	renderer->gpu = fplinux_gpu_output_gpu(renderer->output);
	if (!load_gl(renderer) || !create_scene(renderer))
		goto fail;
	return renderer;
fail:
	saved_errno = errno ? errno : EIO;
	if (error && error_size)
		snprintf(error, error_size, "%s",
			 renderer->error[0] ? renderer->error :
					      strerror(saved_errno));
	fplinux_display_test_gpu_close(renderer);
	errno = saved_errno;
	return NULL;
}

bool fplinux_display_test_gpu_draw(struct fplinux_display_test_gpu *renderer,
				   enum fplinux_display_test_pattern pattern,
				   uint32_t frame_id)
{
	if (!renderer || (unsigned int)pattern > FPLINUX_DISPLAY_TEST_MOTION) {
		errno = EINVAL;
		return false;
	}
	if (renderer->drawn)
		return set_error(renderer, EBUSY,
				 "display-test frame has not been presented");
	renderer->gl.BindFramebuffer(GL_FRAMEBUFFER, renderer->framebuffer);
	renderer->gl.Viewport(0, 0, (GLsizei)renderer->width,
			      (GLsizei)renderer->height);
	renderer->gl.Disable(GL_DITHER);
	renderer->gl.UseProgram(renderer->program);
	renderer->gl.BindVertexArray(renderer->vertex_array);
	renderer->gl.Uniform1i(renderer->pattern_uniform, (GLint)pattern);
	renderer->gl.Uniform1ui(renderer->frame_uniform, frame_id);
	renderer->gl.Uniform1i(renderer->width_uniform, (GLint)renderer->width);
	renderer->gl.Uniform1i(renderer->height_uniform,
			       (GLint)renderer->height);
	renderer->gl.DrawArrays(GL_TRIANGLES, 0, 3);
	if (renderer->gl.GetError() != GL_NO_ERROR)
		return set_error(renderer, EIO,
				 "draw display-test GPU scene failed");
	if (!fplinux_gpu_output_render(renderer->output, renderer->texture))
		return set_error(renderer, errno,
				 fplinux_gpu_output_error(renderer->output));
	renderer->drawn = true;
	return true;
}

bool fplinux_display_test_gpu_present(struct fplinux_display_test_gpu *renderer)
{
	if (!renderer || !renderer->drawn) {
		errno = EINVAL;
		return false;
	}
	if (!fplinux_gpu_output_present(renderer->output))
		return set_error(renderer, errno,
				 fplinux_gpu_output_error(renderer->output));
	renderer->drawn = false;
	return true;
}

bool fplinux_display_test_gpu_verify(struct fplinux_display_test_gpu *renderer,
				     enum fplinux_display_test_pattern pattern,
				     uint32_t frame_id)
{
	uint8_t *reference = NULL;
	void *pixels = NULL;
	size_t stride = 0;
	bool matched = true;
	char mismatch[128];

	if (!renderer || !renderer->drawn ||
	    (unsigned int)pattern > FPLINUX_DISPLAY_TEST_MOTION) {
		errno = EINVAL;
		return false;
	}
	reference = calloc(1, renderer->session->page_bytes);
	if (!reference)
		return set_error(renderer, errno,
				 "allocate GPU reference frame failed");
	if (!fplinux_display_test_draw(reference, renderer->session->page_bytes,
				       renderer->session->stride,
				       renderer->width, renderer->height,
				       pattern, frame_id)) {
		free(reference);
		return set_error(renderer, errno,
				 "draw CPU reference frame failed");
	}
	if (!fplinux_gpu_output_begin_cpu(renderer->output, &pixels, &stride)) {
		free(reference);
		return set_error(renderer, errno,
				 fplinux_gpu_output_error(renderer->output));
	}
	for (unsigned int y = 0; y < renderer->height && matched; ++y) {
		const uint8_t *actual_row =
			(const uint8_t *)pixels + y * stride;
		const uint8_t *expected_row =
			reference + y * renderer->session->stride;

		for (unsigned int x = 0; x < renderer->width; ++x) {
			uint16_t actual =
				(uint16_t)actual_row[x * 2] |
				((uint16_t)actual_row[x * 2 + 1] << 8);
			uint16_t expected =
				(uint16_t)expected_row[x * 2] |
				((uint16_t)expected_row[x * 2 + 1] << 8);

			if (actual != expected) {
				snprintf(
					mismatch, sizeof(mismatch),
					"GPU mismatch at (%u,%u): got 0x%04x, expected 0x%04x",
					x, y, actual, expected);
				matched = false;
				break;
			}
		}
	}
	if (!fplinux_gpu_output_end_cpu(renderer->output)) {
		free(reference);
		return set_error(renderer, errno,
				 fplinux_gpu_output_error(renderer->output));
	}
	free(reference);
	if (!matched)
		return set_error(renderer, EILSEQ, mismatch);
	return true;
}

const char *
fplinux_display_test_gpu_error(const struct fplinux_display_test_gpu *renderer)
{
	return renderer ? renderer->error : "display-test GPU unavailable";
}

bool fplinux_display_test_gpu_close(struct fplinux_display_test_gpu *renderer)
{
	int saved_errno = 0;
	bool ok = true;

	if (!renderer)
		return true;
	if (renderer->gpu) {
		if (renderer->framebuffer)
			renderer->gl.DeleteFramebuffers(1,
							&renderer->framebuffer);
		if (renderer->texture)
			renderer->gl.DeleteTextures(1, &renderer->texture);
		if (renderer->vertex_array)
			renderer->gl.DeleteVertexArrays(
				1, &renderer->vertex_array);
		if (renderer->program)
			renderer->gl.DeleteProgram(renderer->program);
		if (renderer->gl.GetError &&
		    renderer->gl.GetError() != GL_NO_ERROR) {
			saved_errno = EIO;
			ok = false;
		}
	}
	if (renderer->output && !fplinux_gpu_output_close(renderer->output)) {
		if (!saved_errno)
			saved_errno = errno;
		ok = false;
	}
	free(renderer);
	if (!ok)
		errno = saved_errno ? saved_errno : EIO;
	return ok;
}
