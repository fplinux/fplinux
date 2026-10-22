/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-vibrator.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define VIBRATOR_PHYS "fplinux/vibrator0"
#define LONG_BITS (8U * sizeof(unsigned long))
#define BIT_WORDS(count) (((count) + LONG_BITS - 1U) / LONG_BITS)

static bool has_bit(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / LONG_BITS] & (1UL << (bit % LONG_BITS))) != 0;
}

static bool is_event_name(const char *name)
{
	if (strncmp(name, "event", 5) || !name[5])
		return false;
	for (name += 5; *name; ++name) {
		if (*name < '0' || *name > '9')
			return false;
	}
	return true;
}

static bool is_vibrator(int fd)
{
	unsigned long types[BIT_WORDS(EV_CNT)] = { 0 };
	unsigned long effects[BIT_WORDS(FF_CNT)] = { 0 };
	char phys[64] = { 0 };
	struct stat status;

	return fstat(fd, &status) == 0 && S_ISCHR(status.st_mode) &&
	       ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys) >= 0 &&
	       !strcmp(phys, VIBRATOR_PHYS) &&
	       ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) >= 0 &&
	       has_bit(types, EV_FF) &&
	       ioctl(fd, EVIOCGBIT(EV_FF, sizeof(effects)), effects) >= 0 &&
	       has_bit(effects, FF_RUMBLE);
}

static bool write_state(struct fplinux_session_vibrator *vibrator, bool on)
{
	struct input_event event = {
		.type = EV_FF,
		.code = (uint16_t)vibrator->effect_id,
		.value = on ? 1 : 0,
	};
	ssize_t written;

	do {
		written = write(vibrator->fd, &event, sizeof(event));
	} while (written < 0 && errno == EINTR);
	if (written == (ssize_t)sizeof(event)) {
		vibrator->on = on;
		return true;
	}
	if (written >= 0)
		errno = EIO;
	return false;
}

void fplinux_session_vibrator_init(struct fplinux_session_vibrator *vibrator)
{
	*vibrator = (struct fplinux_session_vibrator){
		.fd = -1,
		.effect_id = -1,
	};
}

bool fplinux_session_vibrator_available(
	const struct fplinux_session_vibrator *vibrator)
{
	return vibrator->fd >= 0 && vibrator->effect_id >= 0;
}

bool fplinux_session_vibrator_close(struct fplinux_session_vibrator *vibrator)
{
	int error = 0;

	if (vibrator->fd >= 0) {
		if (vibrator->effect_id >= 0) {
			if (!write_state(vibrator, false))
				error = errno;
			if (ioctl(vibrator->fd, EVIOCRMFF,
				  vibrator->effect_id) < 0 &&
			    !error)
				error = errno;
		}
		if (close(vibrator->fd) < 0 && !error)
			error = errno;
	}
	fplinux_session_vibrator_init(vibrator);
	if (error)
		errno = error;
	return !error;
}

bool fplinux_session_vibrator_open(struct fplinux_session_vibrator *vibrator,
				   const char *input_dir, uint16_t duration_ms,
				   uint16_t magnitude)
{
	struct ff_effect effect = {
		.type = FF_RUMBLE,
		.id = -1,
		.replay.length = duration_ms,
		.u.rumble.strong_magnitude = magnitude,
		.u.rumble.weak_magnitude = magnitude,
	};
	struct dirent *entry;
	DIR *directory;
	int selected = -1;
	int error = 0;

	if (!fplinux_session_vibrator_close(vibrator))
		return false;
	if (!input_dir || !duration_ms) {
		errno = EINVAL;
		return false;
	}
	directory = opendir(input_dir);
	if (!directory)
		return false;
	for (;;) {
		char path[PATH_MAX];
		int fd;

		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			error = errno;
			break;
		}
		if (!is_event_name(entry->d_name) ||
		    snprintf(path, sizeof(path), "%s/%s", input_dir,
			     entry->d_name) >= (int)sizeof(path))
			continue;
		fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (!is_vibrator(fd)) {
			close(fd);
			continue;
		}
		if (selected >= 0) {
			close(fd);
			error = ENOTUNIQ;
			break;
		}
		selected = fd;
	}
	if (closedir(directory) < 0 && !error)
		error = errno;
	if (!error && selected < 0)
		error = ENODEV;
	if (!error && ioctl(selected, EVIOCSFF, &effect) < 0)
		error = errno;
	if (error) {
		if (selected >= 0)
			close(selected);
		errno = error;
		return false;
	}
	vibrator->fd = selected;
	vibrator->effect_id = effect.id;
	return true;
}

bool fplinux_session_vibrator_set(struct fplinux_session_vibrator *vibrator,
				  bool on)
{
	int error;

	if (!fplinux_session_vibrator_available(vibrator)) {
		errno = ENODEV;
		return false;
	}
	if (vibrator->on == on || write_state(vibrator, on))
		return true;
	error = errno;
	fplinux_session_vibrator_close(vibrator);
	errno = error;
	return false;
}
