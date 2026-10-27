/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "runtime-paths.h"
#include "vfs-broker.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct mre_vfs_response request(struct mre_vfs *vfs, uint32_t operation,
				       const char *path, const char *path2)
{
	struct mre_vfs_request query = { .sequence = 1,
					 .operation = operation,
					 .mode = 4 };
	struct mre_vfs_response response;

	snprintf(query.path, sizeof(query.path), "%s", path);
	if (path2)
		snprintf(query.path2, sizeof(query.path2), "%s", path2);
	mre_vfs_dispatch(vfs, &query, &response);
	return response;
}

static void protected_namespace(const char *directory, const char *state)
{
	int external = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int state_root = open(state, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int private = openat(state_root, "private",
			     O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct mre_vfs *vfs =
		mre_vfs_create(external, private, state_root, 8192);
	const char *blocked[] = {
		"E:\\hidden\\session\\state",
		"E:\\hidden\\session\\state\\other-game\\save",
	};
	const char *ancestors[] = {
		"E:\\hidden",
		"E:\\hidden\\session",
		"E:\\hidden\\session\\state",
	};

	assert(external >= 0 && state_root >= 0 && private >= 0 && vfs);
	for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i) {
		assert(request(vfs, MRE_VFS_OPEN, blocked[i], NULL).error ==
		       EACCES);
		assert(request(vfs, MRE_VFS_ATTR, blocked[i], NULL).error ==
		       EACCES);
	}
	for (size_t i = 0; i < sizeof(ancestors) / sizeof(ancestors[0]); ++i) {
		assert(request(vfs, MRE_VFS_RENAME, ancestors[i], "E:\\renamed")
			       .error == EACCES);
		assert(request(vfs, MRE_VFS_RMDIR, ancestors[i], NULL).error ==
		       EACCES);
		assert(request(vfs, MRE_VFS_DELETE, ancestors[i], NULL).error ==
		       EACCES);
	}
	assert(request(vfs, MRE_VFS_MKDIR, "C:\\saves", NULL).error == 0);
	assert(request(vfs, MRE_VFS_MKDIR, "E:\\ordinary", NULL).error == 0);
	assert(request(vfs, MRE_VFS_RENAME, "E:\\ordinary", "E:\\renamed")
		       .error == 0);
	int companion = mre_vfs_open_readonly(vfs, "E:\\companion.txt");
	char content[16] = { 0 };
	assert(companion >= 0 &&
	       read(companion, content, sizeof(content)) == 9);
	assert(!strcmp(content, "companion"));
	close(companion);
	assert(!mre_vfs_entry_visible(vfs, state_root, "."));
	mre_vfs_destroy(vfs);
	errno = 0;
	assert(!mre_vfs_create(private, private, state_root, 8192));
	assert(errno == EACCES);
	close(external);
	close(private);
	close(state_root);
	puts("namespace: protected state and ancestors; companions and C: usable");
}

int main(int argc, char **argv)
{
	assert(argc >= 3);
	if (!strcmp(argv[1], "namespace")) {
		assert(argc == 4);
		protected_namespace(argv[2], argv[3]);
		return 0;
	}
	struct mre_document document;
	if (!mre_document_prepare(&document, argv[2])) {
		printf("error:%d\n", errno);
		return 1;
	}
	printf("%s\n%s\n", document.path, document.state_key);
	int seals = fcntl(document.snapshot_fd, F_GET_SEALS);
	assert((seals &
		(F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) ==
	       (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL));
	if (argc == 4) {
		int root = open(argv[3], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		int folder = mre_document_open_state(&document, root);
		assert(root >= 0 && folder >= 0);
		int saved = openat(folder, "save", O_RDWR | O_CREAT | O_CLOEXEC,
				   0600);
		assert(saved >= 0);
		if (lseek(saved, 0, SEEK_END) == 0)
			assert(write(saved, "retained", 8) == 8);
		close(saved);
		close(folder);
		close(root);
	}
	mre_document_release(&document);
	return 0;
}
