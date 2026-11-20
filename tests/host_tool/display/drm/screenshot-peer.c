/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

int __real_open(const char *path, int flags, ...);

int __wrap_open(const char *path, int flags, ...)
{
	assert(strcmp(path, "/dev/dri/card0") == 0);
	assert((flags & O_ACCMODE) == O_RDONLY);
	return __real_open("pixels", flags);
}

/* glibc selects open64 for the reader's 64-bit mapping offset. */
int __wrap_open64(const char *path, int flags, ...)
{
	return __wrap_open(path, flags);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	static unsigned int crtc_reads;
	const char *scenario = getenv("SCANOUT_SCENARIO");
	void *data;
	va_list args;

	assert(fd >= 0);
	assert(scenario);
	va_start(args, request);
	data = va_arg(args, void *);
	va_end(args);

	switch (request) {
	case DRM_IOCTL_MODE_GETRESOURCES: {
		struct drm_mode_card_res *resources = data;
		uint32_t *ids = (void *)(uintptr_t)resources->crtc_id_ptr;

		assert(resources->count_crtcs >= 1);
		resources->count_crtcs = 1;
		ids[0] = 2;
		return 0;
	}
	case DRM_IOCTL_MODE_GETCRTC: {
		struct drm_mode_crtc *crtc = data;

		assert(crtc->crtc_id == 2);
		crtc->fb_id =
			crtc_reads++ && strcmp(scenario, "page-flip") == 0 ? 5 :
									     4;
		crtc->mode_valid = 1;
		crtc->mode.hdisplay = crtc->mode.vdisplay = 2;
		return 0;
	}
	case DRM_IOCTL_MODE_GETFB2: {
		struct drm_mode_fb_cmd2 *frame = data;

		assert(frame->fb_id == 4);
		frame->width = frame->height = 2;
		frame->handles[0] = 6;
		frame->offsets[0] = 2;
		frame->pitches[0] = 6;
		frame->pixel_format = strcmp(scenario, "unsupported") == 0 ?
					      DRM_FORMAT_XRGB8888 :
					      DRM_FORMAT_RGB565;
		return 0;
	}
	case DRM_IOCTL_MODE_MAP_DUMB: {
		struct drm_mode_map_dumb *map = data;

		assert(map->handle == 6);
		map->offset = 0;
		return 0;
	}
	default:
		abort();
	}
}
