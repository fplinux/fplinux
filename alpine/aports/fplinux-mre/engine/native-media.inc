/* SPDX-License-Identifier: 0BSD */
/* Firmware-style media adapters with a parent-supplied cyclic virtual input:
 * mono, signed little-endian PCM16 at 16000 Hz. Only the caller's owned SDK
 * ring is exposed to the guest. Include after guest-memory/API definitions. */
#define MEDIA_SAMPLE_RATE 16000u
#define MEDIA_DISPATCH_MS 16u
#define MEDIA_RING_MAX_WORDS 2048u

enum {
	MEDIA_SUCCESS = 200,
	MEDIA_FAIL = 201,
	MEDIA_BAD_FORMAT = 204,
	MEDIA_BAD_PARAMETER = 205,
	MEDIA_PCM_16K = 8,
	MEDIA_DATA_NOTIFICATION = 2,
	MEDIA_READ_ERROR = 15
};
/* These original enums have -fshort-enums ABI and fit in one byte. */
typedef void(MRE_ABI *media_notify_fn)(uint8_t);
static struct {
	uint16_t *buffer;
	uint32_t capacity, read_cursor, available, offered;
	uint8_t *fixture;
	uint32_t fixture_words, fixture_cursor;
	media_notify_fn callback;
	uint64_t sampled_ms, next_ms, copied_words, dropped_words;
	int recording;
} media_state;

static int media_buffer_owned(void)
{
	size_t bytes = (size_t)media_state.capacity * sizeof(uint16_t);
	return media_state.buffer && media_state.capacity &&
	       mre_memory_owner(&app.memory, media_state.buffer, NULL) ==
		       media_state.buffer &&
	       mre_memory_contains(&app.memory, media_state.buffer, bytes);
}
static void media_stop_capture(void)
{
	free(media_state.fixture);
	media_state.fixture = NULL;
	media_state.fixture_words = media_state.fixture_cursor = 0;
	media_state.read_cursor = media_state.available = media_state.offered =
		0;
	media_state.recording = 0;
	media_state.callback = NULL;
}
static void media_cleanup(void)
{
	/* This is host-stack cleanup, not a guest vm_free invocation. The SDK
	 * ring remains the original's property and is never freed here. */
	media_stop_capture();
	memset(&media_state, 0, sizeof(media_state));
}
void MRE_ABI media_setbuffer(uint16_t *buffer, uint32_t count_words)
{
	check_guest_api_stack();
	media_cleanup();
	if (!buffer || ((uintptr_t)buffer & 1) || !count_words ||
	    count_words > MEDIA_RING_MAX_WORDS ||
	    mre_memory_owner(&app.memory, buffer, NULL) != buffer ||
	    !mre_memory_contains(&app.memory, buffer,
				 (size_t)count_words * sizeof(*buffer)))
		return;
	media_state.buffer = buffer;
	media_state.capacity = count_words;
}
uint8_t MRE_ABI media_record(uint8_t format, media_notify_fn handler,
			     void *param)
{
	check_guest_api_stack();
	if (format != MEDIA_PCM_16K)
		return MEDIA_BAD_FORMAT;
	if (app.exited || media_state.recording || !media_buffer_owned() ||
	    !guest_code((uintptr_t)handler) ||
	    (param && !guest_data(param, 1, PF_R)))
		return MEDIA_BAD_PARAMETER;
	if (!app.shared || !app.shared->media_bytes) {
		fprintf(stderr,
			"MEDIA no declared virtual PCM input; record failed\n");
		return MEDIA_FAIL;
	}
	uint32_t bytes = app.shared->media_bytes;
	if ((bytes & 1) || bytes > sizeof(app.shared->media_data))
		return MEDIA_BAD_PARAMETER;
	/* Freeze the parent's fixture at Record; subsequent shared media metadata
	 * cannot enlarge the source range. This host snapshot is never guest data. */
	media_state.fixture = malloc(bytes);
	if (!media_state.fixture)
		return MEDIA_FAIL;
	memcpy(media_state.fixture, app.shared->media_data, bytes);
	media_state.fixture_words = bytes / sizeof(uint16_t);
	media_state.fixture_cursor = 0;
	media_state.read_cursor = media_state.available = media_state.offered =
		0;
	media_state.callback = handler;
	media_state.copied_words = media_state.dropped_words = 0;
	media_state.sampled_ms = monotonic_ms();
	media_state.next_ms = media_state.sampled_ms + MEDIA_DISPATCH_MS;
	media_state.recording = 1;
	fprintf(stderr,
		"MEDIA virtual cyclic PCM fixture bytes=%u; mono S16_LE 16000Hz; caller ring=%u words; no hardware microphone\n",
		bytes, media_state.capacity);
	return MEDIA_SUCCESS;
}
void MRE_ABI media_getreadbuffer(uint16_t **buffer, uint32_t *count_words)
{
	check_guest_api_stack();
	if (!buffer || !count_words || ((uintptr_t)buffer & 3) ||
	    ((uintptr_t)count_words & 3) ||
	    !guest_data(buffer, sizeof(*buffer), PF_W) ||
	    !guest_data(count_words, sizeof(*count_words), PF_W))
		fail("MEDIA GetReadBuffer outputs are outside writable guest memory");
	*buffer = NULL;
	*count_words = 0;
	media_state.offered = 0;
	if (!media_state.recording || !media_state.available ||
	    !media_buffer_owned())
		return;
	uint32_t words = media_state.capacity - media_state.read_cursor;
	if (words > media_state.available)
		words = media_state.available;
	*buffer = media_state.buffer + media_state.read_cursor;
	*count_words = media_state.offered = words;
}
void MRE_ABI media_readdatadone(uint32_t count_words)
{
	check_guest_api_stack();
	/* Consume only the range most recently returned by GetReadBuffer. An
	 * oversized acknowledgement must never advance beyond buffered samples. */
	if (!media_state.recording || !media_buffer_owned() ||
	    count_words > media_state.offered)
		return;
	media_state.read_cursor =
		(media_state.read_cursor + count_words) % media_state.capacity;
	media_state.available -= count_words;
	media_state.offered -= count_words;
}
void MRE_ABI media_stop(void)
{
	check_guest_api_stack();
	if (media_state.recording)
		fprintf(stderr,
			"MEDIA stop virtual PCM copied=%" PRIu64
			" words dropped=%" PRIu64 " words\n",
			media_state.copied_words, media_state.dropped_words);
	media_stop_capture();
}
static int media_timeout_ms(uint64_t now)
{
	if (!media_state.recording || !app.visible || app.exited)
		return -1;
	uint64_t delay = media_state.next_ms > now ? media_state.next_ms - now :
						     0;
	return delay > INT32_MAX ? INT32_MAX : (int)delay;
}
static void media_dispatch_due(void)
{
	uint64_t now = monotonic_ms();
	/* Never re-enter the original from a media API or other guest callback. */
	if (app.phase || media_timeout_ms(now) != 0)
		return;
	media_notify_fn callback = media_state.callback;
	uint8_t event = MEDIA_DATA_NOTIFICATION;
	if (!media_buffer_owned()) {
		media_stop_capture();
		event = MEDIA_READ_ERROR;
	} else {
		/* 16 samples per elapsed millisecond is exact for the declared 16kHz
		 * source. A late dispatch copies at most available ring space; elapsed
		 * source samples still advance, with overflow explicitly counted. */
		uint64_t elapsed = now - media_state.sampled_ms;
		uint64_t produced = elapsed * (MEDIA_SAMPLE_RATE / 1000u);
		uint32_t space = media_state.capacity - media_state.available;
		uint32_t copied = produced < space ? (uint32_t)produced : space;
		uint32_t write_cursor =
			(media_state.read_cursor + media_state.available) %
			media_state.capacity;
		for (uint32_t i = 0; i < copied; ++i) {
			uint32_t source = media_state.fixture_cursor * 2u;
			media_state.buffer[write_cursor] =
				(uint16_t)media_state.fixture[source] |
				(uint16_t)media_state.fixture[source + 1] << 8;
			write_cursor =
				(write_cursor + 1) % media_state.capacity;
			media_state.fixture_cursor =
				(media_state.fixture_cursor + 1) %
				media_state.fixture_words;
		}
		media_state.fixture_cursor =
			(uint32_t)((media_state.fixture_cursor +
				    (produced - copied) %
					    media_state.fixture_words) %
				   media_state.fixture_words);
		media_state.available += copied;
		media_state.copied_words += copied;
		media_state.dropped_words += produced - copied;
		media_state.sampled_ms = now;
		media_state.next_ms = now + MEDIA_DISPATCH_MS;
		if (!copied)
			return;
		debug_trace(
			"MEDIA copied actual PCM words=%u available=%u fixture_cursor=%u dropped=%" PRIu64
			"\n",
			copied, media_state.available,
			media_state.fixture_cursor, media_state.dropped_words);
	}
	/* Save all values before entering: this callback may stop or free its ring. */
	if (!callback || !guest_code((uintptr_t)callback))
		return;
	app.phase = 2;
	callback_call((uintptr_t)callback, event, 0, 0, 0);
	app.phase = 0;
}
static uintptr_t media_resolve(const char *name)
{
	/* These APIs resolve even without input; Record reports backend failure. */
#define MEDIA_EXPORT(symbol, function) \
	if (!strcmp(name, symbol))     \
		return (uintptr_t)function;
	MEDIA_EXPORT("mremu_media_setbufer", media_setbuffer)
	MEDIA_EXPORT("mremu_media_record", media_record)
	MEDIA_EXPORT("mremu_media_getreadbuffer", media_getreadbuffer)
	MEDIA_EXPORT("mremu_media_readdatadone", media_readdatadone)
	MEDIA_EXPORT("mremu_media_stop", media_stop)
#undef MEDIA_EXPORT
	return 0;
}
