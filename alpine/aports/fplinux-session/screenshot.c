/* SPDX-License-Identifier: GPL-2.0-only */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_CRTCS 16
#define MAX_DIMENSION 1024
#define MAX_MAPPING_BYTES (16U * 1024U * 1024U)

static bool active_crtc(int fd, struct drm_mode_crtc *selected)
{
	uint32_t ids[MAX_CRTCS];
	struct drm_mode_card_res resources = {
		.crtc_id_ptr = (uintptr_t)ids,
		.count_crtcs = MAX_CRTCS,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0) {
		perror("Read DRM display resources");
		return false;
	}
	if (resources.count_crtcs > MAX_CRTCS) {
		fputs("Unsupported number of DRM displays\n", stderr);
		return false;
	}
	for (uint32_t i = 0; i < resources.count_crtcs; i++) {
		struct drm_mode_crtc crtc = { .crtc_id = ids[i] };

		if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0) {
			perror("Read DRM display state");
			return false;
		}
		if (!crtc.mode_valid || !crtc.fb_id)
			continue;
		if (selected->fb_id) {
			fputs("More than one DRM display is active\n", stderr);
			return false;
		}
		*selected = crtc;
	}
	if (!selected->fb_id) {
		fputs("No active DRM display to capture\n", stderr);
		return false;
	}
	return true;
}

static bool frame_layout_supported(const struct drm_mode_crtc *crtc,
				   const struct drm_mode_fb_cmd2 *frame)
{
	return frame->handles[0] && frame->pixel_format == DRM_FORMAT_RGB565 &&
	       !(frame->flags & DRM_MODE_FB_INTERLACED) &&
	       (!(frame->flags & DRM_MODE_FB_MODIFIERS) ||
		frame->modifier[0] == DRM_FORMAT_MOD_LINEAR) &&
	       !crtc->x && !crtc->y && frame->width && frame->height &&
	       frame->width <= MAX_DIMENSION &&
	       frame->height <= MAX_DIMENSION &&
	       frame->width == crtc->mode.hdisplay &&
	       frame->height == crtc->mode.vdisplay &&
	       frame->pitches[0] >= 2 * frame->width;
}

static void copy_rgb565(const uint8_t *mapping,
			const struct drm_mode_fb_cmd2 *frame, uint8_t *pixels)
{
	for (uint32_t y = 0; y < frame->height; y++) {
		const uint8_t *row = mapping + frame->offsets[0] +
				     (size_t)y * frame->pitches[0];

		for (uint32_t x = 0; x < frame->width; x++) {
			unsigned int pixel = row[2 * x] |
					     (unsigned int)row[2 * x + 1] << 8;

			*pixels++ = (pixel >> 11) * 255 / 31;
			*pixels++ = ((pixel >> 5) & 63) * 255 / 63;
			*pixels++ = (pixel & 31) * 255 / 31;
		}
	}
}

static bool same_scanout(int fd, const struct drm_mode_crtc *before)
{
	struct drm_mode_crtc after = { .crtc_id = before->crtc_id };

	if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &after) < 0) {
		perror("Recheck DRM display state");
		return false;
	}
	if (!after.mode_valid || after.fb_id != before->fb_id ||
	    after.x != before->x || after.y != before->y ||
	    after.mode.hdisplay != before->mode.hdisplay ||
	    after.mode.vdisplay != before->mode.vdisplay) {
		fputs("DRM scanout changed during capture; retry the screenshot\n",
		      stderr);
		return false;
	}
	return true;
}

static bool capture(int fd)
{
	struct drm_mode_crtc crtc = { 0 };
	struct drm_mode_fb_cmd2 frame = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	uint64_t mapping_bytes;
	void *mapping;
	size_t image_bytes;
	uint8_t *pixels;
	bool ok = false;

	if (!active_crtc(fd, &crtc))
		return false;
	frame.fb_id = crtc.fb_id;
	if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &frame) < 0) {
		perror("Read DRM framebuffer description");
		return false;
	}
	if (!frame_layout_supported(&crtc, &frame)) {
		fputs("Unsupported DRM scanout layout; expected a linear RGB565 frame\n",
		      stderr);
		return false;
	}
	mapping_bytes = frame.offsets[0] +
			(uint64_t)(frame.height - 1) * frame.pitches[0] +
			2 * frame.width;
	if (mapping_bytes > MAX_MAPPING_BYTES) {
		fputs("DRM framebuffer mapping exceeds the size limit\n",
		      stderr);
		return false;
	}
	map.handle = frame.handles[0];
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("Map DRM framebuffer");
		return false;
	}
	if (map.offset > INT64_MAX) {
		fputs("Unsupported DRM framebuffer mapping offset\n", stderr);
		return false;
	}
	image_bytes = (size_t)frame.width * frame.height * 3;
	pixels = malloc(image_bytes);
	if (!pixels) {
		perror("Allocate screenshot");
		return false;
	}
	mapping = mmap(NULL, (size_t)mapping_bytes, PROT_READ, MAP_SHARED, fd,
		       (off_t)map.offset);
	if (mapping == MAP_FAILED) {
		perror("Read DRM framebuffer mapping");
		goto done;
	}
	copy_rgb565(mapping, &frame, pixels);
	munmap(mapping, (size_t)mapping_bytes);
	if (!same_scanout(fd, &crtc))
		goto done;
	ok = printf("P6\n%u %u\n255\n", frame.width, frame.height) > 0 &&
	     fwrite(pixels, 1, image_bytes, stdout) == image_bytes &&
	     fflush(stdout) == 0;
	if (!ok)
		perror("Write screenshot");
done:
	free(pixels);
	return ok;
}

int main(void)
{
	int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0) {
		perror("Open DRM display /dev/dri/card0");
		return 1;
	}
	ok = capture(fd);
	/* Closing the DRM file also releases the GEM handles returned by GETFB2. */
	close(fd);
	return ok ? 0 : 1;
}
