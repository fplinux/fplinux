/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_SOC_SPRD_VBC_FIFO_H
#define LINUX_SOC_SPRD_VBC_FIFO_H

#include <linux/errno.h>
#include <linux/types.h>

#define SPRD_VBC_FIFO_FRAMES 320U
#define SPRD_VBC_FIFO_EMPTY 0x40000U
#define SPRD_VBC_FIFO_FULL 0x80000U

static inline unsigned int sprd_vbc_fifo_read(u32 status)
{
	return (status >> 9) & 0x1ff;
}

static inline unsigned int sprd_vbc_fifo_write(u32 status)
{
	return status & 0x1ff;
}

static inline int sprd_vbc_fifo_frames(u32 status)
{
	unsigned int read_ptr = sprd_vbc_fifo_read(status);
	unsigned int write_ptr = sprd_vbc_fifo_write(status);

	if (read_ptr >= SPRD_VBC_FIFO_FRAMES ||
	    write_ptr >= SPRD_VBC_FIFO_FRAMES)
		return -EIO;
	if (status & SPRD_VBC_FIFO_EMPTY)
		return 0;
	if (status & SPRD_VBC_FIFO_FULL)
		return SPRD_VBC_FIFO_FRAMES;
	return (write_ptr + SPRD_VBC_FIFO_FRAMES - read_ptr) %
	       SPRD_VBC_FIFO_FRAMES;
}

static inline int sprd_vbc_fifo_capture_frames(u32 status)
{
	if (status & SPRD_VBC_FIFO_FULL)
		return -EPIPE;
	return sprd_vbc_fifo_frames(status);
}

#endif
