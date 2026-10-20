/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel-copy.h"

void sc9820e_copy_kernel(unsigned char *destination,
			 const unsigned char *source, size_t bytes)
{
	while (bytes != 0) {
		*destination++ = *source++;
		--bytes;
	}
}
