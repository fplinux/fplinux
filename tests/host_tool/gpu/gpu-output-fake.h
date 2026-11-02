/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_OUTPUT_FAKE_H
#define GPU_OUTPUT_FAKE_H

#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stdint.h>

#include "fplinux-drm-session.h"

enum failure {
	NONE,
	IMAGE_TWO,
	INCOMPLETE,
	PROGRAM,
	DRAW,
	WAIT_ERROR,
	WAIT_TIMEOUT,
	WAIT_TIMEOUT_DELETE_ERROR,
	CPU_START,
	CPU_END,
	PRESENT,
	PRESENT_WAIT,
};

struct gl_state {
	GLint framebuffer;
	GLint renderbuffer;
	GLint viewport[4];
	GLint program;
	GLint vertex_array;
	GLenum active_texture;
	GLint textures[8];
	GLboolean mask[4];
	GLboolean enabled[65536];
	GLint polygon_mode[2];
};

struct fplinux_gpu {
	bool live;
	bool desktop;
};

struct gpu_output_fake {
	struct gl_state state;
	struct fplinux_gpu gpu;
	enum failure failure;
	GLenum error;
	bool shaders[256];
	bool programs[256];
	bool arrays[256];
	bool framebuffers[256];
	bool renderbuffers[256];
	bool images[2];
	unsigned int image_calls;
	unsigned int draw_calls;
	unsigned int wait_calls;
	unsigned int sync_starts;
	unsigned int sync_ends;
	unsigned int present_calls;
	unsigned int presentation_waits;
	unsigned int completed_presentations;
	unsigned int scene_submissions;
	unsigned int imported_binds;
	unsigned int releases;
	unsigned int closes;
	unsigned int dispatches;
	unsigned int last_draw_page;
	unsigned int last_present_page;
	int exported_fd;
	bool fence_live;
	bool fence_complete;
	bool cpu_access;
	bool destroy_image_error;
};

extern struct gpu_output_fake fake;
extern uint8_t mapping[192];
extern struct fplinux_drm_session session;

void reset(enum failure failure);

#endif
