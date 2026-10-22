/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <uchar.h>

#include "fplinux-multitap.h"

/* Input languages read from the fixture directory: en, ru, then xx. */
static struct fplinux_multitap_languages fixture;

static const struct fplinux_multitap_language *english(void)
{
	return &fixture.language[0];
}

static const struct fplinux_multitap_language *russian(void)
{
	return &fixture.language[1];
}

struct output {
	char characters[64];
	size_t length;
};

static enum fplinux_multitap_emit_result emit_character(void *opaque,
							uint32_t character)
{
	struct output *output = opaque;

	assert(output->length < sizeof(output->characters));
	output->characters[output->length++] = (char)character;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static enum fplinux_multitap_emit_result block_character(void *opaque,
							 uint32_t character)
{
	(void)opaque;
	(void)character;
	return FPLINUX_MULTITAP_EMIT_BLOCKED;
}

static enum fplinux_multitap_emit_result reject_character(void *opaque,
							  uint32_t character)
{
	(void)opaque;
	(void)character;
	return FPLINUX_MULTITAP_EMIT_REJECTED;
}

struct unicode_output {
	uint32_t *characters;
	size_t length;
};

static enum fplinux_multitap_emit_result emit_unicode_list(void *opaque,
							   uint32_t character)
{
	struct unicode_output *output = opaque;

	assert(output->length < 4);
	output->characters[output->length++] = character;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static void assert_groups(const struct fplinux_multitap_language *language)
{
	static const struct {
		unsigned char key;
		const char *characters;
	} groups[] = {
		{ '0', " 0" },	 { '1', ".,?!1@'\"-()/:_;+&%*=$#^" },
		{ '2', "abc2" }, { '3', "def3" },
		{ '4', "ghi4" }, { '5', "jkl5" },
		{ '6', "mno6" }, { '7', "pqrs7" },
		{ '8', "tuv8" }, { '9', "wxyz9" },
	};
	struct fplinux_multitap state;
	size_t group;

	for (group = 0; group < sizeof(groups) / sizeof(groups[0]); ++group) {
		size_t index;

		fplinux_multitap_init(&state, language, 1000);
		assert(fplinux_multitap_handles(groups[group].key));
		for (index = 0; groups[group].characters[index] != '\0';
		     ++index) {
			assert(fplinux_multitap_press(&state, groups[group].key,
						      0, emit_character,
						      NULL) ==
			       FPLINUX_MULTITAP_PENDING);
			assert(fplinux_multitap_candidate(&state) ==
			       (unsigned char)groups[group].characters[index]);
		}
		assert(fplinux_multitap_press(&state, groups[group].key, 0,
					      emit_character, NULL) ==
		       FPLINUX_MULTITAP_PENDING);
		assert(fplinux_multitap_candidate(&state) ==
		       (unsigned char)groups[group].characters[0]);
	}
	assert(!fplinux_multitap_handles('*'));
	fplinux_multitap_init(&state, language, 1000);
	assert(fplinux_multitap_press(&state, '*', 0, emit_character, NULL) ==
	       FPLINUX_MULTITAP_IGNORED);
	assert(fplinux_multitap_candidate(&state) == '\0');
}

static void assert_timeout_boundary(void)
{
	struct fplinux_multitap state;
	struct output output = { { 0 }, 0 };

	fplinux_multitap_init(&state, english(), 1000);
	assert(fplinux_multitap_press(&state, '2', 0, emit_character,
				      &output) == FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_expire(&state, 999, emit_character, &output) ==
	       FPLINUX_MULTITAP_PENDING);
	assert(output.length == 0);
	assert(fplinux_multitap_candidate(&state) == 'a');
	assert(fplinux_multitap_press(&state, '2', 999, emit_character,
				      &output) == FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_candidate(&state) == 'b');
	assert(fplinux_multitap_expire(&state, 1000, emit_character, &output) ==
	       FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 1);
	assert(output.characters[0] == 'b');
	assert(fplinux_multitap_candidate(&state) == '\0');
	assert(fplinux_multitap_press(&state, '2', 0, emit_character,
				      &output) == FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_press(&state, '2', 1000, emit_character,
				      &output) == FPLINUX_MULTITAP_COMMITTED);
	assert(output.characters[1] == 'a');
	assert(fplinux_multitap_candidate(&state) == 'a');
}

static void assert_cycle_and_change_key_commit(void)
{
	struct fplinux_multitap state;
	struct output output = { { 0 }, 0 };

	fplinux_multitap_init(&state, english(), 1000);
	assert(fplinux_multitap_press(&state, '2', 0, emit_character,
				      &output) == FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_press(&state, '2', 1, emit_character,
				      &output) == FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_candidate(&state) == 'b');
	assert(fplinux_multitap_press(&state, '3', 1, emit_character,
				      &output) == FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 1);
	assert(output.characters[0] == 'b');
	assert(fplinux_multitap_candidate(&state) == 'd');
	assert(fplinux_multitap_commit(&state, emit_character, &output) ==
	       FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 2);
	assert(memcmp(output.characters, "bd", output.length) == 0);
}

static void assert_cancel(void)
{
	struct fplinux_multitap state;

	fplinux_multitap_init(&state, english(), 1000);
	assert(fplinux_multitap_press(&state, '9', 0, emit_character, NULL) ==
	       FPLINUX_MULTITAP_PENDING);
	fplinux_multitap_cancel(&state);
	assert(fplinux_multitap_candidate(&state) == '\0');
	assert(!fplinux_multitap_pending(&state));
}

static void assert_delivery_outcomes(void)
{
	struct fplinux_multitap state;

	fplinux_multitap_init(&state, english(), 1000);
	assert(fplinux_multitap_press(&state, '2', 0, emit_character, NULL) ==
	       FPLINUX_MULTITAP_PENDING);
	assert(fplinux_multitap_press(&state, '3', 1, block_character, NULL) ==
	       FPLINUX_MULTITAP_BLOCKED);
	assert(fplinux_multitap_pending(&state));
	assert(fplinux_multitap_candidate(&state) == 'a');
	assert(fplinux_multitap_expire(&state, 1000, reject_character, NULL) ==
	       FPLINUX_MULTITAP_REJECTED);
	assert(!fplinux_multitap_pending(&state));
	assert(fplinux_multitap_candidate(&state) == '\0');
}

static enum fplinux_multitap_emit_result emit_unicode(void *context,
						      uint32_t character)
{
	uint32_t *output = context;

	*output = character;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

/* Russian keypad letters, each followed by the key's digit. */
static void assert_russian_groups(void)
{
	static const struct {
		unsigned char key;
		const char32_t *characters;
	} groups[] = {
		{ '0', U" 0" },	   { '1', U".,?!1@'\"-()/:_;+&%*=$#^" },
		{ '2', U"абвг2" }, { '3', U"деёжз3" },
		{ '4', U"ийкл4" }, { '5', U"мноп5" },
		{ '6', U"рсту6" }, { '7', U"фхцч7" },
		{ '8', U"шщъы8" }, { '9', U"ьэюя9" },
	};
	struct fplinux_multitap state;

	for (size_t group = 0; group < sizeof(groups) / sizeof(groups[0]);
	     ++group) {
		size_t index;

		fplinux_multitap_init(&state, russian(), 1000);
		for (index = 0; groups[group].characters[index]; ++index) {
			fplinux_multitap_press(&state, groups[group].key, 0,
					       emit_character, NULL);
			assert(fplinux_multitap_candidate(&state) ==
			       groups[group].characters[index]);
		}
		fplinux_multitap_press(&state, groups[group].key, 0,
				       emit_character, NULL);
		assert(fplinux_multitap_candidate(&state) ==
		       groups[group].characters[0]);
	}
}

static void assert_hold_replaces_press_with_digit(void)
{
	struct fplinux_multitap state;
	uint32_t emitted[4];
	struct unicode_output output = { emitted, 0 };

	fplinux_multitap_init(&state, english(), 1000);
	assert(fplinux_multitap_hold(&state, emit_unicode_list, &output) ==
	       FPLINUX_MULTITAP_IGNORED);
	fplinux_multitap_press(&state, '7', 0, emit_unicode_list, &output);
	assert(fplinux_multitap_hold(&state, emit_unicode_list, &output) ==
	       FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 1 && emitted[0] == '7');
	assert(!fplinux_multitap_pending(&state));

	/* A held second press keeps the first press's character. */
	output.length = 0;
	fplinux_multitap_press(&state, '2', 0, emit_unicode_list, &output);
	fplinux_multitap_press(&state, '2', 10, emit_unicode_list, &output);
	assert(fplinux_multitap_candidate(&state) == 'b');
	assert(fplinux_multitap_hold(&state, emit_unicode_list, &output) ==
	       FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 2 && emitted[0] == 'a' && emitted[1] == '2');

	/* A wrapped cycle steps back from its first entry to the digit. */
	output.length = 0;
	fplinux_multitap_set_language(&state, russian());
	for (unsigned int press = 0; press < 6; ++press)
		fplinux_multitap_press(&state, '9', press, emit_unicode_list,
				       &output);
	assert(fplinux_multitap_candidate(&state) == U'ь');
	fplinux_multitap_hold(&state, emit_unicode_list, &output);
	assert(output.length == 2 && emitted[0] == '9' && emitted[1] == '9');

	/* Punctuation and space keys hold to their digits as well. */
	output.length = 0;
	fplinux_multitap_press(&state, '1', 0, emit_unicode_list, &output);
	fplinux_multitap_hold(&state, emit_unicode_list, &output);
	fplinux_multitap_press(&state, '0', 0, emit_unicode_list, &output);
	fplinux_multitap_hold(&state, emit_unicode_list, &output);
	assert(output.length == 2 && emitted[0] == '1' && emitted[1] == '0');

	/* A blocked digit stays pending for a later commit. */
	fplinux_multitap_press(&state, '3', 0, emit_unicode_list, &output);
	assert(fplinux_multitap_hold(&state, block_character, NULL) ==
	       FPLINUX_MULTITAP_BLOCKED);
	assert(fplinux_multitap_candidate(&state) == '3');
}

static void assert_uppercase(void)
{
	static const uint32_t pairs[][2] = {
		{ 'a', 'A' },	{ 'z', 'Z' },	{ U'а', U'А' },
		{ U'я', U'Я' }, { U'ё', U'Ё' }, { 'A', 'A' },
		{ U'Ж', U'Ж' }, { '1', '1' },	{ '.', '.' },
	};

	for (size_t pair = 0; pair < sizeof(pairs) / sizeof(pairs[0]); ++pair)
		assert(fplinux_multitap_uppercase(pairs[pair][0]) ==
		       pairs[pair][1]);
}

static void assert_russian_composition(void)
{
	struct fplinux_multitap state;
	uint32_t emitted = 0;

	fplinux_multitap_init(&state, russian(), 1000);
	fplinux_multitap_press(&state, '3', 0, emit_unicode, &emitted);
	fplinux_multitap_press(&state, '3', 1, emit_unicode, &emitted);
	fplinux_multitap_press(&state, '3', 1, emit_unicode, &emitted);
	assert(fplinux_multitap_candidate(&state) == 0x451);
	fplinux_multitap_commit(&state, emit_unicode, &emitted);
	assert(emitted == 0x451);
	fplinux_multitap_press(&state, '2', 0, emit_unicode, &emitted);
	assert(fplinux_multitap_candidate(&state) == 0x430);
	fplinux_multitap_set_language(&state, english());
	assert(!fplinux_multitap_pending(&state));
	fplinux_multitap_press(&state, '2', 0, emit_unicode, &emitted);
	assert(fplinux_multitap_candidate(&state) == 'a');
}

static void names_are(const struct fplinux_multitap_languages *languages,
		      const char *const codes[], size_t count)
{
	assert(languages->count == count);
	for (size_t index = 0; index < count; ++index)
		assert(!strcmp(languages->language[index].code, codes[index]));
}

/*
 * Input languages are the files with input=yes and a complete key map: the
 * requested first language, then the rest by code, with English as the
 * fallback when no file qualifies.
 */
static void assert_language_files(const char *directory)
{
	static const char *const by_code[] = { "en", "ru", "xx" };
	static const char *const russian_first[] = { "ru", "en", "xx" };
	static const char *const english_only[] = { "en" };
	struct fplinux_multitap_languages languages;

	names_are(&fixture, by_code, 3);
	fplinux_multitap_load_languages(&languages, directory, "ru");
	names_are(&languages, russian_first, 3);
	assert(!strcmp(
		languages.language[0].indicator[FPLINUX_LANGUAGE_SENTENCE],
		"Абв"));
	assert(!strcmp(languages.language[0].indicator[FPLINUX_LANGUAGE_LOWER],
		       "абв"));
	assert(!strcmp(languages.language[0].indicator[FPLINUX_LANGUAGE_UPPER],
		       "АБВ"));
	/* A system-only language is not offered; the order stays by code. */
	fplinux_multitap_load_languages(&languages, directory, "sy");
	names_are(&languages, by_code, 3);
	fplinux_multitap_load_languages(&languages, "/nonexistent/fplinux",
					"ru");
	names_are(&languages, english_only, 1);
	assert(!strcmp(
		languages.language[0].indicator[FPLINUX_LANGUAGE_SENTENCE],
		"Abc"));
	assert_groups(&languages.language[0]);
	fplinux_multitap_load_languages(&languages, NULL, NULL);
	names_are(&languages, english_only, 1);
}

/* A file defines any script: keys cycle multi-byte characters in order. */
static void assert_file_defined_key_map(void)
{
	const struct fplinux_multitap_language *language = &fixture.language[2];
	struct fplinux_multitap state;
	uint32_t emitted[4];
	struct unicode_output output = { emitted, 0 };

	fplinux_multitap_init(&state, language, 1000);
	fplinux_multitap_press(&state, '2', 0, emit_unicode_list, &output);
	assert(fplinux_multitap_candidate(&state) == U'ß');
	fplinux_multitap_press(&state, '2', 1, emit_unicode_list, &output);
	assert(fplinux_multitap_candidate(&state) == U'€');
	fplinux_multitap_press(&state, '2', 2, emit_unicode_list, &output);
	assert(fplinux_multitap_candidate(&state) == U'😀');
	fplinux_multitap_press(&state, '2', 3, emit_unicode_list, &output);
	assert(fplinux_multitap_candidate(&state) == '2');
	fplinux_multitap_press(&state, '2', 4, emit_unicode_list, &output);
	assert(fplinux_multitap_candidate(&state) == U'ß');
	/* The digit is found wherever the file puts it. */
	fplinux_multitap_press(&state, '3', 5, emit_unicode_list, &output);
	assert(output.length == 1 && emitted[0] == U'ß');
	assert(fplinux_multitap_candidate(&state) == '3');
	assert(fplinux_multitap_hold(&state, emit_unicode_list, &output) ==
	       FPLINUX_MULTITAP_COMMITTED);
	assert(output.length == 2 && emitted[1] == '3');
}

int main(int argc, char **argv)
{
	struct fplinux_multitap_languages builtin;

	assert(argc == 2);
	fplinux_multitap_load_languages(&fixture, argv[1], NULL);
	fplinux_multitap_load_languages(&builtin, NULL, NULL);
	assert_groups(&builtin.language[0]);
	assert_groups(english());
	assert_language_files(argv[1]);
	assert_file_defined_key_map();
	assert_timeout_boundary();
	assert_cycle_and_change_key_commit();
	assert_cancel();
	assert_delivery_outcomes();
	assert_russian_composition();
	assert_russian_groups();
	assert_hold_replaces_press_with_digit();
	assert_uppercase();
	return 0;
}
