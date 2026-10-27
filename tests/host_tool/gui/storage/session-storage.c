/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-storage.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <unistd.h>

struct fixture {
	char root[PATH_MAX];
	char mountinfo[PATH_MAX];
	char mountpoint[PATH_MAX];
	char device[PATH_MAX];
	dev_t number;
	bool read_only;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	bool block_probe;
	bool probe_entered;
};

static void path_join(char *path, const char *root, const char *name)
{
	assert(snprintf(path, PATH_MAX, "%s/%s", root, name) < PATH_MAX);
}

static int configured_error(struct fixture *fixture, const char *name)
{
	char path[PATH_MAX];
	FILE *file;
	int error = 0;

	path_join(path, fixture->root, name);
	file = fopen(path, "r");
	if (file) {
		assert(fscanf(file, "%d", &error) == 1);
		fclose(file);
	}
	return error;
}

static void write_error(struct fixture *fixture, int error)
{
	char path[PATH_MAX];
	FILE *file;

	path_join(path, fixture->root, "unmount-error");
	file = fopen(path, "w");
	assert(file);
	assert(fprintf(file, "%d\n", error) > 0);
	assert(fclose(file) == 0);
}

static void record_effect(struct fixture *fixture, const char *effect)
{
	char path[PATH_MAX];
	FILE *file;

	path_join(path, fixture->root, "effects");
	file = fopen(path, "a");
	assert(file);
	assert(fprintf(file, "%s\n", effect) > 0);
	assert(fclose(file) == 0);
}

static int fixture_probe(void *context, const char *device, dev_t number,
			 struct fplinux_session_storage_probe *probe)
{
	struct fixture *fixture = context;
	char path[PATH_MAX];
	FILE *file;

	pthread_mutex_lock(&fixture->mutex);
	fixture->probe_entered = true;
	pthread_cond_signal(&fixture->condition);
	while (fixture->block_probe)
		pthread_cond_wait(&fixture->condition, &fixture->mutex);
	pthread_mutex_unlock(&fixture->mutex);
	assert(snprintf(path, sizeof(path), "%s.probe", device) <
	       (int)sizeof(path));
	file = fopen(path, "r");
	if (!file)
		return ENODEV;
	assert(fgets(probe->filesystem, sizeof(probe->filesystem), file));
	probe->filesystem[strcspn(probe->filesystem, "\n")] = '\0';
	if (fgets(probe->label, sizeof(probe->label), file))
		probe->label[strcspn(probe->label, "\n")] = '\0';
	fclose(file);
	assert(strlen(device) < sizeof(fixture->device));
	strcpy(fixture->device, device);
	fixture->number = number;
	return 0;
}

static void write_mountpoint(FILE *file, const char *path)
{
	for (const unsigned char *cursor = (const unsigned char *)path; *cursor;
	     ++cursor) {
		if (*cursor == ' ')
			assert(fputs("\\040", file) >= 0);
		else
			assert(fputc(*cursor, file) != EOF);
	}
}

static void write_mount(struct fixture *fixture, const char *filesystem,
			unsigned long id)
{
	FILE *file = fopen(fixture->mountinfo, "w");

	assert(file);
	assert(fputs("1 0 0:1 / / rw - tmpfs tmpfs rw\n", file) >= 0);
	if (id) {
		assert(fprintf(file, "%lu 1 %u:%u / ", id,
			       major(fixture->number),
			       minor(fixture->number)) > 0);
		write_mountpoint(file, fixture->mountpoint);
		assert(fprintf(file, " %s - %s %s %s\n",
			       fixture->read_only ? "ro" : "rw", filesystem,
			       fixture->device,
			       fixture->read_only ? "ro" : "rw") > 0);
	}
	assert(fclose(file) == 0);
}

static int fixture_mount(void *context, const char *device, const char *path,
			 const char *filesystem, unsigned long flags,
			 const char *options)
{
	struct fixture *fixture = context;
	char effect[PATH_MAX + 256];
	int error = configured_error(fixture, "mount-error");

	assert(!strcmp(device, fixture->device));
	assert(!strcmp(path, fixture->mountpoint));
	assert((flags & (MS_NODEV | MS_NOSUID | MS_NOEXEC)) ==
	       (MS_NODEV | MS_NOSUID | MS_NOEXEC));
	assert(snprintf(effect, sizeof(effect), "mount %s %s %s %s", device,
			filesystem, flags & MS_RDONLY ? "ro" : "rw",
			options) < (int)sizeof(effect));
	record_effect(fixture, effect);
	if (error)
		return error;
	fixture->read_only = flags & MS_RDONLY;
	write_mount(fixture, filesystem, 31);
	return 0;
}

static int fixture_sync(void *context, const char *path)
{
	struct fixture *fixture = context;

	assert(!strcmp(path, fixture->mountpoint));
	record_effect(fixture, "sync");
	return configured_error(fixture, "sync-error");
}

static int fixture_unmount(void *context, const char *path)
{
	struct fixture *fixture = context;
	int error = configured_error(fixture, "unmount-error");

	assert(!strcmp(path, fixture->mountpoint));
	record_effect(fixture, "unmount");
	if (!error)
		write_mount(fixture, "vfat", 0);
	return error;
}

static void collect(struct fplinux_session_storage *storage,
		    struct fplinux_volume_result *result)
{
	struct pollfd descriptor = {
		.fd = fplinux_session_storage_fd(storage),
		.events = POLLIN,
	};

	assert(poll(&descriptor, 1, 5000) == 1);
	assert(descriptor.revents & POLLIN);
	assert(fplinux_session_storage_take_result(storage, result) == 1);
	assert(fplinux_session_storage_take_result(storage, result) == 0);
}

static void print_result(const struct fplinux_volume_result *result)
{
	printf("result\t%u\t%d\t%zu\t%d\n", result->request_id, result->error,
	       result->snapshot.count, result->snapshot.home_persistent);
	for (size_t i = 0; i < result->snapshot.count; ++i) {
		const struct fplinux_volume *volume =
			&result->snapshot.volumes[i];

		printf("volume\t%u\t%d\t%d\t%d\t%s\t%s\t%u\n", volume->id,
		       volume->state, volume->protection, volume->read_only,
		       volume->label, volume->path, volume->mount_id);
	}
}

static void change_medium(struct fixture *fixture, bool remove)
{
	char path[PATH_MAX];
	FILE *file;

	path_join(path, fixture->root, "sys/mmcblk0/device/cid");
	if (remove) {
		assert(unlink(path) == 0);
		return;
	}
	file = fopen(path, "w");
	assert(file);
	assert(fputs("different-medium\n", file) >= 0);
	assert(fclose(file) == 0);
}

static void run_actions(struct fplinux_session_storage *storage,
			struct fixture *fixture, int argc, char **argv)
{
	struct fplinux_volume_result result = { 0 };
	uint32_t volume_id = 0;

	for (int i = 2; i < argc; ++i) {
		enum fplinux_volume_operation operation =
			FPLINUX_VOLUME_REFRESH;
		bool async = !strcmp(argv[i], "async");

		if (!strcmp(argv[i], "connect") ||
		    !strcmp(argv[i], "replace") || !strcmp(argv[i], "remove"))
			operation = FPLINUX_VOLUME_CONNECT;
		else if (!strcmp(argv[i], "eject") ||
			 !strcmp(argv[i], "busy-eject"))
			operation = FPLINUX_VOLUME_EJECT;
		else if (!strcmp(argv[i], "mount-changed")) {
			write_mount(fixture, "vfat", 32);
			operation = FPLINUX_VOLUME_EJECT;
		} else if (!strcmp(argv[i], "mount-overflow"))
			write_mount(fixture, "vfat",
				    (unsigned long)UINT32_MAX + 1UL);
		if (!strcmp(argv[i], "scan-error")) {
			FILE *file = fopen(fixture->mountinfo, "w");

			assert(file);
			assert(fputs("invalid mount record\n", file) >= 0);
			assert(fclose(file) == 0);
		}
		if (!strcmp(argv[i], "replace") || !strcmp(argv[i], "remove"))
			change_medium(fixture, !strcmp(argv[i], "remove"));
		if (!strcmp(argv[i], "busy-eject"))
			write_error(fixture, EBUSY);
		if (!strcmp(argv[i], "unbusy"))
			write_error(fixture, 0);
		if (async) {
			pthread_mutex_lock(&fixture->mutex);
			fixture->block_probe = true;
			pthread_mutex_unlock(&fixture->mutex);
		}
		assert(fplinux_session_storage_request(storage, operation,
						       volume_id,
						       (uint32_t)i) == 0);
		if (async) {
			pthread_mutex_lock(&fixture->mutex);
			while (!fixture->probe_entered)
				pthread_cond_wait(&fixture->condition,
						  &fixture->mutex);
			assert(fplinux_session_storage_take_result(
				       storage, &result) == 0);
			assert(fplinux_session_storage_request(
				       storage, FPLINUX_VOLUME_REFRESH, 0,
				       500) == -1);
			assert(errno == EBUSY);
			fixture->block_probe = false;
			pthread_cond_signal(&fixture->condition);
			pthread_mutex_unlock(&fixture->mutex);
		}
		collect(storage, &result);
		print_result(&result);
		if (result.snapshot.count)
			volume_id = result.snapshot.volumes[0].id;
	}
}

int main(int argc, char **argv)
{
	struct fixture fixture = { 0 };
	struct fplinux_session_storage_config config = { .uid = 1000,
							 .gid = 1000 };
	struct fplinux_session_storage_io io = {
		.context = &fixture,
		.probe = fixture_probe,
		.mount = fixture_mount,
		.sync = fixture_sync,
		.unmount = fixture_unmount,
	};
	struct fplinux_session_storage *storage;
	char sys_block[PATH_MAX], dev_root[PATH_MAX], swaps[PATH_MAX],
		home[PATH_MAX];

	assert(argc >= 3);
	assert(strlen(argv[1]) < sizeof(fixture.root));
	strcpy(fixture.root, argv[1]);
	path_join(sys_block, fixture.root, "sys");
	path_join(dev_root, fixture.root, "dev");
	path_join(swaps, fixture.root, "swaps");
	path_join(home, fixture.root, "home/user");
	path_join(fixture.mountinfo, fixture.root, "mountinfo");
	path_join(fixture.mountpoint, fixture.root, "card data");
	config.sys_block = sys_block;
	config.dev_root = dev_root;
	config.swaps = swaps;
	config.home_path = home;
	config.mountpoint = fixture.mountpoint;
	config.mountinfo = fixture.mountinfo;
	config.io = &io;
	assert(pthread_mutex_init(&fixture.mutex, NULL) == 0);
	assert(pthread_cond_init(&fixture.condition, NULL) == 0);
	storage = fplinux_session_storage_create(&config);
	assert(storage);
	run_actions(storage, &fixture, argc, argv);
	fplinux_session_storage_destroy(storage);
	assert(pthread_cond_destroy(&fixture.condition) == 0);
	assert(pthread_mutex_destroy(&fixture.mutex) == 0);
	return 0;
}
