/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-softkey-layout.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition);                               \
			return 1;                                          \
		}                                                          \
	} while (0)

static int check_layout(void)
{
	static const struct {
		int widths[3];
		struct fplinux_softkey_box boxes[3];
	} cases[] = {
		{ { 0, 0, 0 }, { { 8, 0 }, { 120, 0 }, { 232, 0 } } },
		{ { 32, 0, 0 }, { { 8, 32 }, { 120, 0 }, { 232, 0 } } },
		{ { 0, 32, 0 }, { { 8, 0 }, { 104, 32 }, { 232, 0 } } },
		{ { 0, 0, 32 }, { { 8, 0 }, { 120, 0 }, { 200, 32 } } },
		{ { 32, 32, 0 }, { { 8, 32 }, { 104, 32 }, { 232, 0 } } },
		{ { 32, 0, 32 }, { { 8, 32 }, { 120, 0 }, { 200, 32 } } },
		{ { 0, 32, 32 }, { { 8, 0 }, { 104, 32 }, { 200, 32 } } },
		{ { 32, 32, 32 }, { { 8, 32 }, { 104, 32 }, { 200, 32 } } },
		{ { 120, 32, 0 }, { { 8, 120 }, { 136, 32 }, { 232, 0 } } },
		{ { 200, 100, 100 }, { { 8, 70 }, { 86, 69 }, { 163, 69 } } },
		{ { 150, 0, 150 }, { { 8, 108 }, { 120, 0 }, { 124, 108 } } },
	};
	struct fplinux_softkey_box boxes[3];

	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
		fplinux_softkey_layout(240, 8, 8, cases[i].widths, boxes);
		for (int slot = 0; slot < 3; ++slot) {
			CHECK(boxes[slot].x == cases[i].boxes[slot].x);
			CHECK(boxes[slot].width == cases[i].boxes[slot].width);
		}
	}
	/* The smallest phone profile, with a short neighbour kept whole. */
	fplinux_softkey_layout(128, 2, 4, (int[]){ 100, 12, 100 }, boxes);
	CHECK(boxes[0].x == 2 && boxes[0].width == 52);
	CHECK(boxes[1].x == 58 && boxes[1].width == 12);
	CHECK(boxes[2].x == 74 && boxes[2].width == 52);
	/* Three visible form actions stay distinct on the 128-pixel profile. */
	fplinux_softkey_layout(128, 2, 4, (int[]){ 54, 48, 36 }, boxes);
	CHECK(boxes[0].x == 2 && boxes[0].width == 40);
	CHECK(boxes[1].x == 46 && boxes[1].width == 40);
	CHECK(boxes[2].x == 90 && boxes[2].width == 36);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "layout"))
		return check_layout();
	return 2;
}
