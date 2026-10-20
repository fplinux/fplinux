// SPDX-License-Identifier: GPL-2.0-or-later
/* fplinux-check: package-embedded */
#define FPLINUX_QUAKE_GL_DISPATCH_IMPLEMENTATION

#include "fplinux-quake-gl.h"
#include "fplinux-gpu.h"

#include <stdio.h>

#define FPLINUX_QUAKE_GL_DEFINE(name) __typeof__(&name) fplinux_quake_##name;
FPLINUX_QUAKE_GL_FUNCTIONS(FPLINUX_QUAKE_GL_DEFINE)
#undef FPLINUX_QUAKE_GL_DEFINE

bool fplinux_quake_gl_load(struct fplinux_gpu *gpu, char *error, size_t size)
{
#define FPLINUX_QUAKE_GL_LOAD(name)                                           \
	fplinux_quake_##name = fplinux_gpu_get_proc(gpu, #name);              \
	if (!fplinux_quake_##name) {                                          \
		snprintf(error, size, "OpenGL entry point %s is unavailable", \
			 #name);                                              \
		return false;                                                 \
	}
	FPLINUX_QUAKE_GL_FUNCTIONS(FPLINUX_QUAKE_GL_LOAD)
#undef FPLINUX_QUAKE_GL_LOAD

	return true;
}

void fplinux_quake_gl_draw_buffer(GLenum mode)
{
	/* The EGL device pbuffer has one front buffer, including during loading. */
	if (mode == GL_FRONT || mode == GL_BACK)
		mode = GL_FRONT;
	fplinux_quake_glDrawBuffer(mode);
}

void fplinux_quake_gl_read_buffer(GLenum mode)
{
	if (mode == GL_FRONT || mode == GL_BACK)
		mode = GL_FRONT;
	fplinux_quake_glReadBuffer(mode);
}
