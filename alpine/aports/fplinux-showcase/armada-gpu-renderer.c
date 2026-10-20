// SPDX-License-Identifier: GPL-2.0-only
#include "armada-gpu-renderer.h"
#include "armada-geometry.h"
#include "armada-math.h"
#include "fplinux-gpu-output.h"

#include <GLES3/gl3.h>
#include <errno.h>
#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ARMADA_SHADOW_SIDE 128U
#define ARMADA_BACKGROUND_SCALE 4U
#define ARMADA_LIGHT_SAMPLES 12U
#define ARMADA_MESH_VERTICES (ARMADA_STROKES * ARMADA_PRISM_FACES * 3U)
#ifndef ARMADA_SHADER_DIRECTORY
#define ARMADA_SHADER_DIRECTORY "/usr/share/fplinux/armada"
#endif

enum armada_gpu_pass {
	ARMADA_GPU_BACKGROUND,
	ARMADA_GPU_GLASS,
	ARMADA_GPU_SHADOW,
	ARMADA_GPU_COPY,
	ARMADA_GPU_PASS_COUNT,
};

struct armada_gl {
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
	void (*Uniform1f)(GLint location, GLfloat value);
	void (*Uniform1fv)(GLint location, GLsizei count, const GLfloat *value);
	void (*Uniform2f)(GLint location, GLfloat x, GLfloat y);
	void (*Uniform2fv)(GLint location, GLsizei count, const GLfloat *value);
	void (*Uniform3f)(GLint location, GLfloat x, GLfloat y, GLfloat z);
	void (*Uniform3fv)(GLint location, GLsizei count, const GLfloat *value);
	void (*Uniform4f)(GLint location, GLfloat x, GLfloat y, GLfloat z,
			  GLfloat w);
	void (*Uniform4fv)(GLint location, GLsizei count, const GLfloat *value);
	void (*UniformMatrix3fv)(GLint location, GLsizei count,
				 GLboolean transpose, const GLfloat *value);
	void (*GenBuffers)(GLsizei count, GLuint *buffers);
	void (*BindBuffer)(GLenum target, GLuint buffer);
	void (*BufferData)(GLenum target, GLsizeiptr size, const void *data,
			   GLenum usage);
	void (*DeleteBuffers)(GLsizei count, const GLuint *buffers);
	void (*GenVertexArrays)(GLsizei count, GLuint *arrays);
	void (*BindVertexArray)(GLuint array);
	void (*DeleteVertexArrays)(GLsizei count, const GLuint *arrays);
	void (*EnableVertexAttribArray)(GLuint index);
	void (*VertexAttribPointer)(GLuint index, GLint size, GLenum type,
				    GLboolean normalized, GLsizei stride,
				    const void *pointer);
	void (*GenTextures)(GLsizei count, GLuint *textures);
	void (*BindTexture)(GLenum target, GLuint texture);
	void (*ActiveTexture)(GLenum texture);
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
	void (*GenRenderbuffers)(GLsizei count, GLuint *renderbuffers);
	void (*BindRenderbuffer)(GLenum target, GLuint renderbuffer);
	void (*RenderbufferStorage)(GLenum target, GLenum format, GLsizei width,
				    GLsizei height);
	void (*FramebufferRenderbuffer)(GLenum target, GLenum attachment,
					GLenum renderbuffer_target,
					GLuint renderbuffer);
	void (*DeleteRenderbuffers)(GLsizei count, const GLuint *renderbuffers);
	void (*Viewport)(GLint x, GLint y, GLsizei width, GLsizei height);
	void (*Scissor)(GLint x, GLint y, GLsizei width, GLsizei height);
	void (*Enable)(GLenum capability);
	void (*Disable)(GLenum capability);
	void (*ClearColor)(GLfloat red, GLfloat green, GLfloat blue,
			   GLfloat alpha);
	void (*Clear)(GLbitfield mask);
	void (*DepthFunc)(GLenum function);
	void (*DrawArrays)(GLenum mode, GLint first, GLsizei count);
	GLenum (*GetError)(void);
};

struct armada_gpu_target {
	GLuint framebuffer;
	GLuint texture;
	GLuint depth;
	unsigned int width;
	unsigned int height;
};

struct armada_gpu_vertex {
	struct armada_vec3 position;
	struct armada_vec3 midpoint;
	struct armada_vec3 normal;
};

struct armada_gpu_uniforms {
	GLint reflection;
	GLint size;
	GLint view_size;
	GLint refine;
	GLint frame;
	GLint rotation;
	GLint scale;
	GLint position;
	GLint color;
	GLint emissive;
	GLint letter_scale;
	GLint reflection_visibility;
	GLint light_color;
	GLint celestial;
	GLint sun;
	GLint moon;
	GLint environment;
	GLint wave;
	GLint ripple_count;
	GLint ripples;
	GLint source;
	GLint shadow;
	GLint shadow_right;
	GLint shadow_up;
	GLint shadow_direction;
	GLint shadow_transform;
	GLint shadow_depth_range;
	GLint shadow_bounds;
	GLint shadow_max_depth;
	GLint shadow_filters;
	GLint shadow_slopes;
	GLint shadow_max_slope;
	GLint shadow_strength;
};

struct armada_gpu_program {
	GLuint handle;
	struct armada_gpu_uniforms uniforms;
};

struct armada_gpu_renderer {
	struct fplinux_gpu_output *output;
	struct fplinux_gpu *gpu;
	struct armada_gl gl;
	struct armada_gpu_program programs[ARMADA_GPU_PASS_COUNT];
	struct armada_gpu_uniforms *uniforms;
	struct armada_gpu_target low;
	struct armada_gpu_target scene[2];
	struct armada_gpu_target shadow;
	GLuint vertex_array;
	GLuint vertex_buffer;
	unsigned int width;
	unsigned int height;
	struct armada_vec3 local_vertices[ARMADA_STROKES][ARMADA_PRISM_VERTICES];
	char name[320];
	char error[512];
};

static struct armada_vec3 cross(struct armada_vec3 first,
				struct armada_vec3 second)
{
	return vec3(first.y * second.z - first.z * second.y,
		    first.z * second.x - first.x * second.z,
		    first.x * second.y - first.y * second.x);
}

static struct armada_vec3 rotate(const struct armada_letter_state *letter,
				 struct armada_vec3 value)
{
	float yaw_x = letter->yaw_cosine * value.x + letter->yaw_sine * value.z;
	float yaw_z =
		-letter->yaw_sine * value.x + letter->yaw_cosine * value.z;
	float pitch_y =
		letter->pitch_cosine * value.y - letter->pitch_sine * yaw_z;
	float pitch_z =
		letter->pitch_sine * value.y + letter->pitch_cosine * yaw_z;

	return vec3(letter->roll_cosine * yaw_x - letter->roll_sine * pitch_y,
		    letter->roll_sine * yaw_x + letter->roll_cosine * pitch_y,
		    pitch_z);
}

static bool load_gl(struct armada_gpu_renderer *renderer)
{
#define LOAD_GL(member)                                                        \
	do {                                                                   \
		void *address =                                                \
			fplinux_gpu_get_proc(renderer->gpu, "gl" #member);     \
		if (!address) {                                                \
			snprintf(renderer->error, sizeof(renderer->error),     \
				 "GLES3 entry point gl%s is unavailable",      \
				 #member);                                     \
			return false;                                          \
		}                                                              \
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
	LOAD_GL(Uniform1f);
	LOAD_GL(Uniform1fv);
	LOAD_GL(Uniform2f);
	LOAD_GL(Uniform2fv);
	LOAD_GL(Uniform3f);
	LOAD_GL(Uniform3fv);
	LOAD_GL(Uniform4f);
	LOAD_GL(Uniform4fv);
	LOAD_GL(UniformMatrix3fv);
	LOAD_GL(GenBuffers);
	LOAD_GL(BindBuffer);
	LOAD_GL(BufferData);
	LOAD_GL(DeleteBuffers);
	LOAD_GL(GenVertexArrays);
	LOAD_GL(BindVertexArray);
	LOAD_GL(DeleteVertexArrays);
	LOAD_GL(EnableVertexAttribArray);
	LOAD_GL(VertexAttribPointer);
	LOAD_GL(GenTextures);
	LOAD_GL(BindTexture);
	LOAD_GL(ActiveTexture);
	LOAD_GL(TexParameteri);
	LOAD_GL(TexImage2D);
	LOAD_GL(DeleteTextures);
	LOAD_GL(GenFramebuffers);
	LOAD_GL(BindFramebuffer);
	LOAD_GL(FramebufferTexture2D);
	LOAD_GL(CheckFramebufferStatus);
	LOAD_GL(DeleteFramebuffers);
	LOAD_GL(GenRenderbuffers);
	LOAD_GL(BindRenderbuffer);
	LOAD_GL(RenderbufferStorage);
	LOAD_GL(FramebufferRenderbuffer);
	LOAD_GL(DeleteRenderbuffers);
	LOAD_GL(Viewport);
	LOAD_GL(Scissor);
	LOAD_GL(Enable);
	LOAD_GL(Disable);
	LOAD_GL(ClearColor);
	LOAD_GL(Clear);
	LOAD_GL(DepthFunc);
	LOAD_GL(DrawArrays);
	LOAD_GL(GetError);
#undef LOAD_GL
	return true;
}

static bool check_gl(struct armada_gpu_renderer *renderer,
		     const char *operation)
{
	GLenum error = renderer->gl.GetError();

	if (error == GL_NO_ERROR)
		return true;
	snprintf(renderer->error, sizeof(renderer->error),
		 "GLES3 %s failed (0x%x)", operation, error);
	return false;
}

static char *read_shader(struct armada_gpu_renderer *renderer, const char *name)
{
	char path[256];
	char *source = NULL;
	FILE *file;
	long size;

	snprintf(path, sizeof(path), "%s/%s", ARMADA_SHADER_DIRECTORY, name);
	file = fopen(path, "rb");
	if (!file)
		goto failed;
	if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
	    size > 128 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
		errno = EINVAL;
		goto close_failed;
	}
	source = malloc((size_t)size + 1U);
	if (!source)
		goto close_failed;
	if (fread(source, 1, (size_t)size, file) != (size_t)size) {
		free(source);
		source = NULL;
		errno = EIO;
		goto close_failed;
	}
	source[size] = '\0';
	if (fclose(file) == 0)
		return source;
	free(source);
	goto failed;

close_failed: {
	int saved_errno = errno;
	fclose(file);
	errno = saved_errno;
}
failed:
	snprintf(renderer->error, sizeof(renderer->error),
		 "cannot read ARMADA shader %s: %s", path, strerror(errno));
	return NULL;
}

static GLuint compile_shader(struct armada_gpu_renderer *renderer, GLenum type,
			     const char *name, enum armada_gpu_pass pass)
{
	struct armada_gl *gl = &renderer->gl;
	char *source = read_shader(renderer, name);
	const char *version = "#version 300 es\n";
	const char *body;
	const GLchar *strings[3];
	GLint lengths[3];
	char definition[40];
	GLuint shader;
	GLint compiled = GL_FALSE;
	char log[384] = { 0 };

	if (!source)
		return 0;
	body = strstr(source, version);
	if (!body) {
		snprintf(renderer->error, sizeof(renderer->error),
			 "ARMADA shader %s requires GLSL ES 3.00", name);
		free(source);
		return 0;
	}
	body += strlen(version);
	snprintf(definition, sizeof(definition), "#define ARMADA_PASS %u\n",
		 (unsigned int)pass);
	strings[0] = source;
	strings[1] = definition;
	strings[2] = body;
	lengths[0] = (GLint)(body - source);
	lengths[1] = -1;
	lengths[2] = -1;
	shader = gl->CreateShader(type);
	gl->ShaderSource(shader, ARRAY_SIZE(strings), strings, lengths);
	gl->CompileShader(shader);
	free(source);
	gl->GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (compiled == GL_TRUE)
		return shader;
	gl->GetShaderInfoLog(shader, sizeof(log), NULL, log);
	snprintf(renderer->error, sizeof(renderer->error),
		 "ARMADA shader %s pass %u: %s", name, (unsigned int)pass, log);
	gl->DeleteShader(shader);
	return 0;
}

static void use_pass(struct armada_gpu_renderer *renderer,
		     enum armada_gpu_pass pass)
{
	struct armada_gpu_program *program = &renderer->programs[pass];

	renderer->gl.UseProgram(program->handle);
	renderer->uniforms = &program->uniforms;
}

static bool create_program(struct armada_gpu_renderer *renderer,
			   enum armada_gpu_pass pass)
{
	struct armada_gl *gl = &renderer->gl;
	struct armada_gpu_program *program = &renderer->programs[pass];
	GLuint vertex = compile_shader(renderer, GL_VERTEX_SHADER,
				       "armada-scene.vert", pass);
	GLuint fragment;
	GLint linked = GL_FALSE;
	char log[384] = { 0 };
	float waves[64];
	unsigned int index;

	if (!vertex)
		return false;
	fragment = compile_shader(renderer, GL_FRAGMENT_SHADER,
				  "armada-scene.frag", pass);
	if (!fragment) {
		gl->DeleteShader(vertex);
		return false;
	}
	program->handle = gl->CreateProgram();
	gl->AttachShader(program->handle, vertex);
	gl->AttachShader(program->handle, fragment);
	gl->LinkProgram(program->handle);
	gl->DeleteShader(fragment);
	gl->DeleteShader(vertex);
	gl->GetProgramiv(program->handle, GL_LINK_STATUS, &linked);
	if (linked != GL_TRUE) {
		gl->GetProgramInfoLog(program->handle, sizeof(log), NULL, log);
		snprintf(renderer->error, sizeof(renderer->error),
			 "ARMADA shader link pass %u: %s", (unsigned int)pass,
			 log);
		return false;
	}
#define UNIFORM(member)            \
	program->uniforms.member = \
		gl->GetUniformLocation(program->handle, "u_" #member)
	UNIFORM(reflection);
	UNIFORM(size);
	UNIFORM(view_size);
	UNIFORM(refine);
	UNIFORM(frame);
	UNIFORM(rotation);
	UNIFORM(scale);
	UNIFORM(position);
	UNIFORM(color);
	UNIFORM(emissive);
	UNIFORM(letter_scale);
	UNIFORM(reflection_visibility);
	UNIFORM(light_color);
	UNIFORM(celestial);
	UNIFORM(sun);
	UNIFORM(moon);
	UNIFORM(environment);
	UNIFORM(wave);
	UNIFORM(ripple_count);
	UNIFORM(ripples);
	UNIFORM(source);
	UNIFORM(shadow);
	UNIFORM(shadow_right);
	UNIFORM(shadow_up);
	UNIFORM(shadow_direction);
	UNIFORM(shadow_transform);
	UNIFORM(shadow_depth_range);
	UNIFORM(shadow_bounds);
	UNIFORM(shadow_max_depth);
	UNIFORM(shadow_filters);
	UNIFORM(shadow_slopes);
	UNIFORM(shadow_max_slope);
	UNIFORM(shadow_strength);
#undef UNIFORM
	use_pass(renderer, pass);
	gl->Uniform1i(program->uniforms.source, 0);
	gl->Uniform1i(program->uniforms.shadow, 1);
	gl->Uniform2f(program->uniforms.size, (float)renderer->width,
		      (float)renderer->height);
	for (index = 0; index < ARRAY_SIZE(waves); ++index)
		waves[index] = wave_sample((int)index);
	gl->Uniform1fv(program->uniforms.wave, ARRAY_SIZE(waves), waves);
	return check_gl(renderer, "program initialization");
}

static bool create_target(struct armada_gpu_renderer *renderer,
			  struct armada_gpu_target *target, unsigned int width,
			  unsigned int height, bool nearest)
{
	struct armada_gl *gl = &renderer->gl;
	GLenum status;

	target->width = width;
	target->height = height;
	gl->GenTextures(1, &target->texture);
	gl->BindTexture(GL_TEXTURE_2D, target->texture);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			  nearest ? GL_NEAREST : GL_LINEAR);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
			  nearest ? GL_NEAREST : GL_LINEAR);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width,
		       (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	gl->GenRenderbuffers(1, &target->depth);
	gl->BindRenderbuffer(GL_RENDERBUFFER, target->depth);
	gl->RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
				(GLsizei)width, (GLsizei)height);
	gl->GenFramebuffers(1, &target->framebuffer);
	gl->BindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				 GL_TEXTURE_2D, target->texture, 0);
	gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				    GL_RENDERBUFFER, target->depth);
	status = gl->CheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		snprintf(renderer->error, sizeof(renderer->error),
			 "ARMADA framebuffer %ux%u is incomplete (0x%x)", width,
			 height, status);
		return false;
	}
	return check_gl(renderer, "framebuffer allocation");
}

static bool create_mesh(struct armada_gpu_renderer *renderer)
{
	struct armada_gpu_vertex vertices[ARMADA_MESH_VERTICES];
	struct armada_prepared_stroke strokes[ARMADA_STROKES];
	struct armada_gl *gl = &renderer->gl;
	unsigned int stroke;
	unsigned int output = 0;

	armada_prepare_stroke_geometry(strokes);
	for (stroke = 0; stroke < ARMADA_STROKES; ++stroke) {
		const struct armada_prepared_stroke *prepared =
			&strokes[stroke];
		unsigned int corner;
		unsigned int face;

		for (corner = 0; corner < ARMADA_PRISM_VERTICES; ++corner) {
			const struct armada_prism_profile *profile =
				&armada_prism_profile
					[corner % ARMADA_PRISM_PROFILE_POINTS];
			float along = corner < ARMADA_PRISM_PROFILE_POINTS ?
					      -prepared->half_length - 0.095f :
					      prepared->half_length + 0.095f;

			renderer->local_vertices[stroke][corner] =
				vec3(prepared->midpoint_x +
					     along * prepared->unit_x -
					     profile->across * prepared->unit_y,
				     prepared->midpoint_y +
					     along * prepared->unit_y +
					     profile->across * prepared->unit_x,
				     profile->depth);
		}
		for (face = 0; face < ARMADA_PRISM_FACES; ++face) {
			struct armada_vec3 first =
				renderer->local_vertices
					[stroke][armada_prism_faces[face][0]];
			struct armada_vec3 second =
				renderer->local_vertices
					[stroke][armada_prism_faces[face][1]];
			struct armada_vec3 third =
				renderer->local_vertices
					[stroke][armada_prism_faces[face][2]];
			struct armada_vec3 midpoint = vec_scale(
				vec_add(vec_add(first, second), third),
				1.0f / 3.0f);
			struct armada_vec3 normal = vec_normalize(cross(
				vec_sub(second, first), vec_sub(third, first)));

			for (corner = 0; corner < 3U; ++corner)
				vertices[output++] = (struct armada_gpu_vertex){
					.position =
						renderer->local_vertices
							[stroke]
							[armada_prism_faces
								 [face][corner]],
					.midpoint = midpoint,
					.normal = normal,
				};
		}
	}
	gl->GenVertexArrays(1, &renderer->vertex_array);
	gl->BindVertexArray(renderer->vertex_array);
	gl->GenBuffers(1, &renderer->vertex_buffer);
	gl->BindBuffer(GL_ARRAY_BUFFER, renderer->vertex_buffer);
	gl->BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices,
		       GL_STATIC_DRAW);
	gl->EnableVertexAttribArray(0);
	gl->EnableVertexAttribArray(1);
	gl->EnableVertexAttribArray(2);
	gl->VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertices[0]),
				(const void *)offsetof(struct armada_gpu_vertex,
						       position));
	gl->VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertices[0]),
				(const void *)offsetof(struct armada_gpu_vertex,
						       midpoint));
	gl->VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(vertices[0]),
				(const void *)offsetof(struct armada_gpu_vertex,
						       normal));
	return check_gl(renderer, "mesh allocation");
}

static void set_vector(struct armada_gpu_renderer *renderer, GLint location,
		       struct armada_vec3 value)
{
	renderer->gl.Uniform3f(location, value.x, value.y, value.z);
}

static void set_letter(struct armada_gpu_renderer *renderer,
		       const struct armada_frame *frame, unsigned int index)
{
	const struct armada_letter_state *letter = &frame->letters[index];
	struct armada_gpu_uniforms *uniforms = renderer->uniforms;
	struct armada_vec3 axis_x = rotate(letter, vec3(1.0f, 0.0f, 0.0f));
	struct armada_vec3 axis_y = rotate(letter, vec3(0.0f, 1.0f, 0.0f));
	struct armada_vec3 axis_z = rotate(letter, vec3(0.0f, 0.0f, 1.0f));
	float rotation[9] = {
		axis_x.x, axis_x.y, axis_x.z, axis_y.x, axis_y.y,
		axis_y.z, axis_z.x, axis_z.y, axis_z.z,
	};
	float half_height =
		letter->scale * (f_abs(axis_x.y * 0.53f * letter->scale_x) +
				 f_abs(axis_y.y * 0.77f * letter->scale_y) +
				 f_abs(axis_z.y * 0.23f));
	float visibility =
		smoothstep((letter->position.y - ARMADA_WATER_Y + half_height) /
			   (2.0f * half_height));

	renderer->gl.UniformMatrix3fv(uniforms->rotation, 1, GL_FALSE,
				      rotation);
	set_vector(renderer, uniforms->position, letter->position);
	renderer->gl.Uniform3f(uniforms->scale, letter->scale * letter->scale_x,
			       letter->scale * letter->scale_y, letter->scale);
	set_vector(renderer, uniforms->color, armada_letter_base_color(index));
	renderer->gl.Uniform1f(uniforms->emissive, letter->emissive);
	renderer->gl.Uniform1f(uniforms->letter_scale, letter->scale);
	renderer->gl.Uniform1f(uniforms->reflection_visibility, visibility);
}

static void draw_letter(struct armada_gpu_renderer *renderer,
			unsigned int index)
{
	const struct armada_glyph *glyph = &armada_glyphs[index];

	renderer->gl.DrawArrays(
		GL_TRIANGLES, (GLint)(glyph->first * ARMADA_PRISM_FACES * 3U),
		(GLsizei)(glyph->count * ARMADA_PRISM_FACES * 3U));
}

static void set_environment(struct armada_gpu_renderer *renderer,
			    const struct armada_frame *frame)
{
	const struct armada_environment *environment = &frame->environment;
	struct armada_gpu_uniforms *uniforms = renderer->uniforms;
	float ripples[ARMADA_LETTERS][4] = { { 0 } };
	unsigned int index;

	for (index = 0; index < frame->ripple_count; ++index) {
		ripples[index][0] = frame->ripples[index].origin.x;
		ripples[index][1] = frame->ripples[index].origin.z;
		ripples[index][2] = frame->ripples[index].radius;
		ripples[index][3] = frame->ripples[index].amplitude;
	}
	renderer->gl.Uniform1ui(uniforms->frame, frame->number);
	renderer->gl.Uniform1i(uniforms->ripple_count,
			       (GLint)frame->ripple_count);
	renderer->gl.Uniform4fv(uniforms->ripples, ARMADA_LETTERS,
				&ripples[0][0]);
	set_vector(renderer, uniforms->light_color, environment->light_color);
	set_vector(renderer, uniforms->celestial,
		   environment->celestial_direction);
	renderer->gl.Uniform4f(uniforms->sun, environment->sun_direction.x,
			       environment->sun_direction.y,
			       environment->sun_direction.z,
			       environment->sun_visibility);
	renderer->gl.Uniform4f(uniforms->moon, environment->moon_direction.x,
			       environment->moon_direction.y,
			       environment->moon_direction.z,
			       environment->moon_visibility);
	renderer->gl.Uniform3f(uniforms->environment, environment->daylight,
			       environment->night, environment->exposure);
}

static void render_shadows(struct armada_gpu_renderer *renderer,
			   const struct armada_frame *frame)
{
	static const float disk[ARMADA_LIGHT_SAMPLES][2] = {
		{ 0.28867513f, 0.00000000f },	{ -0.28867513f, 0.00000000f },
		{ -0.36868444f, 0.33774515f },	{ 0.36868444f, -0.33774515f },
		{ 0.05643306f, -0.64302564f },	{ -0.05643306f, 0.64302564f },
		{ 0.46470286f, 0.60612259f },	{ -0.46470286f, -0.60612259f },
		{ -0.85278689f, -0.15084599f }, { 0.85278689f, 0.15084599f },
		{ 0.80783419f, -0.51387799f },	{ -0.80783419f, 0.51387799f },
	};
	const struct armada_environment *environment = &frame->environment;
	struct armada_gl *gl = &renderer->gl;
	struct armada_gpu_uniforms *uniforms = renderer->uniforms;
	bool sunlight = environment->sun_visibility >=
			environment->moon_visibility;
	struct armada_vec3 direction = sunlight ? environment->sun_direction :
						  environment->moon_direction;
	struct armada_vec3 right =
		vec_normalize(vec3(direction.z, 0.0f, -direction.x));
	struct armada_vec3 up = cross(direction, right);
	struct armada_vec3 center = vec_scale(direction, 1.0f / direction.z);
	float strength =
		(sunlight ? environment->sun_visibility *
				    (0.48f + environment->daylight * 0.44f) :
			    environment->moon_visibility * environment->night *
				    0.28f) *
		smoothstep(direction.y / 0.18f);
	float bounds[ARMADA_LETTERS][4];
	float maximum_depth[ARMADA_LETTERS];
	float filters[ARMADA_LETTERS][3];
	float slopes[ARMADA_LIGHT_SAMPLES][2];
	float maximum_slope[2] = { 0 };
	float minimum_u = FLT_MAX;
	float maximum_u = -FLT_MAX;
	float minimum_v = FLT_MAX;
	float maximum_v = -FLT_MAX;
	float minimum_depth = FLT_MAX;
	float deepest = -FLT_MAX;
	float scale_u;
	float scale_v;
	unsigned int index;

	if (strength <= 0.0f) {
		gl->Uniform1f(uniforms->shadow_strength, 0.0f);
		return;
	}
	for (index = 0; index < ARMADA_LIGHT_SAMPLES; ++index) {
		float radius = sunlight ? 0.046f : 0.040f;
		struct armada_vec3 sample =
			vec_add(center, vec3(disk[index][0] * radius,
					     disk[index][1] * radius, 0.0f));
		float forward = vec_dot(sample, direction);

		slopes[index][0] = vec_dot(sample, right) / forward;
		slopes[index][1] = vec_dot(sample, up) / forward;
		maximum_slope[0] =
			f_max(maximum_slope[0], f_abs(slopes[index][0]));
		maximum_slope[1] =
			f_max(maximum_slope[1], f_abs(slopes[index][1]));
	}
	for (index = 0; index < ARMADA_LETTERS; ++index) {
		const struct armada_letter_state *letter =
			&frame->letters[index];
		const struct armada_glyph *glyph = &armada_glyphs[index];
		struct armada_vec3 filter = vec_scale(
			color_mix(vec3(1.0f, 1.0f, 1.0f),
				  armada_letter_base_color(index), 0.72f),
			0.92f);
		unsigned int stroke;

		bounds[index][0] = bounds[index][2] = FLT_MAX;
		bounds[index][1] = bounds[index][3] = -FLT_MAX;
		maximum_depth[index] = -FLT_MAX;
		filters[index][0] = filter.x;
		filters[index][1] = filter.y;
		filters[index][2] = filter.z;
		if (!letter->visible)
			continue;
		for (stroke = glyph->first;
		     stroke < glyph->first + glyph->count; ++stroke) {
			unsigned int corner;

			for (corner = 0; corner < ARMADA_PRISM_VERTICES;
			     ++corner) {
				struct armada_vec3 local =
					renderer->local_vertices[stroke][corner];
				struct armada_vec3 world;
				float u;
				float v;
				float depth;

				local.x *= letter->scale_x;
				local.y *= letter->scale_y;
				world = vec_add(letter->position,
						vec_scale(rotate(letter, local),
							  letter->scale));
				u = vec_dot(world, right);
				v = vec_dot(world, up);
				depth = vec_dot(world, direction);
				bounds[index][0] = f_min(bounds[index][0], u);
				bounds[index][1] = f_max(bounds[index][1], u);
				bounds[index][2] = f_min(bounds[index][2], v);
				bounds[index][3] = f_max(bounds[index][3], v);
				maximum_depth[index] =
					f_max(maximum_depth[index], depth);
				minimum_u = f_min(minimum_u, u);
				maximum_u = f_max(maximum_u, u);
				minimum_v = f_min(minimum_v, v);
				maximum_v = f_max(maximum_v, v);
				minimum_depth = f_min(minimum_depth, depth);
				deepest = f_max(deepest, depth);
			}
		}
	}
	if (minimum_u == FLT_MAX) {
		gl->Uniform1f(uniforms->shadow_strength, 0.0f);
		return;
	}
	minimum_u -= 0.12f;
	minimum_v -= 0.12f;
	scale_u = 127.0f / (maximum_u - minimum_u + 0.12f);
	scale_v = 127.0f / (maximum_v - minimum_v + 0.12f);
	for (index = 0; index < ARMADA_LETTERS; ++index) {
		if (!frame->letters[index].visible)
			continue;
		bounds[index][0] =
			(bounds[index][0] - minimum_u) * scale_u - 0.5f;
		bounds[index][1] =
			(bounds[index][1] - minimum_u) * scale_u - 0.5f;
		bounds[index][2] =
			(bounds[index][2] - minimum_v) * scale_v - 0.5f;
		bounds[index][3] =
			(bounds[index][3] - minimum_v) * scale_v - 0.5f;
	}
	set_vector(renderer, uniforms->shadow_right, right);
	set_vector(renderer, uniforms->shadow_up, up);
	set_vector(renderer, uniforms->shadow_direction, direction);
	gl->Uniform4f(uniforms->shadow_transform, minimum_u, minimum_v, scale_u,
		      scale_v);
	gl->Uniform2f(uniforms->shadow_depth_range, minimum_depth - 1.0f,
		      deepest - minimum_depth + 2.0f);
	gl->Uniform4fv(uniforms->shadow_bounds, ARMADA_LETTERS, &bounds[0][0]);
	gl->Uniform1fv(uniforms->shadow_max_depth, ARMADA_LETTERS,
		       maximum_depth);
	gl->Uniform3fv(uniforms->shadow_filters, ARMADA_LETTERS,
		       &filters[0][0]);
	gl->Uniform2fv(uniforms->shadow_slopes, ARMADA_LIGHT_SAMPLES,
		       &slopes[0][0]);
	gl->Uniform2fv(uniforms->shadow_max_slope, 1, maximum_slope);
	gl->Uniform1f(uniforms->shadow_strength, strength);
	use_pass(renderer, ARMADA_GPU_SHADOW);
	uniforms = renderer->uniforms;
	set_vector(renderer, uniforms->shadow_right, right);
	set_vector(renderer, uniforms->shadow_up, up);
	set_vector(renderer, uniforms->shadow_direction, direction);
	gl->Uniform4f(uniforms->shadow_transform, minimum_u, minimum_v, scale_u,
		      scale_v);
	gl->Uniform2f(uniforms->shadow_depth_range, minimum_depth - 1.0f,
		      deepest - minimum_depth + 2.0f);
	gl->ActiveTexture(GL_TEXTURE1);
	gl->BindTexture(GL_TEXTURE_2D, 0);
	gl->ActiveTexture(GL_TEXTURE0);
	gl->BindTexture(GL_TEXTURE_2D, 0);
	gl->BindFramebuffer(GL_FRAMEBUFFER, renderer->shadow.framebuffer);
	gl->ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	gl->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	gl->Enable(GL_DEPTH_TEST);
	for (index = 0; index < ARMADA_LETTERS; ++index) {
		if (!frame->letters[index].visible)
			continue;
		gl->Viewport((GLint)(index * ARMADA_SHADOW_SIDE), 0,
			     ARMADA_SHADOW_SIDE, ARMADA_SHADOW_SIDE);
		set_letter(renderer, frame, index);
		draw_letter(renderer, index);
	}
}

static void bind_target(struct armada_gpu_renderer *renderer,
			const struct armada_gpu_target *target)
{
	renderer->gl.BindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	renderer->gl.Viewport(0, 0, (GLsizei)target->width,
			      (GLsizei)target->height);
}

static void copy_target(struct armada_gpu_renderer *renderer,
			const struct armada_gpu_target *source,
			const struct armada_gpu_target *destination)
{
	bind_target(renderer, destination);
	use_pass(renderer, ARMADA_GPU_COPY);
	renderer->gl.Disable(GL_DEPTH_TEST);
	renderer->gl.ActiveTexture(GL_TEXTURE0);
	renderer->gl.BindTexture(GL_TEXTURE_2D, source->texture);
	renderer->gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

static void refine_body(struct armada_gpu_renderer *renderer,
			struct armada_vec3 direction, float visibility,
			float radius)
{
	float denominator = (float)renderer->height * 0.95f;
	float candidate = radius + 8.0f / denominator;
	float center_x;
	float center_y;
	int extent = (int)(candidate * denominator * 0.5f + 2.0f);
	int left;
	int right;
	int top;
	int bottom;

	if (visibility <= 0.0f || direction.z <= 0.05f)
		return;
	center_x = ((float)renderer->width +
		    direction.x / direction.z * denominator - 1.0f) *
		   0.5f;
	center_y = ((float)renderer->height * 1.125f -
		    direction.y / direction.z * denominator - 1.0f) *
		   0.5f;
	left = (int)f_max(center_x - (float)extent, 0.0f);
	right = (int)f_min(center_x + (float)extent,
			   (float)renderer->width - 1.0f);
	top = (int)f_max(center_y - (float)extent, 0.0f);
	bottom = (int)f_min(center_y + (float)extent,
			    (float)renderer->height - 1.0f);
	if (right < left || bottom < top)
		return;
	renderer->gl.Uniform4f(renderer->uniforms->refine, direction.x,
			       direction.y, direction.z, candidate);
	renderer->gl.Scissor(left, (GLint)renderer->height - bottom - 1,
			     right - left + 1, bottom - top + 1);
	renderer->gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

bool armada_gpu_renderer_render(struct armada_gpu_renderer *renderer,
				const struct armada_frame *frame)
{
	struct armada_gl *gl = &renderer->gl;
	unsigned int index;
	unsigned int reflection;

	gl->BindVertexArray(renderer->vertex_array);
	use_pass(renderer, ARMADA_GPU_GLASS);
	set_environment(renderer, frame);
	use_pass(renderer, ARMADA_GPU_BACKGROUND);
	set_environment(renderer, frame);
	render_shadows(renderer, frame);
	use_pass(renderer, ARMADA_GPU_BACKGROUND);
	gl->ActiveTexture(GL_TEXTURE1);
	gl->BindTexture(GL_TEXTURE_2D, renderer->shadow.texture);
	gl->ActiveTexture(GL_TEXTURE0);
	gl->BindTexture(GL_TEXTURE_2D, 0);
	bind_target(renderer, &renderer->low);
	gl->Disable(GL_DEPTH_TEST);
	gl->Uniform4f(renderer->uniforms->refine, 0.0f, 0.0f, 1.0f, 0.0f);
	gl->Uniform2f(renderer->uniforms->view_size, (float)renderer->low.width,
		      (float)renderer->low.height);
	gl->DrawArrays(GL_TRIANGLES, 0, 3);
	copy_target(renderer, &renderer->low, &renderer->scene[0]);
	use_pass(renderer, ARMADA_GPU_BACKGROUND);
	gl->Uniform2f(renderer->uniforms->view_size, (float)renderer->width,
		      (float)renderer->height);
	gl->Enable(GL_SCISSOR_TEST);
	refine_body(renderer, frame->environment.sun_direction,
		    frame->environment.sun_visibility, 0.046f);
	refine_body(renderer, frame->environment.moon_direction,
		    frame->environment.moon_visibility, 0.040f);
	gl->Disable(GL_SCISSOR_TEST);
	/* Complete snapshots keep refraction independent of writes in its own pass. */
	for (reflection = 0; reflection < 2U; ++reflection) {
		unsigned int source = reflection == 0U ? 0U : 1U;
		unsigned int destination = 1U - source;

		copy_target(renderer, &renderer->scene[source],
			    &renderer->scene[destination]);
		gl->Clear(GL_DEPTH_BUFFER_BIT);
		gl->Enable(GL_DEPTH_TEST);
		use_pass(renderer, ARMADA_GPU_GLASS);
		gl->Uniform1i(renderer->uniforms->reflection, reflection == 0U);
		for (index = 0; index < ARMADA_LETTERS; ++index) {
			if (!frame->letters[index].visible)
				continue;
			set_letter(renderer, frame, index);
			draw_letter(renderer, index);
		}
	}
	if (!check_gl(renderer, "scene rendering"))
		return false;
	if (!fplinux_gpu_output_render(renderer->output,
				       renderer->scene[0].texture)) {
		snprintf(renderer->error, sizeof(renderer->error), "%s",
			 fplinux_gpu_output_error(renderer->output));
		return false;
	}
	return true;
}

static void destroy_target(struct armada_gpu_renderer *renderer,
			   struct armada_gpu_target *target)
{
	if (target->framebuffer)
		renderer->gl.DeleteFramebuffers(1, &target->framebuffer);
	if (target->depth)
		renderer->gl.DeleteRenderbuffers(1, &target->depth);
	if (target->texture)
		renderer->gl.DeleteTextures(1, &target->texture);
}

bool armada_gpu_renderer_destroy(struct armada_gpu_renderer *renderer)
{
	unsigned int index;
	bool success;

	if (!renderer)
		return true;
	/* All scene objects die while their owning context remains current. */
	destroy_target(renderer, &renderer->shadow);
	destroy_target(renderer, &renderer->scene[1]);
	destroy_target(renderer, &renderer->scene[0]);
	destroy_target(renderer, &renderer->low);
	if (renderer->vertex_buffer)
		renderer->gl.DeleteBuffers(1, &renderer->vertex_buffer);
	if (renderer->vertex_array)
		renderer->gl.DeleteVertexArrays(1, &renderer->vertex_array);
	for (index = 0; index < ARMADA_GPU_PASS_COUNT; ++index)
		if (renderer->programs[index].handle)
			renderer->gl.DeleteProgram(
				renderer->programs[index].handle);
	success = fplinux_gpu_output_close(renderer->output);
	free(renderer);
	return success;
}

struct armada_gpu_renderer *
armada_gpu_renderer_create(struct fplinux_drm_session *display,
			   unsigned int width, unsigned int height,
			   bool *cleanup_complete, char *error,
			   size_t error_size)
{
	struct armada_gpu_renderer *renderer = calloc(1, sizeof(*renderer));
	unsigned int index;

	*cleanup_complete = true;
	if (!renderer) {
		snprintf(error, error_size, "cannot allocate GPU renderer: %s",
			 strerror(errno));
		return NULL;
	}
	renderer->width = width;
	renderer->height = height;
	renderer->output = fplinux_gpu_output_open(display, FPLINUX_GPU_GLES3,
						   width, height, false,
						   renderer->error,
						   sizeof(renderer->error));
	renderer->gpu = fplinux_gpu_output_gpu(renderer->output);
	if (!renderer->gpu || !load_gl(renderer))
		goto failed;
	for (index = 0; index < ARMADA_GPU_PASS_COUNT; ++index)
		if (!create_program(renderer, (enum armada_gpu_pass)index))
			goto failed;
	if (!create_mesh(renderer) ||
	    !create_target(renderer, &renderer->low,
			   (width + ARMADA_BACKGROUND_SCALE - 1U) /
				   ARMADA_BACKGROUND_SCALE,
			   (height + ARMADA_BACKGROUND_SCALE - 1U) /
				   ARMADA_BACKGROUND_SCALE,
			   false) ||
	    !create_target(renderer, &renderer->scene[0], width, height,
			   false) ||
	    !create_target(renderer, &renderer->scene[1], width, height,
			   false) ||
	    !create_target(renderer, &renderer->shadow,
			   ARMADA_SHADOW_SIDE * ARMADA_LETTERS,
			   ARMADA_SHADOW_SIDE, true))
		goto failed;
	renderer->gl.DepthFunc(GL_LESS);
	renderer->gl.Disable(GL_DITHER);
	if (!check_gl(renderer, "renderer initialization"))
		goto failed;
	snprintf(renderer->name, sizeof(renderer->name), "gpu (%s; %s; %s)",
		 fplinux_gpu_renderer(renderer->gpu),
		 fplinux_gpu_version(renderer->gpu),
		 fplinux_gpu_device(renderer->gpu));
	return renderer;

failed:
	snprintf(error, error_size, "%s", renderer->error);
	*cleanup_complete = armada_gpu_renderer_destroy(renderer);
	if (!*cleanup_complete && error_size) {
		size_t length = strlen(error);

		snprintf(error + length, error_size - length,
			 "; GPU cleanup failed");
	}
	return NULL;
}

struct fplinux_gpu_output *
armada_gpu_renderer_output(const struct armada_gpu_renderer *renderer)
{
	return renderer->output;
}

const char *armada_gpu_renderer_name(const struct armada_gpu_renderer *renderer)
{
	return renderer->name;
}

const char *
armada_gpu_renderer_error(const struct armada_gpu_renderer *renderer)
{
	return renderer->error;
}
