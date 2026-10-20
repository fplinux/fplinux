/* SPDX-License-Identifier: GPL-2.0-only */
#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "fplinux-cli.h"
#include "fplinux-gpu.h"

#define IMAGE_SIZE 64

struct render_context {
	struct fplinux_gpu *gpu;
	GLuint program;
	GLuint framebuffer;
	GLuint texture;
	GLuint vertex_buffer;
};

static GLuint compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint compiled = GL_FALSE;
	char log[1024];

	if (!shader)
		return 0;
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (compiled)
		return shader;
	glGetShaderInfoLog(shader, sizeof(log), NULL, log);
	fprintf(stderr, "fplinux-gpu-probe: shader compilation failed: %s\n",
		log);
	glDeleteShader(shader);
	return 0;
}

static bool initialize_render(struct render_context *render)
{
	static const GLfloat positions[] = { -1.0f, -1.0f, 3.0f,
					     -1.0f, -1.0f, 3.0f };
	static const char vertex_source[] =
		"attribute vec2 position;\n"
		"void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
	static const char fragment_source[] =
		"precision highp float;\n"
		"uniform float blue;\n"
		"void main() {\n"
		"  gl_FragColor = vec4(gl_FragCoord.xy / 64.0, blue, 1.0);\n"
		"}\n";
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
	GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
	GLint linked = GL_FALSE;
	char log[1024];

	if (!vertex || !fragment)
		goto out;
	render->program = glCreateProgram();
	if (!render->program)
		goto out;
	glAttachShader(render->program, vertex);
	glAttachShader(render->program, fragment);
	glBindAttribLocation(render->program, 0, "position");
	glLinkProgram(render->program);
	glGetProgramiv(render->program, GL_LINK_STATUS, &linked);
	if (!linked) {
		glGetProgramInfoLog(render->program, sizeof(log), NULL, log);
		fprintf(stderr, "fplinux-gpu-probe: program link failed: %s\n",
			log);
		goto out;
	}
	glGenBuffers(1, &render->vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, render->vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions,
		     GL_STATIC_DRAW);
	glGenTextures(1, &render->texture);
	glBindTexture(GL_TEXTURE_2D, render->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, IMAGE_SIZE, IMAGE_SIZE, 0,
		     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &render->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, render->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, render->texture, 0);
	linked = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
		 GL_FRAMEBUFFER_COMPLETE;
	if (!linked)
		fprintf(stderr, "fplinux-gpu-probe: incomplete framebuffer\n");
out:
	if (fragment)
		glDeleteShader(fragment);
	if (vertex)
		glDeleteShader(vertex);
	return linked && glGetError() == GL_NO_ERROR;
}

static bool render_pass(const struct render_context *render, unsigned int pass)
{
	static const struct {
		unsigned int x, y;
		unsigned char red, green;
	} samples[] = {
		{ 0, 0, 2, 2 },	   { 31, 0, 126, 2 },	 { 63, 0, 253, 2 },
		{ 0, 31, 2, 126 }, { 31, 31, 126, 126 }, { 63, 31, 253, 126 },
		{ 0, 63, 2, 253 }, { 31, 63, 126, 253 }, { 63, 63, 253, 253 },
	};
	unsigned char pixels[IMAGE_SIZE * IMAGE_SIZE * 4];
	unsigned char blue = pass ? 191 : 64;
	GLint blue_location = glGetUniformLocation(render->program, "blue");

	if (blue_location < 0)
		return false;
	glViewport(0, 0, IMAGE_SIZE, IMAGE_SIZE);
	glDisable(GL_DITHER);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(render->program);
	glUniform1f(blue_location, pass ? 0.75f : 0.25f);
	glBindBuffer(GL_ARRAY_BUFFER, render->vertex_buffer);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	glReadPixels(0, 0, IMAGE_SIZE, IMAGE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE,
		     pixels);
	if (glGetError() != GL_NO_ERROR)
		return false;
	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		const unsigned char *pixel =
			pixels + (samples[i].y * IMAGE_SIZE + samples[i].x) * 4;
		const unsigned char expected[] = {
			samples[i].red,
			samples[i].green,
			blue,
			255,
		};

		for (size_t channel = 0; channel < 4; channel++) {
			if (abs((int)pixel[channel] - expected[channel]) <= 1)
				continue;
			fprintf(stderr,
				"fplinux-gpu-probe: pass %u pixel %u,%u channel %zu: got %u expected %u\n",
				pass + 1, samples[i].x, samples[i].y, channel,
				pixel[channel], expected[channel]);
			return false;
		}
	}
	printf("PASS%u_PIXELS=9\n", pass + 1);
	return true;
}

static void cleanup_render(struct render_context *render)
{
	if (render->gpu) {
		glDeleteBuffers(1, &render->vertex_buffer);
		glDeleteFramebuffers(1, &render->framebuffer);
		glDeleteTextures(1, &render->texture);
		glDeleteProgram(render->program);
	}
	fplinux_gpu_close(render->gpu);
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option options[] = {
		{ "device", "PATH", "Panfrost DRM render node",
		  FPLINUX_CLI_REQUIRED, NULL, 0 },
	};
	struct fplinux_cli cli = {
		.program = "fplinux-gpu-probe",
		.description =
			"Draw two offscreen shader gradients on a Panfrost render node",
		.options = options,
		.option_count = sizeof(options) / sizeof(options[0]),
	};
	struct render_context render = { 0 };
	char error[256];
	int result = fplinux_cli_parse(&cli, argc, argv);

	if (result != FPLINUX_CLI_READY)
		return result;
	result = 1;
	render.gpu = fplinux_gpu_open(FPLINUX_GPU_GLES3, IMAGE_SIZE, IMAGE_SIZE,
				      options[0].value, error, sizeof(error));
	if (!render.gpu) {
		fprintf(stderr, "fplinux-gpu-probe: %s\n", error);
		goto out;
	}
	printf("DRM_DEVICE=%s\nDRM_DRIVER=panfrost\nEGL_DRIVER=panfrost\nGL_RENDERER=%s\nGL_VERSION=%s\n",
	       fplinux_gpu_device(render.gpu), fplinux_gpu_renderer(render.gpu),
	       fplinux_gpu_version(render.gpu));
	if (!initialize_render(&render) || !render_pass(&render, 0) ||
	    !render_pass(&render, 1)) {
		fprintf(stderr, "fplinux-gpu-probe: offscreen render failed\n");
		goto out;
	}
	puts("GPU_RENDER=PASS");
	result = 0;
out:
	cleanup_render(&render);
	return result;
}
