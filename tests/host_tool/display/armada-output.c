/* SPDX-License-Identifier: GPL-2.0-only */
/* Linked ARMADA scene with controlled offscreen GPU entry points. */
#define _POSIX_C_SOURCE 200809L
#include "armada-scene.h"
#include "armada-gpu-renderer.h"
#include "armada-storyboard.h"
#include "fplinux-gpu-frame.h"

#include <GLES3/gl3.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBJECT_CAPACITY 128U
#define GUARD_PIXELS 16U
#define GUARD_VALUE 0xa55aU

enum object_kind {
	OBJECT_NONE,
	OBJECT_SHADER,
	OBJECT_PROGRAM,
	OBJECT_BUFFER,
	OBJECT_ARRAY,
	OBJECT_TEXTURE,
	OBJECT_FRAMEBUFFER,
	OBJECT_RENDERBUFFER,
};

struct gl_object {
	enum object_kind kind;
	GLsizei width;
	GLsizei height;
	GLint format;
	GLuint texture;
};

struct fplinux_gpu {
	bool live;
	bool current;
	unsigned int width;
	unsigned int height;
};

static struct gl_object objects[OBJECT_CAPACITY];
static GLuint next_object = 1U;
static GLuint bound_texture;
static GLuint bound_framebuffer;
static GLuint scene_framebuffers[2];
static unsigned int scene_target_count;
static struct fplinux_gpu *live_gpu;
static const char *scenario;
static void *readback_buffer;
static GLenum pending_error;
static bool frame_started;
static GLint pack_alignment;
static GLint pack_row_length;
static GLint pack_skip_pixels;
static GLint pack_skip_rows;
static GLuint pack_buffer;

static bool fails(const char *name)
{
	return strcmp(scenario, name) == 0;
}

static void assert_pack_state_restored(void)
{
	assert(pack_alignment == 8 && pack_row_length == 449);
	assert(pack_skip_pixels == 2 && pack_skip_rows == 3 &&
	       pack_buffer == 47U);
}

FILE *__real_fopen(const char *path, const char *mode);

FILE *__wrap_fopen(const char *path, const char *mode)
{
	const char *root = getenv("FPLINUX_ARMADA_TEST_SHADER_ROOT");
	const char *name = strrchr(path, '/');
	char replacement[4096];

	if (!root || !name ||
	    (strcmp(name + 1, "armada-scene.vert") &&
	     strcmp(name + 1, "armada-scene.frag")))
		return __real_fopen(path, mode);
	assert(!fails("missing-shaders-software"));
	assert(snprintf(replacement, sizeof(replacement), "%s/%s", root,
			name + 1) < (int)sizeof(replacement));
	return __real_fopen(replacement, mode);
}

static GLuint allocate_object(enum object_kind kind)
{
	GLuint object = next_object++;

	assert(live_gpu && live_gpu->live);
	assert(object < OBJECT_CAPACITY);
	objects[object].kind = kind;
	return object;
}

static void delete_object(GLuint object, enum object_kind kind)
{
	assert(live_gpu && live_gpu->live);
	assert(object < OBJECT_CAPACITY && objects[object].kind == kind);
	objects[object].kind = OBJECT_NONE;
}

static void assert_scene_resources_released(void)
{
	for (GLuint object = 1U; object < next_object; ++object)
		assert(objects[object].kind == OBJECT_NONE);
}

GLuint glCreateShader(GLenum type)
{
	assert(type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER);
	return allocate_object(OBJECT_SHADER);
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings,
		    const GLint *lengths)
{
	assert(objects[shader].kind == OBJECT_SHADER && count > 0);
	for (GLsizei index = 0; index < count; ++index)
		assert(strings[index] && lengths[index] != 0);
}

void glCompileShader(GLuint shader)
{
	assert(objects[shader].kind == OBJECT_SHADER);
}

void glGetShaderiv(GLuint shader, GLenum parameter, GLint *value)
{
	assert(objects[shader].kind == OBJECT_SHADER);
	assert(parameter == GL_COMPILE_STATUS);
	*value = fails("shader-failure") ? GL_FALSE : GL_TRUE;
}

void glGetShaderInfoLog(GLuint shader, GLsizei size, GLsizei *length,
			GLchar *log)
{
	assert(objects[shader].kind == OBJECT_SHADER && !length);
	snprintf(log, (size_t)size, "controlled shader failure");
}

void glDeleteShader(GLuint shader)
{
	delete_object(shader, OBJECT_SHADER);
}

GLuint glCreateProgram(void)
{
	return allocate_object(OBJECT_PROGRAM);
}

void glAttachShader(GLuint program, GLuint shader)
{
	assert(objects[program].kind == OBJECT_PROGRAM);
	assert(objects[shader].kind == OBJECT_SHADER);
}

void glLinkProgram(GLuint program)
{
	assert(objects[program].kind == OBJECT_PROGRAM);
}

void glGetProgramiv(GLuint program, GLenum parameter, GLint *value)
{
	assert(objects[program].kind == OBJECT_PROGRAM);
	assert(parameter == GL_LINK_STATUS);
	*value = fails("link-failure") ? GL_FALSE : GL_TRUE;
}

void glGetProgramInfoLog(GLuint program, GLsizei size, GLsizei *length,
			 GLchar *log)
{
	assert(objects[program].kind == OBJECT_PROGRAM && !length);
	snprintf(log, (size_t)size, "controlled link failure");
}

void glDeleteProgram(GLuint program)
{
	delete_object(program, OBJECT_PROGRAM);
}

void glUseProgram(GLuint program)
{
	assert(objects[program].kind == OBJECT_PROGRAM);
}

GLint glGetUniformLocation(GLuint program, const GLchar *name)
{
	assert(objects[program].kind == OBJECT_PROGRAM && name[0]);
	return 1;
}

void glUniform1i(GLint location, GLint value)
{
	(void)location;
	(void)value;
}

void glUniform1ui(GLint location, GLuint value)
{
	(void)location;
	(void)value;
}

void glUniform1f(GLint location, GLfloat value)
{
	(void)location;
	(void)value;
}

void glUniform1fv(GLint location, GLsizei count, const GLfloat *value)
{
	(void)location;
	assert(count > 0 && value);
}

void glUniform2f(GLint location, GLfloat x, GLfloat y)
{
	(void)location;
	(void)x;
	(void)y;
}

void glUniform2fv(GLint location, GLsizei count, const GLfloat *value)
{
	(void)location;
	assert(count > 0 && value);
}

void glUniform3f(GLint location, GLfloat x, GLfloat y, GLfloat z)
{
	(void)location;
	(void)x;
	(void)y;
	(void)z;
}

void glUniform3fv(GLint location, GLsizei count, const GLfloat *value)
{
	(void)location;
	assert(count > 0 && value);
}

void glUniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	(void)location;
	(void)x;
	(void)y;
	(void)z;
	(void)w;
}

void glUniform4fv(GLint location, GLsizei count, const GLfloat *value)
{
	(void)location;
	assert(count > 0 && value);
}

void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose,
			const GLfloat *value)
{
	(void)location;
	assert(count > 0 && transpose == GL_FALSE && value);
}

void glGenBuffers(GLsizei count, GLuint *buffers)
{
	for (GLsizei index = 0; index < count; ++index)
		buffers[index] = allocate_object(OBJECT_BUFFER);
}

void glBindBuffer(GLenum target, GLuint buffer)
{
	if (target == GL_PIXEL_PACK_BUFFER) {
		assert(buffer == 0U || buffer == 47U);
		pack_buffer = buffer;
		return;
	}
	assert(target == GL_ARRAY_BUFFER &&
	       objects[buffer].kind == OBJECT_BUFFER);
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data,
		  GLenum usage)
{
	assert(target == GL_ARRAY_BUFFER && size > 0 && data);
	assert(usage == GL_STATIC_DRAW);
}

void glDeleteBuffers(GLsizei count, const GLuint *buffers)
{
	for (GLsizei index = 0; index < count; ++index)
		delete_object(buffers[index], OBJECT_BUFFER);
}

void glGenVertexArrays(GLsizei count, GLuint *arrays)
{
	for (GLsizei index = 0; index < count; ++index)
		arrays[index] = allocate_object(OBJECT_ARRAY);
}

void glBindVertexArray(GLuint array)
{
	assert(!array || objects[array].kind == OBJECT_ARRAY);
}

void glDeleteVertexArrays(GLsizei count, const GLuint *arrays)
{
	for (GLsizei index = 0; index < count; ++index)
		delete_object(arrays[index], OBJECT_ARRAY);
}

void glEnableVertexAttribArray(GLuint index)
{
	(void)index;
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
			   GLboolean normalized, GLsizei stride,
			   const void *pointer)
{
	(void)index;
	(void)size;
	(void)type;
	(void)normalized;
	(void)stride;
	(void)pointer;
}

void glGenTextures(GLsizei count, GLuint *textures)
{
	for (GLsizei index = 0; index < count; ++index)
		textures[index] = allocate_object(OBJECT_TEXTURE);
}

void glBindTexture(GLenum target, GLuint texture)
{
	assert(target == GL_TEXTURE_2D);
	assert(!texture || objects[texture].kind == OBJECT_TEXTURE);
	bound_texture = texture;
}

void glActiveTexture(GLenum texture)
{
	assert(texture == GL_TEXTURE0 || texture == GL_TEXTURE1);
}

void glTexParameteri(GLenum target, GLenum parameter, GLint value)
{
	assert(target == GL_TEXTURE_2D);
	(void)parameter;
	(void)value;
}

void glTexImage2D(GLenum target, GLint level, GLint format, GLsizei width,
		  GLsizei height, GLint border, GLenum external_format,
		  GLenum type, const void *pixels)
{
	assert(target == GL_TEXTURE_2D && level == 0 && border == 0);
	assert(external_format == GL_RGBA && type == GL_UNSIGNED_BYTE &&
	       !pixels);
	objects[bound_texture].width = width;
	objects[bound_texture].height = height;
	objects[bound_texture].format = format;
}

void glDeleteTextures(GLsizei count, const GLuint *textures)
{
	for (GLsizei index = 0; index < count; ++index)
		delete_object(textures[index], OBJECT_TEXTURE);
}

void glGenFramebuffers(GLsizei count, GLuint *framebuffers)
{
	for (GLsizei index = 0; index < count; ++index)
		framebuffers[index] = allocate_object(OBJECT_FRAMEBUFFER);
}

void glBindFramebuffer(GLenum target, GLuint framebuffer)
{
	assert(target == GL_FRAMEBUFFER);
	assert(!framebuffer || objects[framebuffer].kind == OBJECT_FRAMEBUFFER);
	bound_framebuffer = framebuffer;
}

void glFramebufferTexture2D(GLenum target, GLenum attachment,
			    GLenum texture_target, GLuint texture, GLint level)
{
	assert(target == GL_FRAMEBUFFER && attachment == GL_COLOR_ATTACHMENT0);
	assert(texture_target == GL_TEXTURE_2D && level == 0);
	objects[bound_framebuffer].texture = texture;
	if (objects[texture].width == (GLsizei)live_gpu->width &&
	    objects[texture].height == (GLsizei)live_gpu->height) {
		assert(scene_target_count < 2U);
		scene_framebuffers[scene_target_count++] = bound_framebuffer;
	}
}

GLenum glCheckFramebufferStatus(GLenum target)
{
	assert(target == GL_FRAMEBUFFER);
	return fails("target-failure") ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT :
					 GL_FRAMEBUFFER_COMPLETE;
}

void glDeleteFramebuffers(GLsizei count, const GLuint *framebuffers)
{
	for (GLsizei index = 0; index < count; ++index)
		delete_object(framebuffers[index], OBJECT_FRAMEBUFFER);
}

void glGenRenderbuffers(GLsizei count, GLuint *renderbuffers)
{
	for (GLsizei index = 0; index < count; ++index)
		renderbuffers[index] = allocate_object(OBJECT_RENDERBUFFER);
}

void glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
	assert(target == GL_RENDERBUFFER);
	assert(objects[renderbuffer].kind == OBJECT_RENDERBUFFER);
}

void glRenderbufferStorage(GLenum target, GLenum format, GLsizei width,
			   GLsizei height)
{
	assert(target == GL_RENDERBUFFER && format == GL_DEPTH_COMPONENT16);
	assert(width > 0 && height > 0);
}

void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
			       GLenum renderbuffer_target, GLuint renderbuffer)
{
	assert(target == GL_FRAMEBUFFER && attachment == GL_DEPTH_ATTACHMENT);
	assert(renderbuffer_target == GL_RENDERBUFFER);
	assert(objects[renderbuffer].kind == OBJECT_RENDERBUFFER);
}

void glDeleteRenderbuffers(GLsizei count, const GLuint *renderbuffers)
{
	for (GLsizei index = 0; index < count; ++index)
		delete_object(renderbuffers[index], OBJECT_RENDERBUFFER);
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	assert(x >= 0 && y >= 0 && width > 0 && height > 0);
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	assert(x >= 0 && y >= 0 && width > 0 && height > 0);
}

void glEnable(GLenum capability)
{
	(void)capability;
}

void glDisable(GLenum capability)
{
	(void)capability;
}

void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	(void)red;
	(void)green;
	(void)blue;
	(void)alpha;
}

void glClear(GLbitfield mask)
{
	assert(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
}

void glDepthFunc(GLenum function)
{
	assert(function == GL_LESS);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	assert(mode == GL_TRIANGLES && first >= 0 && count > 0);
	frame_started = true;
	if (fails("render-failure"))
		pending_error = GL_INVALID_OPERATION;
}

GLenum glGetError(void)
{
	GLenum error = pending_error;

	pending_error = GL_NO_ERROR;
	return error;
}

static GLint *pack_parameter(GLenum parameter)
{
	switch (parameter) {
	case GL_PACK_ALIGNMENT:
		return &pack_alignment;
	case GL_PACK_ROW_LENGTH:
		return &pack_row_length;
	case GL_PACK_SKIP_PIXELS:
		return &pack_skip_pixels;
	case GL_PACK_SKIP_ROWS:
		return &pack_skip_rows;
	default:
		assert(0 && "unexpected pixel-pack parameter");
		return NULL;
	}
}

void glGetIntegerv(GLenum parameter, GLint *value)
{
	if (parameter == GL_PIXEL_PACK_BUFFER_BINDING)
		*value = (GLint)pack_buffer;
	else
		*value = *pack_parameter(parameter);
}

void glPixelStorei(GLenum parameter, GLint value)
{
	*pack_parameter(parameter) = value;
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
		  GLenum format, GLenum type, void *pixels)
{
	const struct gl_object *texture =
		&objects[objects[bound_framebuffer].texture];
	uint8_t *rgba = pixels;

	assert(live_gpu && live_gpu->live && frame_started);
	assert(x == 0 && y == 0 && width == (GLsizei)live_gpu->width &&
	       height == (GLsizei)live_gpu->height);
	assert(format == GL_RGBA && type == GL_UNSIGNED_BYTE && pixels);
	assert(pack_alignment == 1 && !pack_row_length && !pack_skip_pixels &&
	       !pack_skip_rows && !pack_buffer);
	assert(texture->kind == OBJECT_TEXTURE && texture->format == GL_RGBA8);
	assert(texture->width == width && texture->height == height);
	assert(scene_target_count == 2U &&
	       bound_framebuffer == scene_framebuffers[0]);
	if (readback_buffer)
		assert(readback_buffer == pixels);
	readback_buffer = pixels;
	if (fails("readback-failure")) {
		memset(rgba, 0xff, (size_t)width * (size_t)height * 4U);
		pending_error = GL_INVALID_OPERATION;
		return;
	}
	memset(rgba, 0, (size_t)width * (size_t)height * 4U);
	memcpy(rgba, (const uint8_t[]){ 255, 0, 0, 255 }, 4U);
	memcpy(rgba + (size_t)(width - 1) * 4U,
	       (const uint8_t[]){ 0, 255, 0, 19 }, 4U);
	memcpy(rgba + (size_t)(height - 1) * (size_t)width * 4U,
	       (const uint8_t[]){ 0, 0, 255, 0 }, 4U);
	memcpy(rgba + ((size_t)height * (size_t)width - 1U) * 4U,
	       (const uint8_t[]){ 255, 255, 255, 255 }, 4U);
	memcpy(rgba + ((size_t)width + 1U) * 4U,
	       (const uint8_t[]){ 128, 64, 192, 3 }, 4U);
	memcpy(rgba + ((size_t)width * 2U + 1U) * 4U,
	       (const uint8_t[]){ 4, 2, 4, 255 }, 4U);
	memcpy(rgba + ((size_t)width * 2U + 2U) * 4U,
	       (const uint8_t[]){ 5, 3, 5, 255 }, 4U);
}

void *fplinux_gpu_get_proc(struct fplinux_gpu *gpu, const char *name)
{
	assert(gpu && gpu->live);
	if (fails("missing-procedure") && strcmp(name, "glReadPixels") == 0)
		return NULL;
#define PROCEDURE(member)                    \
	if (strcmp(name, "gl" #member) == 0) \
	return (void *)gl##member
	PROCEDURE(CreateShader);
	PROCEDURE(ShaderSource);
	PROCEDURE(CompileShader);
	PROCEDURE(GetShaderiv);
	PROCEDURE(GetShaderInfoLog);
	PROCEDURE(DeleteShader);
	PROCEDURE(CreateProgram);
	PROCEDURE(AttachShader);
	PROCEDURE(LinkProgram);
	PROCEDURE(GetProgramiv);
	PROCEDURE(GetProgramInfoLog);
	PROCEDURE(DeleteProgram);
	PROCEDURE(UseProgram);
	PROCEDURE(GetUniformLocation);
	PROCEDURE(Uniform1i);
	PROCEDURE(Uniform1ui);
	PROCEDURE(Uniform1f);
	PROCEDURE(Uniform1fv);
	PROCEDURE(Uniform2f);
	PROCEDURE(Uniform2fv);
	PROCEDURE(Uniform3f);
	PROCEDURE(Uniform3fv);
	PROCEDURE(Uniform4f);
	PROCEDURE(Uniform4fv);
	PROCEDURE(UniformMatrix3fv);
	PROCEDURE(GenBuffers);
	PROCEDURE(BindBuffer);
	PROCEDURE(BufferData);
	PROCEDURE(DeleteBuffers);
	PROCEDURE(GenVertexArrays);
	PROCEDURE(BindVertexArray);
	PROCEDURE(DeleteVertexArrays);
	PROCEDURE(EnableVertexAttribArray);
	PROCEDURE(VertexAttribPointer);
	PROCEDURE(GenTextures);
	PROCEDURE(BindTexture);
	PROCEDURE(ActiveTexture);
	PROCEDURE(TexParameteri);
	PROCEDURE(TexImage2D);
	PROCEDURE(DeleteTextures);
	PROCEDURE(GenFramebuffers);
	PROCEDURE(BindFramebuffer);
	PROCEDURE(FramebufferTexture2D);
	PROCEDURE(CheckFramebufferStatus);
	PROCEDURE(DeleteFramebuffers);
	PROCEDURE(GenRenderbuffers);
	PROCEDURE(BindRenderbuffer);
	PROCEDURE(RenderbufferStorage);
	PROCEDURE(FramebufferRenderbuffer);
	PROCEDURE(DeleteRenderbuffers);
	PROCEDURE(Viewport);
	PROCEDURE(Scissor);
	PROCEDURE(Enable);
	PROCEDURE(Disable);
	PROCEDURE(ClearColor);
	PROCEDURE(Clear);
	PROCEDURE(DepthFunc);
	PROCEDURE(DrawArrays);
	PROCEDURE(ReadPixels);
	PROCEDURE(GetIntegerv);
	PROCEDURE(PixelStorei);
	PROCEDURE(GetError);
#undef PROCEDURE
	return NULL;
}

bool fplinux_gpu_is_current(const struct fplinux_gpu *gpu)
{
	return gpu && gpu->live && gpu->current;
}

const char *fplinux_gpu_device(const struct fplinux_gpu *gpu)
{
	assert(gpu->live);
	return "controlled render node";
}

const char *fplinux_gpu_renderer(const struct fplinux_gpu *gpu)
{
	assert(gpu->live);
	return "controlled GLES3 renderer";
}

const char *fplinux_gpu_version(const struct fplinux_gpu *gpu)
{
	assert(gpu->live);
	return "OpenGL ES 3.0";
}

struct fplinux_gpu *fplinux_gpu_open(enum fplinux_gpu_api api,
				     unsigned int width, unsigned int height,
				     const char *device, char *error,
				     size_t error_size)
{
	assert(!live_gpu && api == FPLINUX_GPU_GLES3 && !device);
	assert(width > 0U && height > 0U);
	if (fails("context-failure") || fails("forced-gpu-failure")) {
		snprintf(error, error_size, "controlled context failure");
		errno = ENODEV;
		return NULL;
	}
	live_gpu = calloc(1, sizeof(*live_gpu));
	assert(live_gpu);
	live_gpu->width = width;
	live_gpu->height = height;
	live_gpu->live = true;
	live_gpu->current = true;
	pack_alignment = 8;
	pack_row_length = 449;
	pack_skip_pixels = 2;
	pack_skip_rows = 3;
	pack_buffer = 47U;
	frame_started = false;
	readback_buffer = NULL;
	scene_target_count = 0U;
	return live_gpu;
}

void fplinux_gpu_close(struct fplinux_gpu *gpu)
{
	if (!gpu)
		return;
	assert(gpu == live_gpu && gpu->live);
	assert_pack_state_restored();
	assert_scene_resources_released();
	gpu->live = false;
	free(gpu);
	live_gpu = NULL;
}

static void assert_readback(const uint16_t *pixels, unsigned int width,
			    unsigned int height)
{
	assert(pixels[0] == 0x001fU);
	assert(pixels[width - 1U] == 0xffffU);
	assert(pixels[(size_t)(height - 1U) * width] == 0xf800U);
	assert(pixels[(size_t)height * width - 1U] == 0x07e0U);
	assert(pixels[(size_t)(height - 2U) * width + 1U] == 0x8217U);
	assert(pixels[(size_t)(height - 3U) * width + 1U] == 0x0000U);
	assert(pixels[(size_t)(height - 3U) * width + 2U] == 0x0821U);
	assert(pixels[width + 1U] == 0x0000U);
}

static void guarded_readback(unsigned int width, unsigned int height)
{
	struct armada_storyboard storyboard = { 0 };
	struct armada_frame frame;
	struct armada_outputs cues;
	struct armada_gpu_renderer *renderer;
	size_t count = (size_t)width * height;
	uint16_t *storage =
		malloc((count + 2U * GUARD_PIXELS) * sizeof(*storage));
	uint16_t *pixels;
	char error[512] = { 0 };

	assert(storage);
	for (size_t index = 0; index < count + 2U * GUARD_PIXELS; ++index)
		storage[index] = GUARD_VALUE;
	pixels = storage + GUARD_PIXELS;
	renderer = armada_gpu_renderer_create(width, height, pixels, error,
					      sizeof(error));
	assert(renderer);
	armada_storyboard_update(&storyboard, 720U, &frame, &cues);
	assert(armada_gpu_renderer_render(renderer, &frame));
	assert_pack_state_restored();
	assert_readback(pixels, width, height);
	memset(pixels, 0xa5, count * sizeof(*pixels));
	assert(armada_gpu_renderer_render(renderer, &frame));
	assert_pack_state_restored();
	assert_readback(pixels, width, height);
	for (size_t index = 0; index < GUARD_PIXELS; ++index) {
		assert(storage[index] == GUARD_VALUE);
		assert(storage[GUARD_PIXELS + count + index] == GUARD_VALUE);
	}
	assert(armada_gpu_renderer_destroy(renderer));
	free(storage);
}

static void frame_contract(unsigned int width, unsigned int height)
{
	size_t count = (size_t)width * height;
	uint16_t *pixels = malloc(count * sizeof(*pixels));
	struct fplinux_gpu_frame *frame;
	char error[256] = { 0 };

	assert(pixels && live_gpu);
	for (size_t index = 0; index < count; ++index)
		pixels[index] = GUARD_VALUE;
	if (fails("frame-invalid-dimensions")) {
		assert(!fplinux_gpu_frame_open(live_gpu, 0U, height, error,
					       sizeof(error)));
		assert(strstr(error, "dimensions"));
		assert(!fplinux_gpu_frame_open(live_gpu, UINT32_MAX, UINT32_MAX,
					       error, sizeof(error)));
		assert(strstr(error, "dimensions"));
	}
	frame = fplinux_gpu_frame_open(live_gpu, width, height, error,
				       sizeof(error));
	assert(frame && !error[0]);
	if (fails("frame-small-output")) {
		assert(!fplinux_gpu_frame_read_rgb565(frame, pixels, count - 1U,
						      error, sizeof(error)));
		assert(strstr(error, "capacity"));
	} else if (fails("frame-wrong-context")) {
		live_gpu->current = false;
		assert(!fplinux_gpu_frame_open(live_gpu, width, height, error,
					       sizeof(error)));
		assert(strstr(error, "context is not current"));
		assert(!fplinux_gpu_frame_read_rgb565(frame, pixels, count,
						      error, sizeof(error)));
		assert(strstr(error, "context is not current"));
		live_gpu->current = true;
	} else if (fails("frame-pending-error")) {
		pending_error = GL_INVALID_OPERATION;
		assert(!fplinux_gpu_frame_read_rgb565(frame, pixels, count,
						      error, sizeof(error)));
		assert(strstr(error, "pending GL error"));
	}
	assert_pack_state_restored();
	for (size_t index = 0; index < count; ++index)
		assert(pixels[index] == GUARD_VALUE);
	/* The second adapter has its own reusable scratch, on the same GPU. */
	readback_buffer = NULL;
	assert(fplinux_gpu_frame_read_rgb565(frame, pixels, count, error,
					     sizeof(error)));
	assert(!error[0]);
	assert_readback(pixels, width, height);
	assert_pack_state_restored();
	fplinux_gpu_frame_close(frame);
	free(pixels);
}

int main(int argc, char **argv)
{
	struct armada_scene *scene;
	struct armada_outputs cues;
	unsigned int width;
	unsigned int height;
	enum armada_render_mode mode;
	char error[512] = { 0 };

	assert(argc == 4);
	scenario = argv[1];
	width = (unsigned int)strtoul(argv[2], NULL, 10);
	height = (unsigned int)strtoul(argv[3], NULL, 10);
	if (fails("forced-gpu-failure") || fails("missing-shaders-gpu"))
		mode = ARMADA_RENDER_GPU;
	else if (fails("missing-shaders-software"))
		mode = ARMADA_RENDER_SOFTWARE;
	else
		mode = ARMADA_RENDER_AUTO;
	scene = armada_scene_create_with_renderer(width, height, mode, error,
						  sizeof(error));
	if (fails("forced-gpu-failure") || fails("missing-shaders-gpu")) {
		assert(!scene && error[0]);
		if (fails("missing-shaders-gpu")) {
			assert(strstr(error, "cannot read ARMADA shader"));
			assert(strstr(error, "No such file or directory"));
		}
	} else if (fails("context-failure") || fails("missing-procedure") ||
		   fails("shader-failure") || fails("link-failure") ||
		   fails("target-failure") || fails("missing-shaders-auto") ||
		   fails("missing-shaders-software")) {
		const uint16_t *pixels;
		size_t count = (size_t)width * height;
		size_t different = 0;

		assert(scene && !live_gpu);
		assert(armada_scene_pixels(scene));
		assert(strcmp(armada_scene_renderer(scene), "software") == 0);
		if (fails("missing-shaders-software"))
			assert(!armada_scene_fallback_reason(scene)[0]);
		else
			assert(armada_scene_fallback_reason(scene)[0]);
		if (fails("missing-shaders-auto")) {
			assert(strstr(armada_scene_fallback_reason(scene),
				      "cannot read ARMADA shader"));
			assert(strstr(armada_scene_fallback_reason(scene),
				      "No such file or directory"));
		}
		assert(armada_scene_render(scene, 0U, &cues));
		pixels = armada_scene_pixels(scene);
		for (size_t index = 1; index < count; ++index)
			if (pixels[index] != pixels[0])
				++different;
		assert(different > count / 16U);
	} else {
		assert(scene && live_gpu && armada_scene_pixels(scene));
		assert(strncmp(armada_scene_renderer(scene), "gpu (", 5U) == 0);
		assert(!armada_scene_fallback_reason(scene)[0]);
		if (fails("render-failure") || fails("readback-failure")) {
			const uint16_t *pixels = armada_scene_pixels(scene);

			assert(!armada_scene_render(scene, 720U, &cues));
			assert(strstr(armada_scene_error(scene),
				      fails("render-failure") ?
					      "scene rendering" :
					      "frame readback"));
			assert_pack_state_restored();
			for (size_t index = 0; index < (size_t)width * height;
			     ++index)
				assert(pixels[index] == 0U);
		} else {
			assert(armada_scene_render(scene, 720U, &cues));
			assert_pack_state_restored();
			assert(cues.keypad && cues.rumble && cues.cue_id == 1U);
			assert(cues.rumble_remaining_ms == 100U);
			assert_readback(armada_scene_pixels(scene), width,
					height);
			assert(armada_scene_render(scene, 0U, &cues));
			assert_pack_state_restored();
			assert_readback(armada_scene_pixels(scene), width,
					height);
			if (strncmp(scenario, "frame-", 6U) == 0)
				frame_contract(width, height);
		}
	}
	assert(armada_scene_destroy(scene));
	assert(!live_gpu);
	assert_scene_resources_released();
	if (fails("render"))
		guarded_readback(width, height);
	return EXIT_SUCCESS;
}
