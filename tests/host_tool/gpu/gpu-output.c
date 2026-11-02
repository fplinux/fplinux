/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <GLES3/gl3.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fplinux-gpu-output.h"
#include "gpu-output-fake.h"

#define GL_COLOR_LOGIC_OP 0x0bf2
#define GL_FRAMEBUFFER_SRGB 0x8db9
#define GL_ALPHA_TEST 0x0bc0
#define GL_LINE 0x1b01
#define GL_POINT 0x1b00

static void caller_state(unsigned int seed)
{
	struct gl_state *state = &fake.state;

	state->framebuffer = 170 + seed;
	state->renderbuffer = 180 + seed;
	state->viewport[0] = 3 + seed;
	state->viewport[1] = 5 + seed;
	state->viewport[2] = 103 + seed;
	state->viewport[3] = 71 + seed;
	state->program = 190 + seed;
	state->vertex_array = 200 + seed;
	state->active_texture = GL_TEXTURE0 + 3;
	state->textures[0] = 210 + seed;
	state->textures[3] = 220 + seed;
	state->mask[0] = GL_FALSE;
	state->mask[1] = GL_TRUE;
	state->mask[2] = GL_FALSE;
	state->mask[3] = GL_TRUE;
	state->enabled[GL_BLEND] = GL_TRUE;
	state->enabled[GL_DEPTH_TEST] = GL_TRUE;
	state->enabled[GL_SCISSOR_TEST] = GL_TRUE;
	state->enabled[GL_DITHER] = GL_TRUE;
	state->enabled[GL_SAMPLE_COVERAGE] = GL_TRUE;
	state->enabled[GL_COLOR_LOGIC_OP] = GL_TRUE;
	state->enabled[GL_FRAMEBUFFER_SRGB] = GL_TRUE;
	state->enabled[GL_ALPHA_TEST] = GL_TRUE;
	state->polygon_mode[0] = GL_LINE;
	state->polygon_mode[1] = GL_POINT;
}

static void assert_state(const struct gl_state *expected)
{
	assert(fake.state.framebuffer == expected->framebuffer);
	assert(fake.state.renderbuffer == expected->renderbuffer);
	assert(fake.state.program == expected->program);
	assert(fake.state.vertex_array == expected->vertex_array);
	assert(fake.state.active_texture == expected->active_texture);
	assert(!memcmp(fake.state.viewport, expected->viewport,
		       sizeof(expected->viewport)));
	assert(!memcmp(fake.state.textures, expected->textures,
		       sizeof(expected->textures)));
	assert(!memcmp(fake.state.mask, expected->mask,
		       sizeof(expected->mask)));
	assert(!memcmp(fake.state.enabled, expected->enabled,
		       sizeof(expected->enabled)));
	assert(!memcmp(fake.state.polygon_mode, expected->polygon_mode,
		       sizeof(expected->polygon_mode)));
}

static void assert_clean(void)
{
	for (unsigned int id = 0; id < 256; ++id) {
		assert(!fake.shaders[id] && !fake.programs[id] &&
		       !fake.arrays[id]);
		assert(!fake.framebuffers[id] && !fake.renderbuffers[id]);
	}
	assert(!fake.images[0] && !fake.images[1]);
	assert(!fake.fence_live && !fake.gpu.live);
	assert(!fake.cpu_access && session.present_fence < 0);
	assert(!session.buffer_acquired);
	assert(fake.exported_fd == -1);
	assert(fake.releases == 1 && fake.closes == 1);
	assert(fake.dispatches == 0);
}

static struct fplinux_gpu_output *open_output(bool desktop)
{
	struct fplinux_gpu_output *output;
	char error[256];

	output = fplinux_gpu_output_open(
		&session, desktop ? FPLINUX_GPU_OPENGL2 : FPLINUX_GPU_GLES3, 8,
		12, true, error, sizeof(error));
	if (!output)
		fprintf(stderr, "open: %s\n", error);
	assert(output && !error[0]);
	assert(fplinux_gpu_output_gpu(output) == &fake.gpu);
	return output;
}

static void expect_failed_frame(struct fplinux_gpu_output *output,
				int expected_errno)
{
	char first_error[256];
	void *pixels = (void *)(uintptr_t)123;
	size_t stride = 456;
	unsigned int draw_calls = fake.draw_calls;
	unsigned int present_calls = fake.present_calls;

	assert(errno == expected_errno);
	snprintf(first_error, sizeof(first_error), "%s",
		 fplinux_gpu_output_error(output));
	assert(first_error[0]);
	fake.failure = NONE;
	errno = 0;
	assert(!fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
	assert(errno == expected_errno && pixels == (void *)(uintptr_t)123 &&
	       stride == 456);
	assert(!fplinux_gpu_output_present(output) && errno == expected_errno);
	assert(!fplinux_gpu_output_render(output, 77) &&
	       errno == expected_errno);
	assert(!strcmp(fplinux_gpu_output_error(output), first_error));
	assert(fake.draw_calls == draw_calls &&
	       fake.present_calls == present_calls);
}

static void normal_frames(bool desktop)
{
	struct fplinux_gpu_output *output;
	struct gl_state expected;
	void *pixels;
	size_t stride;
	const unsigned int selected[] = { 0, 1, 1, 0 };

	reset(NONE);
	caller_state(0);
	expected = fake.state;
	output = open_output(desktop);
	assert_state(&expected);
	assert(fake.image_calls == 2);
	for (unsigned int frame = 0; frame < 4; ++frame) {
		unsigned int shown;

		caller_state(frame + 1);
		expected = fake.state;
		/* A fresh frame must use the current shown page, including external changes. */
		if (frame == 2) {
			assert(fplinux_drm_session_wait_present(&session));
			session.shown_page = 0;
		}
		fake.sync_starts = 0;
		fake.present_calls = 0;
		assert(fplinux_gpu_output_render(output, 77));
		assert_state(&expected);
		assert(fake.last_draw_page == selected[frame] &&
		       fake.fence_complete);
		assert(session.present_fence < 0);
		assert(!fake.sync_starts && !fake.present_calls);
		assert(fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
		assert(pixels == mapping + selected[frame] * 96 &&
		       stride == 20);
		assert(fplinux_gpu_output_end_cpu(output));
		shown = session.shown_page;
		assert(fplinux_gpu_output_present(output));
		assert(fake.last_present_page == selected[frame]);
		assert(session.shown_page == shown &&
		       session.pending_page == selected[frame] &&
		       session.present_fence >= 0);
		assert(fake.completed_presentations == frame);
		assert(fake.dispatches == 0);
	}
	assert(fake.wait_calls == 4 && fake.sync_ends == 4);
	assert(fplinux_gpu_output_close(output));
	assert(session.shown_page == 0 && fake.completed_presentations == 4 &&
	       fake.presentation_waits == 4);
	assert_clean();
	/* Closing releases the exclusive borrow and owned descriptor for reopening. */
	fake.image_calls = 0;
	fake.releases = 0;
	fake.closes = 0;
	output = open_output(desktop);
	assert(fplinux_gpu_output_close(output));
	assert_clean();
}

static void setup_failure(enum failure failure)
{
	struct gl_state expected;
	char error[256];

	reset(failure);
	caller_state(0);
	expected = fake.state;
	assert(!fplinux_gpu_output_open(&session, FPLINUX_GPU_GLES3, 8, 12,
					true, error, sizeof(error)));
	assert(errno == EIO && error[0]);
	assert_state(&expected);
	assert_clean();
}

static void render_failure(enum failure failure, bool producer_error)
{
	struct fplinux_gpu_output *output;
	struct gl_state expected;

	reset(failure);
	output = open_output(false);
	caller_state(3);
	expected = fake.state;
	if (producer_error)
		fake.error = GL_INVALID_OPERATION;
	assert(!fplinux_gpu_output_render(output, 77));
	assert_state(&expected);
	assert(!fake.sync_starts && !fake.present_calls && !fake.fence_live);
	assert(fake.draw_calls == (producer_error ? 0U : 1U));
	expect_failed_frame(
		output, failure == WAIT_TIMEOUT ||
					failure == WAIT_TIMEOUT_DELETE_ERROR ?
				ETIMEDOUT :
				EIO);
	assert(fplinux_gpu_output_close(output));
	assert_clean();
}

static void cpu_failure(enum failure failure)
{
	struct fplinux_gpu_output *output;
	void *pixels;
	size_t stride;

	reset(failure);
	output = open_output(false);
	assert(fplinux_gpu_output_render(output, 77));
	if (failure == CPU_START) {
		assert(!fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
		expect_failed_frame(output, EACCES);
		assert(fake.sync_starts == 1 && fake.sync_ends == 0);
	} else {
		assert(fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
		assert(!fplinux_gpu_output_end_cpu(output));
		expect_failed_frame(output, EBUSY);
		assert(fake.cpu_access && fake.sync_ends == 1);
	}
	assert(fplinux_gpu_output_close(output));
	assert(fake.sync_ends == (failure == CPU_END ? 2U : 0U));
	assert(!fake.cpu_access);
	assert_clean();
}

static void present_failure(void)
{
	struct fplinux_gpu_output *output;
	unsigned int shown;

	reset(PRESENT);
	output = open_output(false);
	shown = session.shown_page;
	assert(fplinux_gpu_output_render(output, 77));
	assert(!fplinux_gpu_output_present(output) && errno == ENODEV);
	assert(session.shown_page == shown && fake.present_calls == 1);
	/* The first failed presentation terminates this output without retrying it. */
	fake.present_calls = 0;
	expect_failed_frame(output, ENODEV);
	assert(fplinux_gpu_output_close(output));
	assert_clean();
}

static void close_cpu_access(void)
{
	struct fplinux_gpu_output *output;
	void *pixels;
	size_t stride;

	reset(NONE);
	output = open_output(false);
	assert(fplinux_gpu_output_render(output, 77));
	assert(fplinux_gpu_output_begin_cpu(output, &pixels, &stride));
	assert(fplinux_gpu_output_close(output));
	assert(fake.sync_ends == 1 && !fake.cpu_access && !fake.present_calls);
	assert_clean();
}

static void presentation_wait_failure(void)
{
	struct fplinux_gpu_output *output;
	struct gl_state expected;
	unsigned int imported_binds;
	unsigned int scene_submissions;

	reset(NONE);
	output = open_output(false);
	assert(fplinux_gpu_output_render(output, 77));
	assert(fplinux_gpu_output_present(output));
	assert(session.shown_page == 1 && session.present_fence >= 0);
	caller_state(4);
	expected = fake.state;
	imported_binds = fake.imported_binds;
	scene_submissions = fake.scene_submissions;
	fake.failure = PRESENT_WAIT;
	assert(!fplinux_gpu_output_render(output, 77));
	assert(errno == ETIMEDOUT);
	assert_state(&expected);
	assert(fake.scene_submissions > scene_submissions);
	assert(fake.imported_binds == imported_binds && fake.draw_calls == 1 &&
	       fake.wait_calls == 1 && !fake.sync_starts);
	assert(session.shown_page == 1 && session.present_fence < 0 &&
	       !fake.completed_presentations);
	expect_failed_frame(output, ETIMEDOUT);
	/* A later cleanup error must not replace the deferred presentation error. */
	fake.destroy_image_error = true;
	assert(!fplinux_gpu_output_close(output) && errno == ETIMEDOUT);
	assert_clean();
}

static void close_pending_presentation_failure(void)
{
	struct fplinux_gpu_output *output;

	reset(NONE);
	output = open_output(false);
	assert(fplinux_gpu_output_render(output, 77));
	assert(fplinux_gpu_output_present(output));
	fake.failure = PRESENT_WAIT;
	fake.destroy_image_error = true;
	assert(!fplinux_gpu_output_close(output) && errno == ETIMEDOUT);
	assert(fake.presentation_waits == 1 && !fake.completed_presentations &&
	       session.shown_page == 1);
	assert_clean();
}

static void reject_unsupported_display(void)
{
	const struct {
		uint32_t format;
		unsigned int source_width;
	} cases[] = {
		{ DRM_FORMAT_NV16, 8 },
		{ DRM_FORMAT_RGB565, 7 },
	};
	char error[256];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		reset(NONE);
		session.format = cases[i].format;
		assert(!fplinux_gpu_output_open(&session, FPLINUX_GPU_GLES3,
						cases[i].source_width, 12, true,
						error, sizeof(error)));
		assert(errno == EINVAL && error[0]);
		assert(!session.buffer_acquired && fake.exported_fd == -1);
		assert(!fake.gpu.live && !fake.image_calls);
		assert(!fake.releases && !fake.closes);
	}
}

int main(void)
{
	normal_frames(false);
	normal_frames(true);
	setup_failure(IMAGE_TWO);
	setup_failure(INCOMPLETE);
	setup_failure(PROGRAM);
	render_failure(NONE, true);
	render_failure(DRAW, false);
	render_failure(WAIT_ERROR, false);
	render_failure(WAIT_TIMEOUT, false);
	render_failure(WAIT_TIMEOUT_DELETE_ERROR, false);
	cpu_failure(CPU_START);
	cpu_failure(CPU_END);
	present_failure();
	close_cpu_access();
	presentation_wait_failure();
	close_pending_presentation_failure();
	reject_unsupported_display();
	puts("gpu-output host contracts: PASS (18 scenarios)");
	return 0;
}
