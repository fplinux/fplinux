/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_GETTEXT_H
#define FPLINUX_GETTEXT_H

#include <libintl.h>
#include <string.h>

/* The encoded key is context, '\004', then the English fallback. */
static inline const char *fplinux_gettext_context(const char *key)
{
	const char *translated = gettext(key);
	const char *separator = strchr(key, '\004');

	return separator && !strcmp(translated, key) ? separator + 1 :
						       translated;
}

#endif
