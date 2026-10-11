// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>

#include "ums9117-pcm-route.h"

int main(void)
{
	static const struct {
		unsigned int requested;
		bool headphones_only;
		unsigned int expected;
	} cases[] = {
		{ 0, false, 0 }, { 1, false, 1 }, { 2, false, 2 },
		{ 3, false, 3 }, { 0, true, 0 },  { 1, true, 1 },
		{ 2, true, 2 },	 { 3, true, 1 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		unsigned int actual = ums9117_pcm_effective_outputs(
			cases[i].requested, cases[i].headphones_only);

		if (actual != cases[i].expected) {
			fprintf(stderr, "route case %u: got %u, expected %u\n",
				i, actual, cases[i].expected);
			return 1;
		}
	}
	return 0;
}
