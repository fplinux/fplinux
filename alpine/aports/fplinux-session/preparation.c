/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "preparation.h"
#include "fplinux-cli.h"
#include "fplinux-drm-session.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#define PREPARATION_ANIMATION_MS 250

static bool parse_count(const char *text, uint64_t *value)
{
	uint64_t count = 0;

	if (!*text)
		return false;
	for (; *text; ++text) {
		unsigned int digit;

		if (*text < '0' || *text > '9')
			return false;
		digit = (unsigned int)(*text - '0');
		if (count > (UINT64_MAX - digit) / 10)
			return false;
		count = count * 10 + digit;
	}
	*value = count;
	return true;
}

static int watch_status(const char *path)
{
	char *directory = strdup(path);
	char *slash;
	int fd;
	int saved_errno;

	if (!directory)
		return -1;
	slash = strrchr(directory, '/');
	if (!slash) {
		free(directory);
		directory = strdup(".");
		if (!directory)
			return -1;
	} else if (slash == directory) {
		slash[1] = '\0';
	} else {
		*slash = '\0';
	}
	fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (fd >= 0 &&
	    inotify_add_watch(fd, directory,
			      IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE |
				      IN_MOVED_FROM) < 0) {
		saved_errno = errno;
		close(fd);
		fd = -1;
		errno = saved_errno;
	}
	saved_errno = errno;
	free(directory);
	errno = saved_errno;
	return fd;
}

static bool drain_changes(int fd)
{
	char events[4096];
	ssize_t count;

	do {
		count = read(fd, events, sizeof(events));
	} while (count > 0 || (count < 0 && errno == EINTR));
	return count < 0 && errno == EAGAIN;
}

static uint64_t monotonic_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static bool same_status(const struct fplinux_preparation_status *left,
			const struct fplinux_preparation_status *right)
{
	return left->stage == right->stage &&
	       left->completed == right->completed &&
	       left->total == right->total;
}

static bool change_active(struct fplinux_drm_session *display, bool active,
			  void *data)
{
	bool *dirty = data;

	(void)display;
	if (active)
		*dirty = true;
	return true;
}

static int display_status(const char *path, int changes, int stop_fd,
			  struct fplinux_drm_session *display,
			  const struct fplinux_font *font)
{
	struct fplinux_preparation_status status = { 0 };
	uint64_t next_frame = monotonic_ms();
	unsigned int phase = 0;
	bool dirty = true;
	int result = 1;

	if (!fplinux_preparation_read(path, &status))
		fprintf(stderr, "preparation: cannot read status: %s\n",
			strerror(errno));
	if (!fplinux_drm_session_set_active_handler(display, change_active,
						    &dirty))
		return 1;
	for (;;) {
		struct pollfd descriptors[] = {
			{ .fd = changes, .events = POLLIN },
			{ .fd = fplinux_drm_session_fd(display),
			  .events = POLLIN },
			{ .fd = stop_fd, .events = POLLIN },
		};
		bool animated = display->active &&
				fplinux_preparation_is_busy(status.stage);
		uint64_t now = monotonic_ms();
		int timeout = -1;

		if (animated && now >= next_frame) {
			dirty = true;
			++phase;
			next_frame = now + PREPARATION_ANIMATION_MS;
		}
		if (dirty && display->active) {
			unsigned int page =
				(display->shown_page + 1) % display->pages;
			struct fplinux_preparation_surface surface = {
				.pixels =
					(uint16_t *)(display->mapping +
						     (size_t)page *
							     display->page_bytes),
				.stride_bytes = display->stride,
				.width = display->width,
				.height = display->height,
			};

			if (!fplinux_drm_session_wait_present(display))
				break;
			fplinux_preparation_render(&surface, font, &status,
						   phase);
			if (!fplinux_drm_session_present(display, page))
				break;
			dirty = false;
		}
		if (animated) {
			now = monotonic_ms();
			timeout = now >= next_frame ? 0 :
						      (int)(next_frame - now);
		}
		if (poll(descriptors, 3, timeout) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (descriptors[2].revents & POLLIN) {
			struct signalfd_siginfo signal;

			if (read(stop_fd, &signal, sizeof(signal)) ==
			    sizeof(signal))
				result = 128 + (int)signal.ssi_signo;
			break;
		}
		if (descriptors[1].revents &&
		    !fplinux_drm_session_dispatch(display))
			break;
		if (descriptors[0].revents & POLLIN) {
			struct fplinux_preparation_status updated;

			if (!drain_changes(changes))
				break;
			if (!fplinux_preparation_read(path, &updated)) {
				fprintf(stderr,
					"preparation: cannot read status: %s\n",
					strerror(errno));
			} else if (!same_status(&status, &updated)) {
				status = updated;
				phase = 0;
				dirty = true;
				next_frame = monotonic_ms() +
					     PREPARATION_ANIMATION_MS;
			}
		}
		if ((descriptors[0].revents | descriptors[1].revents |
		     descriptors[2].revents) &
		    (POLLERR | POLLHUP | POLLNVAL)) {
			errno = EIO;
			break;
		}
	}
	fplinux_drm_session_set_active_handler(display, NULL, NULL);
	return result;
}

static int run_display(const char *path)
{
	struct fplinux_drm_session display;
	struct fplinux_font font = { 0 };
	sigset_t stops;
	sigset_t previous_mask;
	char error[256];
	int changes = -1;
	int stop_fd = -1;
	int result = 1;
	bool opened = false;

	sigemptyset(&stops);
	sigaddset(&stops, SIGTERM);
	sigaddset(&stops, SIGINT);
	sigaddset(&stops, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &stops, &previous_mask) < 0)
		return 1;
	stop_fd = signalfd(-1, &stops, SFD_CLOEXEC | SFD_NONBLOCK);
	changes = watch_status(path);
	if (stop_fd < 0 || changes < 0) {
		fprintf(stderr, "preparation: cannot watch status: %s\n",
			strerror(errno));
		goto done;
	}
	if (!fplinux_drm_session_open(&display, NULL, NULL, DRM_FORMAT_RGB565,
				      error, sizeof(error))) {
		fprintf(stderr, "preparation: %s\n", error);
		goto done;
	}
	opened = true;
	if (!fplinux_font_open_default(&font, display.width, error,
				       sizeof(error))) {
		fprintf(stderr, "preparation: %s\n", error);
		goto done;
	}
	if (display.width < 4 * font.width ||
	    display.height < 10 * font.height) {
		fprintf(stderr, "preparation: display is too small\n");
		goto done;
	}
	result = display_status(path, changes, stop_fd, &display, &font);
	if (result == 1)
		fprintf(stderr, "preparation: display failed: %s\n",
			strerror(errno));
done:
	fplinux_font_close(&font);
	if (opened && !fplinux_drm_session_close(&display)) {
		fprintf(stderr, "preparation: cannot restore display: %s\n",
			strerror(errno));
		result = 1;
	}
	if (changes >= 0)
		close(changes);
	if (stop_fd >= 0)
		close(stop_fd);
	sigprocmask(SIG_SETMASK, &previous_mask, NULL);
	return result;
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option options[] = {
		{ "stage", "STAGE",
		  "Write waiting, reading, extracting, ready, error or cancelled",
		  0, NULL, 0 },
		{ "completed", "BYTES", "Bytes read from the phone", 0, NULL,
		  0 },
		{ "total", "BYTES", "Total bytes to read from the phone", 0,
		  NULL, 0 },
		{ NULL, "STATUS_FILE", "Preparation status file",
		  FPLINUX_CLI_REQUIRED, NULL, 0 },
	};
	struct fplinux_cli cli = {
		.program = "preparation",
		.description =
			"Show first-start progress, or atomically update its status.",
		.options = options,
		.option_count = sizeof(options) / sizeof(options[0]),
	};
	struct fplinux_preparation_status status = { 0 };
	enum fplinux_cli_result parsed = fplinux_cli_parse(&cli, argc, argv);

	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	if (!options[0].value) {
		if (options[1].value || options[2].value)
			return fplinux_cli_error(
				&cli, "byte counts require --stage reading");
		return run_display(options[3].value);
	}
	if (!fplinux_preparation_parse_stage(options[0].value, &status.stage))
		return fplinux_cli_error(&cli, "unknown preparation stage");
	if (status.stage == FPLINUX_PREPARATION_READING) {
		if (!options[1].value || !options[2].value ||
		    !parse_count(options[1].value, &status.completed) ||
		    !parse_count(options[2].value, &status.total) ||
		    !status.total || status.completed > status.total)
			return fplinux_cli_error(
				&cli,
				"reading requires 0 <= completed <= total and total > 0");
	} else if (options[1].value || options[2].value) {
		return fplinux_cli_error(&cli,
					 "byte counts require --stage reading");
	}
	if (!fplinux_preparation_write(options[3].value, &status)) {
		fprintf(stderr, "preparation: cannot write status: %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}
