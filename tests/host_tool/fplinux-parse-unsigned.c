/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-cli.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

int main(void)
{
	static const struct {
		const char *text;
		unsigned int minimum;
		unsigned int maximum;
		bool accepted;
		unsigned int expected;
	} cases[] = {
		{ "1", 1, 4, true, 1 },
		{ "+1", 1, 4, true, 1 },
		{ " \t1", 1, 4, true, 1 },
		{ "01", 1, 4, true, 1 },
		{ "4", 1, 4, true, 4 },
		{ "0", 0, 4, true, 0 },
		{ "-0", 0, 4, false, 0 },
		{ " \t-0", 0, 4, false, 0 },
		{ "0", 1, 4, false, 0 },
		{ "5", 1, 4, false, 0 },
		{ "", 0, 4, false, 0 },
		{ " ", 0, 4, false, 0 },
		{ "1 ", 0, 4, false, 0 },
		{ "1x", 0, 4, false, 0 },
		{ "0x1", 0, 4, false, 0 },
		{ "+", 0, 4, false, 0 },
		{ "1.0", 0, 4, false, 0 },
		{ "65535", 0, 65535, true, 65535 },
		{ "65536", 0, 65535, false, 0 },
		{ "-1", 0, 65535, false, 0 },
		{ "-1", 0, UINT_MAX, false, 0 },
		{ " -1", 0, UINT_MAX, false, 0 },
		{ "-4294967295", 1, 4, false, 0 },
		{ "99999999999999999999999999999999999999", 0, UINT_MAX, false,
		  0 },
	};
	size_t index;
	char maximum[32];
	unsigned int value = 0;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		bool accepted;

		value = 12345;
		errno = ERANGE;
		accepted = fplinux_cli_unsigned(cases[index].text,
						cases[index].minimum,
						cases[index].maximum, &value);
		assert(accepted == cases[index].accepted);
		assert(value == (accepted ? cases[index].expected : 12345));
	}
	snprintf(maximum, sizeof(maximum), "%u", UINT_MAX);
	assert(fplinux_cli_unsigned(maximum, 0, UINT_MAX, &value));
	assert(value == UINT_MAX);
	return 0;
}
