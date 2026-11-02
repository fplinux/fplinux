/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-pty.h"
#include "fplinux-language.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

bool __real_fplinux_terminal_pty_open(struct fplinux_terminal_pty *pty,
				      unsigned int columns, unsigned int rows,
				      const char *shell, const char *startup);

/* Keep PTY ownership and lifecycle; replace only Bash's shell stream. */
bool __wrap_fplinux_terminal_pty_open(struct fplinux_terminal_pty *pty,
				      unsigned int columns, unsigned int rows,
				      const char *shell, const char *startup)
{
	(void)shell;
	return __real_fplinux_terminal_pty_open(
		pty, columns, rows, FPLINUX_SCREEN_SHELL_PATH, startup);
}

size_t __real_fplinux_language_list(const char *directory,
				    char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
				    size_t capacity);
bool __real_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language);

static const char *language_directory(const char *directory)
{
	const char *source = getenv("FPLINUX_SCREEN_LANGUAGES");

	assert(source);
	return directory && !strcmp(directory, FPLINUX_LANGUAGE_DIRECTORY) ?
		       source :
		       directory;
}

/* Use the packaged definitions through the normal language-file parser. */
size_t __wrap_fplinux_language_list(const char *directory,
				    char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
				    size_t capacity)
{
	return __real_fplinux_language_list(language_directory(directory),
					    codes, capacity);
}

bool __wrap_fplinux_language_load(const char *directory, const char *code,
				  struct fplinux_language *language)
{
	return __real_fplinux_language_load(language_directory(directory), code,
					    language);
}
