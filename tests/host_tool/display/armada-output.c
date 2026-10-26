/* SPDX-License-Identifier: GPL-2.0-only */
/* Linked ARMADA scene with controlled GPU and native-output boundaries. */
#define _POSIX_C_SOURCE 200809L
#include "armada-scene.h"
#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"

#include <GLES3/gl3.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBJECT_CAPACITY 128U
#define GUARD_BYTES 32U
#define GUARD_VALUE 0xa5
#define BACKGROUND 0x1234U

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
};

struct fplinux_gpu_output {
	struct fplinux_drm_session *display;
	struct fplinux_gpu gpu;
	unsigned int width;
	unsigned int height;
	unsigned int page;
	bool ready;
	bool cpu_access;
};

static struct gl_object objects[OBJECT_CAPACITY];
static GLuint next_object = 1U;
static GLuint bound_texture;
static GLuint bound_framebuffer;
static struct fplinux_gpu_output *live_output;
static const char *scenario;
static const char *output_error = "controlled output failure";

static bool fails(const char *name)
{
	return strcmp(scenario, name) == 0;
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

	assert(live_output && live_output->gpu.live);
	assert(object < OBJECT_CAPACITY);
	objects[object].kind = kind;
	return object;
}

static void delete_object(GLuint object, enum object_kind kind)
{
	assert(live_output && live_output->gpu.live);
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
}

GLenum glCheckFramebufferStatus(GLenum target)
{
	assert(target == GL_FRAMEBUFFER);
	return fails("target-failure") || fails("cleanup-failure") ?
		       GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT :
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
}

GLenum glGetError(void)
{
	return GL_NO_ERROR;
}

void *fplinux_gpu_get_proc(struct fplinux_gpu *gpu, const char *name)
{
	assert(gpu && gpu->live);
	if (fails("missing-procedure") && strcmp(name, "glTexParameteri") == 0)
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
	PROCEDURE(GetError);
#undef PROCEDURE
	return NULL;
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

struct fplinux_gpu_output *
fplinux_gpu_output_open(struct fplinux_drm_session *display,
			enum fplinux_gpu_api api, unsigned int width,
			unsigned int height, bool clockwise, char *error,
			size_t error_size)
{
	assert(!live_output && display && display->active);
	assert(api == FPLINUX_GPU_GLES3 && !clockwise);
	assert(width == display->width && height == display->height);
	if (fails("output-failure") || fails("forced-gpu-failure")) {
		snprintf(error, error_size, "%s", output_error);
		errno = ENODEV;
		return NULL;
	}
	live_output = calloc(1, sizeof(*live_output));
	assert(live_output);
	live_output->display = display;
	live_output->width = width;
	live_output->height = height;
	live_output->gpu.live = true;
	return live_output;
}

struct fplinux_gpu *
fplinux_gpu_output_gpu(const struct fplinux_gpu_output *output)
{
	return output ? (struct fplinux_gpu *)&output->gpu : NULL;
}

bool fplinux_gpu_output_render(struct fplinux_gpu_output *output,
			       unsigned int texture)
{
	struct gl_object *source = &objects[texture];

	assert(output == live_output && !output->ready && !output->cpu_access);
	assert(output->display->active && source->kind == OBJECT_TEXTURE);
	assert(source->width == (GLsizei)output->width);
	assert(source->height == (GLsizei)output->height &&
	       source->format == GL_RGBA8);
	assert(objects[bound_framebuffer].texture == texture);
	if (fails("render-failure"))
		return false;
	output->page = 1U - output->display->shown_page;
	for (unsigned int row = 0; row < output->height; ++row) {
		uint16_t *pixels =
			(uint16_t *)(output->display->mapping +
				     output->page *
					     output->display->page_bytes +
				     row * output->display->stride);

		for (unsigned int column = 0; column < output->width; ++column)
			pixels[column] = BACKGROUND;
	}
	output->ready = true;
	return true;
}

bool fplinux_gpu_output_begin_cpu(struct fplinux_gpu_output *output,
				  void **pixels, size_t *stride)
{
	assert(output == live_output && output->ready && !output->cpu_access);
	if (fails("begin-failure"))
		return false;
	output->cpu_access = true;
	*pixels = output->display->mapping +
		  output->page * output->display->page_bytes;
	*stride = output->display->stride;
	return true;
}

bool fplinux_gpu_output_end_cpu(struct fplinux_gpu_output *output)
{
	assert(output == live_output && output->cpu_access);
	if (fails("end-failure"))
		return false;
	output->cpu_access = false;
	return true;
}

bool fplinux_gpu_output_present(struct fplinux_gpu_output *output)
{
	assert(output == live_output && output->ready && !output->cpu_access);
	output->display->shown_page = output->page;
	output->ready = false;
	return true;
}

const char *fplinux_gpu_output_error(const struct fplinux_gpu_output *output)
{
	assert(output == live_output);
	return output_error;
}

bool fplinux_gpu_output_close(struct fplinux_gpu_output *output)
{
	if (!output)
		return true;
	assert(output == live_output && output->gpu.live);
	assert_scene_resources_released();
	output->gpu.live = false;
	free(output);
	live_output = NULL;
	if (fails("cleanup-failure")) {
		errno = EIO;
		return false;
	}
	return true;
}

static void assert_untouched(const void *buffer, size_t size)
{
	const unsigned char *bytes = buffer;

	for (size_t index = 0; index < size; ++index)
		assert(bytes[index] == GUARD_VALUE);
}

static void assert_page(const struct fplinux_drm_session *display,
			unsigned int page, unsigned int glyph_height)
{
	const uint8_t *pixels = display->mapping + page * display->page_bytes;
	size_t white = 0;
	size_t cyan = 0;

	for (unsigned int row = 0; row < display->height; ++row) {
		const uint16_t *line =
			(const uint16_t *)(pixels + row * display->stride);

		for (unsigned int column = 0; column < display->width;
		     ++column) {
			assert(line[column] == BACKGROUND ||
			       line[column] == 0xe7bfU ||
			       line[column] == 0x3f1fU);
			if (line[column] == 0xe7bfU) {
				assert(row >= display->height / 12U &&
				       row < display->height / 12U +
						       glyph_height);
				++white;
			}
			if (line[column] == 0x3f1fU) {
				assert(row >= display->height - glyph_height -
						       2U &&
				       row < display->height - 2U);
				++cyan;
			}
		}
		assert_untouched(pixels + row * display->stride +
					 display->width * 2U,
				 display->stride - display->width * 2U);
	}
	assert(white == (display->width == 128U ? 936U : 1664U));
	assert(cyan == (display->width == 128U ? 1296U : 2304U));
}

int main(int argc, char **argv)
{
	struct fplinux_drm_session display = { .pages = 2U, .active = true };
	struct fplinux_font font = { 0 };
	struct armada_scene *scene;
	struct armada_outputs cues;
	uint8_t *storage;
	enum armada_render_mode mode;
	char error[512] = { 0 };

	assert(argc == 5);
	scenario = argv[1];
	display.width = (uint32_t)strtoul(argv[2], NULL, 10);
	display.height = (uint32_t)strtoul(argv[3], NULL, 10);
	display.stride = display.width * 2U + 32U;
	display.page_bytes = display.stride * display.height;
	display.size = 2U * display.page_bytes;
	storage = malloc(display.size + 2U * GUARD_BYTES);
	assert(storage && fplinux_font_open(&font, argv[4]));
	memset(storage, GUARD_VALUE, display.size + 2U * GUARD_BYTES);
	display.mapping = storage + GUARD_BYTES;
	if (fails("forced-gpu-failure") || fails("missing-shaders-gpu"))
		mode = ARMADA_RENDER_GPU;
	else if (fails("missing-shaders-software"))
		mode = ARMADA_RENDER_SOFTWARE;
	else
		mode = ARMADA_RENDER_AUTO;
	scene = armada_scene_create_with_renderer(display.width, display.height,
						  &font, &display, mode, error,
						  sizeof(error));
	if (fails("forced-gpu-failure") || fails("cleanup-failure") ||
	    fails("missing-shaders-gpu")) {
		assert(!scene && error[0]);
		if (fails("cleanup-failure"))
			assert(strstr(error, "GPU cleanup failed"));
		if (fails("missing-shaders-gpu")) {
			assert(strstr(error, "cannot read ARMADA shader"));
			assert(strstr(error, "No such file or directory"));
		}
	} else if (fails("output-failure") || fails("missing-procedure") ||
		   fails("shader-failure") || fails("link-failure") ||
		   fails("target-failure") || fails("missing-shaders-auto") ||
		   fails("missing-shaders-software")) {
		const uint16_t *pixels;
		size_t count = (size_t)display.width * display.height;
		size_t different = 0;

		assert(scene && !live_output && !armada_scene_output(scene));
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
		assert(armada_scene_render(scene, 0U, NULL, &cues));
		pixels = armada_scene_pixels(scene);
		for (size_t index = 1; index < count; ++index)
			if (pixels[index] != pixels[0])
				++different;
		assert(different > count / 16U);
		assert_untouched(display.mapping, display.size);
	} else {
		assert(scene && armada_scene_output(scene) == live_output);
		assert(!armada_scene_pixels(scene));
		assert(strncmp(armada_scene_renderer(scene), "gpu (", 5U) == 0);
		assert(!armada_scene_fallback_reason(scene)[0]);
		if (fails("render-failure") || fails("begin-failure") ||
		    fails("end-failure")) {
			assert(!armada_scene_render(scene, 720U, NULL, &cues));
			assert(strcmp(armada_scene_error(scene),
				      output_error) == 0);
			assert(display.shown_page == 0U);
		} else {
			assert(armada_scene_render(scene, 720U, NULL, &cues));
			assert(cues.keypad && cues.rumble && cues.cue_id == 1U);
			assert(display.shown_page == 0U);
			assert_page(&display, 1U, font.height);
			assert_untouched(display.mapping, display.page_bytes);
			assert(fplinux_gpu_output_present(
				armada_scene_output(scene)));
			assert(display.shown_page == 1U);
			assert(armada_scene_render(scene, 0U, NULL, &cues));
			assert_page(&display, 0U, font.height);
			assert(fplinux_gpu_output_present(
				armada_scene_output(scene)));
			assert(display.shown_page == 0U);
		}
	}
	assert(armada_scene_destroy(scene));
	assert(!live_output && display.active);
	assert_scene_resources_released();
	assert_untouched(storage, GUARD_BYTES);
	assert_untouched(storage + GUARD_BYTES + display.size, GUARD_BYTES);
	fplinux_font_close(&font);
	free(storage);
	return EXIT_SUCCESS;
}
