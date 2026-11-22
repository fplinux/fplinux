/* SPDX-License-Identifier: GPL-2.0-only */
#include "files-preview-catalog.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct files_preview_catalog *catalog;
	const struct files_preview_handler *handler;
	const char *icon;
	const char *program;
	int result = 0;

	if (argc != 6)
		return 2;
	catalog = files_preview_catalog_open(argv[1], getuid());
	handler = files_preview_catalog_find(catalog, argv[2]);
	icon = handler && handler->icon ? handler->icon : "";
	program = handler && handler->program ? handler->program : "";
	if ((handler != NULL) != !strcmp(argv[3], "match") ||
	    strcmp(icon, argv[4]) || strcmp(program, argv[5])) {
		fprintf(stderr, "handler=%s icon='%s' program='%s'\n",
			handler ? "match" : "none", icon, program);
		result = 1;
	}
	files_preview_catalog_close(catalog);
	return result;
}
