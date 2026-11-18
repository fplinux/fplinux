/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-gpu-frame.h"

#include <GLES3/gl3.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fplinux_gpu_frame {
	struct fplinux_gpu *gpu;
	unsigned int width;
	unsigned int height;
	uint8_t *rgba;
	PFNGLGETINTEGERVPROC get_integer;
	PFNGLPIXELSTOREIPROC pixel_store;
	PFNGLBINDBUFFERPROC bind_buffer;
	PFNGLREADPIXELSPROC read_pixels;
	PFNGLGETERRORPROC get_error;
};

static bool frame_error(char *error, size_t error_size, const char *format, ...)
{
	va_list arguments;

	if (error && error_size) {
		va_start(arguments, format);
		vsnprintf(error, error_size, format, arguments);
		va_end(arguments);
	}
	return false;
}

static bool load_functions(struct fplinux_gpu_frame *frame, char *error,
			   size_t error_size)
{
#define LOAD_GL(member, type, name)                                           \
	do {                                                                  \
		frame->member = (type)fplinux_gpu_get_proc(frame->gpu, name); \
		if (!frame->member)                                           \
			return frame_error(                                   \
				error, error_size,                            \
				"GPU frame entry point unavailable: %s",      \
				name);                                        \
	} while (0)
	LOAD_GL(get_integer, PFNGLGETINTEGERVPROC, "glGetIntegerv");
	LOAD_GL(pixel_store, PFNGLPIXELSTOREIPROC, "glPixelStorei");
	LOAD_GL(bind_buffer, PFNGLBINDBUFFERPROC, "glBindBuffer");
	LOAD_GL(read_pixels, PFNGLREADPIXELSPROC, "glReadPixels");
	LOAD_GL(get_error, PFNGLGETERRORPROC, "glGetError");
#undef LOAD_GL
	return true;
}

struct fplinux_gpu_frame *fplinux_gpu_frame_open(struct fplinux_gpu *gpu,
						 unsigned int width,
						 unsigned int height,
						 char *error, size_t error_size)
{
	struct fplinux_gpu_frame *frame;

	if (error && error_size)
		error[0] = '\0';
	if (!width || !height || width > INT_MAX || height > INT_MAX ||
	    (size_t)width > SIZE_MAX / height / 4U) {
		frame_error(error, error_size, "invalid GPU frame dimensions");
		return NULL;
	}
	if (!fplinux_gpu_is_current(gpu)) {
		frame_error(error, error_size,
			    "GPU frame context is not current");
		return NULL;
	}
	frame = calloc(1, sizeof(*frame));
	if (!frame) {
		frame_error(error, error_size, "allocate GPU frame: %s",
			    strerror(errno));
		return NULL;
	}
	frame->gpu = gpu;
	frame->width = width;
	frame->height = height;
	frame->rgba = malloc((size_t)width * height * 4U);
	if (!frame->rgba) {
		frame_error(error, error_size,
			    "allocate GPU frame readback: %s", strerror(errno));
		goto failed;
	}
	if (!load_functions(frame, error, error_size))
		goto failed;
	return frame;

failed:
	fplinux_gpu_frame_close(frame);
	return NULL;
}

bool fplinux_gpu_frame_read_rgb565(struct fplinux_gpu_frame *frame,
				   uint16_t *pixels, size_t pixel_capacity,
				   char *error, size_t error_size)
{
	static const GLenum parameters[] = {
		GL_PACK_ALIGNMENT,
		GL_PACK_ROW_LENGTH,
		GL_PACK_SKIP_PIXELS,
		GL_PACK_SKIP_ROWS,
	};
	GLint saved[4];
	GLint buffer;
	GLenum read_error;
	GLenum restore_error;

	if (error && error_size)
		error[0] = '\0';
	if (!frame || !pixels ||
	    pixel_capacity < (size_t)frame->width * frame->height)
		return frame_error(error, error_size,
				   "invalid GPU frame output capacity");
	if (!fplinux_gpu_is_current(frame->gpu))
		return frame_error(error, error_size,
				   "GPU frame context is not current");
	if ((read_error = frame->get_error()) != GL_NO_ERROR)
		return frame_error(error, error_size,
				   "GPU frame has a pending GL error (0x%x)",
				   read_error);
	for (size_t index = 0; index < 4U; ++index)
		frame->get_integer(parameters[index], &saved[index]);
	frame->get_integer(GL_PIXEL_PACK_BUFFER_BINDING, &buffer);
	if ((read_error = frame->get_error()) != GL_NO_ERROR)
		return frame_error(error, error_size,
				   "GPU frame pixel-pack query failed (0x%x)",
				   read_error);
	frame->bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
	for (size_t index = 0; index < 4U; ++index)
		frame->pixel_store(parameters[index], index == 0U ? 1 : 0);
	read_error = frame->get_error();
	if (read_error == GL_NO_ERROR) {
		frame->read_pixels(0, 0, (GLsizei)frame->width,
				   (GLsizei)frame->height, GL_RGBA,
				   GL_UNSIGNED_BYTE, frame->rgba);
		read_error = frame->get_error();
	}
	for (size_t index = 0; index < 4U; ++index)
		frame->pixel_store(parameters[index], saved[index]);
	frame->bind_buffer(GL_PIXEL_PACK_BUFFER, (GLuint)buffer);
	restore_error = frame->get_error();
	if (read_error != GL_NO_ERROR || restore_error != GL_NO_ERROR)
		return frame_error(
			error, error_size, "GPU frame readback failed (0x%x)",
			read_error != GL_NO_ERROR ? read_error : restore_error);
	for (unsigned int y = 0; y < frame->height; ++y) {
		const uint8_t *source =
			frame->rgba +
			(size_t)(frame->height - y - 1U) * frame->width * 4U;
		uint16_t *destination = pixels + (size_t)y * frame->width;

		for (unsigned int x = 0; x < frame->width; ++x) {
			unsigned int red = (source[0] * 31U + 127U) / 255U;
			unsigned int green = (source[1] * 63U + 127U) / 255U;
			unsigned int blue = (source[2] * 31U + 127U) / 255U;

			destination[x] =
				(uint16_t)((red << 11) | (green << 5) | blue);
			source += 4U;
		}
	}
	return true;
}

void fplinux_gpu_frame_close(struct fplinux_gpu_frame *frame)
{
	if (!frame)
		return;
	free(frame->rgba);
	free(frame);
}
