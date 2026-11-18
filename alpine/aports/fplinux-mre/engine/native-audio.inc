/* SPDX-License-Identifier: 0BSD */
/* Guest memory and callbacks stay in the engine. The parent receives bounded
 * copies and opaque IDs; preparation replies wait for a real output stream. */
typedef void(MRE_ABI *sdk_audio_callback_fn)(int32_t);

struct native_audio_handle {
	uint32_t id, kind;
	uintptr_t callback;
	int start_pending, terminal_pending, terminal_event, live;
};

static struct {
	struct native_audio_handle handles[MRE_AUDIO_HANDLES];
	uint32_t next_id, sequence;
	struct mre_audio_request request;
} native_audio;

static int32_t native_audio_exchange(void);
static uint32_t callback_call(uintptr_t callback, uint32_t first,
			      uint32_t second, uint32_t third, uint32_t fourth);

static struct native_audio_handle *native_audio_find(uint32_t id)
{
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (id && native_audio.handles[i].id == id)
			return &native_audio.handles[i];
	return NULL;
}

static int native_audio_receive(const char *line)
{
	if (strncmp(line, "AUDIO_EVENT\t", 12))
		return 0;
	unsigned int id;
	int event, used;

	if (sscanf(line + 12, "%u\t%d%n", &id, &event, &used) != 2 ||
	    line[12 + used])
		fail("invalid audio event record");
	struct native_audio_handle *handle = native_audio_find(id);

	if (!handle || !handle->live)
		return 1;
	if (handle->kind == MRE_AUDIO_MIDI && event == 1) {
		handle->start_pending = 1;
	} else if ((handle->kind == MRE_AUDIO_MIDI && event == 2) ||
		   (handle->kind == MRE_AUDIO_GENERIC &&
		    (event == 5 || event == -1))) {
		handle->terminal_pending = 1;
		handle->terminal_event = event;
		handle->live = 0;
	} else {
		fail("audio event does not belong to the guest ABI");
	}
	return 1;
}

static void native_audio_begin_request(uint32_t operation, uint32_t id)
{
	memset(&native_audio.request, 0, sizeof(native_audio.request));
	native_audio.request.operation = operation;
	native_audio.request.handle = id;
}

static int32_t native_audio_stop_handle(struct native_audio_handle *handle)
{
	uint32_t id = handle->id;

	/* Cancellation invalidates queued completions before the IPC wait. */
	memset(handle, 0, sizeof(*handle));
	native_audio_begin_request(MRE_AUDIO_STOP, id);
	return native_audio_exchange();
}

static int32_t native_audio_play(const void *data, uint32_t bytes,
				 uint32_t kind, uint32_t format,
				 uint32_t start_ms, int32_t repeat,
				 uintptr_t callback)
{
	if (!app.shared || app.exited || !bytes || bytes > MRE_AUDIO_BYTES ||
	    !guest_data(data, bytes, PF_R) || repeat < 0 || repeat > 255 ||
	    (callback && !guest_code(callback)) ||
	    native_audio.next_id == INT32_MAX)
		return -1;
	struct native_audio_handle *handle = NULL;

	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (!native_audio.handles[i].id) {
			handle = &native_audio.handles[i];
			break;
		}
	if (!handle)
		return -1;
	uint32_t id = ++native_audio.next_id;

	*handle = (struct native_audio_handle){
		.id = id,
		.kind = kind,
		.callback = callback,
		.live = 1,
	};
	native_audio_begin_request(MRE_AUDIO_BEGIN, id);
	native_audio.request.kind = kind;
	native_audio.request.format = format;
	native_audio.request.total_bytes = bytes;
	native_audio.request.start_ms = start_ms;
	native_audio.request.repeat = repeat;
	if (native_audio_exchange() < 0)
		goto failure;
	for (uint32_t offset = 0; offset < bytes;) {
		uint32_t count = bytes - offset;

		if (count > MRE_AUDIO_CHUNK_BYTES)
			count = MRE_AUDIO_CHUNK_BYTES;
		native_audio_begin_request(MRE_AUDIO_WRITE, id);
		native_audio.request.offset = offset;
		native_audio.request.bytes = count;
		memcpy(native_audio.request.data,
		       (const uint8_t *)data + offset, count);
		if (native_audio_exchange() < 0)
			goto failure;
		offset += count;
	}
	native_audio_begin_request(MRE_AUDIO_PLAY, id);
	int32_t result = native_audio_exchange();

	if (result < 0 || (kind == MRE_AUDIO_MIDI && result != (int32_t)id))
		goto failure;
	return result;
failure:
	(void)native_audio_stop_handle(handle);
	return -1;
}

void MRE_ABI host_volume(int32_t volume)
{
	check_guest_api_stack();
	if (volume < 0)
		volume = 0;
	if (volume > 6)
		volume = 6;
	native_audio_begin_request(MRE_AUDIO_VOLUME, 0);
	native_audio.request.volume = volume;
	(void)native_audio_exchange();
}

int32_t MRE_ABI host_midi_play(uint8_t *data, int32_t bytes, int32_t repeat,
			       event_fn callback)
{
	check_guest_api_stack();
	if (bytes <= 0)
		return -1;
	return native_audio_play(data, (uint32_t)bytes, MRE_AUDIO_MIDI,
				 MRE_AUDIO_SMF, 0, repeat, (uintptr_t)callback);
}

void MRE_ABI host_midi_stop(int32_t id)
{
	check_guest_api_stack();
	struct native_audio_handle *handle = native_audio_find((uint32_t)id);

	if (handle && handle->kind == MRE_AUDIO_MIDI)
		(void)native_audio_stop_handle(handle);
}

static int32_t native_audio_midi_command(int32_t id, uint32_t operation)
{
	struct native_audio_handle *handle = native_audio_find((uint32_t)id);

	if (!handle || !handle->live || handle->kind != MRE_AUDIO_MIDI)
		return -1;
	native_audio_begin_request(operation, (uint32_t)id);
	return native_audio_exchange();
}

int32_t MRE_ABI host_midi_pause(int32_t id)
{
	check_guest_api_stack();
	return native_audio_midi_command(id, MRE_AUDIO_PAUSE);
}

int32_t MRE_ABI host_midi_resume(int32_t id)
{
	check_guest_api_stack();
	return native_audio_midi_command(id, MRE_AUDIO_RESUME);
}

void MRE_ABI host_midi_stop_all(void)
{
	check_guest_api_stack();
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (native_audio.handles[i].kind == MRE_AUDIO_MIDI)
			memset(&native_audio.handles[i], 0,
			       sizeof(native_audio.handles[i]));
	native_audio_begin_request(MRE_AUDIO_STOP_MIDI, 0);
	(void)native_audio_exchange();
}

int32_t MRE_ABI host_audio_play_bytes(void *data, uint32_t bytes,
				      uint8_t format, uint32_t start_time,
				      uint32_t path,
				      sdk_audio_callback_fn callback)
{
	check_guest_api_stack();
	if (format != MRE_AUDIO_MP3 ||
	    (path != 0 && path != 4 && path != 5 && path != 6) ||
	    (callback && !guest_code((uintptr_t)callback)) || !bytes ||
	    bytes > MRE_AUDIO_BYTES || !guest_data(data, bytes, PF_R))
		return -1;
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (native_audio.handles[i].kind == MRE_AUDIO_GENERIC)
			(void)native_audio_stop_handle(
				&native_audio.handles[i]);
	return native_audio_play(data, bytes, MRE_AUDIO_GENERIC, format,
				 start_time, 1, (uintptr_t)callback);
}

int32_t MRE_ABI host_audio_stop(sdk_audio_callback_fn callback)
{
	check_guest_api_stack();
	if (callback && !guest_code((uintptr_t)callback))
		return -1;
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i) {
		struct native_audio_handle *handle = &native_audio.handles[i];

		if (handle->kind != MRE_AUDIO_GENERIC || !handle->live)
			continue;
		uint32_t id = handle->id;

		if (native_audio_stop_handle(handle) < 0)
			return -1;
		if (callback)
			*handle = (struct native_audio_handle){
				.id = id,
				.kind = MRE_AUDIO_GENERIC,
				.callback = (uintptr_t)callback,
				.terminal_pending = 1,
				.terminal_event = 1,
			};
		return 0;
	}
	return -1;
}

static int native_audio_pending(void)
{
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (native_audio.handles[i].id)
			return 1;
	return 0;
}

static int native_audio_events_pending(void)
{
	if (!app.visible)
		return 0;
	for (size_t i = 0; i < MRE_AUDIO_HANDLES; ++i)
		if (native_audio.handles[i].start_pending ||
		    native_audio.handles[i].terminal_pending)
			return 1;
	return 0;
}

static void native_audio_dispatch(void)
{
	if (app.phase)
		fail("audio callback attempted guest reentry");
	if (!app.visible || app.exited)
		return;
	for (size_t i = 0; i < MRE_AUDIO_HANDLES && !app.exited; ++i) {
		struct native_audio_handle *handle = &native_audio.handles[i];
		uintptr_t callback = handle->callback;
		uint32_t id = handle->id, kind = handle->kind;
		int event;

		if (handle->start_pending) {
			handle->start_pending = 0;
			event = 1;
		} else if (handle->terminal_pending) {
			event = handle->terminal_event;
			memset(handle, 0, sizeof(*handle));
		} else {
			continue;
		}
		if (callback)
			(void)callback_call(
				callback,
				kind == MRE_AUDIO_MIDI ? id : (uint32_t)event,
				kind == MRE_AUDIO_MIDI ? (uint32_t)event : 0, 0,
				0);
	}
}

static void native_audio_cleanup(void)
{
	memset(native_audio.handles, 0, sizeof(native_audio.handles));
}
