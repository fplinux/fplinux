/* SPDX-License-Identifier: GPL-2.0-only */
#include "ram-probe.h"

#include <stdio.h>
#include <string.h>

struct probe_case {
	const char *name;
	unsigned int word_indices[3];
	uint32_t expected_size;
};

static int check_probe(const struct probe_case *test)
{
	const uint32_t original[] = { 0xaabbccdd, 0x00726f74, 0xfeedcafe,
				      0x12345679, 0x11223344 };
	uint32_t memory[5];
	volatile uint32_t *probe_words[3];
	uint32_t size;
	unsigned int i;

	memcpy(memory, original, sizeof(memory));
	for (i = 0; i < 3; ++i)
		probe_words[i] = &memory[test->word_indices[i]];

	size = ums9117_probe_ram_size(probe_words);
	if (size != test->expected_size) {
		fprintf(stderr, "%s: detected %u bytes, expected %u\n",
			test->name, (unsigned int)size,
			(unsigned int)test->expected_size);
		return 1;
	}
	if (memcmp(memory, original, sizeof(memory)) != 0) {
		fprintf(stderr, "%s: RAM contents changed during probing\n",
			test->name);
		return 1;
	}
	return 0;
}

int main(void)
{
	static const struct probe_case cases[] = {
		{ "distinct words", { 1, 2, 3 }, 0x04000000 },
		{ "first and second alias", { 1, 1, 3 }, 0x02000000 },
		{ "first and third alias", { 1, 2, 1 }, 0x03000000 },
		{ "second and third alias", { 1, 2, 2 }, 0x03000000 },
		{ "all words alias", { 1, 1, 1 }, 0x02000000 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		if (check_probe(&cases[i]) != 0)
			return 1;
	}
	return 0;
}
