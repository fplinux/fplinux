/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_SETUP_HOST_KERNEL_H
#define FPLINUX_MARLIN2_SETUP_HOST_KERNEL_H

#include "../../bluetooth-compat/linux/kernel.h"
#include <stddef.h>
#include <linux/errno.h>

#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, data) ((value) = (data))

#endif
