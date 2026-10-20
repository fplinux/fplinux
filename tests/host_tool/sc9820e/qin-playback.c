// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "qin-playback.h"

struct output {
	u32 left[1024];
	u32 right[1024];
	unsigned int count;
};

static void write_frame(void *context, u32 left, u32 right)
{
	struct output *output = context;

	assert(output->count < 1024);
	output->left[output->count] = left;
	output->right[output->count++] = right;
}

static void test_samples(void)
{
	const u8 samples[] = { 0x34, 0x12, 0xff, 0x7f, 0xff, 0xff, 0x00, 0x80,
			       0x00, 0x00, 0xfe, 0xff, 0x00, 0x80, 0x01, 0x00 };
	struct qin_playback playback = {};
	struct qin_playback_buffer buffer = {
		.samples = samples,
		.application = 4,
		.boundary = 16,
		.frames = 4,
		.period = 2,
	};
	struct output output = {};

	assert(qin_playback_prime(&playback, &buffer, 0x40000, 0x40000,
				  write_frame, &output) == 0);
	assert(output.count == 4);
	assert(output.left[0] == 0x1234 && output.right[0] == 0x7fff);
	assert(output.left[1] == 0xffffffff && output.right[1] == 0xffff8000);
	assert(output.left[2] == 0 && output.right[2] == 0xfffffffe);
	assert(output.left[3] == 0xffff8000 && output.right[3] == 1);
	assert(playback.submitted_frames == 4 && playback.consumed_frames == 0);
	assert(playback.hw_pos == 0 && playback.submit_ptr == 4);
	assert(qin_playback_check_prefill(&playback, 4, 4) == 0);
	assert(qin_playback_check_prefill(&playback, 4, 0x40000) == -EIO);
	assert(qin_playback_check_prefill(&playback, 0x40000, 4) == -EIO);
	assert(qin_playback_check_prefill(&playback, 0x1ff, 4) == -EIO);
}

static void test_consume_and_drain(void)
{
	const u8 samples[2048] = {};
	struct qin_playback playback = {};
	struct qin_playback_buffer buffer = {
		.samples = samples,
		.application = 512,
		.boundary = 4096,
		.frames = 512,
		.period = 192,
	};
	struct qin_playback_step step;
	struct output output = {};

	assert(qin_playback_prime(&playback, &buffer, 0x40000, 0x40000,
				  write_frame, &output) == 0);
	assert(output.count == 320 && playback.hw_pos == 0);
	/* The lane snapshots hold 272/273 frames: only 47 are safely consumed. */
	assert(qin_playback_service(&playback, &buffer, 0x6000, 0x5e00,
				    write_frame, &output,
				    &step) == QIN_PLAYBACK_IDLE);
	assert(step.advanced == 47 && step.written == 47);
	assert(playback.submitted_frames == 367 &&
	       playback.consumed_frames == 47);
	assert(playback.hw_pos == 47 && output.count == 367);
	assert(qin_playback_service(&playback, &buffer, 0xc02f, 0xc02f,
				    write_frame, &output,
				    &step) == QIN_PLAYBACK_IDLE);
	assert(step.advanced == 49 && step.written == 49);
	assert(playback.submitted_frames == 416 &&
	       playback.consumed_frames == 96);
	assert(qin_playback_service(&playback, &buffer, 0x18060, 0x18060,
				    write_frame, &output,
				    &step) == QIN_PLAYBACK_PERIOD);
	assert(step.advanced == 96 && step.written == 96);
	assert(playback.submitted_frames == 512 &&
	       playback.consumed_frames == 192);
	assert(playback.hw_pos == 192 && output.count == 512);
	buffer.draining = true;
	assert(qin_playback_service(&playback, &buffer, 0xe0c0, 0xe0c0,
				    write_frame, &output,
				    &step) == QIN_PLAYBACK_PERIOD);
	assert(step.advanced == 240 && step.written == 0);
	assert(playback.consumed_frames == 432 && playback.hw_pos == 432);
	assert(qin_playback_service(&playback, &buffer, 0x580c0, 0x580c0,
				    write_frame, &output,
				    &step) == QIN_PLAYBACK_DRAINED);
	assert(step.advanced == 80 && step.written == 0);
	assert(playback.consumed_frames == 512 && playback.hw_pos == 0);
	assert(output.count == 512);
}

static void test_ring_wrap(void)
{
	const u8 samples[64] = {
		[0] = 1,   [4] = 2,   [8] = 3,	 [12] = 4,
		[16] = 5,  [20] = 6,  [24] = 7,	 [28] = 8,
		[32] = 9,  [36] = 10, [40] = 11, [44] = 12,
		[48] = 13, [52] = 14, [56] = 15, [60] = 16,
	};
	struct qin_playback playback = {
		.submit_ptr = 30,
		.hw_pos = 10,
		.period_frames = 2,
		.submitted_frames = 30,
		.consumed_frames = 26,
	};
	struct qin_playback_buffer buffer = {
		.samples = samples,
		.application = 3,
		.boundary = 32,
		.frames = 16,
		.period = 4,
	};
	struct qin_playback_step step;
	struct output output = {};
	unsigned long free_frames;

	assert(qin_playback_service(&playback, &buffer, 4, 4, write_frame,
				    &output, &step) == QIN_PLAYBACK_IDLE);
	assert(output.count == 5);
	assert(output.left[0] == 15 && output.left[1] == 16);
	assert(output.left[2] == 1 && output.left[3] == 2 &&
	       output.left[4] == 3);
	assert(playback.submit_ptr == 3 && playback.submitted_frames == 35);
	assert(playback.consumed_frames == 26 && playback.hw_pos == 10);
	assert(qin_capture_ring_free(3, 30, 32, 16, &free_frames) == 0);
	assert(free_frames == 11);
}

static void test_underrun(void)
{
	const u8 samples[2048] = {};
	struct qin_playback playback = {};
	struct qin_playback_buffer buffer = {
		.samples = samples,
		.application = 512,
		.boundary = 4096,
		.frames = 512,
		.period = 192,
	};
	struct qin_playback_step step;
	struct output output = {};

	assert(qin_playback_prime(&playback, &buffer, 0x40000, 0x40000,
				  write_frame, &output) == 0);
	assert(qin_playback_service(&playback, &buffer, 0x40000, 0x40000,
				    write_frame, &output, &step) == -EPIPE);
	assert(playback.consumed_frames == 320 && playback.hw_pos == 320);
	assert(output.count == 320 && step.written == 0);
	/* DRAINING cannot hide a discontinuity while committed data remains. */
	buffer.draining = true;
	assert(qin_playback_service(&playback, &buffer, 0x40000, 0x40000,
				    write_frame, &output, &step) == -EPIPE);
	assert(output.count == 320);
	buffer.application = 320;
	buffer.draining = false;
	assert(qin_playback_service(&playback, &buffer, 0x40000, 0x40000,
				    write_frame, &output, &step) == -EPIPE);
	buffer.draining = true;
	assert(qin_playback_service(&playback, &buffer, 0x40000, 0x40000,
				    write_frame, &output,
				    &step) == QIN_PLAYBACK_DRAINED);
}

static void test_invalid_state(void)
{
	const u8 samples[32] = {};
	struct qin_playback playback = {};
	struct qin_playback_buffer buffer = {
		.samples = samples,
		.application = 0,
		.boundary = 32,
		.frames = 8,
		.period = 4,
	};
	struct qin_playback_step step;
	struct output output = {};

	assert(qin_playback_prime(&playback, &buffer, 0x40000, 0x40000,
				  write_frame, &output) == -EPIPE);
	buffer.application = 4;
	assert(qin_playback_prime(&playback, &buffer, 0x40000, 0x40000,
				  write_frame, &output) == 0);
	assert(qin_playback_service(&playback, &buffer, 0x1ff, 4, write_frame,
				    &output, &step) == -EIO);
	assert(qin_playback_service(&playback, &buffer, 5, 5, write_frame,
				    &output, &step) == -EIO);
	buffer.application = 32;
	assert(qin_playback_service(&playback, &buffer, 4, 4, write_frame,
				    &output, &step) == -EPIPE);
	assert(output.count == 4);
	/* A new PREPARE starts with no inherited FIFO tail or accounting. */
	playback = (struct qin_playback){};
	buffer.application = 4;
	assert(qin_playback_prime(&playback, &buffer, 0x40000, 0x40000,
				  write_frame, &output) == 0);
	assert(playback.submitted_frames == 4 && playback.consumed_frames == 0);
	assert(playback.hw_pos == 0 && output.count == 8);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "samples"))
		test_samples();
	else if (!strcmp(argv[1], "consume-drain"))
		test_consume_and_drain();
	else if (!strcmp(argv[1], "ring-wrap"))
		test_ring_wrap();
	else if (!strcmp(argv[1], "underrun"))
		test_underrun();
	else if (!strcmp(argv[1], "invalid-state"))
		test_invalid_state();
	else
		return 2;
	puts("Qin playback helper contracts passed");
	return 0;
}
