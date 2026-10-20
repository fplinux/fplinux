/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include "fplinux-drm-session.h"
#include "display-test-gpu.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE_BYTES (240U * 320U * 2U)

static uint8_t pixels[PAGE_BYTES * 2];
static uint8_t shown_pixels[PAGE_BYTES];
static const char *scenario;
static uint64_t clock_ns;
static unsigned int presentations;
static bool presentation_pending;

struct fplinux_display_test_gpu {
	struct fplinux_drm_session *session;
	enum fplinux_display_test_pattern pattern;
	uint32_t frame_id;
	unsigned int draws;
	unsigned int checks;
	bool drawn;
	const char *error;
};

static struct fplinux_display_test_gpu *live_gpu;

/* The CPU patterns stay real; DRM/VT, GPU and time are controlled boundaries. */
bool fplinux_drm_session_open(struct fplinux_drm_session *session,
			      const char *drm_path, const char *tty_path,
			      uint32_t format, char *error, size_t error_size)
{
	(void)tty_path;
	(void)error;
	(void)error_size;
	assert(format == DRM_FORMAT_RGB565);
	assert(!live_gpu);
	scenario = drm_path ? drm_path : "normal";
	clock_ns = 1000000000ULL;
	presentations = 0;
	memset(pixels, 0x5a, sizeof(pixels));
	*session = (struct fplinux_drm_session){
		.mapping = pixels,
		.size = sizeof(pixels),
		.page_bytes = PAGE_BYTES,
		.stride = 480,
		.width = 240,
		.height = 320,
		.format = format,
		.pages = 2,
		.active = strcmp(scenario, "inactive") != 0,
	};
	if (!strcmp(scenario, "invalid-layout"))
		session->page_bytes = PAGE_BYTES - 1;
	fputs("fake-session opened\n", stderr);
	return true;
}

int fplinux_drm_session_fd(const struct fplinux_drm_session *session)
{
	(void)session;
	return -1;
}

bool fplinux_drm_session_dispatch(struct fplinux_drm_session *session)
{
	(void)session;
	return true;
}

bool fplinux_drm_session_wait_until(struct fplinux_drm_session *session,
				    const struct timespec *deadline)
{
	uint64_t deadline_ns =
		(uint64_t)deadline->tv_sec * 1000000000ULL + deadline->tv_nsec;

	(void)session;
	if (deadline_ns > clock_ns)
		clock_ns = deadline_ns;
	return true;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *value)
{
	assert(clock == CLOCK_MONOTONIC);
	value->tv_sec = (time_t)(clock_ns / 1000000000ULL);
	value->tv_nsec = (long)(clock_ns % 1000000000ULL);
	clock_ns += 1000000ULL;
	return 0;
}

int __wrap_poll(struct pollfd *events, nfds_t count, int timeout_ms)
{
	assert(count == 1 && events[0].fd == -1 && timeout_ms > 0);
	clock_ns += (uint64_t)timeout_ms * 1000000ULL;
	return 0;
}

bool fplinux_drm_session_present(struct fplinux_drm_session *session,
				 unsigned int page)
{
	assert(page < 2 && page != session->shown_page);
	++presentations;
	if (session->presented)
		assert(memcmp(shown_pixels,
			      pixels + session->shown_page * PAGE_BYTES,
			      PAGE_BYTES) == 0);
	if (!strcmp(scenario, "present-error")) {
		errno = EIO;
		return false;
	}
	memcpy(shown_pixels, pixels + page * PAGE_BYTES, PAGE_BYTES);
	session->shown_page = page;
	session->presented = true;
	if (!strcmp(scenario, "signal"))
		assert(raise(SIGTERM) == 0);
	return true;
}

bool fplinux_drm_session_wait_present(struct fplinux_drm_session *session)
{
	(void)session;
	if (presentation_pending && !strcmp(scenario, "gpu-wait-error")) {
		errno = EIO;
		return false;
	}
	presentation_pending = false;
	return true;
}

bool fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	assert(!live_gpu);
	assert(!presentation_pending || !strcmp(scenario, "gpu-wait-error"));
	if (session->presented)
		assert(memcmp(shown_pixels,
			      pixels + session->shown_page * PAGE_BYTES,
			      PAGE_BYTES) == 0);
	fprintf(stderr, "fake-session restored presentations=%u\n",
		presentations);
	return true;
}

struct fplinux_display_test_gpu *
fplinux_display_test_gpu_open(struct fplinux_drm_session *session, char *error,
			      size_t error_size)
{
	assert(!live_gpu);
	if (!strcmp(scenario, "gpu-open-error")) {
		snprintf(error, error_size, "controlled GPU startup failure");
		errno = ENODEV;
		return NULL;
	}
	live_gpu = calloc(1, sizeof(*live_gpu));
	assert(live_gpu);
	live_gpu->session = session;
	live_gpu->error = "";
	fputs("fake-gpu opened\n", stderr);
	return live_gpu;
}

bool fplinux_display_test_gpu_draw(struct fplinux_display_test_gpu *gpu,
				   enum fplinux_display_test_pattern pattern,
				   uint32_t frame_id)
{
	assert(gpu == live_gpu && !gpu->drawn);
	assert(frame_id == gpu->draws);
	if (!strcmp(scenario, "gpu-draw-error") ||
	    (!strcmp(scenario, "gpu-second-draw-error") && frame_id == 1)) {
		gpu->error = "controlled GPU draw failure";
		errno = EIO;
		return false;
	}
	if (!fplinux_drm_session_wait_present(gpu->session))
		return false;
	/* A later CPU draw would overwrite this test-owned GPU page witness. */
	memset(pixels + (1U - gpu->session->shown_page) * PAGE_BYTES, 0xa7,
	       PAGE_BYTES);
	gpu->pattern = pattern;
	gpu->frame_id = frame_id;
	++gpu->draws;
	gpu->drawn = true;
	return true;
}

bool fplinux_display_test_gpu_verify(struct fplinux_display_test_gpu *gpu,
				     enum fplinux_display_test_pattern pattern,
				     uint32_t frame_id)
{
	assert(gpu == live_gpu && gpu->drawn);
	assert(gpu->pattern == pattern && gpu->frame_id == frame_id);
	++gpu->checks;
	if (!strcmp(scenario, "gpu-verify-error") ||
	    (!strcmp(scenario, "gpu-second-verify-error") && frame_id == 1)) {
		gpu->error = "controlled GPU pixel mismatch";
		errno = EILSEQ;
		return false;
	}
	return true;
}

bool fplinux_display_test_gpu_present(struct fplinux_display_test_gpu *gpu)
{
	unsigned int page = 1U - gpu->session->shown_page;

	assert(gpu == live_gpu && gpu->drawn);
	for (size_t offset = 0; offset < PAGE_BYTES; ++offset)
		assert(pixels[page * PAGE_BYTES + offset] == 0xa7);
	if (!fplinux_drm_session_present(gpu->session, page)) {
		gpu->error = "controlled GPU completion failure";
		return false;
	}
	gpu->drawn = false;
	presentation_pending = true;
	return true;
}

const char *
fplinux_display_test_gpu_error(const struct fplinux_display_test_gpu *gpu)
{
	return gpu->error;
}

bool fplinux_display_test_gpu_close(struct fplinux_display_test_gpu *gpu)
{
	bool failed = !strcmp(scenario, "gpu-close-error");

	if (!gpu)
		return true;
	assert(gpu == live_gpu);
	if (!fplinux_drm_session_wait_present(gpu->session))
		failed = true;
	fprintf(stderr, "fake-gpu restored draws=%u checks=%u\n", gpu->draws,
		gpu->checks);
	free(gpu);
	live_gpu = NULL;
	if (failed)
		errno = EIO;
	return !failed;
}
