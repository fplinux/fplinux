/* SPDX-License-Identifier: 0BSD */
/* Native SDK services for any loaded guest. Included before the
 * resolver; allocator hooks and provider dispatch are declared by engine.c. */
struct sdk_malloc_stat {
	int32_t malloc_count, free_count, current, peak;
	int32_t fail_times, max_fail_size, avail_heap_size;
};
_Static_assert(sizeof(struct sdk_malloc_stat) == 28 &&
		       offsetof(struct sdk_malloc_stat, avail_heap_size) == 24,
	       "SDK malloc_stat_t ABI");
static struct sdk_malloc_stat service_malloc_stat;

static int service_guest_data(const void *pointer, size_t bytes)
{
	return inside_pointer(pointer, bytes, &service_malloc_stat,
			      sizeof(service_malloc_stat));
}
static void service_memory_refresh(void)
{
	size_t used = app.memory.bytes;
	service_malloc_stat.current = used > INT32_MAX ? INT32_MAX :
							 (int32_t)used;
	if (service_malloc_stat.current > service_malloc_stat.peak)
		service_malloc_stat.peak = service_malloc_stat.current;
	size_t available = 0;
	if (app.canvas_bytes <= app.ram_limit &&
	    used <= app.ram_limit - app.canvas_bytes)
		available = app.ram_limit - app.canvas_bytes - used;
	service_malloc_stat.avail_heap_size =
		available > INT32_MAX ? INT32_MAX : (int32_t)available;
}
static void service_memory_allocation(int32_t bytes, const void *result)
{
	if (service_malloc_stat.malloc_count < INT32_MAX)
		service_malloc_stat.malloc_count++;
	if (!result) {
		if (service_malloc_stat.fail_times < INT32_MAX)
			service_malloc_stat.fail_times++;
		if (bytes > service_malloc_stat.max_fail_size)
			service_malloc_stat.max_fail_size = bytes;
	}
	service_memory_refresh();
}
static void service_memory_free_call(void)
{
	if (service_malloc_stat.free_count < INT32_MAX)
		service_malloc_stat.free_count++;
}
struct sdk_malloc_stat *MRE_ABI host_get_malloc_stat(void)
{
	check_guest_api_stack();
	service_memory_refresh();
	return &service_malloc_stat;
}

enum {
	SDK_STORAGE_PARAMETER = -9,
	SDK_STORAGE_OPEN_FAILED = -7,
	SDK_STORAGE_READ_FAILED = -4,
	SDK_STORAGE_AUTHORIZATION = -3
};
int32_t MRE_ABI host_storage_open(void)
{
	check_guest_api_stack();
	fprintf(stderr,
		"STORAGE vm_storage_open() -> -7 VM_STORAGE_ERR_FILE_OPEN (no writable application store)\n");
	return SDK_STORAGE_OPEN_FAILED;
}
void MRE_ABI host_storage_close(int32_t handle)
{
	check_guest_api_stack();
	debug_trace("STORAGE vm_storage_close(%d); no live storage handle\n",
		    handle);
}
static int service_storage_count(uint32_t *count)
{
	if (((uintptr_t)count & 3) || !guest_data(count, 4, PF_W))
		return 0;
	*count = 0;
	return 1;
}
int32_t MRE_ABI host_storage_read(int32_t handle, void *data, uint32_t offset,
				  uint32_t bytes, uint32_t *count)
{
	check_guest_api_stack();
	if (!service_storage_count(count) || offset > FILE_CAP ||
	    bytes > FILE_CAP - offset ||
	    (bytes && !guest_data(data, bytes, PF_W)) || handle < 0)
		return SDK_STORAGE_PARAMETER;
	debug_trace("STORAGE vm_storage_read(%d,%u,%u) -> -4, count=0\n",
		    handle, offset, bytes);
	return SDK_STORAGE_READ_FAILED;
}
int32_t MRE_ABI host_storage_write(int32_t handle, const void *data,
				   uint32_t offset, uint32_t bytes,
				   uint32_t *count)
{
	check_guest_api_stack();
	if (!service_storage_count(count) || offset > FILE_CAP ||
	    bytes > FILE_CAP - offset ||
	    (bytes && !guest_data(data, bytes, PF_R)) || handle < 0)
		return SDK_STORAGE_PARAMETER;
	debug_trace("STORAGE vm_storage_write(%d,%u,%u) -> -3, count=0\n",
		    handle, offset, bytes);
	return SDK_STORAGE_AUTHORIZATION;
}

static const struct sdk_resource *service_language_resource(void)
{
	for (size_t i = 0; i < app.resource_count; ++i)
		if (app.resources[i].name_bytes == 8 &&
		    !strncasecmp(app.resources[i].name, "lang.bin", 8))
			return &app.resources[i];
	return NULL;
}
/* The original lang.bin stores {id, body_bytes}, followed by language records
 * {language_id, text_bytes, UCS2 text}. Both levels end with {0, 0}; lengths
 * include the text terminator. Integer fields need not be four-byte aligned. */
static int service_language_lookup(const uint8_t *data, size_t bytes,
				   int32_t resource_id, int32_t language_id,
				   size_t *text_offset, uint32_t *text_bytes)
{
	size_t cursor = 0, records = 0;
	*text_offset = 0;
	*text_bytes = 0;
	while (bytes - cursor >= 8) {
		uint32_t id = read32(data + cursor);
		uint32_t body_bytes = read32(data + cursor + 4);
		cursor += 8;
		if (!id)
			return !body_bytes && cursor == bytes;
		if (++records > RESOURCE_CAP || id > INT32_MAX ||
		    body_bytes < 8 || body_bytes > bytes - cursor)
			return 0;
		size_t end = cursor + body_bytes;
		unsigned languages = 0;
		int terminated = 0;
		while (end - cursor >= 8) {
			uint32_t language = read32(data + cursor);
			uint32_t length = read32(data + cursor + 4);
			cursor += 8;
			if (!language) {
				if (length || cursor != end)
					return 0;
				terminated = 1;
				break;
			}
			if (language > 3 || (languages & (1u << language)) ||
			    length < 2 || (length & 1) ||
			    length > end - cursor || length > INT32_MAX ||
			    data[cursor + length - 2] ||
			    data[cursor + length - 1])
				return 0;
			languages |= 1u << language;
			if ((int32_t)id == resource_id &&
			    (int32_t)language == language_id) {
				if (*text_bytes)
					return 0;
				*text_offset = cursor;
				*text_bytes = length;
			}
			cursor += length;
		}
		if (!terminated)
			return 0;
	}
	return 0;
}
char *MRE_ABI host_mul_lang_init(void)
{
	check_guest_api_stack();
	const struct sdk_resource *resource = service_language_resource();
	if (!resource || resource->bytes > INT32_MAX)
		return NULL;
	size_t offset;
	uint32_t length;
	const uint8_t *source = app.ads.resources + resource->offset;
	if (!service_language_lookup(source, resource->bytes, 0, 1, &offset,
				     &length))
		return NULL;
	char *handle = host_calloc((int32_t)resource->bytes);
	if (!handle)
		return NULL;
	memcpy(handle, source, resource->bytes);
	fprintf(stderr,
		"LANGUAGE vm_mul_lang_init() -> %p original lang.bin, bytes=%u\n",
		handle, resource->bytes);
	return handle;
}
void MRE_ABI host_mul_lang_deinit(char *handle)
{
	check_guest_api_stack();
	if (handle)
		host_free(handle);
}
int32_t MRE_ABI host_mul_lang_get_string(char *handle, int32_t resource_id,
					 int32_t language_id, char **text)
{
	check_guest_api_stack();
	if (((uintptr_t)text & 3) || !guest_data(text, sizeof(*text), PF_W))
		fail("vm_mul_lang_get_string output bounds/alignment");
	*text = NULL;
	const struct sdk_resource *resource = service_language_resource();
	size_t bytes = 0;
	if (!resource || resource_id <= 0 || language_id < 1 ||
	    language_id > 3 ||
	    mre_memory_owner(&app.memory, handle, &bytes) != handle ||
	    bytes != resource->bytes || !guest_data(handle, bytes, PF_R))
		return 0;
	size_t offset;
	uint32_t length;
	if (!service_language_lookup((const uint8_t *)handle, bytes,
				     resource_id, language_id, &offset,
				     &length) ||
	    !length)
		return 0;
	*text = handle + offset;
	debug_trace("LANGUAGE id=%d lang=%d -> original UCS2 bytes=%u\n",
		    resource_id, language_id, length);
	return (int32_t)length;
}

typedef uint8_t *(MRE_ABI *sdk_res_provider_fn)(int32_t, int32_t *);
static sdk_res_provider_fn service_res_provider;
/* engine.c dispatches on a separate bounded guest stack region and restores
 * the suspended ADS API stack pointer after any nested guest SDK calls. */
static uint32_t call_native_service_provider(uintptr_t callback,
					     int32_t resource_id,
					     int32_t *bytes);
void MRE_ABI host_reg_res_provider(sdk_res_provider_fn callback)
{
	check_guest_api_stack();
	if (callback && !guest_code((uintptr_t)callback))
		fail("vm_reg_res_provider callback outside guest executable segments");
	service_res_provider = callback;
	debug_trace("RESOURCE provider=%p registered by original guest\n",
		    (void *)(uintptr_t)callback);
}
int32_t MRE_ABI host_midi_play_resource(int32_t resource_id, int32_t repeat,
					event_fn callback)
{
	check_guest_api_stack();
	if (resource_id <= 0 || (callback && !guest_code((uintptr_t)callback)))
		return -1;
	if (service_res_provider) {
		int32_t *bytes = host_calloc(sizeof(*bytes));
		if (!bytes)
			return -1;
		uint8_t *data =
			(uint8_t *)(uintptr_t)call_native_service_provider(
				(uintptr_t)service_res_provider, resource_id,
				bytes);
		if (mre_memory_owner(&app.memory, bytes, NULL) != bytes)
			fail("resource provider released its SDK length output");
		int32_t length = *bytes;
		host_free(bytes);
		if (!data || length <= 0 || length > (int32_t)FILE_CAP ||
		    !guest_data(data, (size_t)length, PF_R))
			return -1;
		debug_trace(
			"RESOURCE MIDI provider id=%d -> %p bytes=%d original guest buffer\n",
			resource_id, data, length);
	}
	fprintf(stderr,
		"AUDIO vm_midi_play(id=%d,repeat=%d) -> -1 VM_MIDI_FAILED (no MIDI device)\n",
		resource_id, repeat);
	return -1;
}
int32_t MRE_ABI host_midi_pause(int32_t handle)
{
	check_guest_api_stack();
	debug_trace("AUDIO vm_midi_pause(%d) -> -1 (no live MIDI handle)\n",
		    handle);
	return -1;
}
int32_t MRE_ABI host_midi_resume(int32_t handle)
{
	check_guest_api_stack();
	debug_trace("AUDIO vm_midi_resume(%d) -> -1 (no live MIDI handle)\n",
		    handle);
	return -1;
}
void MRE_ABI host_midi_stop_all(void)
{
	check_guest_api_stack();
	debug_trace("AUDIO vm_midi_stop_all(); no live MIDI handles\n");
}
typedef void(MRE_ABI *sdk_audio_callback_fn)(int32_t);
int32_t MRE_ABI host_audio_play_bytes(void *data, uint32_t bytes,
				      uint8_t format, uint32_t start_time,
				      uint32_t path,
				      sdk_audio_callback_fn callback)
{
	check_guest_api_stack();
	if (!bytes || bytes > FILE_CAP || !guest_data(data, bytes, PF_R) ||
	    (callback && !guest_code((uintptr_t)callback)))
		return -1;
	fprintf(stderr,
		"AUDIO vm_audio_play_bytes(bytes=%u,format=%u,start=%u,path=%u) -> -1 VM_AUDIO_FAILED (no audio device)\n",
		bytes, format, start_time, path);
	return -1;
}
int32_t MRE_ABI host_audio_stop(sdk_audio_callback_fn callback)
{
	check_guest_api_stack();
	if (callback && !guest_code((uintptr_t)callback))
		return -1;
	debug_trace(
		"AUDIO vm_audio_stop() -> -1 VM_AUDIO_FAILED (no live playback)\n");
	return -1;
}
