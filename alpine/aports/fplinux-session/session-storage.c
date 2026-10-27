/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-storage.h"
#include "fplinux-utf8.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct storage_volume {
	struct fplinux_volume public;
	char disk[NAME_MAX + 1];
	char device[NAME_MAX + 1];
	char cid[128];
	char location[PATH_MAX];
	dev_t number;
	unsigned long mount_id;
	bool has_partitions;
	bool mounted;
	struct fplinux_session_storage_probe probe;
};

struct fplinux_session_storage {
	char sys_block[PATH_MAX];
	char dev_root[PATH_MAX];
	char mountinfo[PATH_MAX];
	char swaps[PATH_MAX];
	char mountpoint[PATH_MAX];
	char home_path[PATH_MAX];
	uid_t uid;
	gid_t gid;
	struct fplinux_session_storage_io io;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	int notification[2];
	bool stop;
	bool pending;
	bool ready;
	enum fplinux_volume_operation operation;
	uint32_t volume_id;
	uint32_t request_id;
	uint32_t next_id;
	size_t count;
	bool home_persistent;
	bool mountpoint_occupied;
	struct storage_volume volumes[FPLINUX_VOLUME_MAX];
	struct fplinux_volume_result result;
};

static bool copy_text(char *destination, size_t capacity, const char *text)
{
	size_t length = strlen(text);

	if (length >= capacity) {
		errno = ENAMETOOLONG;
		return false;
	}
	memcpy(destination, text, length + 1);
	return true;
}

static bool make_path(char *path, size_t capacity, const char *directory,
		      const char *name)
{
	if (snprintf(path, capacity, "%s/%s", directory, name) >=
	    (int)capacity) {
		errno = ENAMETOOLONG;
		return false;
	}
	return true;
}

static bool read_attribute(struct fplinux_session_storage *storage,
			   const char *device, const char *attribute,
			   char *text, size_t capacity)
{
	char directory[PATH_MAX], path[PATH_MAX];
	size_t length = 0;
	ssize_t count;
	int fd;

	if (!make_path(directory, sizeof(directory), storage->sys_block,
		       device) ||
	    !make_path(path, sizeof(path), directory, attribute))
		return false;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	do {
		count = read(fd, text + length, capacity - length - 1);
		if (count > 0)
			length += (size_t)count;
	} while ((count > 0 && length < capacity - 1) ||
		 (count < 0 && errno == EINTR));
	close(fd);
	if (count < 0 || length == capacity - 1)
		return false;
	while (length && isspace((unsigned char)text[length - 1]))
		--length;
	text[length] = '\0';
	return length > 0;
}

static bool read_number(struct fplinux_session_storage *storage,
			const char *device, dev_t *number)
{
	char text[64], tail;
	unsigned int device_major, device_minor;

	if (!read_attribute(storage, device, "dev", text, sizeof(text)) ||
	    sscanf(text, "%u:%u%c", &device_major, &device_minor, &tail) != 2)
		return false;
	*number = makedev(device_major, device_minor);
	return true;
}

static bool disk_name(const char *name)
{
	if (strncmp(name, "mmcblk", 6) || !isdigit((unsigned char)name[6]))
		return false;
	for (const char *cursor = name + 6; *cursor; ++cursor)
		if (!isdigit((unsigned char)*cursor))
			return false;
	return true;
}

static bool partition_name(const char *disk, const char *name)
{
	size_t length = strlen(disk);
	const char *cursor;

	if (strncmp(disk, name, length) || name[length] != 'p' ||
	    !isdigit((unsigned char)name[length + 1]))
		return false;
	for (cursor = name + length + 1; *cursor; ++cursor)
		if (!isdigit((unsigned char)*cursor))
			return false;
	return true;
}

static bool belongs_to_card(struct fplinux_session_storage *storage,
			    const struct storage_volume *volume, dev_t number)
{
	struct dirent *entry;
	DIR *directory = opendir(storage->sys_block);
	bool found = false;

	if (!directory)
		return false;
	while ((entry = readdir(directory))) {
		dev_t candidate;

		if ((strcmp(entry->d_name, volume->disk) &&
		     !partition_name(volume->disk, entry->d_name)) ||
		    !read_number(storage, entry->d_name, &candidate))
			continue;
		if (candidate == number) {
			found = true;
			break;
		}
	}
	closedir(directory);
	return found;
}

static void decode_path(char *text)
{
	char *output = text;

	for (char *cursor = text; *cursor; ++cursor) {
		if (cursor[0] == '\\' && cursor[1] >= '0' && cursor[1] <= '7' &&
		    cursor[2] >= '0' && cursor[2] <= '7' && cursor[3] >= '0' &&
		    cursor[3] <= '7') {
			*output++ =
				(char)((cursor[1] - '0') * 64 +
				       (cursor[2] - '0') * 8 + cursor[3] - '0');
			cursor += 3;
		} else {
			*output++ = *cursor;
		}
	}
	*output = '\0';
}

static bool path_contains(const char *directory, const char *path)
{
	size_t length = strlen(directory);

	return !strcmp(directory, "/") ||
	       (!strncmp(directory, path, length) &&
		(path[length] == '/' || path[length] == '\0'));
}

static bool option_present(const char *options, const char *option)
{
	size_t length = strlen(option);

	for (const char *cursor = options; *cursor;) {
		if (!strncmp(cursor, option, length) &&
		    (cursor[length] == ',' || cursor[length] == '\0'))
			return true;
		cursor = strchr(cursor, ',');
		if (!cursor)
			break;
		++cursor;
	}
	return false;
}

static int inspect_mounts(struct fplinux_session_storage *storage,
			  struct storage_volume *volumes, size_t count)
{
	char *line = NULL;
	size_t capacity = 0, home_mount_length = 0;
	FILE *file = fopen(storage->mountinfo, "re");
	int error = 0;

	storage->mountpoint_occupied = false;
	storage->home_persistent = false;
	if (!file)
		return errno;
	while (getline(&line, &capacity, file) >= 0) {
		char *fields[6], *save = NULL, *separator, *filesystem;
		unsigned int device_major, device_minor;
		unsigned long mount_id;
		char tail;
		bool valid = true;

		separator = strstr(line, " - ");
		if (!separator) {
			error = EIO;
			break;
		}
		*separator = '\0';
		filesystem = strtok_r(separator + 3, " ", &save);
		for (size_t i = 0; i < 6; ++i) {
			fields[i] = strtok_r(i ? NULL : line, " ", &save);
			if (!fields[i])
				valid = false;
		}
		if (!valid || !filesystem ||
		    sscanf(fields[0], "%lu%c", &mount_id, &tail) != 1 ||
		    sscanf(fields[2], "%u:%u%c", &device_major, &device_minor,
			   &tail) != 2) {
			error = EIO;
			break;
		}
		decode_path(fields[4]);
		if (path_contains(fields[4], storage->home_path) &&
		    strlen(fields[4]) >= home_mount_length) {
			home_mount_length = strlen(fields[4]);
			storage->home_persistent =
				!strcmp(filesystem, "ext4") ||
				!strcmp(filesystem, "vfat");
		}
		if (!strcmp(fields[4], storage->mountpoint))
			storage->mountpoint_occupied = true;
		for (size_t i = 0; i < count; ++i) {
			struct storage_volume *volume = &volumes[i];
			dev_t number = makedev(device_major, device_minor);

			if (!belongs_to_card(storage, volume, number))
				continue;
			if (!strcmp(fields[4], "/")) {
				volume->public.protection =
					FPLINUX_VOLUME_SYSTEM_ROOT;
			} else if (number == volume->number &&
				   !strcmp(fields[4], storage->mountpoint) &&
				   !strcmp(fields[3], "/")) {
				volume->mounted = true;
				volume->mount_id = mount_id;
				volume->public.read_only |=
					option_present(fields[5], "ro");
			} else if (!volume->public.protection) {
				volume->public.protection =
					FPLINUX_VOLUME_OTHER_MOUNT;
			}
		}
	}
	if (ferror(file))
		error = errno ? errno : EIO;
	free(line);
	fclose(file);
	return error;
}

static int inspect_swaps(struct fplinux_session_storage *storage,
			 struct storage_volume *volumes, size_t count)
{
	char *line = NULL;
	size_t capacity = 0;
	FILE *file = fopen(storage->swaps, "re");
	int error = 0;

	if (!file)
		return errno;
	/* The first line describes the columns, not a swap entry. */
	if (getline(&line, &capacity, file) < 0)
		error = EIO;
	while (!error && getline(&line, &capacity, file) >= 0) {
		char *save, *path = strtok_r(line, " \t\n", &save);
		struct stat status;
		dev_t number;
		const char *name;

		if (!path)
			continue;
		decode_path(path);
		name = strrchr(path, '/');
		if (path_contains(storage->dev_root, path) && name &&
		    read_number(storage, name + 1, &number)) {
			/* Resolve partition swaps even before /dev has been stat'ed. */
		} else if (stat(path, &status) == 0) {
			number = S_ISBLK(status.st_mode) ? status.st_rdev :
							   status.st_dev;
		} else {
			error = errno;
			break;
		}
		for (size_t i = 0; i < count; ++i)
			if (belongs_to_card(storage, &volumes[i], number) &&
			    volumes[i].public.protection !=
				    FPLINUX_VOLUME_SYSTEM_ROOT)
				volumes[i].public.protection =
					FPLINUX_VOLUME_SWAP;
	}
	if (ferror(file))
		error = errno ? errno : EIO;
	free(line);
	fclose(file);
	return error;
}

static void probe_field(const char *output, const char *field, char *text,
			size_t capacity)
{
	const char *cursor = strstr(output, field);
	size_t length;

	if (!cursor)
		return;
	cursor += strlen(field);
	length = strcspn(cursor, "\"\r\n");
	if (length >= capacity)
		length = capacity - 1;
	memcpy(text, cursor, length);
	text[length] = '\0';
}

static int probe_device(void *context, const char *device, dev_t number,
			struct fplinux_session_storage_probe *probe)
{
	char output[4096];
	char *arguments[] = { "busybox", "blkid", (char *)device, NULL };
	posix_spawn_file_actions_t actions;
	struct stat status;
	int descriptors[2], result, child_status;
	pid_t child;
	size_t length = 0;
	(void)context;

	if (stat(device, &status) < 0)
		return errno;
	if (!S_ISBLK(status.st_mode) || status.st_rdev != number)
		return ENODEV;
	if (pipe2(descriptors, O_CLOEXEC) < 0)
		return errno;
	result = posix_spawn_file_actions_init(&actions);
	if (result) {
		close(descriptors[0]);
		close(descriptors[1]);
		return result;
	}
	result = posix_spawn_file_actions_adddup2(&actions, descriptors[1],
						  STDOUT_FILENO);
	if (!result)
		result = posix_spawn_file_actions_addopen(
			&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	if (!result)
		result = posix_spawn(&child, "/bin/busybox", &actions, NULL,
				     arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(descriptors[1]);
	if (result) {
		close(descriptors[0]);
		return result;
	}
	for (;;) {
		char buffer[512];
		ssize_t bytes = read(descriptors[0], buffer, sizeof(buffer));

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			break;
		if ((size_t)bytes < sizeof(output) - length) {
			memcpy(output + length, buffer, (size_t)bytes);
			length += (size_t)bytes;
		}
	}
	close(descriptors[0]);
	while (waitpid(child, &child_status, 0) < 0)
		if (errno != EINTR)
			return errno;
	if (!WIFEXITED(child_status) || WEXITSTATUS(child_status))
		return EIO;
	output[length] = '\0';
	probe_field(output, " TYPE=\"", probe->filesystem,
		    sizeof(probe->filesystem));
	probe_field(output, " LABEL=\"", probe->label, sizeof(probe->label));
	for (size_t i = 0; probe->label[i]; ++i)
		if ((unsigned char)probe->label[i] < 0x20 ||
		    probe->label[i] == 0x7f)
			probe->label[i] = ' ';
	return 0;
}

static int mount_device(void *context, const char *device, const char *path,
			const char *filesystem, unsigned long flags,
			const char *options)
{
	(void)context;
	return mount(device, path, filesystem, flags, options) < 0 ? errno : 0;
}

static int sync_volume(void *context, const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	int error;
	(void)context;

	if (fd < 0)
		return errno;
	error = syncfs(fd) < 0 ? errno : 0;
	close(fd);
	return error;
}

static int unmount_volume(void *context, const char *path)
{
	(void)context;
	return umount2(path, 0) < 0 ? errno : 0;
}

static int compare_volumes(const void *left, const void *right)
{
	const struct storage_volume *a = left, *b = right;

	return strcmp(a->disk, b->disk);
}

static bool same_medium(const struct storage_volume *left,
			const struct storage_volume *right)
{
	return left->number == right->number &&
	       left->mount_id == right->mount_id &&
	       !strcmp(left->location, right->location) &&
	       !strcmp(left->cid, right->cid);
}

static bool label_valid(const char *label)
{
	const unsigned char *cursor = (const unsigned char *)label;
	const unsigned char *end = cursor + strlen(label);
	uint32_t scalar;

	while (cursor < end)
		if (!fplinux_utf8_next(&cursor, end, &scalar) ||
		    scalar < 0x20 || scalar == 0x7f)
			return false;
	return label[0] != '\0';
}

static int scan(struct fplinux_session_storage *storage)
{
	struct storage_volume volumes[FPLINUX_VOLUME_MAX] = { 0 };
	struct dirent *entry;
	DIR *directory = opendir(storage->sys_block);
	size_t count = 0;
	int error;

	if (!directory)
		return errno;
	while ((entry = readdir(directory))) {
		struct storage_volume *volume;
		char type[16], path[PATH_MAX], first[NAME_MAX + 1],
			read_only[8];
		struct dirent *partition;
		DIR *partitions;
		bool has_first = false;

		if (!disk_name(entry->d_name) ||
		    !read_attribute(storage, entry->d_name, "device/type", type,
				    sizeof(type)) ||
		    strcmp(type, "SD"))
			continue;
		if (count == FPLINUX_VOLUME_MAX) {
			closedir(directory);
			return EOVERFLOW;
		}
		volume = &volumes[count];
		memset(volume, 0, sizeof(*volume));
		if (!copy_text(volume->disk, sizeof(volume->disk),
			       entry->d_name) ||
		    !read_attribute(storage, volume->disk, "device/cid",
				    volume->cid, sizeof(volume->cid)) ||
		    !make_path(path, sizeof(path), storage->sys_block,
			       volume->disk) ||
		    !realpath(path, volume->location))
			continue;
		if (snprintf(first, sizeof(first), "%sp1", volume->disk) >=
		    (int)sizeof(first))
			continue;
		partitions = opendir(storage->sys_block);
		if (!partitions) {
			error = errno;
			closedir(directory);
			return error;
		}
		while ((partition = readdir(partitions))) {
			if (!partition_name(volume->disk, partition->d_name))
				continue;
			volume->has_partitions = true;
			if (!strcmp(partition->d_name, first))
				has_first = true;
		}
		closedir(partitions);
		copy_text(volume->device, sizeof(volume->device),
			  has_first ? first : volume->disk);
		if (!read_number(storage, volume->device, &volume->number))
			continue;
		if (read_attribute(storage, volume->device, "ro", read_only,
				   sizeof(read_only)))
			volume->public.read_only = !strcmp(read_only, "1");
		copy_text(volume->public.label, sizeof(volume->public.label),
			  "microSD");
		volume->public.state = FPLINUX_VOLUME_UNSUPPORTED;
		if ((!volume->has_partitions || has_first) &&
		    make_path(path, sizeof(path), storage->dev_root,
			      volume->device)) {
			error = storage->io.probe(storage->io.context, path,
						  volume->number,
						  &volume->probe);
			if (!error &&
			    (!strcmp(volume->probe.filesystem, "vfat") ||
			     !strcmp(volume->probe.filesystem, "ext4")))
				volume->public.state = FPLINUX_VOLUME_AVAILABLE;
			if (label_valid(volume->probe.label))
				copy_text(volume->public.label,
					  sizeof(volume->public.label),
					  volume->probe.label);
			if (!strcmp(volume->probe.label, "FPLBOOT"))
				volume->public.protection = FPLINUX_VOLUME_BOOT;
		}
		++count;
	}
	closedir(directory);
	error = inspect_mounts(storage, volumes, count);
	if (!error)
		error = inspect_swaps(storage, volumes, count);
	if (error)
		return error;
	qsort(volumes, count, sizeof(volumes[0]), compare_volumes);
	for (size_t i = 0; i < count; ++i) {
		struct storage_volume *volume = &volumes[i];

		if (volume->public.protection) {
			volume->public.state = FPLINUX_VOLUME_PROTECTED;
		} else if (volume->mounted) {
			if (!volume->mount_id || volume->mount_id > UINT32_MAX)
				return EOVERFLOW;
			volume->public.state = FPLINUX_VOLUME_MOUNTED;
			volume->public.mount_id = (uint32_t)volume->mount_id;
			copy_text(volume->public.path,
				  sizeof(volume->public.path),
				  storage->mountpoint);
		}
		for (size_t j = 0; j < storage->count; ++j)
			if (same_medium(volume, &storage->volumes[j])) {
				volume->public.id =
					storage->volumes[j].public.id;
				break;
			}
		if (!volume->public.id) {
			if (!++storage->next_id)
				++storage->next_id;
			volume->public.id = storage->next_id;
		}
	}
	memcpy(storage->volumes, volumes, sizeof(volumes));
	storage->count = count;
	return 0;
}

static int discover(struct fplinux_session_storage *storage)
{
	int error = scan(storage);

	if (error) {
		/* Unknown mount state cannot authorize access to an old path. */
		storage->count = 0;
		storage->home_persistent = false;
	}
	return error;
}

static int operate(struct fplinux_session_storage *storage,
		   enum fplinux_volume_operation operation, uint32_t volume_id)
{
	struct storage_volume *volume = NULL;
	char device[PATH_MAX], options[128];
	struct stat status;
	unsigned long flags = MS_NODEV | MS_NOSUID | MS_NOEXEC;
	int error = discover(storage);

	if (error || operation == FPLINUX_VOLUME_REFRESH)
		return error;
	for (size_t i = 0; i < storage->count; ++i)
		if (storage->volumes[i].public.id == volume_id)
			volume = &storage->volumes[i];
	if (!volume)
		return ESTALE;
	if (volume->public.protection)
		return EPERM;
	if (operation == FPLINUX_VOLUME_EJECT) {
		if (!volume->mounted)
			return ENODEV;
		error = storage->io.sync(storage->io.context,
					 storage->mountpoint);
		if (!error)
			error = storage->io.unmount(storage->io.context,
						    storage->mountpoint);
		return error;
	}
	if (volume->mounted)
		return 0;
	if (volume->public.state != FPLINUX_VOLUME_AVAILABLE)
		return EOPNOTSUPP;
	if (storage->mountpoint_occupied)
		return EBUSY;
	if (mkdir(storage->mountpoint, 0755) < 0 && errno != EEXIST)
		return errno;
	if (lstat(storage->mountpoint, &status) < 0)
		return errno;
	if (!S_ISDIR(status.st_mode))
		return ENOTDIR;
	if (!make_path(device, sizeof(device), storage->dev_root,
		       volume->device))
		return errno;
	options[0] = '\0';
	if (!strcmp(volume->probe.filesystem, "vfat"))
		snprintf(options, sizeof(options),
			 "uid=%lu,gid=%lu,dmask=0022,fmask=0133,utf8=1",
			 (unsigned long)storage->uid,
			 (unsigned long)storage->gid);
	if (volume->public.read_only)
		flags |= MS_RDONLY;
	error = storage->io.mount(storage->io.context, device,
				  storage->mountpoint, volume->probe.filesystem,
				  flags, options);
	if (error == EROFS && !(flags & MS_RDONLY))
		error = storage->io.mount(storage->io.context, device,
					  storage->mountpoint,
					  volume->probe.filesystem,
					  flags | MS_RDONLY, options);
	return error;
}

static void *storage_worker(void *context)
{
	struct fplinux_session_storage *storage = context;

	pthread_mutex_lock(&storage->mutex);
	for (;;) {
		uint8_t notification = 1;
		int error;

		while (!storage->pending && !storage->stop)
			pthread_cond_wait(&storage->condition, &storage->mutex);
		if (!storage->pending && storage->stop)
			break;
		pthread_mutex_unlock(&storage->mutex);
		error = operate(storage, storage->operation,
				storage->volume_id);
		if (storage->operation != FPLINUX_VOLUME_REFRESH) {
			int scan_error = discover(storage);

			if (!error)
				error = scan_error;
		}
		pthread_mutex_lock(&storage->mutex);
		memset(&storage->result, 0, sizeof(storage->result));
		storage->result.request_id = storage->request_id;
		storage->result.error = error;
		storage->result.snapshot.home_persistent =
			storage->home_persistent;
		storage->result.snapshot.count = storage->count;
		for (size_t i = 0; i < storage->count; ++i)
			storage->result.snapshot.volumes[i] =
				storage->volumes[i].public;
		storage->pending = false;
		storage->ready = true;
		while (write(storage->notification[1], &notification, 1) < 0 &&
		       errno == EINTR)
			;
	}
	pthread_mutex_unlock(&storage->mutex);
	return NULL;
}

struct fplinux_session_storage *fplinux_session_storage_create(
	const struct fplinux_session_storage_config *config)
{
	struct fplinux_session_storage *storage;
	int error;

	if (!config) {
		errno = EINVAL;
		return NULL;
	}
	storage = calloc(1, sizeof(*storage));
	if (!storage)
		return NULL;
	if (!copy_text(storage->sys_block, sizeof(storage->sys_block),
		       config->sys_block ? config->sys_block :
					   "/sys/class/block") ||
	    !copy_text(storage->dev_root, sizeof(storage->dev_root),
		       config->dev_root ? config->dev_root : "/dev") ||
	    !copy_text(storage->mountinfo, sizeof(storage->mountinfo),
		       config->mountinfo ? config->mountinfo :
					   "/proc/self/mountinfo") ||
	    !copy_text(storage->swaps, sizeof(storage->swaps),
		       config->swaps ? config->swaps : "/proc/swaps") ||
	    !copy_text(storage->mountpoint, sizeof(storage->mountpoint),
		       config->mountpoint ? config->mountpoint : "/mnt/card") ||
	    !copy_text(storage->home_path, sizeof(storage->home_path),
		       config->home_path ? config->home_path :
					   "/home/fplinux")) {
		free(storage);
		return NULL;
	}
	storage->uid = config->uid;
	storage->gid = config->gid;
	storage->io = config->io ? *config->io :
				   (struct fplinux_session_storage_io){
					   .probe = probe_device,
					   .mount = mount_device,
					   .sync = sync_volume,
					   .unmount = unmount_volume,
				   };
	if (!storage->io.probe || !storage->io.mount || !storage->io.sync ||
	    !storage->io.unmount) {
		free(storage);
		errno = EINVAL;
		return NULL;
	}
	if (pipe2(storage->notification, O_CLOEXEC | O_NONBLOCK) < 0) {
		free(storage);
		return NULL;
	}
	error = pthread_mutex_init(&storage->mutex, NULL);
	if (error)
		goto fail_pipe;
	error = pthread_cond_init(&storage->condition, NULL);
	if (error)
		goto fail_mutex;
	error = pthread_create(&storage->thread, NULL, storage_worker, storage);
	if (error)
		goto fail_condition;
	return storage;

fail_condition:
	pthread_cond_destroy(&storage->condition);
fail_mutex:
	pthread_mutex_destroy(&storage->mutex);
fail_pipe:
	close(storage->notification[1]);
	close(storage->notification[0]);
	free(storage);
	errno = error;
	return NULL;
}

void fplinux_session_storage_destroy(struct fplinux_session_storage *storage)
{
	if (!storage)
		return;
	pthread_mutex_lock(&storage->mutex);
	storage->stop = true;
	pthread_cond_signal(&storage->condition);
	pthread_mutex_unlock(&storage->mutex);
	pthread_join(storage->thread, NULL);
	pthread_cond_destroy(&storage->condition);
	pthread_mutex_destroy(&storage->mutex);
	close(storage->notification[1]);
	close(storage->notification[0]);
	free(storage);
}

int fplinux_session_storage_fd(const struct fplinux_session_storage *storage)
{
	return storage ? storage->notification[0] : -1;
}

int fplinux_session_storage_request(struct fplinux_session_storage *storage,
				    enum fplinux_volume_operation operation,
				    uint32_t volume_id, uint32_t request_id)
{
	if (!storage || operation < FPLINUX_VOLUME_REFRESH ||
	    operation > FPLINUX_VOLUME_EJECT ||
	    (operation != FPLINUX_VOLUME_REFRESH && !volume_id)) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&storage->mutex);
	if (storage->pending || storage->ready || storage->stop) {
		pthread_mutex_unlock(&storage->mutex);
		errno = EBUSY;
		return -1;
	}
	storage->operation = operation;
	storage->volume_id = volume_id;
	storage->request_id = request_id;
	storage->pending = true;
	pthread_cond_signal(&storage->condition);
	pthread_mutex_unlock(&storage->mutex);
	return 0;
}

int fplinux_session_storage_take_result(struct fplinux_session_storage *storage,
					struct fplinux_volume_result *result)
{
	uint8_t notification;

	if (!storage || !result) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&storage->mutex);
	if (!storage->ready) {
		pthread_mutex_unlock(&storage->mutex);
		return 0;
	}
	*result = storage->result;
	storage->ready = false;
	pthread_mutex_unlock(&storage->mutex);
	while (read(storage->notification[0], &notification, 1) < 0 &&
	       errno == EINTR)
		;
	return 1;
}
