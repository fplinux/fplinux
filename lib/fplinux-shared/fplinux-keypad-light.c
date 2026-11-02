// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "fplinux-keypad-light.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int attribute_path(char *path, size_t size, const char *directory,
			  const char *attribute)
{
	int length = snprintf(path, size, "%s/%s", directory, attribute);

	if (length < 0 || (size_t)length >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int read_number(const char *directory, const char *attribute, int *value)
{
	char path[FPLINUX_KEYPAD_LIGHT_PATH_BYTES];
	char buffer[32];
	char *end;
	long parsed;
	ssize_t count;
	int descriptor, saved_errno;

	if (attribute_path(path, sizeof(path), directory, attribute) < 0)
		return -1;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	do {
		count = read(descriptor, buffer, sizeof(buffer) - 1U);
	} while (count < 0 && errno == EINTR);
	saved_errno = errno;
	if (close(descriptor) < 0 && count >= 0)
		return -1;
	if (count <= 0) {
		errno = count < 0 ? saved_errno : EINVAL;
		return -1;
	}
	buffer[count] = '\0';
	errno = 0;
	parsed = strtol(buffer, &end, 10);
	if (errno || end == buffer || (*end != '\0' && *end != '\n') ||
	    parsed < 0 || parsed > INT_MAX) {
		errno = EINVAL;
		return -1;
	}
	*value = (int)parsed;
	return 0;
}

static int read_trigger(struct fplinux_keypad_light *light)
{
	char path[FPLINUX_KEYPAD_LIGHT_PATH_BYTES];
	char *buffer = NULL;
	char *selected, *end;
	size_t capacity = 0;
	ssize_t count;
	int saved_errno;
	FILE *file;

	if (attribute_path(path, sizeof(path), light->directory, "trigger") < 0)
		return -1;
	file = fopen(path, "re");
	if (!file)
		return errno == ENOENT ? 0 : -1;
	errno = 0;
	count = getline(&buffer, &capacity, file);
	saved_errno = errno;
	if (fclose(file) < 0 && count >= 0) {
		free(buffer);
		return -1;
	}
	if (count < 0) {
		free(buffer);
		errno = saved_errno ? saved_errno : EIO;
		return -1;
	}
	selected = strchr(buffer, '[');
	end = selected ? strchr(selected + 1, ']') : NULL;
	if (!end || end == selected + 1) {
		free(buffer);
		errno = EINVAL;
		return -1;
	}
	*end = '\0';
	memmove(buffer, selected + 1, (size_t)(end - selected));
	light->original_trigger = buffer;
	return 0;
}

static int write_attribute(const struct fplinux_keypad_light *light,
			   const char *attribute, const char *value)
{
	char path[FPLINUX_KEYPAD_LIGHT_PATH_BYTES];
	size_t length = strlen(value);
	ssize_t written;
	int descriptor, saved_errno;

	if (attribute_path(path, sizeof(path), light->directory, attribute) < 0)
		return -1;
	descriptor = open(path, O_WRONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	do {
		written = write(descriptor, value, length);
	} while (written < 0 && errno == EINTR);
	saved_errno = errno;
	if (close(descriptor) < 0 && written == (ssize_t)length)
		return -1;
	if (written != (ssize_t)length) {
		errno = written < 0 ? saved_errno : EIO;
		return -1;
	}
	return 0;
}

static int write_brightness(const struct fplinux_keypad_light *light, int value)
{
	char buffer[32];

	snprintf(buffer, sizeof(buffer), "%d\n", value);
	return write_attribute(light, "brightness", buffer);
}

static bool keyboard_led_path(const char *path)
{
	static const char function[] = "kbd_backlight";
	const char *brightness = strrchr(path, '/');
	const char *component;
	size_t length;

	if (!brightness || strcmp(brightness, "/brightness"))
		return false;
	component = brightness;
	while (component > path && component[-1] != '/')
		--component;
	length = (size_t)(brightness - component);
	return (length == sizeof(function) - 1U &&
		!memcmp(component, function, sizeof(function) - 1U)) ||
	       (length > sizeof(function) - 1U &&
		component[length - sizeof(function)] == ':' &&
		!memcmp(component + length - sizeof(function) + 1U, function,
			sizeof(function) - 1U));
}

int fplinux_keypad_light_find(char *directory, size_t size,
			      const char *leds_directory)
{
	char pattern[FPLINUX_KEYPAD_LIGHT_PATH_BYTES];
	glob_t matches = { 0 };
	const char *selected = NULL;
	size_t index, length;
	int result, error = ENODEV;

	if (attribute_path(pattern, sizeof(pattern), leds_directory,
			   "*/brightness") < 0)
		return -1;
	result = glob(pattern, 0, NULL, &matches);
	if (result) {
		globfree(&matches);
		errno = result == GLOB_NOMATCH ? ENODEV : EIO;
		return -1;
	}
	for (index = 0; index < matches.gl_pathc; ++index) {
		if (!keyboard_led_path(matches.gl_pathv[index]))
			continue;
		if (selected) {
			error = ENOTUNIQ;
			goto fail;
		}
		selected = matches.gl_pathv[index];
	}
	if (!selected)
		goto fail;
	length = (size_t)(strrchr(selected, '/') - selected);
	if (length >= size) {
		error = ENAMETOOLONG;
		goto fail;
	}
	memcpy(directory, selected, length);
	directory[length] = '\0';
	globfree(&matches);
	return 0;
fail:
	globfree(&matches);
	errno = error;
	return -1;
}

int fplinux_keypad_light_open(struct fplinux_keypad_light *light,
			      const char *directory)
{
	size_t length = strlen(directory);
	int saved_errno;

	memset(light, 0, sizeof(*light));
	if (length >= sizeof(light->directory)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(light->directory, directory, length + 1);
	if (read_number(directory, "max_brightness", &light->maximum) < 0 ||
	    read_number(directory, "brightness", &light->original_brightness) <
		    0)
		return -1;
	if (light->maximum < 1 || light->original_brightness > light->maximum) {
		errno = EINVAL;
		return -1;
	}
	if (read_trigger(light) < 0)
		return -1;
	light->claimed = true;
	if ((light->original_trigger &&
	     write_attribute(light, "trigger", "none") < 0) ||
	    write_brightness(light, 0) < 0) {
		saved_errno = errno;
		fplinux_keypad_light_close(light);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

int fplinux_keypad_light_set(struct fplinux_keypad_light *light, bool on)
{
	if (!light->claimed) {
		errno = EBADF;
		return -1;
	}
	return write_brightness(light, on ? light->maximum : 0);
}

int fplinux_keypad_light_close(struct fplinux_keypad_light *light)
{
	int error = 0;

	if (light->claimed) {
		if (write_brightness(light, light->original_brightness) < 0)
			error = errno;
		/* A zero brightness write removes the trigger: restore it last. */
		if (light->original_trigger &&
		    write_attribute(light, "trigger", light->original_trigger) <
			    0 &&
		    !error)
			error = errno;
	}
	free(light->original_trigger);
	memset(light, 0, sizeof(*light));
	if (error) {
		errno = error;
		return -1;
	}
	return 0;
}
