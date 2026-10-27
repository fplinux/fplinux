/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-gettext.h"

#include <assert.h>
#include <locale.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	assert(argc == 2);
	assert(setlocale(LC_ALL, ""));
	assert(bindtextdomain("gettext-context", argv[1]));
	assert(bind_textdomain_codeset("gettext-context", "UTF-8"));
	assert(textdomain("gettext-context"));
	puts(fplinux_gettext_context("short command\004Retry"));
	puts(gettext("Retry"));
	puts(fplinux_gettext_context("missing context\004Retry"));
	puts(fplinux_gettext_context("Done"));
	return 0;
}
