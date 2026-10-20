/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SC9820E_KERNEL_COPY_H
#define FPLINUX_SC9820E_KERNEL_COPY_H

#include <stddef.h>

/* Both accessible RAM ranges cover the requested byte count and are disjoint. */
void sc9820e_copy_kernel(unsigned char *destination,
			 const unsigned char *source, size_t bytes);

#endif
