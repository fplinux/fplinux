/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel-copy.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
	unsigned char source[] = { 0xc1, 0x01, 0x02, 0x00, 0x04,
				   0xfe, 0x06, 0x07, 0xc2 };
	const unsigned char source_expected[] = { 0xc1, 0x01, 0x02, 0x00, 0x04,
						  0xfe, 0x06, 0x07, 0xc2 };
	unsigned char destination[] = { 0xa1, 0xa2, 0xcc, 0xcc, 0xcc,
					0xcc, 0xcc, 0xa3, 0xa4 };
	const unsigned char expected[] = { 0xa1, 0xa2, 0x02, 0x00, 0x04,
					   0xfe, 0x06, 0xa3, 0xa4 };
	unsigned char complete[11] = { 0xb1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xb2 };
	const unsigned char complete_expected[] = { 0xb1, 0xc1, 0x01, 0x02,
						    0x00, 0x04, 0xfe, 0x06,
						    0x07, 0xc2, 0xb2 };

	sc9820e_copy_kernel(destination + 2, source + 2, 5);
	if (memcmp(destination, expected, sizeof(expected)) != 0 ||
	    memcmp(source, source_expected, sizeof(source_expected)) != 0) {
		fputs("partial copy changed a boundary or source byte\n",
		      stderr);
		return 1;
	}
	sc9820e_copy_kernel(destination + 1, source + 1, 0);
	sc9820e_copy_kernel(NULL, NULL, 0);
	if (memcmp(destination, expected, sizeof(expected)) != 0) {
		fputs("empty copy changed its destination\n", stderr);
		return 1;
	}
	sc9820e_copy_kernel(complete + 1, source, sizeof(source));
	if (memcmp(complete, complete_expected, sizeof(complete_expected)) !=
		    0 ||
	    memcmp(source, source_expected, sizeof(source_expected)) != 0) {
		fputs("complete copy changed a boundary or source byte\n",
		      stderr);
		return 1;
	}
	return 0;
}
