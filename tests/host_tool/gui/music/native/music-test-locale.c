/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-language.h"

#include <stdlib.h>

bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	const char *languages = getenv("FPLINUX_MUSIC_TEST_LANGUAGES");

	return __real_fplinux_language_load(languages ? languages : directory,
					    code, language);
}

char *__real_libintl_bindtextdomain(const char *domain, const char *directory);

char *__wrap_libintl_bindtextdomain(const char *domain, const char *directory)
{
	const char *catalogues = getenv("FPLINUX_MUSIC_TEST_LOCALE_DIR");

	return __real_libintl_bindtextdomain(domain, catalogues ? catalogues :
								  directory);
}
