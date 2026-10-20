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

static int measure(const char *text, void *context)
{
	int width = 0;
	int advance = *(int *)context;

	for (; *text; ++text)
		if (((unsigned char)*text & 0xc0) != 0x80)
			width += advance;
	return width;
}

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
	return 0;
}

static int check_fitting(void)
{
	static const struct {
		const char *text;
		int width;
		const char *expected;
	} cases[] = {
		{ "Hello", 30, "Hello" },
		{ "Калькулятор", 66, "Калькулятор" },
		{ "Калькулятор", 36, "Кальк…" },
		{ "A🙂中BC", 18, "A🙂…" },
		{ "Мои заметки", 30, "Мои…" },
		{ "Ab   cdefgh", 30, "Ab…" },
		{ "Hello", 0, "…" },
		{ "Hello", 5, "…" },
		{ "", 0, "" },
	};
	int advance = 6;
	char result[64];

	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
		fplinux_softkey_fit_text(cases[i].text, cases[i].width, measure,
					 &advance, result, sizeof(result));
		CHECK(!strcmp(result, cases[i].expected));
	}
	memset(result, 'x', sizeof(result));
	fplinux_softkey_fit_text("Hello", 0, measure, &advance, result, 0);
	CHECK(result[0] == 'x');
	fplinux_softkey_fit_text("Hello", 0, measure, &advance, result, 1);
	CHECK(result[0] == '\0' && result[1] == 'x');
	fplinux_softkey_fit_text("Калькулятор", 18, measure, &advance, result,
				 8);
	CHECK(!strcmp(result, "Ка…") && result[8] == 'x');
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "layout"))
		return check_layout();
	if (!strcmp(argv[1], "fitting"))
		return check_fitting();
	return 2;
}
