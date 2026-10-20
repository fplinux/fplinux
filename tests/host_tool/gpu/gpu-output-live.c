/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <GLES3/gl3.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include "fplinux-drm-session.h"
#include "fplinux-gpu-output.h"

#define GL_ALPHA_TEST 0x0bc0

/* This peer exercises real imported BOs; its presenter does not drive a panel. */
struct marker {
	unsigned int source_x;
	unsigned int source_y;
	unsigned int output_x;
	unsigned int output_y;
	uint8_t rgba[4];
	uint16_t rgb565;
};

struct geometry {
	const char *name;
	unsigned int width;
	unsigned int height;
	unsigned int source_width;
	unsigned int source_height;
	bool clockwise;
	struct marker markers[6];
};

static const struct geometry geometries[] = {
	{ "native-240x320",
	  240,
	  320,
	  240,
	  320,
	  false,
	  {
		  { 0, 319, 0, 0, { 255, 0, 0, 0 }, 0xf800 },
		  { 239, 319, 239, 0, { 0, 255, 0, 255 }, 0x07e0 },
		  { 0, 0, 0, 319, { 0, 0, 255, 255 }, 0x001f },
		  { 239, 0, 239, 319, { 255, 255, 255, 255 }, 0xffff },
		  { 7, 300, 7, 19, { 0, 255, 255, 255 }, 0x07ff },
		  { 113, 17, 113, 302, { 255, 0, 255, 255 }, 0xf81f },
	  } },
	{ "native-128x160",
	  128,
	  160,
	  128,
	  160,
	  false,
	  {
		  { 0, 159, 0, 0, { 255, 0, 0, 0 }, 0xf800 },
		  { 127, 159, 127, 0, { 0, 255, 0, 255 }, 0x07e0 },
		  { 0, 0, 0, 159, { 0, 0, 255, 255 }, 0x001f },
		  { 127, 0, 127, 159, { 255, 255, 255, 255 }, 0xffff },
		  { 7, 140, 7, 19, { 0, 255, 255, 255 }, 0x07ff },
		  { 93, 17, 93, 142, { 255, 0, 255, 255 }, 0xf81f },
	  } },
	{ "native-320x240",
	  320,
	  240,
	  320,
	  240,
	  false,
	  {
		  { 0, 239, 0, 0, { 255, 0, 0, 0 }, 0xf800 },
		  { 319, 239, 319, 0, { 0, 255, 0, 255 }, 0x07e0 },
		  { 0, 0, 0, 239, { 0, 0, 255, 255 }, 0x001f },
		  { 319, 0, 319, 239, { 255, 255, 255, 255 }, 0xffff },
		  { 7, 220, 7, 19, { 0, 255, 255, 255 }, 0x07ff },
		  { 293, 17, 293, 222, { 255, 0, 255, 255 }, 0xf81f },
	  } },
	{ "clockwise-240x320",
	  240,
	  320,
	  320,
	  240,
	  true,
	  {
		  { 0, 0, 0, 0, { 255, 0, 0, 0 }, 0xf800 },
		  { 0, 239, 239, 0, { 0, 255, 0, 255 }, 0x07e0 },
		  { 319, 0, 0, 319, { 0, 0, 255, 255 }, 0x001f },
		  { 319, 239, 239, 319, { 255, 255, 255, 255 }, 0xffff },
		  { 19, 7, 7, 19, { 0, 255, 255, 255 }, 0x07ff },
		  { 302, 113, 113, 302, { 255, 0, 255, 255 }, 0xf81f },
	  } },
	{ "clockwise-scale2-128x160",
	  128,
	  160,
	  320,
	  256,
	  true,
	  {
		  { 0, 1, 0, 0, { 255, 0, 0, 0 }, 0xf800 },
		  { 0, 255, 127, 0, { 0, 255, 0, 255 }, 0x07e0 },
		  { 318, 1, 0, 159, { 0, 0, 255, 255 }, 0x001f },
		  { 318, 255, 127, 159, { 255, 255, 255, 255 }, 0xffff },
		  { 38, 15, 7, 19, { 0, 255, 255, 255 }, 0x07ff },
		  { 284, 187, 93, 142, { 255, 0, 255, 255 }, 0xf81f },
	  } },
};

static struct {
	struct fplinux_drm_session *session;
	unsigned int page;
} presentation;

static void fail(const char *operation)
{
	fprintf(stderr, "GPU output peer: %s: %s\n", operation,
		strerror(errno));
	exit(EXIT_FAILURE);
}

bool fplinux_drm_session_acquire_buffer(struct fplinux_drm_session *session,
					int *fd)
{
	if (session->buffer_acquired) {
		errno = EBUSY;
		return false;
	}
	if (drmPrimeHandleToFD(session->drm, session->handle,
			       DRM_CLOEXEC | DRM_RDWR, fd) < 0)
		return false;
	session->buffer_acquired = true;
	return true;
}

void fplinux_drm_session_release_buffer(struct fplinux_drm_session *session)
{
	if (presentation.session) {
		errno = EBUSY;
		fail("presentation still pending at buffer release");
	}
	session->buffer_acquired = false;
}

bool fplinux_drm_session_queue_present(struct fplinux_drm_session *session,
				       unsigned int page)
{
	if (presentation.session) {
		errno = EBUSY;
		return false;
	}
	if (page >= 2 || page == session->shown_page) {
		errno = EINVAL;
		return false;
	}
	presentation.session = session;
	presentation.page = page;
	return true;
}

bool fplinux_drm_session_wait_present(struct fplinux_drm_session *session)
{
	if (!presentation.session)
		return true;
	if (presentation.session != session) {
		errno = EINVAL;
		return false;
	}
	session->shown_page = presentation.page;
	session->presented = true;
	presentation.session = NULL;
	return true;
}

static bool create_buffer(struct fplinux_drm_session *session, int drm,
			  const struct geometry *geometry)
{
	struct drm_mode_create_dumb create = {
		.width = geometry->width,
		.height = geometry->height * 2,
		.bpp = 16,
	};
	struct drm_mode_map_dumb map = { 0 };

	memset(session, 0, sizeof(*session));
	session->drm = drm;
	session->present_fence = -1;
	if (drmIoctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
		return false;
	session->handle = create.handle;
	session->format = DRM_FORMAT_RGB565;
	session->width = geometry->width;
	session->height = geometry->height;
	session->stride = create.pitch;
	session->size = create.size;
	session->page_bytes = (size_t)create.pitch * geometry->height;
	session->pages = 2;
	session->active = true;
	map.handle = create.handle;
	if (session->page_bytes > session->size / 2 ||
	    drmIoctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
		return false;
	session->mapping = mmap(NULL, session->size, PROT_READ | PROT_WRITE,
				MAP_SHARED, drm, map.offset);
	if (session->mapping == MAP_FAILED) {
		session->mapping = NULL;
		return false;
	}
	return true;
}

static void destroy_buffer(struct fplinux_drm_session *session)
{
	struct drm_mode_destroy_dumb destroy = { .handle = session->handle };

	if (session->buffer_acquired || presentation.session) {
		errno = EBUSY;
		fail("buffer still borrowed at teardown");
	}
	if (session->mapping && munmap(session->mapping, session->size) < 0)
		fail("unmap dumb BO");
	if (session->handle &&
	    drmIoctl(session->drm, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
		fail("destroy dumb BO");
}

static uint16_t expected_pixel(const struct geometry *geometry, unsigned int x,
			       unsigned int y)
{
	for (size_t i = 0;
	     i < sizeof(geometry->markers) / sizeof(geometry->markers[0]); ++i)
		if (geometry->markers[i].output_x == x &&
		    geometry->markers[i].output_y == y)
			return geometry->markers[i].rgb565;
	return 0;
}

static void compare_page(const struct geometry *geometry, const uint8_t *page,
			 size_t stride)
{
	for (unsigned int y = 0; y < geometry->height; ++y) {
		for (unsigned int x = 0; x < geometry->width; ++x) {
			uint16_t pixel;
			uint16_t expected = expected_pixel(geometry, x, y);

			memcpy(&pixel, page + y * stride + x * sizeof(pixel),
			       sizeof(pixel));
			if (pixel != expected) {
				fprintf(stderr,
					"%s (%u,%u): actual=%04x expected=%04x\n",
					geometry->name, x, y, pixel, expected);
				exit(EXIT_FAILURE);
			}
		}
		for (size_t x = geometry->width * sizeof(uint16_t); x < stride;
		     ++x)
			if (page[y * stride + x] != 0x5a) {
				fprintf(stderr, "%s row padding changed\n",
					geometry->name);
				exit(EXIT_FAILURE);
			}
	}
}

static void run_geometry(int drm, enum fplinux_gpu_api api,
			 const struct geometry *geometry)
{
	struct fplinux_drm_session session;
	struct fplinux_gpu_output *output;
	struct fplinux_gpu *gpu;
	PFNGLGENTEXTURESPROC gen_textures;
	PFNGLBINDTEXTUREPROC bind_texture;
	PFNGLTEXPARAMETERIPROC tex_parameter;
	PFNGLTEXIMAGE2DPROC tex_image;
	PFNGLDELETETEXTURESPROC delete_textures;
	PFNGLENABLEPROC enable;
	void (*alpha_func)(GLenum function, GLfloat reference);
	GLuint texture = 0;
	uint8_t *rgba;
	uint8_t *snapshot;
	char error[256];

	if (!create_buffer(&session, drm, geometry))
		fail("create real two-page buffer");
	output = fplinux_gpu_output_open(&session, api, geometry->source_width,
					 geometry->source_height,
					 geometry->clockwise, error,
					 sizeof(error));
	if (!output) {
		fprintf(stderr, "output open %s: %s\n", geometry->name, error);
		destroy_buffer(&session);
		exit(EXIT_FAILURE);
	}
	gpu = fplinux_gpu_output_gpu(output);
#define PROC(variable, name)                                \
	do {                                                \
		variable = fplinux_gpu_get_proc(gpu, name); \
		if (!variable)                              \
			fail(name);                         \
	} while (0)
	PROC(gen_textures, "glGenTextures");
	PROC(bind_texture, "glBindTexture");
	PROC(tex_parameter, "glTexParameteri");
	PROC(tex_image, "glTexImage2D");
	PROC(delete_textures, "glDeleteTextures");
	if (api == FPLINUX_GPU_OPENGL2) {
		PROC(enable, "glEnable");
		PROC(alpha_func, "glAlphaFunc");
		alpha_func(GL_GREATER, 0.666f);
		enable(GL_ALPHA_TEST);
	}
#undef PROC
	rgba = calloc((size_t)geometry->source_width * geometry->source_height,
		      4);
	snapshot = malloc(session.size);
	if (!rgba || !snapshot)
		fail("allocate peer-owned patterns");
	for (size_t i = 0;
	     i < sizeof(geometry->markers) / sizeof(geometry->markers[0]);
	     ++i) {
		const struct marker *marker = &geometry->markers[i];

		memcpy(rgba + ((size_t)marker->source_y *
				       geometry->source_width +
			       marker->source_x) *
				       4,
		       marker->rgba, sizeof(marker->rgba));
	}
	if (geometry->source_height == 256) {
		static const uint8_t decoys[][4] = {
			{ 0, 255, 0, 255 },
			{ 255, 255, 255, 255 },
			{ 255, 0, 255, 255 },
		};

		memcpy(rgba, decoys[0], 4);
		memcpy(rgba + 4, decoys[1], 4);
		memcpy(rgba + ((size_t)geometry->source_width + 1) * 4,
		       decoys[2], 4);
	}
	gen_textures(1, &texture);
	bind_texture(GL_TEXTURE_2D, texture);
	tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	tex_image(GL_TEXTURE_2D, 0, GL_RGBA8, geometry->source_width,
		  geometry->source_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	for (unsigned int round = 0; round < 4; ++round) {
		unsigned int old_page;
		unsigned int page;
		void *pixels;
		size_t stride;

		/* Initialize before submissions, within a test-owned DMA-BUF RW access. */
		if (!round) {
			int fd;
			struct dma_buf_sync sync = {
				.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW
			};

			if (drmPrimeHandleToFD(drm, session.handle,
					       DRM_CLOEXEC | DRM_RDWR,
					       &fd) < 0 ||
			    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
				fail("begin initial peer BO access");
			memset(session.mapping, 0x5a, session.size);
			memcpy(snapshot, session.mapping, session.size);
			sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
			if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 ||
			    close(fd) < 0)
				fail("end initial peer BO access");
		}
		if (!fplinux_gpu_output_render(output, texture) ||
		    !fplinux_gpu_output_begin_cpu(output, &pixels, &stride)) {
			fprintf(stderr, "render/access %s: %s\n",
				geometry->name,
				fplinux_gpu_output_error(output));
			exit(EXIT_FAILURE);
		}
		old_page = session.shown_page;
		page = 1 - old_page;
		if (pixels != session.mapping + page * session.page_bytes ||
		    stride != session.stride)
			fail("selected original page metadata");
		compare_page(geometry, pixels, stride);
		if (memcmp(snapshot + old_page * session.page_bytes,
			   session.mapping + old_page * session.page_bytes,
			   session.page_bytes) ||
		    memcmp(snapshot + session.page_bytes * 2,
			   session.mapping + session.page_bytes * 2,
			   session.size - session.page_bytes * 2))
			fail("other page or trailing BO guard changed");
		memcpy(snapshot, session.mapping, session.size);
		if (!fplinux_gpu_output_end_cpu(output) ||
		    !fplinux_gpu_output_present(output)) {
			fprintf(stderr, "end/present %s: %s\n", geometry->name,
				fplinux_gpu_output_error(output));
			exit(EXIT_FAILURE);
		}
		if (session.shown_page != old_page ||
		    presentation.session != &session ||
		    presentation.page != page)
			fail("presentation completed before wait");
	}
	printf("PASS %s %s pitch=%zu page_bytes=%zu size=%zu rounds=4\n",
	       api == FPLINUX_GPU_GLES3 ? "GLES3" : "OpenGL", geometry->name,
	       session.stride, session.page_bytes, session.size);
	delete_textures(1, &texture);
	if (!fplinux_gpu_output_close(output))
		fail("close GPU output");
	if (presentation.session || session.shown_page != 0)
		fail("final presentation was not completed at close");
	free(snapshot);
	free(rgba);
	destroy_buffer(&session);
}

int main(int argc, char **argv)
{
	int drm;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/dri/cardN\n", argv[0]);
		return 2;
	}
	drm = open(argv[1], O_RDWR | O_CLOEXEC);
	if (drm < 0)
		fail("open requested display node");
	for (unsigned int api = 0; api < 2; ++api)
		for (size_t i = 0;
		     i < sizeof(geometries) / sizeof(geometries[0]); ++i)
			run_geometry(drm,
				     api == 0 ? FPLINUX_GPU_GLES3 :
						FPLINUX_GPU_OPENGL2,
				     &geometries[i]);
	if (close(drm) < 0)
		fail("close requested display node");
	puts("PASS real GPU output geometry matrix; presenter controlled, no panel claim");
	return EXIT_SUCCESS;
}
