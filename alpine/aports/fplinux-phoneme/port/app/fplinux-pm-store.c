/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-pm-store.h"
#include "../../vm/fplinux-pm-vm.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int digest_field(EVP_MD_CTX *context, const char *field)
{
	uint64_t length = strlen(field);
	unsigned char encoded[8];

	for (size_t i = 0; i < sizeof(encoded); ++i)
		encoded[sizeof(encoded) - 1 - i] =
			(unsigned char)(length >> (i * 8));
	return EVP_DigestUpdate(context, encoded, sizeof(encoded)) &&
	       EVP_DigestUpdate(context, field, (size_t)length);
}

int fplinux_pm_store_key(const struct fplinux_pm_suite *suite,
			 const char *jar_path,
			 char key[FPLINUX_PM_STORE_KEY_SIZE])
{
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int length;
	EVP_MD_CTX *context;
	char *canonical = NULL;
	int ok;

	if (!suite || !suite->name || !suite->vendor || !jar_path) {
		errno = EINVAL;
		return 0;
	}
	if (!suite->name[0] || !suite->vendor[0]) {
		canonical = realpath(jar_path, NULL);
		if (!canonical)
			return 0;
	}
	context = EVP_MD_CTX_new();
	if (!context) {
		free(canonical);
		errno = ENOMEM;
		return 0;
	}
	ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL);
	if (canonical)
		ok = ok && digest_field(context, "path") &&
		     digest_field(context, canonical);
	else
		ok = ok && digest_field(context, "suite") &&
		     digest_field(context, suite->vendor) &&
		     digest_field(context, suite->name);
	ok = ok && EVP_DigestFinal_ex(context, digest, &length) && length == 32;
	EVP_MD_CTX_free(context);
	free(canonical);
	if (!ok) {
		errno = EIO;
		return 0;
	}
	for (size_t i = 0; i < 32; ++i)
		snprintf(key + i * 2, 3, "%02x", digest[i]);
	return 1;
}

static int is_private_directory(int fd)
{
	struct stat status;

	if (fstat(fd, &status) < 0)
		return 0;
	if (status.st_uid != geteuid() || (status.st_mode & 0077)) {
		errno = EACCES;
		return 0;
	}
	return 1;
}

char *fplinux_pm_store_open(const char *state_root,
			    const struct fplinux_pm_suite *suite,
			    const char *jar_path)
{
	char key[FPLINUX_PM_STORE_KEY_SIZE];
	char *path = NULL;
	int root, stores = -1, folder = -1, error;

	if (!state_root || state_root[0] != '/') {
		errno = EINVAL;
		return NULL;
	}
	if (!fplinux_pm_store_key(suite, jar_path, key))
		return NULL;
	root = open(state_root,
		    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root < 0)
		return NULL;
	if (!is_private_directory(root))
		goto failed;
	if (mkdirat(root, "rms", 0700) < 0 && errno != EEXIST)
		goto failed;
	stores = openat(root, "rms",
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (stores < 0)
		goto failed;
	if (!is_private_directory(stores))
		goto failed;
	if (mkdirat(stores, key, 0700) < 0 && errno != EEXIST)
		goto failed;
	folder = openat(stores, key,
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (folder < 0)
		goto failed;
	if (!is_private_directory(folder))
		goto failed;
	if (asprintf(&path, "%s/rms/%s", state_root, key) < 0)
		goto failed;
	close(folder);
	close(stores);
	close(root);
	return path;
failed:
	error = errno;
	if (folder >= 0)
		close(folder);
	if (stores >= 0)
		close(stores);
	close(root);
	errno = error;
	return NULL;
}
