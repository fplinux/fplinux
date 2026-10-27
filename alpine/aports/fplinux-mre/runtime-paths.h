/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MRE_RUNTIME_PATHS_H
#define FPLINUX_MRE_RUNTIME_PATHS_H

#include <limits.h>
#include <stdbool.h>

struct mre_document {
	int directory_fd;
	int snapshot_fd;
	char path[PATH_MAX];
	char name[NAME_MAX + 1];
	char state_key[65];
};

/* Prepare an immutable executable without changing a running guest. */
bool mre_document_prepare(struct mre_document *document, const char *path);
void mre_document_release(struct mre_document *document);
int mre_document_open_state(const struct mre_document *document,
			    int state_root);

#endif
