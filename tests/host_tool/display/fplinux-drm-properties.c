/* SPDX-License-Identifier: GPL-2.0-only */
/* Presentation/close boundary with synthetic selected objects and DRM calls. */
#define _GNU_SOURCE
#include "fplinux-drm-session.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <linux/vt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

struct property_definition {
	uint32_t object;
	uint32_t type;
	uint32_t id;
	const char *name;
};

struct atomic_value {
	uint32_t object;
	uint32_t property;
	uint64_t value;
};

static const struct property_definition definitions[] = {
	{ 31, DRM_MODE_OBJECT_CONNECTOR, 103, "CRTC_ID" },
	{ 37, DRM_MODE_OBJECT_CRTC, 107, "MODE_ID" },
	{ 37, DRM_MODE_OBJECT_CRTC, 109, "ACTIVE" },
	{ 37, DRM_MODE_OBJECT_CRTC, 113, "OUT_FENCE_PTR" },
	{ 41, DRM_MODE_OBJECT_PLANE, 127, "FB_ID" },
	{ 41, DRM_MODE_OBJECT_PLANE, 131, "CRTC_ID" },
	{ 41, DRM_MODE_OBJECT_PLANE, 137, "CRTC_X" },
	{ 41, DRM_MODE_OBJECT_PLANE, 139, "CRTC_Y" },
	{ 41, DRM_MODE_OBJECT_PLANE, 149, "CRTC_W" },
	{ 41, DRM_MODE_OBJECT_PLANE, 151, "CRTC_H" },
	{ 41, DRM_MODE_OBJECT_PLANE, 157, "SRC_X" },
	{ 41, DRM_MODE_OBJECT_PLANE, 163, "SRC_Y" },
	{ 41, DRM_MODE_OBJECT_PLANE, 167, "SRC_W" },
	{ 41, DRM_MODE_OBJECT_PLANE, 173, "SRC_H" },
};

static struct fplinux_drm_session session = { .drm = -1, .present_fence = -1 };
static drmModeAtomicReq *current_request;
static struct atomic_value values[14];
static size_t value_count;
static unsigned int object_queries;
static unsigned int property_queries;
static unsigned int objects_allocated;
static unsigned int objects_freed;
static unsigned int properties_allocated;
static unsigned int properties_freed;
static unsigned int requests_allocated;
static unsigned int requests_freed;
static unsigned int commits;
static unsigned int fence_checks;
static unsigned int removed_framebuffers;
static unsigned int removed_blobs;
static unsigned int object_offset;
static unsigned int property_offset;
static uint32_t missing_property;
static uint32_t failed_add_property;
static bool fail_query;
static bool fail_commit;
static int fence_status = 1;
static int fences[8];
static size_t fence_count;
static uint32_t expected_flags = 0x0400;
static bool fence_ready = true;
static bool omit_fence;
static bool lost_master;
static bool focus_released;

drmModeAtomicReq *__real_drmModeAtomicAlloc(void);
void __real_drmModeAtomicFree(drmModeAtomicReq *request);

static const struct atomic_value *find_value(uint32_t object, uint32_t property)
{
	const struct atomic_value *found = NULL;

	for (size_t index = 0; index < value_count; ++index)
		if (values[index].object == object &&
		    values[index].property == property) {
			assert(!found);
			found = &values[index];
		}
	assert(found);
	return found;
}

drmModeObjectPropertiesPtr
__wrap_drmModeObjectGetProperties(int fd, uint32_t object, uint32_t type)
{
	drmModeObjectPropertiesPtr result;

	assert(fd == session.drm);
	++object_queries;
	if (fail_query) {
		errno = EIO;
		return NULL;
	}
	result = calloc(1, sizeof(*result));
	assert(result);
	result->props = calloc(14, sizeof(*result->props));
	result->prop_values = calloc(14, sizeof(*result->prop_values));
	assert(result->props && result->prop_values);
	for (size_t index = 0; index < 14; ++index)
		if (definitions[index].object + object_offset == object) {
			assert(definitions[index].type == type);
			if (definitions[index].id != missing_property)
				result->props[result->count_props++] =
					definitions[index].id + property_offset;
		}
	++objects_allocated;
	return result;
}

void __wrap_drmModeFreeObjectProperties(drmModeObjectPropertiesPtr object)
{
	free(object->props);
	free(object->prop_values);
	free(object);
	++objects_freed;
}

drmModePropertyPtr __wrap_drmModeGetProperty(int fd, uint32_t id)
{
	assert(fd == session.drm);
	++property_queries;
	for (size_t index = 0; index < 14; ++index)
		if (definitions[index].id + property_offset == id) {
			drmModePropertyPtr result = calloc(1, sizeof(*result));

			assert(result);
			result->prop_id = id;
			snprintf(result->name, sizeof(result->name), "%s",
				 definitions[index].name);
			++properties_allocated;
			return result;
		}
	assert(false);
	return NULL;
}

void __wrap_drmModeFreeProperty(drmModePropertyPtr property)
{
	free(property);
	++properties_freed;
}

drmModeAtomicReq *__wrap_drmModeAtomicAlloc(void)
{
	assert(!current_request);
	current_request = __real_drmModeAtomicAlloc();
	assert(current_request);
	value_count = 0;
	++requests_allocated;
	return current_request;
}

int __wrap_drmModeAtomicAddProperty(drmModeAtomicReq *request, uint32_t object,
				    uint32_t property, uint64_t value)
{
	assert(request == current_request && value_count < 14);
	if (property == failed_add_property) {
		errno = ENOSPC;
		return -1;
	}
	values[value_count++] =
		(struct atomic_value){ object, property, value };
	return (int)value_count;
}

int __wrap_drmModeAtomicCommit(int fd, drmModeAtomicReq *request,
			       uint32_t flags, void *data)
{
	const struct atomic_value *fence_value;
	int *fence;

	assert(fd == session.drm && request == current_request);
	assert(flags == expected_flags && !data);
	assert(value_count == 14);
	++commits;
	if (fail_commit) {
		errno = EBUSY;
		return -1;
	}
	fence_value = object_offset ? find_value(67, 1113) :
				      find_value(37, 113);
	fence = (int *)(uintptr_t)fence_value->value;
	assert(fence && *fence == -1 && fence_count < 8);
	if (omit_fence)
		return 0;
	*fence = eventfd(fence_ready ? 1 : 0, EFD_CLOEXEC | EFD_NONBLOCK);
	assert(*fence >= 0);
	fences[fence_count++] = *fence;
	return 0;
}

void __wrap_drmModeAtomicFree(drmModeAtomicReq *request)
{
	assert(request == current_request);
	__real_drmModeAtomicFree(request);
	current_request = NULL;
	++requests_freed;
	/* Teardown must not replace the error returned by present(). */
	errno = ECHILD;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	struct sync_file_info *info;
	va_list arguments;

	if (request == VT_RELDISP) {
		assert(fd == session.tty && lost_master);
		return 0;
	}
	assert(fence_count && fd == fences[fence_count - 1]);
	assert(request == SYNC_IOC_FILE_INFO);
	va_start(arguments, request);
	info = va_arg(arguments, struct sync_file_info *);
	va_end(arguments);
	info->status = fence_status;
	++fence_checks;
	return 0;
}

int __wrap_drmDropMaster(int fd)
{
	assert(fd == session.drm && focus_released);
	assert(session.present_fence == -1 && session.shown_page == 0);
	lost_master = true;
	return 0;
}

int __wrap_drmModeRmFB(int fd, uint32_t framebuffer)
{
	assert(fd == session.drm);
	assert(framebuffer == 211 || framebuffer == 223 || framebuffer == 311 ||
	       framebuffer == 323);
	++removed_framebuffers;
	return 0;
}

int __wrap_drmModeDestroyPropertyBlob(int fd, uint32_t blob)
{
	assert(fd == session.drm && (blob == 73 || blob == 83));
	++removed_blobs;
	return 0;
}

static void select_session(bool next)
{
	/* Deliberately do not clear the struct: production close owns the reset. */
	assert(session.drm == -1);
	object_offset = next ? 30 : 0;
	property_offset = next ? 1000 : 0;
	session.drm = open("/dev/null", O_RDWR | O_CLOEXEC);
	assert(session.drm >= 0);
	session.tty = -1;
	session.control = -1;
	session.signal_fd = -1;
	session.guardian_fd = -1;
	session.present_fence = -1;
	session.connector = next ? 61 : 31;
	session.crtc = next ? 67 : 37;
	session.plane = next ? 71 : 41;
	session.mode_blob = next ? 83 : 73;
	session.framebuffers[0] = next ? 311 : 211;
	session.framebuffers[1] = next ? 323 : 223;
	session.width = 80;
	session.height = 12;
	session.pages = 2;
	session.shown_page = 1;
	session.active = true;
}

static void expect_request(unsigned int page, bool next)
{
	const struct atomic_value first[] = {
		{ 31, 103, 37 },      { 37, 107, 73 },	   { 37, 109, 1 },
		{ 37, 113, 0 },	      { 41, 127, 211 },	   { 41, 131, 37 },
		{ 41, 137, 0 },	      { 41, 139, 0 },	   { 41, 149, 80 },
		{ 41, 151, 12 },      { 41, 157, 0 },	   { 41, 163, 0 },
		{ 41, 167, 5242880 }, { 41, 173, 786432 },
	};
	const struct atomic_value second[] = {
		{ 61, 1103, 67 },      { 67, 1107, 83 },     { 67, 1109, 1 },
		{ 67, 1113, 0 },       { 71, 1127, 311 },    { 71, 1131, 67 },
		{ 71, 1137, 0 },       { 71, 1139, 0 },	     { 71, 1149, 80 },
		{ 71, 1151, 12 },      { 71, 1157, 0 },	     { 71, 1163, 0 },
		{ 71, 1167, 5242880 }, { 71, 1173, 786432 },
	};
	const struct atomic_value *expected = next ? second : first;

	assert(value_count == 14);
	for (size_t index = 0; index < 14; ++index) {
		const struct atomic_value *actual = find_value(
			expected[index].object, expected[index].property);
		uint64_t value = expected[index].value;

		if (expected[index].property == 113 ||
		    expected[index].property == 1113) {
			assert(actual->value);
			continue;
		}
		if ((expected[index].property == 127 ||
		     expected[index].property == 1127) &&
		    page == 1)
			value = next ? 323 : 223;
		assert(actual->value == value);
	}
}

static void expect_released_resources(void)
{
	assert(!current_request && requests_allocated == requests_freed);
	assert(objects_allocated == objects_freed);
	assert(properties_allocated == properties_freed);
	for (size_t index = 0; index < fence_count; ++index)
		assert(fcntl(fences[index], F_GETFD) < 0 && errno == EBADF);
}

static void expect_cached_frame(void)
{
	unsigned int previous_objects = object_queries;
	unsigned int previous_properties = property_queries;

	assert(previous_objects && previous_properties);
	assert(fplinux_drm_session_present(&session, 1));
	expect_request(1, false);
	assert(object_queries == previous_objects);
	assert(property_queries == previous_properties);
	assert(session.shown_page == 1 && session.presented);
	expect_released_resources();
}

static void cached_frames(void)
{
	assert(fplinux_drm_session_present(&session, 0));
	expect_request(0, false);
	assert(session.shown_page == 0 && session.presented);
	expect_cached_frame();
	assert(commits == 2 && fence_checks == 2);
}

static void close_resets_selected_properties(void)
{
	unsigned int previous_objects;
	int previous_fd = session.drm;

	assert(fplinux_drm_session_present(&session, 0));
	expect_request(0, false);
	assert(fplinux_drm_session_close(&session));
	assert(fcntl(previous_fd, F_GETFD) < 0 && errno == EBADF);
	assert(removed_framebuffers == 2 && removed_blobs == 1);
	previous_objects = object_queries;
	select_session(true);
	assert(fplinux_drm_session_present(&session, 1));
	expect_request(1, true);
	assert(object_queries > previous_objects);
	expect_released_resources();
}

static void missing_property_retries(void)
{
	unsigned int previous_objects;

	missing_property = 151;
	assert(!fplinux_drm_session_present(&session, 0));
	assert(errno == ENOTSUP && !commits);
	assert(session.shown_page == 1 && !session.presented);
	expect_released_resources();
	previous_objects = object_queries;
	assert(!fplinux_drm_session_present(&session, 0));
	assert(errno == ENOTSUP && !commits);
	assert(object_queries > previous_objects);
	expect_released_resources();
	missing_property = 0;
	assert(fplinux_drm_session_present(&session, 0));
	expect_request(0, false);
	expect_cached_frame();
}

static void query_error_retries(void)
{
	fail_query = true;
	assert(!fplinux_drm_session_present(&session, 0));
	assert(errno == EIO && !commits && !value_count);
	assert(!session.presented);
	expect_released_resources();
	fail_query = false;
	assert(fplinux_drm_session_present(&session, 0));
	expect_request(0, false);
	expect_cached_frame();
}

static void atomic_add_error_preserved(void)
{
	failed_add_property = 109;
	assert(!fplinux_drm_session_present(&session, 0));
	assert(errno == ENOSPC && !commits);
	assert(session.shown_page == 1 && !session.presented);
	expect_released_resources();
	failed_add_property = 0;
	assert(fplinux_drm_session_present(&session, 0));
	expect_request(0, false);
	expect_cached_frame();
}

static void commit_error_preserved(void)
{
	fail_commit = true;
	assert(!fplinux_drm_session_present(&session, 0));
	assert(errno == EBUSY && commits == 1 && !fence_count);
	assert(session.shown_page == 1 && !session.presented);
	expect_request(0, false);
	expect_released_resources();
	fail_commit = false;
	expect_cached_frame();
}

static void fence_error_preserved(void)
{
	fence_status = -EPIPE;
	assert(!fplinux_drm_session_present(&session, 0));
	assert(errno == EPIPE && commits == 1 && fence_checks == 1);
	assert(session.shown_page == 1 && !session.presented);
	expect_request(0, false);
	expect_released_resources();
	fence_status = 1;
	expect_cached_frame();
}

static void signal_display_completion(void)
{
	const uint64_t value = 1;

	assert(session.present_fence >= 0);
	assert(write(session.present_fence, &value, sizeof(value)) ==
	       sizeof(value));
}

static void queue_returns_before_completion(void)
{
	unsigned int previous_objects;
	unsigned int previous_properties;

	expected_flags = 0x0600;
	fence_ready = false;
	assert(fplinux_drm_session_queue_present(&session, 0));
	expect_request(0, false);
	assert(session.shown_page == 1 && !session.presented && !fence_checks);
	assert(fcntl(session.present_fence, F_GETFD) >= 0);
	assert(!fplinux_drm_session_queue_present(&session, 1) &&
	       errno == EBUSY);
	assert(commits == 1 && !fence_checks);
	previous_objects = object_queries;
	previous_properties = property_queries;
	signal_display_completion();
	assert(fplinux_drm_session_wait_present(&session));
	assert(session.shown_page == 0 && session.presented &&
	       fence_checks == 1);
	expect_released_resources();
	assert(fplinux_drm_session_queue_present(&session, 1));
	assert(object_queries == previous_objects &&
	       property_queries == previous_properties);
	assert(session.shown_page == 0);
	signal_display_completion();
	assert(fplinux_drm_session_wait_present(&session));
	assert(session.shown_page == 1 && fence_checks == 2);
}

static void blocking_present_drains_previous_frame(void)
{
	expected_flags = 0x0600;
	assert(fplinux_drm_session_queue_present(&session, 0));
	assert(!session.presented);
	expected_flags = 0x0400;
	assert(fplinux_drm_session_present(&session, 1));
	assert(commits == 2 && fence_checks == 2 && session.shown_page == 1);
	expect_released_resources();
}

static bool require_completed_frame(struct fplinux_drm_session *display,
				    bool active, void *data)
{
	(void)data;
	assert(display == &session);
	if (!active) {
		assert(display->present_fence == -1 &&
		       display->shown_page == 0);
		assert(fence_checks == 1);
		focus_released = true;
	}
	return true;
}

static void dispatch_drains_only_for_vt_release(void)
{
	struct signalfd_siginfo signal = { .ssi_signo = SIGRTMIN + 4 };
	int events[2];

	assert(pipe2(events, O_CLOEXEC | O_NONBLOCK) == 0);
	session.signal_fd = events[0];
	session.tty = open("/dev/null", O_RDWR | O_CLOEXEC);
	assert(session.tty >= 0);
	assert(fplinux_drm_session_set_active_handler(
		&session, require_completed_frame, NULL));
	expected_flags = 0x0600;
	fence_ready = false;
	assert(fplinux_drm_session_queue_present(&session, 0));
	assert(fplinux_drm_session_dispatch(&session));
	assert(!fence_checks && session.shown_page == 1);
	signal_display_completion();
	assert(write(events[1], &signal, sizeof(signal)) == sizeof(signal));
	assert(fplinux_drm_session_dispatch(&session));
	assert(!session.active && focus_released && lost_master);
	close(events[1]);
}

static void close_drains_final_frame(void)
{
	expected_flags = 0x0600;
	assert(fplinux_drm_session_set_active_handler(
		&session, require_completed_frame, NULL));
	assert(fplinux_drm_session_queue_present(&session, 0));
	assert(!fence_checks);
	assert(fplinux_drm_session_close(&session));
	assert(focus_released && session.present_fence == -1);
	expect_released_resources();
	select_session(false);
}

static bool fail_active_cleanup(struct fplinux_drm_session *display,
				bool active, void *data)
{
	(void)data;
	assert(display == &session);
	if (active)
		return true;
	errno = ENOSPC;
	return false;
}

static void deferred_error_terminates_presentation(int status, bool ready,
						   bool missing)
{
	int expected_error = EPIPE;
	int descriptor;

	if (!ready)
		expected_error = ETIMEDOUT;
	if (missing || status == 0)
		expected_error = EIO;
	expected_flags = 0x0600;
	fence_status = status;
	fence_ready = ready;
	omit_fence = missing;
	if (missing) {
		assert(!fplinux_drm_session_queue_present(&session, 0));
	} else {
		assert(fplinux_drm_session_queue_present(&session, 0));
		assert(!fplinux_drm_session_wait_present(&session));
	}
	assert(errno == expected_error && !session.presented &&
	       session.shown_page == 1);
	assert(!fplinux_drm_session_queue_present(&session, 1) &&
	       errno == expected_error);
	assert(!fplinux_drm_session_present(&session, 1) &&
	       errno == expected_error);
	assert(commits == 1 && session.present_fence == -1);
	assert(fplinux_drm_session_set_active_handler(
		&session, fail_active_cleanup, NULL));
	descriptor = session.drm;
	assert(!fplinux_drm_session_close(&session) && errno == expected_error);
	assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
	assert(removed_framebuffers == 2 && removed_blobs == 1);
	expect_released_resources();
	select_session(false);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	select_session(false);
	if (!strcmp(argv[1], "cached-frames"))
		cached_frames();
	else if (!strcmp(argv[1], "close-reset"))
		close_resets_selected_properties();
	else if (!strcmp(argv[1], "missing-property"))
		missing_property_retries();
	else if (!strcmp(argv[1], "query-error"))
		query_error_retries();
	else if (!strcmp(argv[1], "atomic-add-error"))
		atomic_add_error_preserved();
	else if (!strcmp(argv[1], "commit-error"))
		commit_error_preserved();
	else if (!strcmp(argv[1], "fence-error"))
		fence_error_preserved();
	else if (!strcmp(argv[1], "async-queue"))
		queue_returns_before_completion();
	else if (!strcmp(argv[1], "blocking-drain"))
		blocking_present_drains_previous_frame();
	else if (!strcmp(argv[1], "vt-drain"))
		dispatch_drains_only_for_vt_release();
	else if (!strcmp(argv[1], "close-drain"))
		close_drains_final_frame();
	else if (!strcmp(argv[1], "async-fence-error"))
		deferred_error_terminates_presentation(-EPIPE, true, false);
	else if (!strcmp(argv[1], "async-false-fence"))
		deferred_error_terminates_presentation(0, true, false);
	else if (!strcmp(argv[1], "async-timeout"))
		deferred_error_terminates_presentation(1, false, false);
	else if (!strcmp(argv[1], "async-missing-fence"))
		deferred_error_terminates_presentation(1, true, true);
	else
		assert(false);
	assert(fplinux_drm_session_close(&session));
	expect_released_resources();
	printf("DRM presentation properties: %s passed\n", argv[1]);
	return 0;
}
