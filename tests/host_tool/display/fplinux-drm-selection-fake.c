/* SPDX-License-Identifier: GPL-2.0-only */
/* Synthetic DRM query responses; the linked session still owns selection. */
#define _GNU_SOURCE
#include "fplinux-drm-selection-fake.h"

#include <drm_fourcc.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static const char *scenario;
static char *first_nodes[] = { "/dev/dri/card0", NULL, NULL };
static char *second_nodes[] = { "/dev/dri/card1", NULL, NULL };
static drmDevice devices[] = {
	{ .nodes = first_nodes, .available_nodes = 1 << DRM_NODE_PRIMARY },
	{ .nodes = second_nodes, .available_nodes = 1 << DRM_NODE_PRIMARY },
};
static int probe_fds[] = { -1, -1 };
static int opened_card = -1;
static int discovery_calls;
static int master_calls;
static bool connector_probed;
static uint32_t connector_id = 11;
static uint32_t encoder_id = 12;
static uint32_t crtc_id = 13;
static uint32_t plane_id = 14;
static uint32_t property_id = 15;
static uint64_t plane_type = DRM_PLANE_TYPE_PRIMARY;
static uint32_t formats[] = { DRM_FORMAT_RGB565, DRM_FORMAT_NV16 };
static drmModeModeInfo mode = { .hdisplay = 240, .vdisplay = 320 };
static drmModeRes resources = {
	.count_connectors = 1,
	.connectors = &connector_id,
	.count_crtcs = 1,
	.crtcs = &crtc_id,
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
	.count_formats = 2,
	.formats = formats,
};
static drmModeObjectProperties properties = {
	.count_props = 1,
	.props = &property_id,
	.prop_values = &plane_type,
};
static drmModePropertyRes property = { .prop_id = 15, .name = "type" };

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);

bool fplinux_drm_selection_setup(const char *name)
{
	static const char *const names[] = {
		"gpu-panel",	    "panel-card7",  "ambiguous",
		"no-panel",	    "explicit-gpu", "explicit-panel",
		"explicit-missing", "nv16-panel",   "nv16-unsupported",
		"probe-error",	    "cold-panel",
	};

	for (size_t index = 0; index < sizeof(names) / sizeof(names[0]);
	     ++index)
		if (!strcmp(name, names[index])) {
			scenario = name;
			if (!strcmp(name, "panel-card7"))
				second_nodes[0] = "/dev/dri/card7";
			if (!strcmp(name, "nv16-unsupported"))
				plane.count_formats = 1;
			return true;
		}
	return false;
}

const char *fplinux_drm_selection_path(void)
{
	if (!strcmp(scenario, "explicit-gpu"))
		return "/dev/dri/card0";
	if (!strcmp(scenario, "explicit-panel"))
		return "/dev/dri/card1";
	if (!strcmp(scenario, "explicit-missing"))
		return "/dev/dri/card8";
	return NULL;
}

uint32_t fplinux_drm_selection_format(void)
{
	return !strncmp(scenario, "nv16-", 5) ? DRM_FORMAT_NV16 :
						DRM_FORMAT_RGB565;
}

bool fplinux_drm_selection_open(const char *path, int flags, int *result)
{
	int card;

	if (!strcmp(path, "/dev/dri/card8")) {
		*result = -1;
		errno = ENOENT;
		return true;
	}
	if (!strcmp(path, first_nodes[0]))
		card = 0;
	else if (!strcmp(path, second_nodes[0]))
		card = 1;
	else
		return false;
	if ((flags & O_ACCMODE) == O_RDONLY) {
		probe_fds[card] = __real_open("/dev/null", flags);
		*result = probe_fds[card];
	} else {
		/* Stop before real display writes, after recording the chosen node. */
		opened_card = card;
		*result = -1;
		errno = ENOSPC;
	}
	return true;
}

int __wrap_close(int fd)
{
	for (size_t index = 0; index < 2; ++index)
		if (probe_fds[index] == fd)
			probe_fds[index] = -1;
	return __real_close(fd);
}

int __wrap_drmGetDevices2(uint32_t flags, drmDevicePtr output[], int maximum)
{
	int count = !strcmp(scenario, "panel-card7") ? 1 : 2;

	(void)flags;
	++discovery_calls;
	if (!output)
		return count;
	if (maximum < count)
		return -EINVAL;
	output[0] = count == 1 ? &devices[1] : &devices[0];
	if (count == 2)
		output[1] = &devices[1];
	return count;
}

void __wrap_drmFreeDevices(drmDevicePtr output[], int count)
{
	(void)output;
	(void)count;
}

int __wrap_drmSetMaster(int fd)
{
	(void)fd;
	++master_calls;
	errno = EBUSY;
	return -1;
}

int __wrap_drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
	assert(capability == 3 && value == 1);
	if (fd == probe_fds[0] && strcmp(scenario, "ambiguous")) {
		errno = EOPNOTSUPP;
		return -1;
	}
	return 0;
}

drmModeResPtr __wrap_drmModeGetResources(int fd)
{
	(void)fd;
	if (!strcmp(scenario, "probe-error")) {
		errno = EIO;
		return NULL;
	}
	return &resources;
}

drmModeConnectorPtr __wrap_drmModeGetConnectorCurrent(int fd, uint32_t id)
{
	bool cold = !strcmp(scenario, "cold-panel") && !connector_probed;

	(void)fd;
	(void)id;
	connector.connection = DRM_MODE_CONNECTED;
	if (cold)
		connector.connection = DRM_MODE_UNKNOWNCONNECTION;
	else if (!strcmp(scenario, "no-panel"))
		connector.connection = DRM_MODE_DISCONNECTED;
	connector.count_modes = cold ? 0 : 1;
	return &connector;
}

drmModeConnectorPtr __wrap_drmModeGetConnector(int fd, uint32_t id)
{
	connector_probed = true;
	return __wrap_drmModeGetConnectorCurrent(fd, id);
}

drmModeEncoderPtr __wrap_drmModeGetEncoder(int fd, uint32_t id)
{
	(void)fd;
	(void)id;
	return &encoder;
}

drmModePlaneResPtr __wrap_drmModeGetPlaneResources(int fd)
{
	(void)fd;
	return &planes;
}

drmModePlanePtr __wrap_drmModeGetPlane(int fd, uint32_t id)
{
	(void)fd;
	(void)id;
	return &plane;
}

drmModeObjectPropertiesPtr
__wrap_drmModeObjectGetProperties(int fd, uint32_t id, uint32_t type)
{
	(void)fd;
	(void)id;
	(void)type;
	return &properties;
}

drmModePropertyPtr __wrap_drmModeGetProperty(int fd, uint32_t id)
{
	(void)fd;
	(void)id;
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

bool fplinux_drm_selection_check(int error)
{
	bool explicit = !strncmp(scenario, "explicit-", 9);
	int expected_error = ENOSPC;
	int expected_card = 1;

	if (!strcmp(scenario, "ambiguous")) {
		expected_error = ENOTUNIQ;
		expected_card = -1;
	} else if (!strcmp(scenario, "no-panel") ||
		   !strcmp(scenario, "nv16-unsupported") ||
		   !strcmp(scenario, "probe-error")) {
		expected_error = ENODEV;
		expected_card = -1;
	} else if (!strcmp(scenario, "explicit-gpu")) {
		expected_card = 0;
	} else if (!strcmp(scenario, "explicit-missing")) {
		expected_error = ENOENT;
		expected_card = -1;
	}
	if (error == expected_error && opened_card == expected_card &&
	    (explicit ? discovery_calls == 0 : discovery_calls > 0) &&
	    master_calls == 0 && probe_fds[0] == -1 && probe_fds[1] == -1)
		return true;
	fprintf(stderr,
		"selection %s: errno=%d card=%d discovery=%d master=%d probes=%d,%d\n",
		scenario, error, opened_card, discovery_calls, master_calls,
		probe_fds[0], probe_fds[1]);
	return false;
}
