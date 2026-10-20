/* SPDX-License-Identifier: GPL-2.0-only */
/* Real session acquisition and signal dispatch with synthetic VT/DRM devices. */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>

enum master_owner {
	MASTER_NONE,
	MASTER_APPLICATION,
	MASTER_TERMINAL,
};

static char console_path[] = "/tmp/fplinux-drm-handoff-XXXXXX";
static const char *guardian_path;
static int console = -1;
static struct vt_mode console_mode;
static int pending_vt = -1;
static enum master_owner master;
static bool release_during_acquire;
static bool initial_acknowledged;
static bool input_active;
static uint32_t framebuffer;
static uint32_t connector_id = 11;
static uint32_t crtc_id = 12;
static uint32_t encoder_id = 13;
static uint32_t plane_id = 14;
static uint32_t property_id = 15;
static uint32_t format = DRM_FORMAT_RGB565;
static uint64_t plane_type = DRM_PLANE_TYPE_PRIMARY;
static drmModeModeInfo mode = { .hdisplay = 240, .vdisplay = 320 };
static drmModeRes resources = {
	.count_crtcs = 1,
	.crtcs = &crtc_id,
	.count_connectors = 1,
	.connectors = &connector_id,
};
static drmModeConnector connector = {
	.connector_id = 11,
	.connection = DRM_MODE_CONNECTED,
	.count_modes = 1,
	.modes = &mode,
	.count_encoders = 1,
	.encoders = &encoder_id,
};
static drmModeEncoder encoder = { .possible_crtcs = 1 };
static drmModePlaneRes planes = { .count_planes = 1, .planes = &plane_id };
static drmModePlane plane = {
	.plane_id = 14,
	.possible_crtcs = 1,
	.count_formats = 1,
	.formats = &format,
};
static drmModeObjectProperties properties = {
	.count_props = 1,
	.props = &property_id,
	.prop_values = &plane_type,
};
static drmModePropertyRes property = { .prop_id = 15, .name = "type" };

int __real_open(const char *path, int flags, ...);
int __real_execv(const char *path, char *const arguments[]);

int __wrap_open(const char *path, int flags, ...)
{
	if (!strcmp(path, "/dev/tty5"))
		path = console_path;
	else if (!strcmp(path, "test-drm"))
		path = "/dev/zero";
	return __real_open(path, flags);
}

int __wrap_execv(const char *path, char *const arguments[])
{
	(void)path;
	return __real_execv(guardian_path, arguments);
}

static int active_vt(void)
{
	int active;

	assert(pread(console, &active, sizeof(active), 0) == sizeof(active));
	return active;
}

static void set_active_vt(int active)
{
	assert(pwrite(console, &active, sizeof(active), 0) == sizeof(active));
}

static void request_release(void)
{
	pending_vt = 1;
	assert(kill(getpid(), console_mode.relsig) == 0);
}

int __wrap_ioctl(int descriptor, unsigned long request, ...)
{
	va_list arguments;

	(void)descriptor;
	va_start(arguments, request);
	switch (request) {
	case VT_GETSTATE: {
		struct vt_stat *state = va_arg(arguments, struct vt_stat *);

		memset(state, 0, sizeof(*state));
		state->v_active = active_vt();
		break;
	}
	case VT_OPENQRY:
		*va_arg(arguments, int *) = 5;
		break;
	case VT_SETMODE:
		console_mode = *va_arg(arguments, struct vt_mode *);
		break;
	case VT_ACTIVATE: {
		int selected = va_arg(arguments, int);

		if (selected != active_vt()) {
			set_active_vt(selected);
			if (selected == 5 && console_mode.mode == VT_PROCESS) {
				master = MASTER_NONE;
				assert(kill(getpid(), console_mode.acqsig) ==
				       0);
			}
		}
		break;
	}
	case VT_WAITACTIVE:
		assert(va_arg(arguments, int) == active_vt());
		break;
	case VT_RELDISP: {
		int response = va_arg(arguments, int);

		if (response == VT_ACKACQ) {
			if (!initial_acknowledged) {
				initial_acknowledged = true;
				if (release_during_acquire)
					request_release();
			}
		} else {
			assert(response == 1 && pending_vt == 1);
			assert(master == MASTER_NONE);
			set_active_vt(pending_vt);
			pending_vt = -1;
			/* The terminal acquires its display before this app runs again. */
			master = MASTER_TERMINAL;
		}
		break;
	}
	case KDSETMODE:
	case VT_DISALLOCATE:
		break;
	default:
		va_end(arguments);
		errno = ENOTTY;
		return -1;
	}
	va_end(arguments);
	return 0;
}

int __wrap_drmSetMaster(int descriptor)
{
	(void)descriptor;
	if (master == MASTER_TERMINAL) {
		errno = EBUSY;
		return -1;
	}
	master = MASTER_APPLICATION;
	return 0;
}

int __wrap_drmDropMaster(int descriptor)
{
	(void)descriptor;
	assert(master == MASTER_APPLICATION);
	master = MASTER_NONE;
	return 0;
}

int __wrap_drmSetClientCap(int descriptor, uint64_t capability, uint64_t value)
{
	(void)descriptor;
	assert(capability == DRM_CLIENT_CAP_ATOMIC && value == 1);
	return 0;
}

drmModeResPtr __wrap_drmModeGetResources(int descriptor)
{
	(void)descriptor;
	return &resources;
}

drmModeConnectorPtr __wrap_drmModeGetConnector(int descriptor, uint32_t id)
{
	(void)descriptor;
	assert(id == 11);
	return &connector;
}

drmModeEncoderPtr __wrap_drmModeGetEncoder(int descriptor, uint32_t id)
{
	(void)descriptor;
	assert(id == 13);
	return &encoder;
}

drmModePlaneResPtr __wrap_drmModeGetPlaneResources(int descriptor)
{
	(void)descriptor;
	return &planes;
}

drmModePlanePtr __wrap_drmModeGetPlane(int descriptor, uint32_t id)
{
	(void)descriptor;
	assert(id == 14);
	return &plane;
}

drmModeObjectPropertiesPtr
__wrap_drmModeObjectGetProperties(int descriptor, uint32_t id, uint32_t type)
{
	(void)descriptor;
	assert(id == 14 && type == DRM_MODE_OBJECT_PLANE);
	return &properties;
}

drmModePropertyPtr __wrap_drmModeGetProperty(int descriptor, uint32_t id)
{
	(void)descriptor;
	assert(id == 15);
	return &property;
}

void __wrap_drmModeFreeResources(drmModeResPtr object)
{
	(void)object;
}

void __wrap_drmModeFreeConnector(drmModeConnectorPtr object)
{
	(void)object;
}

void __wrap_drmModeFreeEncoder(drmModeEncoderPtr object)
{
	(void)object;
}

void __wrap_drmModeFreePlaneResources(drmModePlaneResPtr object)
{
	(void)object;
}

void __wrap_drmModeFreePlane(drmModePlanePtr object)
{
	(void)object;
}

void __wrap_drmModeFreeObjectProperties(drmModeObjectPropertiesPtr object)
{
	(void)object;
}

void __wrap_drmModeFreeProperty(drmModePropertyPtr object)
{
	(void)object;
}

int __wrap_drmModeCreatePropertyBlob(int descriptor, const void *data,
				     size_t length, uint32_t *id)
{
	(void)descriptor;
	(void)data;
	(void)length;
	*id = 16;
	return 0;
}

int __wrap_drmModeDestroyPropertyBlob(int descriptor, uint32_t id)
{
	(void)descriptor;
	assert(id == 16);
	return 0;
}

int __wrap_drmIoctl(int descriptor, unsigned long request, void *argument)
{
	(void)descriptor;
	switch (request) {
	case DRM_IOCTL_MODE_CREATE_DUMB: {
		struct drm_mode_create_dumb *buffer = argument;

		buffer->handle = 1;
		buffer->pitch = 480;
		buffer->size = 307200;
		break;
	}
	case DRM_IOCTL_MODE_MAP_DUMB:
		((struct drm_mode_map_dumb *)argument)->offset = 0;
		break;
	case DRM_IOCTL_MODE_DESTROY_DUMB:
		break;
	default:
		errno = ENOTTY;
		return -1;
	}
	return 0;
}

int __wrap_drmModeAddFB2(int descriptor, uint32_t width, uint32_t height,
			 uint32_t pixel_format, const uint32_t handles[4],
			 const uint32_t pitches[4], const uint32_t offsets[4],
			 uint32_t *id, uint32_t flags)
{
	(void)descriptor;
	(void)width;
	(void)height;
	(void)pixel_format;
	(void)handles;
	(void)pitches;
	(void)offsets;
	(void)flags;
	*id = ++framebuffer;
	return 0;
}

int __wrap_drmModeRmFB(int descriptor, uint32_t id)
{
	(void)descriptor;
	assert(id == 1 || id == 2);
	return 0;
}

static bool change_focus(struct fplinux_drm_session *session, bool active,
			 void *data)
{
	(void)session;
	(void)data;
	input_active = active;
	return true;
}

static bool exercise_handoff(struct fplinux_drm_session *session)
{
	if (!fplinux_drm_session_set_active_handler(session, change_focus,
						    NULL))
		return false;
	if (!release_during_acquire)
		request_release();
	/* Initialization can finish after a switch request, before the first pump. */
	if (!fplinux_drm_session_dispatch(session))
		return false;
	if (session->active || input_active || active_vt() != 1 ||
	    master != MASTER_TERMINAL) {
		errno = EIO;
		return false;
	}
	if (ioctl(session->control, VT_ACTIVATE, 5) < 0 ||
	    !fplinux_drm_session_dispatch(session))
		return false;
	return session->active && input_active && active_vt() == 5 &&
	       master == MASTER_APPLICATION;
}

int main(int argc, char **argv)
{
	struct fplinux_drm_session session;
	char error[160];
	const char *step = "console setup";
	bool opened = false;
	bool ok = false;
	int initial_vt;

	if (argc != 3)
		return EXIT_FAILURE;
	guardian_path = argv[1];
	initial_vt = !strcmp(argv[2], "already-active") ? 5 : 1;
	release_during_acquire = !strcmp(argv[2], "pending-release");
	console = mkstemp(console_path);
	if (console < 0)
		goto cleanup;
	set_active_vt(initial_vt);
	step = "open session";
	if (!fplinux_drm_session_open(&session, "test-drm", console_path,
				      DRM_FORMAT_RGB565, error,
				      sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		goto cleanup;
	}
	opened = true;
	step = "release and reacquire";
	if (!exercise_handoff(&session))
		goto cleanup;
	step = "close session";
	opened = false;
	if (!fplinux_drm_session_close(&session))
		goto cleanup;
	step = "restore original VT";
	ok = active_vt() == initial_vt && !input_active;
cleanup:
	if (!ok)
		fprintf(stderr, "DRM handoff %s failed at %s: %s\n", argv[2],
			step, strerror(errno));
	if (opened)
		fplinux_drm_session_close(&session);
	if (console >= 0)
		close(console);
	unlink(console_path);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
