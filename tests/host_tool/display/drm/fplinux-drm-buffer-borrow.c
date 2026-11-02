/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

static struct fplinux_drm_session session;
static bool fail_export;
static unsigned int exports;
static unsigned int callbacks;
static unsigned int removed_framebuffers;
static unsigned int removed_blobs;
static unsigned int removed_buffers;
static unsigned int unmapped;
static int exported_fd = -1;
static uint8_t *original_mapping;

int __real_munmap(void *address, size_t size);

int __wrap_drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags,
			      int *result)
{
	assert(fd == session.drm && handle == 97);
	assert(flags == (DRM_CLOEXEC | DRM_RDWR));
	exports++;
	if (fail_export) {
		errno = EIO;
		return -1;
	}
	exported_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	assert(exported_fd >= 0);
	*result = exported_fd;
	return 0;
}

int __wrap_drmModeRmFB(int fd, uint32_t framebuffer)
{
	assert(fd == session.drm);
	assert(framebuffer == 113 || framebuffer == 117);
	removed_framebuffers++;
	return 0;
}

int __wrap_drmModeDestroyPropertyBlob(int fd, uint32_t blob)
{
	assert(fd == session.drm && blob == 121);
	removed_blobs++;
	return 0;
}

int __wrap_drmIoctl(int fd, unsigned long request, void *argument)
{
	struct drm_mode_destroy_dumb *destroy = argument;

	assert(fd == session.drm && request == DRM_IOCTL_MODE_DESTROY_DUMB);
	assert(destroy->handle == 97);
	removed_buffers++;
	return 0;
}

int __wrap_munmap(void *address, size_t size)
{
	assert(address == original_mapping && size == 4096);
	unmapped++;
	return __real_munmap(address, size);
}

static bool active_changed(struct fplinux_drm_session *display, bool active,
			   void *data)
{
	assert(display == &session && !active && data == &callbacks);
	assert(display->mapping[0] == 0xa7);
	assert(fcntl(exported_fd, F_GETFD) < 0 && errno == EBADF);
	callbacks++;
	return true;
}

static void prepare_session(void)
{
	memset(&session, 0, sizeof(session));
	session.drm = open("/dev/null", O_RDWR | O_CLOEXEC);
	assert(session.drm >= 0);
	session.tty = -1;
	session.control = -1;
	session.signal_fd = -1;
	session.present_fence = -1;
	session.guardian_fd = -1;
	session.size = 4096;
	session.page_bytes = 1920;
	session.stride = 160;
	session.width = 80;
	session.height = 12;
	session.pages = 2;
	session.format = DRM_FORMAT_RGB565;
	session.shown_page = 1;
	session.active = true;
	session.presented = true;
	session.handle = 97;
	session.framebuffers[0] = 113;
	session.framebuffers[1] = 117;
	session.mode_blob = 121;
	session.mapping = mmap(NULL, session.size, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(session.mapping != MAP_FAILED);
	session.mapping[0] = 0xa7;
	original_mapping = session.mapping;
	session.active_handler = active_changed;
	session.active_data = &callbacks;
}

static void expect_no_teardown(void)
{
	assert(!callbacks && !removed_framebuffers && !removed_blobs &&
	       !removed_buffers && !unmapped);
	assert(session.mapping == original_mapping &&
	       session.mapping[0] == 0xa7);
	assert(fcntl(session.drm, F_GETFD) >= 0);
}

static void reject_invalid_arguments(void)
{
	struct fplinux_drm_session invalid = session;
	int fd = 42;

	assert(!fplinux_drm_session_acquire_buffer(NULL, &fd));
	assert(errno == EINVAL && fd == -1);
	assert(!fplinux_drm_session_acquire_buffer(&session, NULL));
	assert(errno == EINVAL);
	invalid.drm = -1;
	assert(!fplinux_drm_session_acquire_buffer(&invalid, &fd));
	assert(errno == EINVAL && fd == -1);
	invalid = session;
	invalid.mapping = NULL;
	assert(!fplinux_drm_session_acquire_buffer(&invalid, &fd));
	assert(errno == EINVAL && fd == -1);
	invalid = session;
	invalid.handle = 0;
	assert(!fplinux_drm_session_acquire_buffer(&invalid, &fd));
	assert(errno == EINVAL && fd == -1 && !exports);
}

int main(void)
{
	struct fplinux_drm_session attached;
	int fd = 42;
	int duplicate = 42;
	int drm;

	prepare_session();
	reject_invalid_arguments();
	fail_export = true;
	assert(!fplinux_drm_session_acquire_buffer(&session, &fd));
	assert(errno == EIO && fd == -1 && !session.buffer_acquired);
	expect_no_teardown();
	fail_export = false;
	assert(fplinux_drm_session_acquire_buffer(&session, &fd));
	assert(fd >= 0 && session.buffer_acquired && exports == 2);
	assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
	assert(session.format == DRM_FORMAT_RGB565 && session.stride == 160 &&
	       session.page_bytes == 1920 && session.size == 4096 &&
	       session.pages == 2 && session.shown_page == 1);
	assert(!fplinux_drm_session_acquire_buffer(&session, &duplicate));
	assert(errno == EBUSY && duplicate == -1 && exports == 2);
	attached = session;
	assert(!fplinux_drm_session_close(&session));
	assert(errno == EBUSY && !memcmp(&session, &attached, sizeof(session)));
	expect_no_teardown();
	assert(fcntl(fd, F_GETFD) >= 0);
	assert(close(fd) == 0);
	fplinux_drm_session_release_buffer(&session);
	assert(!session.buffer_acquired);
	drm = session.drm;
	assert(fplinux_drm_session_close(&session));
	assert(callbacks == 1 && removed_framebuffers == 2 &&
	       removed_blobs == 1 && removed_buffers == 1 && unmapped == 1);
	assert(!session.mapping && session.drm == -1 &&
	       !session.buffer_acquired);
	assert(fcntl(drm, F_GETFD) < 0 && errno == EBADF);
	puts("DRM borrow: validation, exact export, export failure, duplicate, "
	     "close guard, release cleanup passed");
	return 0;
}
