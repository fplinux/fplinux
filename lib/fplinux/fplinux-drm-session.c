/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/sync_file.h>
#include <linux/vt.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>

#define FPLINUX_DRM_VT_GUARDIAN_PATH "/usr/libexec/fplinux/vt-guardian"

static void initialize(struct fplinux_drm_session *session)
{
	memset(session, 0, sizeof(*session));
	session->drm = -1;
	session->tty = -1;
	session->control = -1;
	session->signal_fd = -1;
	session->guardian_fd = -1;
	session->present_fence = -1;
}

static uint32_t property_id(int fd, uint32_t object, uint32_t type,
			    const char *name, uint64_t *value)
{
	drmModeObjectProperties *properties =
		drmModeObjectGetProperties(fd, object, type);
	uint32_t result = 0;
	uint32_t index;

	if (!properties)
		return 0;
	for (index = 0; index < properties->count_props; ++index) {
		drmModePropertyRes *property =
			drmModeGetProperty(fd, properties->props[index]);

		if (property && !strcmp(property->name, name)) {
			result = property->prop_id;
			if (value)
				*value = properties->prop_values[index];
		}
		if (property)
			drmModeFreeProperty(property);
		if (result)
			break;
	}
	drmModeFreeObjectProperties(properties);
	if (!result)
		errno = ENOTSUP;
	return result;
}

static bool add_property(struct fplinux_drm_session *session,
			 drmModeAtomicReq *request, uint32_t object,
			 uint32_t type, uint32_t *property, const char *name,
			 uint64_t value)
{
	/* IDs remain valid for the selected objects until the DRM fd closes. */
	if (!*property)
		*property = property_id(session->drm, object, type, name, NULL);

	return *property &&
	       drmModeAtomicAddProperty(request, object, *property, value) >= 0;
}

static bool find_display(struct fplinux_drm_session *session, uint32_t format)
{
	drmModeRes *resources = drmModeGetResources(session->drm);
	drmModePlaneRes *planes = NULL;
	unsigned int crtc_index = 0;
	int index;
	bool ok = false;

	if (!resources)
		return false;
	for (index = 0; index < resources->count_connectors; ++index) {
		/* A cold connector may have no cached modes to enumerate. */
		drmModeConnector *connector = drmModeGetConnector(
			session->drm, resources->connectors[index]);

		if (!connector)
			goto done;
		if (connector->connection == DRM_MODE_CONNECTED &&
		    connector->count_modes > 0) {
			if (session->connector) {
				drmModeFreeConnector(connector);
				errno = ENOTUNIQ;
				goto done;
			}
			session->connector = connector->connector_id;
			session->mode = connector->modes[0];
			for (int e = 0; e < connector->count_encoders; ++e) {
				drmModeEncoder *encoder = drmModeGetEncoder(
					session->drm, connector->encoders[e]);

				if (!encoder)
					continue;
				for (int c = 0;
				     c < resources->count_crtcs && c < 32; ++c)
					if (encoder->possible_crtcs &
					    (1U << c)) {
						session->crtc =
							resources->crtcs[c];
						crtc_index = (unsigned int)c;
						break;
					}
				drmModeFreeEncoder(encoder);
				if (session->crtc)
					break;
			}
		}
		drmModeFreeConnector(connector);
	}
	if (!session->connector || !session->crtc) {
		errno = ENODEV;
		goto done;
	}
	planes = drmModeGetPlaneResources(session->drm);
	if (!planes)
		goto done;
	for (uint32_t p = 0; p < planes->count_planes; ++p) {
		drmModePlane *plane =
			drmModeGetPlane(session->drm, planes->planes[p]);
		uint64_t type = 0;

		if (!plane)
			goto done;
		if ((plane->possible_crtcs & (1U << crtc_index)) &&
		    property_id(session->drm, plane->plane_id,
				DRM_MODE_OBJECT_PLANE, "type", &type) &&
		    type == DRM_PLANE_TYPE_PRIMARY) {
			for (uint32_t f = 0; f < plane->count_formats; ++f)
				if (plane->formats[f] == format)
					session->plane = plane->plane_id;
		}
		drmModeFreePlane(plane);
		if (session->plane)
			break;
	}
	if (!session->plane) {
		errno = ENOTSUP;
		goto done;
	}
	session->width = session->mode.hdisplay;
	session->height = session->mode.vdisplay;
	if (!session->width || !session->height) {
		errno = EINVAL;
		goto done;
	}
	ok = true;
done:
	if (planes)
		drmModeFreePlaneResources(planes);
	drmModeFreeResources(resources);
	return ok;
}

static char *find_display_path(uint32_t format)
{
	drmDevicePtr *devices;
	char *path = NULL;
	int count = drmGetDevices2(0, NULL, 0);
	int saved_errno = ENODEV;

	if (count <= 0) {
		errno = count < 0 ? -count : ENODEV;
		return NULL;
	}
	devices = calloc((size_t)count, sizeof(*devices));
	if (!devices)
		return NULL;
	count = drmGetDevices2(0, devices, count);
	if (count < 0) {
		saved_errno = -count;
		goto done;
	}
	for (int index = 0; index < count; ++index) {
		struct fplinux_drm_session candidate;
		bool usable;
		int probe_errno;

		if (!(devices[index]->available_nodes &
		      (1 << DRM_NODE_PRIMARY)))
			continue;
		initialize(&candidate);
		candidate.drm = open(devices[index]->nodes[DRM_NODE_PRIMARY],
				     O_RDONLY | O_CLOEXEC);
		if (candidate.drm < 0)
			continue;
		/* Enumeration never claims a VT or requests DRM master. */
		usable = drmSetClientCap(candidate.drm, DRM_CLIENT_CAP_ATOMIC,
					 1) == 0 &&
			 find_display(&candidate, format);
		probe_errno = errno;
		close(candidate.drm);
		if (!usable && probe_errno != ENOTUNIQ)
			continue;
		if (!usable || path) {
			saved_errno = ENOTUNIQ;
			free(path);
			path = NULL;
			goto done;
		}
		path = strdup(devices[index]->nodes[DRM_NODE_PRIMARY]);
		if (!path) {
			saved_errno = errno;
			goto done;
		}
	}
done:
	if (count > 0)
		drmFreeDevices(devices, count);
	free(devices);
	errno = saved_errno;
	return path;
}

static bool create_buffers(struct fplinux_drm_session *session, uint32_t format)
{
	bool nv16 = format == DRM_FORMAT_NV16;
	struct drm_mode_create_dumb create = {
		.width = session->width,
		.height = session->height * (nv16 ? 4U : 2U),
		.bpp = nv16 ? 8U : 16U,
	};
	struct drm_mode_map_dumb map = { 0 };
	uint32_t handles[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t offsets[4] = { 0 };

	if (drmIoctl(session->drm, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
		return false;
	session->handle = create.handle;
	if (create.size > SIZE_MAX ||
	    create.pitch > SIZE_MAX / session->height / (nv16 ? 2U : 1U)) {
		errno = EOVERFLOW;
		return false;
	}
	session->size = create.size;
	session->stride = create.pitch;
	session->page_bytes =
		(size_t)create.pitch * session->height * (nv16 ? 2U : 1U);
	if (session->page_bytes > UINT32_MAX / 2U ||
	    create.size < session->page_bytes * 2U ||
	    create.pitch != session->width * (nv16 ? 1U : 2U)) {
		errno = EOVERFLOW;
		return false;
	}
	handles[0] = create.handle;
	pitches[0] = create.pitch;
	if (nv16) {
		handles[1] = create.handle;
		pitches[1] = create.pitch;
	}
	for (unsigned int page = 0; page < 2; ++page) {
		offsets[0] = (uint32_t)(page * session->page_bytes);
		if (nv16)
			offsets[1] =
				offsets[0] + create.pitch * session->height;
		if (drmModeAddFB2(session->drm, session->width, session->height,
				  format, handles, pitches, offsets,
				  &session->framebuffers[page], 0) < 0)
			return false;
	}
	map.handle = create.handle;
	if (drmIoctl(session->drm, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
		return false;
	session->mapping = mmap(NULL, session->size, PROT_READ | PROT_WRITE,
				MAP_SHARED, session->drm, map.offset);
	if (session->mapping == MAP_FAILED) {
		session->mapping = NULL;
		return false;
	}
	session->pages = 2;
	session->format = format;
	return true;
}

static bool restore_vt(struct fplinux_drm_session *session)
{
	struct vt_mode mode = { .mode = VT_AUTO };
	struct vt_stat state;
	bool ok = true;

	if (ioctl(session->tty, VT_SETMODE, &mode) < 0 ||
	    ioctl(session->tty, KDSETMODE, KD_TEXT) < 0)
		ok = false;
	if (ioctl(session->control, VT_GETSTATE, &state) < 0)
		return false;
	if (state.v_active == session->vt) {
		if (ioctl(session->control, VT_ACTIVATE, session->previous_vt) <
			    0 ||
		    ioctl(session->control, VT_WAITACTIVE,
			  session->previous_vt) < 0)
			ok = false;
	}
	return ok;
}

static void close_inherited_fds(int channel, int process, int tty, int control)
{
	int keep[] = { channel, process, tty, control };
	unsigned int first = 0;

	for (unsigned int i = 0; i < 4; ++i)
		for (unsigned int j = i + 1; j < 4; ++j)
			if (keep[j] < keep[i]) {
				int temporary = keep[i];

				keep[i] = keep[j];
				keep[j] = temporary;
			}
	for (unsigned int i = 0; i < 4; ++i) {
		if ((unsigned int)keep[i] > first)
			syscall(SYS_close_range, first,
				(unsigned int)keep[i] - 1U, 0);
		first = (unsigned int)keep[i] + 1U;
	}
	syscall(SYS_close_range, first, UINT_MAX, 0);
}

static bool start_guardian(struct fplinux_drm_session *session)
{
	int channel[2];
	int process = (int)syscall(SYS_pidfd_open, getpid(), 0);
	int values[6];
	char fields[6][16];
	char *arguments[8] = { "fplinux-vtguard" };
	pid_t child;

	if (process < 0)
		return false;
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) < 0) {
		close(process);
		return false;
	}
	values[0] = channel[0];
	values[1] = process;
	values[2] = session->tty;
	values[3] = session->control;
	values[4] = session->vt;
	values[5] = session->previous_vt;
	for (unsigned int i = 0; i < 6; ++i) {
		snprintf(fields[i], sizeof(fields[i]), "%d", values[i]);
		arguments[i + 1] = fields[i];
	}
	child = fork();
	if (child == 0) {
		setsid();
		signal(SIGHUP, SIG_IGN);
		signal(SIGINT, SIG_IGN);
		signal(SIGQUIT, SIG_IGN);
		signal(SIGTERM, SIG_IGN);
		for (unsigned int i = 0; i < 4; ++i)
			if (fcntl(values[i], F_SETFD, 0) < 0)
				_exit(1);
		close_inherited_fds(channel[0], process, session->tty,
				    session->control);
		/* A separate executable identity survives name-based app termination. */
		execv(FPLINUX_DRM_VT_GUARDIAN_PATH, arguments);
		_exit(1);
	}
	close(channel[0]);
	close(process);
	if (child < 0) {
		close(channel[1]);
		return false;
	}
	session->guardian = child;
	session->guardian_fd = channel[1];
	{
		char ready;
		ssize_t count;

		do {
			count = read(channel[1], &ready, 1);
		} while (count < 0 && errno == EINTR);
		if (count != 1 || ready != 'R') {
			int status;

			kill(child, SIGKILL);
			close(session->guardian_fd);
			session->guardian_fd = -1;
			do {
				status = waitpid(child, NULL, 0);
			} while (status < 0 && errno == EINTR);
			session->guardian = 0;
			errno = EIO;
			return false;
		}
	}
	return true;
}

static bool acquire_vt(struct fplinux_drm_session *session, const char *path)
{
	struct vt_stat state;
	struct vt_mode mode = {
		.mode = VT_PROCESS,
		.relsig = SIGRTMIN + 4,
		.acqsig = SIGRTMIN + 5,
	};
	const struct timespec no_wait = { 0 };
	sigset_t signals;
	char tty_path[32];
	int acquired;

	session->control =
		open(path ? path : "/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (session->control < 0 ||
	    ioctl(session->control, VT_GETSTATE, &state) < 0 ||
	    ioctl(session->control, VT_OPENQRY, &session->vt) < 0)
		return false;
	if (session->vt <= 0) {
		errno = EBUSY;
		return false;
	}
	session->previous_vt = state.v_active;
	snprintf(tty_path, sizeof(tty_path), "/dev/tty%d", session->vt);
	session->tty = open(tty_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (session->tty < 0)
		return false;
	sigemptyset(&signals);
	sigaddset(&signals, mode.relsig);
	sigaddset(&signals, mode.acqsig);
	if (sigprocmask(SIG_BLOCK, &signals, &session->previous_mask) < 0)
		return false;
	session->signals_blocked = true;
	session->signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
	if (session->signal_fd < 0 ||
	    ioctl(session->tty, VT_SETMODE, &mode) < 0)
		return false;
	session->vt_owned = true;
	if (!start_guardian(session) ||
	    ioctl(session->tty, KDSETMODE, KD_GRAPHICS) < 0 ||
	    ioctl(session->control, VT_ACTIVATE, session->vt) < 0 ||
	    ioctl(session->control, VT_WAITACTIVE, session->vt) < 0)
		return false;
	if (ioctl(session->tty, VT_RELDISP, VT_ACKACQ) < 0)
		return false;
	/* Do not let this completed acquisition overtake a later release. */
	sigdelset(&signals, mode.relsig);
	do {
		acquired = sigtimedwait(&signals, NULL, &no_wait);
	} while (acquired < 0 && errno == EINTR);
	/* An already active unused VT does not generate an acquire signal. */
	return acquired >= 0 || errno == EAGAIN;
}

bool fplinux_drm_session_open(struct fplinux_drm_session *session,
			      const char *drm_path, const char *tty_path,
			      uint32_t format, char *error, size_t error_size)
{
	char *automatic_path = NULL;
	int saved_errno;

	if (!session)
		return false;
	initialize(session);
	if (format != DRM_FORMAT_RGB565 && format != DRM_FORMAT_NV16) {
		errno = EINVAL;
		goto fail;
	}
	if (!drm_path) {
		automatic_path = find_display_path(format);
		if (!automatic_path)
			goto fail;
		drm_path = automatic_path;
	}
	if (!acquire_vt(session, tty_path))
		goto fail;
	session->drm = open(drm_path, O_RDWR | O_CLOEXEC);
	if (session->drm < 0 || drmSetMaster(session->drm) < 0 ||
	    drmSetClientCap(session->drm, DRM_CLIENT_CAP_ATOMIC, 1) < 0 ||
	    !find_display(session, format) ||
	    drmModeCreatePropertyBlob(session->drm, &session->mode,
				      sizeof(session->mode),
				      &session->mode_blob) < 0 ||
	    !create_buffers(session, format))
		goto fail;
	session->active = true;
	free(automatic_path);
	return true;
fail:
	saved_errno = errno;
	free(automatic_path);
	if (error && error_size)
		snprintf(error, error_size, "cannot open DRM display: %s",
			 strerror(saved_errno));
	fplinux_drm_session_close(session);
	errno = saved_errno;
	return false;
}

bool fplinux_drm_session_set_active_handler(struct fplinux_drm_session *session,
					    fplinux_drm_active_handler handler,
					    void *data)
{
	session->active_handler = handler;
	session->active_data = data;
	return !handler || handler(session, session->active, data);
}

int fplinux_drm_session_fd(const struct fplinux_drm_session *session)
{
	return session->signal_fd;
}

bool fplinux_drm_session_dispatch(struct fplinux_drm_session *session)
{
	struct signalfd_siginfo signal;
	ssize_t count;

	while ((count = read(session->signal_fd, &signal, sizeof(signal))) >
	       0) {
		if (count != sizeof(signal)) {
			errno = EIO;
			return false;
		}
		if (signal.ssi_signo == (uint32_t)(SIGRTMIN + 4)) {
			if (!fplinux_drm_session_wait_present(session))
				return false;
			if (session->active_handler &&
			    !session->active_handler(session, false,
						     session->active_data))
				return false;
			session->active = false;
			if (drmDropMaster(session->drm) < 0 ||
			    ioctl(session->tty, VT_RELDISP, 1) < 0)
				return false;
		} else if (signal.ssi_signo == (uint32_t)(SIGRTMIN + 5)) {
			if (!fplinux_drm_session_wait_present(session))
				return false;
			if (ioctl(session->tty, VT_RELDISP, VT_ACKACQ) < 0 ||
			    drmSetMaster(session->drm) < 0)
				return false;
			session->active = true;
			if (session->presented &&
			    !fplinux_drm_session_present(session,
							 session->shown_page))
				return false;
			if (session->active_handler &&
			    !session->active_handler(session, true,
						     session->active_data))
				return false;
		}
	}
	return count < 0 && (errno == EAGAIN || errno == EINTR);
}

bool fplinux_drm_session_wait_until(struct fplinux_drm_session *session,
				    const struct timespec *deadline)
{
	struct pollfd event = { .fd = session->signal_fd, .events = POLLIN };
	struct timespec remaining;
	struct timespec now;

	for (;;) {
		if (!fplinux_drm_session_dispatch(session) ||
		    clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return false;
		remaining.tv_sec = deadline->tv_sec - now.tv_sec;
		remaining.tv_nsec = deadline->tv_nsec - now.tv_nsec;
		if (remaining.tv_nsec < 0) {
			--remaining.tv_sec;
			remaining.tv_nsec += 1000000000L;
		}
		if (remaining.tv_sec < 0 ||
		    (remaining.tv_sec == 0 && remaining.tv_nsec == 0))
			return true;
		if (ppoll(&event, 1, &remaining, NULL) < 0)
			return false;
	}
}

static bool wait_fence(int fd)
{
	struct pollfd descriptor = { .fd = fd, .events = POLLIN };
	struct sync_file_info info = { 0 };
	int result;

	do {
		result = poll(&descriptor, 1, 3000);
	} while (result < 0 && errno == EINTR);
	if (result == 0) {
		errno = ETIMEDOUT;
		return false;
	}
	if (result < 0 || ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0)
		return false;
	if (info.status <= 0) {
		errno = info.status < 0 ? -info.status : EIO;
		return false;
	}
	return true;
}

static bool submit_present(struct fplinux_drm_session *session,
			   unsigned int page, uint32_t flags, int *fence)
{
	struct fplinux_drm_property_ids *properties;
	drmModeAtomicReq *request;
	bool ok;
	int saved_errno;

	if (!session || page >= session->pages) {
		errno = EINVAL;
		return false;
	}
	if (!session->active) {
		errno = EAGAIN;
		return false;
	}
	request = drmModeAtomicAlloc();
	if (!request)
		return false;
	properties = &session->property_ids;
	ok = add_property(session, request, session->connector,
			  DRM_MODE_OBJECT_CONNECTOR,
			  &properties->connector_crtc_id, "CRTC_ID",
			  session->crtc) &&
	     add_property(session, request, session->crtc, DRM_MODE_OBJECT_CRTC,
			  &properties->crtc_mode_id, "MODE_ID",
			  session->mode_blob) &&
	     add_property(session, request, session->crtc, DRM_MODE_OBJECT_CRTC,
			  &properties->crtc_active, "ACTIVE", 1) &&
	     add_property(session, request, session->crtc, DRM_MODE_OBJECT_CRTC,
			  &properties->crtc_out_fence_ptr, "OUT_FENCE_PTR",
			  (uintptr_t)fence) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_fb_id,
			  "FB_ID", session->framebuffers[page]) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_crtc_id,
			  "CRTC_ID", session->crtc) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_crtc_x,
			  "CRTC_X", 0) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_crtc_y,
			  "CRTC_Y", 0) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_crtc_w,
			  "CRTC_W", session->width) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_crtc_h,
			  "CRTC_H", session->height) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_src_x,
			  "SRC_X", 0) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_src_y,
			  "SRC_Y", 0) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_src_w,
			  "SRC_W", (uint64_t)session->width << 16) &&
	     add_property(session, request, session->plane,
			  DRM_MODE_OBJECT_PLANE, &properties->plane_src_h,
			  "SRC_H", (uint64_t)session->height << 16);
	if (ok)
		ok = drmModeAtomicCommit(session->drm, request, flags, NULL) ==
		     0;
	saved_errno = errno;
	drmModeAtomicFree(request);
	errno = saved_errno;
	return ok;
}

bool fplinux_drm_session_wait_present(struct fplinux_drm_session *session)
{
	bool ok;
	int saved_errno;

	if (!session) {
		errno = EINVAL;
		return false;
	}
	if (session->presentation_error) {
		errno = session->presentation_error;
		return false;
	}
	if (session->present_fence < 0)
		return true;
	ok = wait_fence(session->present_fence);
	saved_errno = errno;
	close(session->present_fence);
	session->present_fence = -1;
	if (!ok) {
		session->presentation_error = saved_errno;
		errno = saved_errno;
		return false;
	}
	session->shown_page = session->pending_page;
	session->presented = true;
	return true;
}

bool fplinux_drm_session_queue_present(struct fplinux_drm_session *session,
				       unsigned int page)
{
	int fence = -1;
	int saved_errno;
	bool ok;

	if (!session) {
		errno = EINVAL;
		return false;
	}
	if (session->presentation_error) {
		errno = session->presentation_error;
		return false;
	}
	if (session->present_fence >= 0) {
		errno = EBUSY;
		return false;
	}
	ok = submit_present(session, page,
			    DRM_MODE_ATOMIC_ALLOW_MODESET |
				    DRM_MODE_ATOMIC_NONBLOCK,
			    &fence);
	saved_errno = errno;
	if (!ok) {
		if (fence >= 0)
			close(fence);
		errno = saved_errno;
		return false;
	}
	if (fence < 0) {
		session->presentation_error = EIO;
		errno = EIO;
		return false;
	}
	session->present_fence = fence;
	session->pending_page = page;
	return true;
}

bool fplinux_drm_session_present(struct fplinux_drm_session *session,
				 unsigned int page)
{
	int fence = -1;
	bool ok;
	int saved_errno;

	if (!fplinux_drm_session_wait_present(session))
		return false;
	ok = submit_present(session, page, DRM_MODE_ATOMIC_ALLOW_MODESET,
			    &fence);
	if (ok) {
		if (fence < 0) {
			errno = EIO;
			ok = false;
		} else {
			ok = wait_fence(fence);
		}
	}
	saved_errno = errno;
	if (fence >= 0)
		close(fence);
	if (ok) {
		session->shown_page = page;
		session->presented = true;
	}
	errno = saved_errno;
	return ok;
}

bool fplinux_drm_session_acquire_buffer(struct fplinux_drm_session *session,
					int *fd)
{
	if (fd)
		*fd = -1;
	if (!session || !fd || session->drm < 0 || !session->handle ||
	    !session->mapping) {
		errno = EINVAL;
		return false;
	}
	if (session->buffer_acquired) {
		errno = EBUSY;
		return false;
	}
	if (!fplinux_drm_session_wait_present(session))
		return false;
	if (drmPrimeHandleToFD(session->drm, session->handle,
			       DRM_CLOEXEC | DRM_RDWR, fd) < 0)
		return false;
	session->buffer_acquired = true;
	return true;
}

void fplinux_drm_session_release_buffer(struct fplinux_drm_session *session)
{
	if (!session)
		return;
	/* Completion failures remain observable through wait and close. */
	fplinux_drm_session_wait_present(session);
	session->buffer_acquired = false;
}

bool fplinux_drm_session_close(struct fplinux_drm_session *session)
{
	bool ok = true;
	int completion_error = 0;

	if (!session)
		return false;
	if (session->buffer_acquired) {
		errno = EBUSY;
		return false;
	}
	if (!fplinux_drm_session_wait_present(session)) {
		completion_error = errno;
		ok = false;
	}
	if (session->active_handler && session->active)
		ok = session->active_handler(session, false,
					     session->active_data) &&
		     ok;
	if (session->mapping && munmap(session->mapping, session->size) < 0)
		ok = false;
	if (session->drm >= 0) {
		for (unsigned int page = 0; page < 2; ++page)
			if (session->framebuffers[page])
				drmModeRmFB(session->drm,
					    session->framebuffers[page]);
		if (session->mode_blob)
			drmModeDestroyPropertyBlob(session->drm,
						   session->mode_blob);
		if (session->handle) {
			struct drm_mode_destroy_dumb destroy = {
				.handle = session->handle
			};

			drmIoctl(session->drm, DRM_IOCTL_MODE_DESTROY_DUMB,
				 &destroy);
		}
		close(session->drm);
	}
	if (session->guardian_fd >= 0) {
		int status;
		pid_t child;

		if (send(session->guardian_fd, "C", 1, MSG_NOSIGNAL) != 1)
			ok = false;
		close(session->guardian_fd);
		do {
			child = waitpid(session->guardian, &status, 0);
		} while (child < 0 && errno == EINTR);
		if (child < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
			restore_vt(session);
			ok = false;
		}
	} else if (session->vt_owned) {
		ok = restore_vt(session) && ok;
	}
	if (session->signal_fd >= 0) {
		struct signalfd_siginfo pending;

		while (read(session->signal_fd, &pending, sizeof(pending)) > 0)
			;
		close(session->signal_fd);
	}
	if (session->signals_blocked)
		sigprocmask(SIG_SETMASK, &session->previous_mask, NULL);
	if (session->tty >= 0)
		close(session->tty);
	if (session->control >= 0) {
		if (session->vt > 0)
			ioctl(session->control, VT_DISALLOCATE, session->vt);
		close(session->control);
	}
	initialize(session);
	if (completion_error)
		errno = completion_error;
	return ok;
}
