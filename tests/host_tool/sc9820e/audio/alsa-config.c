// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition);                               \
			exit(1);                                           \
		}                                                          \
	} while (0)

static void check_string(snd_config_t *root, const char *path,
			 const char *expected)
{
	snd_config_t *node;
	const char *value;

	CHECK(snd_config_search(root, path, &node) == 0);
	CHECK(snd_config_get_string(node, &value) == 0);
	CHECK(!strcmp(value, expected));
}

static void check_rate(snd_config_t *root, const char *path, long expected)
{
	snd_config_t *node;
	long value;

	CHECK(snd_config_search(root, path, &node) == 0);
	CHECK(snd_config_get_integer(node, &value) == 0);
	CHECK(value == expected);
}

static void load_config(snd_config_t *root, const char *path)
{
	snd_input_t *input;

	CHECK(snd_input_stdio_open(&input, path, "r") == 0);
	CHECK(snd_config_load(root, input) == 0);
	CHECK(snd_input_close(input) == 0);
}

int main(int argc, char **argv)
{
	snd_config_t *root;
	snd_config_t *functions;
	snd_config_t *driver_function;
	snd_config_t *node;
	snd_config_t *identity;
	snd_config_t *expanded;

	CHECK(argc == 6);
	CHECK(snd_config_top(&root) == 0);
	if (strcmp(argv[2], "-"))
		load_config(root, argv[2]);
	load_config(root, argv[1]);
	CHECK(snd_config_make_compound(&functions, "func", 0) == 0);
	CHECK(snd_config_add(root, functions) == 0);
	CHECK(snd_config_make_compound(&driver_function, "card_driver", 0) ==
	      0);
	CHECK(snd_config_add(functions, driver_function) == 0);
	CHECK(snd_config_imake_string(&node, "lib", argv[3]) == 0);
	CHECK(snd_config_add(driver_function, node) == 0);
	CHECK(snd_config_imake_string(&node, "func", "qin_test_card_driver") ==
	      0);
	CHECK(snd_config_add(driver_function, node) == 0);
	CHECK(snd_config_imake_string(&identity, "driver", argv[4]) == 0);
	CHECK(snd_config_search(root, "pcm.default", &node) == 0);
	CHECK(snd_config_expand(node, root, NULL, identity, &expanded) >= 0);
	check_string(expanded, "type", "asym");
	check_string(expanded, "playback.pcm.type", "plug");
	check_string(expanded, "playback.pcm.slave.pcm", "hw:0,0");
	check_rate(expanded, "playback.pcm.slave.rate",
		   strtol(argv[5], NULL, 10));
	check_string(expanded, "capture.pcm.type", "plug");
	check_string(expanded, "capture.pcm.slave.pcm", "hw:0,0");
	check_rate(expanded, "capture.pcm.slave.rate", 48000);
	snd_config_delete(expanded);
	snd_config_delete(identity);
	snd_config_delete(root);
	return 0;
}
