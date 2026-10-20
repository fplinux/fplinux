// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <linux/soc/sprd/vbc-fifo.h>

static void check_forward_occupancy(void)
{
	assert(sprd_vbc_fifo_read(0x0a09) == 5);
	assert(sprd_vbc_fifo_write(0x0a09) == 9);
	assert(sprd_vbc_fifo_frames(0x0a09) == 4);
	assert(sprd_vbc_fifo_capture_frames(0x0a09) == 4);
}

static void check_wrapped_occupancy(void)
{
	assert(sprd_vbc_fifo_read(0x27c02) == 318);
	assert(sprd_vbc_fifo_write(0x27c02) == 2);
	assert(sprd_vbc_fifo_frames(0x27c02) == 4);
	assert(sprd_vbc_fifo_capture_frames(0x27c02) == 4);
}

static void check_equal_empty_and_full(void)
{
	assert(sprd_vbc_fifo_read(0x160b) == 11);
	assert(sprd_vbc_fifo_write(0x160b) == 11);
	assert(sprd_vbc_fifo_frames(0x160b) == 0);
	assert(sprd_vbc_fifo_frames(0x40e07) == 0);
	assert(sprd_vbc_fifo_capture_frames(0x40e07) == 0);
	assert(sprd_vbc_fifo_frames(0x80000) == 320);
	assert(sprd_vbc_fifo_capture_frames(0x80000) == -EPIPE);
}

static void check_pointer_errors_and_flag_precedence(void)
{
	assert(sprd_vbc_fifo_read(0x28000) == 320);
	assert(sprd_vbc_fifo_frames(0x28000) == -EIO);
	assert(sprd_vbc_fifo_capture_frames(0x28000) == -EIO);
	assert(sprd_vbc_fifo_write(0x001ff) == 511);
	assert(sprd_vbc_fifo_frames(0x001ff) == -EIO);
	assert(sprd_vbc_fifo_capture_frames(0x001ff) == -EIO);
	assert(sprd_vbc_fifo_frames(0x40000 | 0x28000) == -EIO);
	assert(sprd_vbc_fifo_capture_frames(0x40000 | 0x28000) == -EIO);
	assert(sprd_vbc_fifo_frames(0xe8000) == -EIO);
	assert(sprd_vbc_fifo_capture_frames(0xe8000) == -EPIPE);
	assert(sprd_vbc_fifo_frames(0xc2800) == 0);
	assert(sprd_vbc_fifo_capture_frames(0xc0e09) == -EPIPE);
}

int main(void)
{
	check_forward_occupancy();
	check_wrapped_occupancy();
	check_equal_empty_and_full();
	check_pointer_errors_and_flag_precedence();
	puts("VBC FIFO status checks passed");
	return 0;
}
