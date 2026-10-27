/* SPDX-License-Identifier: 0BSD */
/* Bounded virtual CAMERA backend. The parent supplies a 240x240 UYVY
 * fixture independently of the selected executable. SDK-visible records use
 * the guest pool and the SDK short-enum ABI. Callbacks run after guest return. */
#define CAMERA_WIDTH MRE_CAMERA_WIDTH
#define CAMERA_HEIGHT MRE_CAMERA_HEIGHT
#define CAMERA_FRAME_BYTES MRE_CAMERA_BYTES
#define CAMERA_INTERVAL_MS 100u

enum {
	CAMERA_SUCCESS = 0,
	CAMERA_NOMEMORY = -1,
	CAMERA_BAD_STATUS = -2,
	CAMERA_BAD_PARAM = -3,
	CAMERA_INVALID_HANDLE = -4,
	CAMERA_UNSUPPORTED = -5,
	CAMERA_INSTANCE_CREATED = -6,
	CAMERA_NO_NOTIFY = -7,
	CAMERA_INVALID_ID = -8,
	CAMERA_INTERNAL_ERROR = -9,
	CAMERA_NO_FEATURE = -10,
	CAMERA_BACKGROUND = -11,
	CAMERA_MAIN_ID = 1,
	CAMERA_START_DONE = 1,
	CAMERA_STOP_DONE = 3,
	CAMERA_FRAME_RECEIVED = 5,
	CAMERA_READY = 1,
	CAMERA_STARTING = 2,
	CAMERA_STOPPING = 3,
	CAMERA_PREVIEW = 4,
	CAMERA_UYVY = 7
};

struct camera_size {
	uint32_t width, height;
};
struct camera_notification {
	int32_t handle;
	uint8_t message, status, padding[2];
};
struct camera_frame {
	uint32_t row_pixel, col_pixel;
	uint8_t format, padding[3];
	uint32_t pixel_data;
	int32_t reserved;
};
_Static_assert(sizeof(struct camera_size) == 8, "CAMERA SDK size ABI");
_Static_assert(sizeof(struct camera_notification) == 8 &&
		       offsetof(struct camera_notification, message) == 4 &&
		       offsetof(struct camera_notification, status) == 5,
	       "CAMERA SDK short-enum notification ABI");
_Static_assert(sizeof(struct camera_frame) == 20 &&
		       offsetof(struct camera_frame, format) == 8 &&
		       offsetof(struct camera_frame, pixel_data) == 12 &&
		       offsetof(struct camera_frame, reserved) == 16,
	       "CAMERA SDK short-enum frame ABI");

typedef void(MRE_ABI *camera_notify_fn)(const struct camera_notification *,
					void *);
static struct {
	int32_t handle;
	uint8_t status, pending_message;
	camera_notify_fn callback;
	void *user_data;
	struct camera_size *size;
	struct camera_notification *notification;
	struct camera_frame *frame;
	uint8_t *pixels;
	uint64_t next_ms;
	uint32_t frames;
} camera_state;
static uint32_t camera_next_handle;

static int camera_valid_handle(int32_t handle)
{
	return handle > 0 && handle == camera_state.handle;
}
static int camera_guest_struct(const void *pointer, size_t bytes,
			       uint32_t flags)
{
	return pointer && !((uintptr_t)pointer & 3) &&
	       guest_data(pointer, bytes, flags);
}
static void camera_free_owned(void *pointer)
{
	if (!pointer)
		return;
	/* Dispatch/cleanup run on the host stack; host_free checks a guest API
	 * stack. This is the same pool release without pretending to be an API. */
	enum mre_memory_release result =
		mre_memory_free(&app.memory, pointer, NULL);
	if (result != MRE_MEMORY_RELEASED && result != MRE_MEMORY_RETIRED)
		fail("virtual CAMERA lost an owned SDK allocation");
}
static void camera_drop_pixels(void)
{
	camera_free_owned(camera_state.pixels);
	camera_state.pixels = NULL;
	if (camera_state.frame &&
	    mre_memory_contains(&app.memory, camera_state.frame,
				sizeof(*camera_state.frame)))
		camera_state.frame->pixel_data = 0;
	service_memory_refresh();
}
static void camera_cleanup(void)
{
	camera_free_owned(camera_state.pixels);
	camera_free_owned(camera_state.frame);
	camera_free_owned(camera_state.notification);
	camera_free_owned(camera_state.size);
	memset(&camera_state, 0, sizeof(camera_state));
	service_memory_refresh();
}
int32_t MRE_ABI camera_main_id(void)
{
	check_guest_api_stack();
	return CAMERA_MAIN_ID;
}
int32_t MRE_ABI camera_create(int32_t camera_id, int32_t *handle)
{
	check_guest_api_stack();
	if (!camera_guest_struct(handle, sizeof(*handle), PF_W))
		return CAMERA_BAD_PARAM;
	if (camera_id != CAMERA_MAIN_ID)
		return CAMERA_INVALID_ID;
	if (camera_state.handle)
		return CAMERA_INSTANCE_CREATED;
	if (!app.visible || app.exited)
		return CAMERA_BACKGROUND;
	if (!app.shared || app.shared->camera_bytes != CAMERA_FRAME_BYTES)
		return CAMERA_NO_FEATURE;
	camera_state.size = host_calloc(sizeof(*camera_state.size));
	camera_state.notification =
		host_calloc(sizeof(*camera_state.notification));
	camera_state.frame = host_calloc(sizeof(*camera_state.frame));
	if (!camera_state.size || !camera_state.notification ||
	    !camera_state.frame) {
		camera_cleanup();
		return CAMERA_NOMEMORY;
	}
	if (++camera_next_handle > INT32_MAX)
		camera_next_handle = 1;
	camera_state.handle = (int32_t)camera_next_handle;
	camera_state.status = CAMERA_READY;
	*camera_state.size =
		(struct camera_size){ CAMERA_WIDTH, CAMERA_HEIGHT };
	*camera_state.frame = (struct camera_frame){ .row_pixel = CAMERA_WIDTH,
						     .col_pixel = CAMERA_HEIGHT,
						     .format = CAMERA_UYVY };
	*handle = camera_state.handle;
	fprintf(stderr,
		"CAMERA virtual fixture instance=%d; preview=240x240 UYVY, 10fps; no live camera\n",
		camera_state.handle);
	return CAMERA_SUCCESS;
}
int32_t MRE_ABI camera_release(int32_t handle)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	/* Release also retires a preview and its pending notification. */
	fprintf(stderr, "CAMERA release virtual instance=%d frames=%u\n",
		handle, camera_state.frames);
	camera_cleanup();
	return CAMERA_SUCCESS;
}
int32_t MRE_ABI camera_get_status(int32_t handle, uint8_t *status)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	if (!status || !guest_data(status, 1, PF_W))
		return CAMERA_BAD_PARAM;
	/* VM_CAMERA_STATUS is one byte in the SDK -fshort-enums ABI. */
	*status = camera_state.status;
	return CAMERA_SUCCESS;
}
int32_t MRE_ABI camera_register_notify(int32_t handle,
				       camera_notify_fn callback,
				       void *user_data)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	if (!guest_code((uintptr_t)callback) ||
	    (user_data && !guest_data(user_data, 1, PF_R)))
		return CAMERA_BAD_PARAM;
	if (camera_state.status != CAMERA_READY)
		return CAMERA_BAD_STATUS;
	camera_state.callback = callback;
	camera_state.user_data = user_data;
	return CAMERA_SUCCESS;
}
int32_t MRE_ABI camera_support_size(int32_t handle, uint32_t *list,
				    uint32_t *count)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	if (!camera_guest_struct(list, sizeof(*list), PF_W) ||
	    !camera_guest_struct(count, sizeof(*count), PF_W))
		return CAMERA_BAD_PARAM;
	if (!mre_memory_contains(&app.memory, camera_state.size,
				 sizeof(*camera_state.size)))
		return CAMERA_INTERNAL_ERROR;
	*list = (uint32_t)(uintptr_t)camera_state.size;
	*count = 1;
	return CAMERA_SUCCESS;
}
int32_t MRE_ABI camera_set_size(int32_t handle, const struct camera_size *size)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	if (!camera_guest_struct(size, sizeof(*size), PF_R))
		return CAMERA_BAD_PARAM;
	if (camera_state.status != CAMERA_READY)
		return CAMERA_BAD_STATUS;
	if (size->width != CAMERA_WIDTH || size->height != CAMERA_HEIGHT)
		return CAMERA_INTERNAL_ERROR;
	if (!mre_memory_contains(&app.memory, camera_state.size,
				 sizeof(*camera_state.size)))
		return CAMERA_INTERNAL_ERROR;
	*camera_state.size = *size;
	return CAMERA_SUCCESS;
}
int32_t MRE_ABI camera_set_fps(int32_t handle, uint32_t fps)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	if (camera_state.status != CAMERA_READY)
		return CAMERA_BAD_STATUS;
	if (!fps)
		return CAMERA_BAD_PARAM;
	/* This controlled backend declares only its actual 10fps cadence. */
	return fps == 10 ? CAMERA_SUCCESS : CAMERA_UNSUPPORTED;
}
int32_t MRE_ABI camera_preview_start(int32_t handle)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	if (!app.visible || app.exited)
		return CAMERA_BACKGROUND;
	if (camera_state.status != CAMERA_READY)
		return CAMERA_BAD_STATUS;
	if (!camera_state.callback)
		return CAMERA_NO_NOTIFY;
	if (!mre_memory_contains(&app.memory, camera_state.frame,
				 sizeof(*camera_state.frame)) ||
	    !mre_memory_contains(&app.memory, camera_state.notification,
				 sizeof(*camera_state.notification)))
		return CAMERA_INTERNAL_ERROR;
	if (!app.shared || app.shared->camera_bytes != CAMERA_FRAME_BYTES) {
		fprintf(stderr,
			"CAMERA virtual backend has no 240x240 UYVY source\n");
		return CAMERA_NO_FEATURE;
	}
	camera_state.pixels = host_calloc(CAMERA_FRAME_BYTES);
	if (!camera_state.pixels)
		return CAMERA_NOMEMORY;
	memcpy(camera_state.pixels, app.shared->camera_data,
	       CAMERA_FRAME_BYTES);
	camera_state.frame->pixel_data =
		(uint32_t)(uintptr_t)camera_state.pixels;
	camera_state.status = CAMERA_STARTING;
	camera_state.pending_message = CAMERA_START_DONE;
	camera_state.next_ms = monotonic_ms() + 1;
	camera_state.frames = 0;
	fprintf(stderr,
		"CAMERA queued virtual preview start; copied declared UYVY fixture into owned SDK heap\n");
	return CAMERA_SUCCESS;
}
int32_t MRE_ABI camera_preview_stop(int32_t handle)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	if (!camera_state.callback)
		return CAMERA_NO_NOTIFY;
	if (camera_state.status != CAMERA_STARTING &&
	    camera_state.status != CAMERA_PREVIEW)
		return CAMERA_BAD_STATUS;
	camera_drop_pixels();
	camera_state.status = CAMERA_STOPPING;
	camera_state.pending_message = CAMERA_STOP_DONE;
	camera_state.next_ms = monotonic_ms() + 1;
	return CAMERA_SUCCESS;
}
int32_t MRE_ABI camera_get_frame(int32_t handle, struct camera_frame *frame)
{
	check_guest_api_stack();
	if (!camera_valid_handle(handle))
		return CAMERA_INVALID_HANDLE;
	if (!camera_guest_struct(frame, sizeof(*frame), PF_W))
		return CAMERA_BAD_PARAM;
	if (camera_state.status != CAMERA_PREVIEW)
		return CAMERA_BAD_STATUS;
	if (!mre_memory_contains(&app.memory, camera_state.frame,
				 sizeof(*camera_state.frame)) ||
	    !mre_memory_contains(&app.memory, camera_state.pixels,
				 CAMERA_FRAME_BYTES))
		return CAMERA_INTERNAL_ERROR;
	memcpy(frame, camera_state.frame, sizeof(*frame));
	return CAMERA_SUCCESS;
}
static int camera_timeout_ms(uint64_t now)
{
	if (!camera_state.handle || !camera_state.callback || !app.visible ||
	    app.exited ||
	    (!camera_state.pending_message &&
	     camera_state.status != CAMERA_PREVIEW))
		return -1;
	uint64_t delay =
		camera_state.next_ms > now ? camera_state.next_ms - now : 0;
	return delay > INT32_MAX ? INT32_MAX : (int)delay;
}
static void camera_dispatch_due(void)
{
	/* API calls may queue work but never enter an original callback. The
	 * event loop calls here only after original vm_main and CREATE return. */
	uint64_t now = monotonic_ms();
	if (app.phase || camera_timeout_ms(now) != 0)
		return;
	if (!mre_memory_contains(&app.memory, camera_state.notification,
				 sizeof(*camera_state.notification)))
		fail("virtual CAMERA notification is no longer SDK-owned");
	uint8_t message = camera_state.pending_message;
	if (message == CAMERA_START_DONE)
		camera_state.status = CAMERA_PREVIEW;
	else if (message == CAMERA_STOP_DONE)
		camera_state.status = CAMERA_READY;
	else {
		message = CAMERA_FRAME_RECEIVED;
		camera_state.frames++;
	}
	/* Advance before calling: the callback may stop/release this instance.
	 * Coalesce missed ticks instead of delivering a burst after visibility. */
	camera_state.pending_message = 0;
	camera_state.next_ms = now + CAMERA_INTERVAL_MS;
	*camera_state.notification =
		(struct camera_notification){ .handle = camera_state.handle,
					      .message = message,
					      .status = camera_state.status };
	camera_notify_fn callback = camera_state.callback;
	void *user_data = camera_state.user_data;
	struct camera_notification *notification = camera_state.notification;
	debug_trace(
		"DISPATCH CAMERA virtual message=%u status=%u frame=%u original callback=%p\n",
		message, camera_state.status, camera_state.frames,
		(void *)callback);
	app.phase = 2;
	callback_call((uintptr_t)callback, (uint32_t)(uintptr_t)notification,
		      (uint32_t)(uintptr_t)user_data, 0, 0);
	app.phase = 0;
}
static uintptr_t camera_resolve(const char *name)
{
#define CAMERA_EXPORT(symbol, function) \
	if (!strcmp(name, symbol))      \
		return (uintptr_t)function;
	CAMERA_EXPORT("vm_camera_get_main_camera_id", camera_main_id)
	CAMERA_EXPORT("vm_create_camera_instance", camera_create)
	CAMERA_EXPORT("vm_release_camera_instance", camera_release)
	CAMERA_EXPORT("vm_get_camera_status", camera_get_status)
	CAMERA_EXPORT("vm_camera_register_notify", camera_register_notify)
	CAMERA_EXPORT("vm_camera_get_support_preview_size", camera_support_size)
	CAMERA_EXPORT("vm_camera_set_preview_size", camera_set_size)
	CAMERA_EXPORT("vm_camera_set_preview_fps", camera_set_fps)
	CAMERA_EXPORT("vm_camera_preview_start", camera_preview_start)
	CAMERA_EXPORT("vm_camera_preview_stop", camera_preview_stop)
	CAMERA_EXPORT("vm_camera_get_frame", camera_get_frame)
#undef CAMERA_EXPORT
	return 0;
}
