/* SPDX-License-Identifier: 0BSD */
/* fplinux-check: package-embedded
 * ARM PCS and musl signal context require the actual target headers. */
/* Isolated interactive engine for native ARM VXP images.
 * Native code shares this process memory; seccomp limits its Linux capabilities. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/memfd.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <stddef.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "mre-ipc.h"
#include "vfs-broker.h"
#include "text-codec.h"
#include "ads/ads_image.h"
#include "gif/gif-image.h"
#include "gif/resize-image.h"
#include "image/image-decode.h"
#include "memory/mre-memory.h"
#include <fplinux/fplinux-font.h>

#ifndef __arm__
#error This runner must execute as a real ARM process.
#endif
#define MRE_ABI __attribute__((pcs("aapcs")))
#define FILE_CAP MRE_EXECUTABLE_BYTES
#define IMAGE_CAP (8u * 1024u * 1024u)
#define STACK_SIZE (64u * 1024u)
#define SEGMENT_CAP 16u
#define EDITOR_CHARS MRE_EDITOR_UNITS
#define WIRE_LINE_BYTES 1024u
#define PROTOCOL_CAP 16384u
#define TIMER_CAP 10u
#define ALL_TIMER_CAP (2u * TIMER_CAP)
#define RESOURCE_CAP 1024u
#define LAYER_CAP 8u

typedef void(MRE_ABI *input_fn)(int32_t, uint16_t *);
typedef void(MRE_ABI *event_fn)(int32_t, int32_t);
typedef void(MRE_ABI *pen_fn)(int32_t, int32_t, int32_t);
typedef void(MRE_ABI *timer_fn)(int32_t);
typedef int32_t(MRE_ABI *selector_fn)(uint16_t *, int32_t);
extern uintptr_t ads_active_frame, ads_active_api_sp;
#include "ads-host.h"
extern uint32_t arm_guest_call(uintptr_t fn, void *stack_top, uint32_t r0,
			       uint32_t r1, uint32_t r2);
extern uint32_t arm_guest_call4(uintptr_t fn, void *stack_top, uint32_t r0,
				uint32_t r1, uint32_t r2, uint32_t r3);
struct segment {
	uint32_t addr, size, flags;
};
struct sdk_timer {
	timer_fn callback;
	uint32_t interval_ms;
	uint64_t next_ms;
};
struct sdk_resource {
	const char *name;
	size_t name_bytes;
	uint32_t offset, bytes;
};
struct sdk_img_property {
	int32_t width, height;
};
_Static_assert(sizeof(struct sdk_img_property) == 8, "SDK image property ABI");
struct sdk_frame_property {
	uint8_t flag;
	uint16_t left, top, width, height, delay_time;
	uint8_t trans_color_index;
	uint16_t trans_color;
	uint32_t offset;
};
_Static_assert(sizeof(struct sdk_frame_property) == 20 &&
		       offsetof(struct sdk_frame_property, width) == 6 &&
		       offsetof(struct sdk_frame_property, height) == 8 &&
		       offsetof(struct sdk_frame_property, offset) == 16,
	       "SDK frame property ABI");
struct sdk_canvas {
	struct sdk_canvas *next;
	uint8_t *buffer, *alpha;
	size_t bytes, mapping_bytes, alpha_bytes;
	struct sdk_frame_property property;
};
struct sdk_layer {
	uint8_t *buffer;
	uint16_t *pixels;
	int live, transparent, opacity;
};
static struct {
	uint8_t *image, *stack_map, *stack_low, *stack_top;
	size_t image_size, stack_mapping_size, page_size;
	struct segment segments[SEGMENT_CAP];
	size_t segment_count;
	uint32_t ram_limit;
	uintptr_t entry;
	unsigned stack_api_checks, editors, file_writes, format_calls;
	int exited, pending;
	int32_t editor_max, editor_method;
	input_fn input_callback;
	uint16_t *input_text;
	char editor_title[256], editor_default[MRE_EDITOR_BYTES + 1U];
	size_t input_text_bytes;
	int phase;
	int ads_abi, screen_width, screen_height;
	char exec_path[MRE_VFS_PATH_BYTES];
	struct ads_image ads;
	struct mre_memory memory;
	struct sdk_canvas *canvases;
	size_t canvas_bytes;
	uintptr_t canvas_hint, retired_canvases[256];
	size_t retired_canvas_cursor;
	struct sdk_timer timers[ALL_TIMER_CAP];
	struct sdk_resource resources[RESOURCE_CAP];
	size_t resource_count;
	uint8_t *string_source, *strings_live;
	size_t string_source_bytes, string_count;
	uint32_t string_ids[RESOURCE_CAP], string_offsets[RESOURCE_CAP];
	char game_language[3];
	event_fn system_callback, key_callback;
	pen_fn pen_callback;
	selector_fn selector_callback;
	struct mre_shared *shared;
	struct fplinux_font font;
	int font_size, font_bold, font_italic, font_underline;
	struct sdk_layer layers[LAYER_CAP];
	uint32_t draw_color[2]; /* SDK global RGB565 and RGB888 values. */
	int active_layer, alpha_layer, frame_pending, paint_pending;
	int selector_pending, visible;
	int clip_x, clip_y, clip_width, clip_height;
	uint32_t frame_sequence;
	int editor_resuming;
	char deferred[32][WIRE_LINE_BYTES];
	unsigned deferred_count;
	uint32_t vfs_sequence;
} app;
static int free_canvas_pointer(void *pointer);
struct pixel_surface {
	uint16_t *pixels;
	int width, height, transparent;
	uint8_t *alpha;
};
static int owned_surface(uint8_t *, struct pixel_surface *);
static void handheld_configure_owned_layer(int32_t, int32_t, int32_t, int32_t,
					   int32_t);
static int handheld_layer_surface(int32_t, struct pixel_surface *);
static int handheld_screen_pixel(int32_t, int32_t, int32_t, uint16_t *);
static uintptr_t adapt_api_address(uintptr_t);
static void handheld_forget_layer(int32_t handle);
static uint8_t *handheld_layer_buffer(int32_t handle);
static void call_event(event_fn callback, int32_t first, int32_t second,
		       const char *kind);
static uint16_t blend565(uint16_t source, uint16_t destination, unsigned alpha);
static int service_guest_data(const void *pointer, size_t bytes);
static void service_memory_refresh(void);
static void service_memory_allocation(int32_t bytes, const void *result);
static void service_memory_free_call(void);
static int utility_vfs_exchange(const struct mre_vfs_request *request,
				struct mre_vfs_response *response);
static void native_http_cancel_all(void);
static int native_http_receive_response(uint32_t id);
static void utility_http_accept(uint32_t id);
static int native_http_response_pending(void);
static void native_http_dispatch_response(void);
static int receive_viewer_line(char line[static WIRE_LINE_BYTES]);
static int utility_charset(int32_t source, int32_t destination,
			   const char *text, uint16_t *out, int32_t bytes);
static int trace_enabled;
static void debug_trace(const char *format, ...)
	__attribute__((format(printf, 1, 2)));
static void debug_trace(const char *format, ...)
{
	if (!trace_enabled)
		return;
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}
static int guest_uses_ads_abi(void)
{
	return app.ads_abi;
}
static int guest_screen_width(void)
{
	return app.screen_width;
}
static int guest_screen_height(void)
{
	return app.screen_height;
}
static size_t layer_bytes(void)
{
	return (size_t)guest_screen_width() * guest_screen_height() *
	       sizeof(uint16_t);
}
struct display_viewport {
	int x, y, width, height;
};
static struct display_viewport display_viewport(void)
{
	int width =
		MRE_FRAME_HEIGHT * guest_screen_width() / guest_screen_height();
	if (width > MRE_FRAME_WIDTH) {
		int height = MRE_FRAME_WIDTH * guest_screen_height() /
			     guest_screen_width();
		return (struct display_viewport){ 0,
						  (MRE_FRAME_HEIGHT - height) /
							  2,
						  MRE_FRAME_WIDTH, height };
	}
	return (struct display_viewport){ (MRE_FRAME_WIDTH - width) / 2, 0,
					  width, MRE_FRAME_HEIGHT };
}

static void write_protocol(const char *line, size_t length)
{
	if (length > PROTOCOL_CAP)
		_exit(65);
	while (length) {
		ssize_t count = write(STDOUT_FILENO, line, length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			_exit(0); /* The parent closed its pipe. */
		line += count;
		length -= (size_t)count;
	}
}
static void hex_encode(char *out, const char *text, size_t length)
{
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < length; ++i) {
		unsigned byte = (uint8_t)text[i];
		out[2 * i] = digits[byte >> 4];
		out[2 * i + 1] = digits[byte & 15];
	}
}
static void fail(const char *format, ...) __attribute__((noreturn));
static void fail(const char *format, ...)
{
	char message[512], line[1032];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	fprintf(stderr, "REJECT: %s\n", message);
	size_t length = strlen(message);
	memcpy(line, "ERROR\t", 6);
	hex_encode(line + 6, message, length);
	line[6 + 2 * length] = '\n';
	write_protocol(line, 7 + 2 * length);
	_exit(65);
}
static char *fault_field(char *out, const char *name, uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	while (*name)
		*out++ = *name++;
	*out++ = '=';
	*out++ = '0';
	*out++ = 'x';
	for (int shift = 28; shift >= 0; shift -= 4)
		*out++ = digits[(value >> shift) & 15];
	*out++ = ' ';
	return out;
}
static void guest_fault(int signal_number, siginfo_t *info, void *context)
{
	const ucontext_t *state = context;
	const mcontext_t *registers = &state->uc_mcontext;
	uint32_t pc = (uint32_t)registers->arm_pc;
	uint32_t lr = (uint32_t)registers->arm_lr;
	uint32_t base = (uint32_t)(uintptr_t)app.image;
	uint32_t ro_pc = pc >= base && pc - base < app.ads.info.ro_size ?
				 pc - base :
				 UINT32_MAX;
	uint32_t ro_lr = lr >= base && lr - base < app.ads.info.ro_size ?
				 lr - base :
				 UINT32_MAX;
	char line[768], *out = line;
	*out++ = 'F';
	*out++ = 'A';
	*out++ = 'U';
	*out++ = 'L';
	*out++ = 'T';
	*out++ = ' ';
	out = fault_field(out, "sig", (uint32_t)signal_number);
	out = fault_field(out, "addr", (uint32_t)(uintptr_t)info->si_addr);
	out = fault_field(out, "pc", pc);
	out = fault_field(out, "lr", lr);
	out = fault_field(out, "sp", (uint32_t)registers->arm_sp);
	out = fault_field(out, "r9", (uint32_t)registers->arm_r9);
	out = fault_field(out, "r0", (uint32_t)registers->arm_r0);
	out = fault_field(out, "r1", (uint32_t)registers->arm_r1);
	out = fault_field(out, "r2", (uint32_t)registers->arm_r2);
	out = fault_field(out, "r3", (uint32_t)registers->arm_r3);
	out = fault_field(out, "r4", (uint32_t)registers->arm_r4);
	out = fault_field(out, "r5", (uint32_t)registers->arm_r5);
	out = fault_field(out, "ro_pc", ro_pc);
	out = fault_field(out, "ro_lr", ro_lr);
	out = fault_field(out, "phase", (uint32_t)app.phase);
	out = fault_field(out, "pool_live", (uint32_t)app.memory.count);
	out = fault_field(out, "pool_cap", (uint32_t)app.memory.capacity);
	out = fault_field(out, "pool_bytes", (uint32_t)app.memory.bytes);
	*out++ = '\n';
	char *cursor = line;
	size_t remaining = (size_t)(out - line);
	while (remaining) {
		ssize_t written = write(STDERR_FILENO, cursor, remaining);
		if (written <= 0)
			break;
		cursor += written;
		remaining -= (size_t)written;
	}
	_exit(128 + signal_number);
}
static void prepare_fault_diagnostic(void)
{
	/* Keep this guarded alternate stack until process exit. Reconfiguring
	 * signal state after entry is outside the strict seccomp policy. */
	uint8_t *mapping = mmap(NULL, STACK_SIZE + 2 * app.page_size, PROT_NONE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED ||
	    mprotect(mapping + app.page_size, STACK_SIZE,
		     PROT_READ | PROT_WRITE))
		fail("preparing the fault diagnostic alternate stack failed");
	stack_t stack = { .ss_sp = mapping + app.page_size,
			  .ss_size = STACK_SIZE };
	struct sigaction action = { .sa_sigaction = guest_fault,
				    .sa_flags = SA_SIGINFO | SA_ONSTACK |
						SA_RESETHAND };
	if (sigemptyset(&action.sa_mask) || sigaltstack(&stack, NULL) ||
	    sigaction(SIGSEGV, &action, NULL))
		fail("installing the pre-entry fault diagnostic failed");
}

static int within(uint32_t offset, uint32_t length, size_t total)
{
	return offset <= total && length <= total - offset;
}
static uint32_t read32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static int in_segment(uint32_t offset, uint32_t length, uint32_t flags)
{
	for (size_t i = 0; i < app.segment_count; ++i) {
		const struct segment *s = &app.segments[i];
		if ((s->flags & flags) == flags && offset >= s->addr &&
		    within(offset - s->addr, length, s->size))
			return 1;
	}
	return 0;
}
static int inside_pointer(const void *p, size_t length, const void *base,
			  size_t size)
{
	uintptr_t address = (uintptr_t)p, low = (uintptr_t)base;
	return address >= low && address - low <= size &&
	       length <= size - (address - low);
}
static int guest_data(const void *p, size_t length, uint32_t flags)
{
	if (!(flags & PF_W) && service_guest_data(p, length))
		return 1;
	if (!(flags & PF_W) && inside_pointer(p, length, app.game_language,
					      sizeof(app.game_language)))
		return 1;
	if (mre_memory_contains(&app.memory, p, length))
		return 1;
	for (const struct sdk_canvas *canvas = app.canvases; canvas;
	     canvas = canvas->next) {
		if (inside_pointer(p, length, canvas->buffer, canvas->bytes))
			return 1;
		if (!(flags & PF_W) &&
		    inside_pointer(p, length, &canvas->property,
				   sizeof(canvas->property)))
			return 1;
	}
	if ((uintptr_t)p >= (uintptr_t)app.image &&
	    (uintptr_t)p - (uintptr_t)app.image <= UINT32_MAX &&
	    in_segment((uint32_t)((uintptr_t)p - (uintptr_t)app.image), length,
		       flags))
		return 1;
	if (inside_pointer(p, length, app.stack_low, STACK_SIZE))
		return 1;
	if (app.input_text &&
	    inside_pointer(p, length, app.input_text, app.input_text_bytes))
		return 1;
	for (size_t i = 0; i < LAYER_CAP; ++i)
		if (app.layers[i].live &&
		    inside_pointer(p, length, app.layers[i].buffer,
				   32 + layer_bytes()))
			return 1;
	return 0;
}
static void check_guest_api_stack(void)
{
	uintptr_t sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	if (guest_uses_ads_abi()) {
		if (!app.phase || !ads_active_frame ||
		    (!inside_pointer((void *)ads_active_api_sp, 40,
				     app.stack_low, STACK_SIZE) &&
		     !mre_memory_contains(&app.memory,
					  (void *)ads_active_api_sp, 40)) ||
		    sp >= ads_active_frame ||
		    ads_active_frame - sp > STACK_SIZE)
			fail("ADS API called outside the active aligned bridge");
		app.stack_api_checks++;
		return;
	}
	if (!app.phase ||
	    (!inside_pointer((void *)sp, 4, app.stack_low, STACK_SIZE) &&
	     !(!(sp & 7) && mre_memory_contains(&app.memory, (void *)sp, 8))))
		fail("host API was called outside the managed guest stack");
	app.stack_api_checks++;
}
static int guest_code(uintptr_t address)
{
	uintptr_t code = address & ~(uintptr_t)1;
	return code >= (uintptr_t)app.image &&
	       code - (uintptr_t)app.image <= UINT32_MAX &&
	       in_segment((uint32_t)(code - (uintptr_t)app.image), 2, PF_X);
}

void *MRE_ABI host_calloc(int32_t bytes)
{
	check_guest_api_stack();
	void *pointer = NULL;
	if (bytes > 0 && app.canvas_bytes <= app.ram_limit &&
	    app.memory.bytes <= app.ram_limit - app.canvas_bytes &&
	    (size_t)bytes <=
		    app.ram_limit - app.canvas_bytes - app.memory.bytes)
		pointer = mre_memory_calloc(&app.memory, (size_t)bytes);
	service_memory_allocation(bytes, pointer);
	debug_trace("ALLOC vm_calloc(%d) -> %p live=%zu/%zu\n", bytes, pointer,
		    app.memory.bytes, app.memory.limit);
	return pointer;
}
void *MRE_ABI host_malloc(int32_t bytes)
{
	return host_calloc(bytes);
}

void MRE_ABI host_free(void *pointer)
{
	check_guest_api_stack();
	service_memory_free_call();
	if (!pointer)
		return;
	if (free_canvas_pointer(pointer)) {
		service_memory_refresh();
		return;
	}
	uintptr_t address = (uintptr_t)pointer;
	size_t owner_bytes = 0, released_bytes = 0;
	void *owner = mre_memory_owner(&app.memory, pointer, &owner_bytes);
	enum mre_memory_release result =
		mre_memory_free(&app.memory, pointer, &released_bytes);
	if (result == MRE_MEMORY_RELEASED) {
		service_memory_refresh();
		debug_trace("FREE vm_free(0x%" PRIxPTR
			    ") bytes=%zu live=%zu slots=%zu\n",
			    address, released_bytes, app.memory.bytes,
			    app.memory.count);
		return;
	}
	uintptr_t return_address =
		guest_uses_ads_abi() ?
			*(const uint32_t *)(ads_active_api_sp + 36) :
			0;
	uintptr_t return_offset =
		guest_code(return_address) ?
			return_address - (uintptr_t)app.image :
			UINT32_MAX;
	if (result == MRE_MEMORY_INTERIOR)
		fail("vm_free interior pointer=%p allocation=%p bytes=%zu guest_return_RO=0x%" PRIxPTR,
		     pointer, owner, owner_bytes, return_offset);
	if (result == MRE_MEMORY_RETIRED) {
		debug_trace(
			"FREE known retired SDK pointer=%p -> no-op guest_return_RO=0x%" PRIxPTR
			"\n",
			pointer, return_offset);
		return;
	}
	fail("vm_free unowned pointer=%p GMobi_language=%p guest_return_RO=0x%" PRIxPTR,
	     pointer, (void *)app.game_language, return_offset);
}

void *MRE_ABI host_realloc(void *pointer, int32_t bytes)
{
	check_guest_api_stack();
	size_t old_bytes = 0;
	if (pointer &&
	    mre_memory_owner(&app.memory, pointer, &old_bytes) != pointer)
		fail("vm_realloc pointer was not allocated by this SDK pool");
	void *replacement = NULL;
	if (bytes > 0 &&
	    (size_t)bytes <= app.ram_limit - app.canvas_bytes -
				     (app.memory.bytes - old_bytes))
		replacement = mre_memory_realloc(&app.memory, pointer, bytes);
	service_memory_allocation(bytes, replacement);
	debug_trace(
		"ALLOC vm_realloc oldbytes=%zu newbytes=%d -> %p live=%zu/%zu slots=%zu/%zu\n",
		old_bytes, bytes, replacement, app.memory.bytes,
		app.memory.limit, app.memory.count, app.memory.capacity);
	return replacement;
}

static uint64_t monotonic_ms(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0)
		fail("reading the monotonic timer clock failed");
	return (uint64_t)now.tv_sec * 1000 + (uint32_t)now.tv_nsec / 1000000;
}
int32_t MRE_ABI host_tick_count(void)
{
	check_guest_api_stack();
	return (int32_t)(uint32_t)monotonic_ms();
}
struct sdk_time {
	int32_t year, mon, day, hour, min, sec;
};
_Static_assert(sizeof(struct sdk_time) == 24, "SDK vm_time_t ABI");
int32_t MRE_ABI host_get_time(struct sdk_time *out)
{
	check_guest_api_stack();
	if (!guest_data(out, sizeof(*out), PF_W) || ((uintptr_t)out & 3))
		fail("vm_get_time output bounds/alignment");
	struct timespec now;
	struct tm calendar;
	if (clock_gettime(CLOCK_REALTIME, &now) ||
	    !gmtime_r(&now.tv_sec, &calendar))
		return -1;
	*out = (struct sdk_time){ calendar.tm_year + 1900, calendar.tm_mon + 1,
				  calendar.tm_mday,	   calendar.tm_hour,
				  calendar.tm_min,	   calendar.tm_sec };
	fprintf(stderr, "TIME vm_get_time UTC=%04d-%02d-%02d %02d:%02d:%02d\n",
		out->year, out->mon, out->day, out->hour, out->min, out->sec);
	return 0;
}
static int32_t create_timer(uint32_t interval_ms, timer_fn callback,
			    int background)
{
	if (!interval_ms || !guest_code((uintptr_t)callback))
		return -1;
	size_t first = background ? TIMER_CAP : 0;
	for (size_t i = first; i < first + TIMER_CAP; ++i) {
		if (app.timers[i].callback)
			continue;
		app.timers[i] =
			(struct sdk_timer){ callback, interval_ms,
					    monotonic_ms() + interval_ms };
		fprintf(stderr,
			"TIMER create id=%zu interval=%ums background=%d callback=%p queued until guest returns\n",
			i + 1, interval_ms, background, (void *)callback);
		return (int32_t)i + 1;
	}
	return -5; /* SDK VM_TIMER_MTK_TIMER_NO_FREE: ten timers are live. */
}
int32_t MRE_ABI host_create_timer(uint32_t interval_ms, timer_fn callback)
{
	check_guest_api_stack();
	return create_timer(interval_ms, callback, 0);
}
int32_t MRE_ABI host_create_timer_ex(uint32_t interval_ms, timer_fn callback)
{
	check_guest_api_stack();
	return create_timer(interval_ms, callback, 1);
}
int32_t MRE_ABI host_delete_timer(int32_t handle)
{
	check_guest_api_stack();
	if (handle <= 0 || handle > (int32_t)TIMER_CAP ||
	    !app.timers[handle - 1].callback)
		return -1;
	app.timers[handle - 1] = (struct sdk_timer){ 0 };
	fprintf(stderr, "TIMER delete id=%d\n", handle);
	return 0;
}
int32_t MRE_ABI host_delete_timer_ex(int32_t handle)
{
	check_guest_api_stack();
	if (handle <= (int32_t)TIMER_CAP || handle > (int32_t)ALL_TIMER_CAP ||
	    !app.timers[handle - 1].callback)
		return -1;
	app.timers[handle - 1] = (struct sdk_timer){ 0 };
	fprintf(stderr, "TIMER delete background id=%d\n", handle);
	return 0;
}

/* SDK adapters preserve the declared C types. The VXP performs its own work
 * and registers the callbacks resumed by the event dispatcher.
 */
static size_t ascii_length(const char *text)
{
	for (size_t i = 0; i < FILE_CAP; ++i) {
		if (!guest_data(text + i, 1, PF_R))
			fail("ASCII input is outside guest memory");
		if (!text[i])
			return i;
	}
	fail("unterminated ASCII input");
}
void MRE_ABI host_app_log(const char *text)
{
	check_guest_api_stack();
	size_t length = ascii_length(text);
	debug_trace("GUEST_LOG %.*s\n", (int)length, text);
}
static void load_resource_index(void);
static unsigned ascii_lower(unsigned byte)
{
	return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
}
char *MRE_ABI host_gmobi_language(void)
{
	check_guest_api_stack();
	/* The original calls this private extension with no arguments, then
	 * reads two bytes from R0. It expects a pointer, not vm_language_t. */
	fprintf(stderr,
		"LANGUAGE vm_get_gmobi_language() -> 'en' (configured game locale)\n");
	return app.game_language;
}
void MRE_ABI host_lower_case(char *destination, char *source)
{
	check_guest_api_stack();
	size_t length = ascii_length(source);
	if (!guest_data(destination, length + 1, PF_W))
		fail("lower-case destination exceeds guest memory");
	memmove(destination, source, length + 1);
	for (size_t i = 0; i < length; ++i)
		destination[i] = (char)ascii_lower((uint8_t)destination[i]);
}
uint8_t *MRE_ABI host_load_resource(char *name, int32_t *bytes)
{
	check_guest_api_stack();
	size_t length = ascii_length(name);
	if (!guest_data(bytes, 4, PF_W) || ((uintptr_t)bytes & 3))
		fail("vm_load_resource size output bounds/alignment");
	*bytes = 0;
	if (length >= 128)
		return NULL;
	for (size_t i = 0; i < app.resource_count; ++i) {
		const struct sdk_resource *resource = &app.resources[i];
		if (resource->name_bytes != length)
			continue;
		size_t matching = 0;
		while (matching < length &&
		       ascii_lower((uint8_t)name[matching]) ==
			       ascii_lower((uint8_t)resource->name[matching]))
			matching++;
		if (matching != length)
			continue;
		uint8_t *copy = host_calloc((int32_t)resource->bytes);
		if (!copy)
			return NULL;
		memcpy(copy, app.ads.resources + resource->offset,
		       resource->bytes);
		*bytes = (int32_t)resource->bytes;
		fprintf(stderr,
			"RESOURCE vm_load_resource('%s') -> %p original payload offset=%u bytes=%u owned SDK copy\n",
			name, copy, resource->offset, resource->bytes);
		return copy;
	}
	fprintf(stderr,
		"RESOURCE vm_load_resource('%s') -> NULL (name absent in original VXP)\n",
		name);
	return NULL;
}
int32_t MRE_ABI host_res_init(void)
{
	check_guest_api_stack();
	if (!app.string_source)
		return -3; /* SDK VM_RES_ITEM_NOT_FOUND. */
	if (app.strings_live)
		host_free(app.strings_live);
	app.strings_live = host_calloc((int32_t)app.string_source_bytes);
	if (!app.strings_live)
		return -1; /* SDK VM_RES_OUT_OF_MEM. */
	memcpy(app.strings_live, app.string_source, app.string_source_bytes);
	return 0;
}
int32_t MRE_ABI host_res_deinit(void)
{
	check_guest_api_stack();
	if (!app.strings_live)
		return -3;
	host_free(app.strings_live);
	app.strings_live = NULL;
	return 0;
}
uint8_t *MRE_ABI host_res_string(uint32_t id)
{
	check_guest_api_stack();
	if (app.strings_live)
		for (size_t i = 0; i < app.string_count; ++i)
			if (app.string_ids[i] == id)
				return app.strings_live + app.string_offsets[i];
	return NULL;
}
static size_t wide_length(const uint16_t *text)
{
	if ((uintptr_t)text & 1)
		fail("unaligned UCS2 input");
	for (size_t i = 0; i < FILE_CAP / sizeof(*text); ++i) {
		if (!guest_data(text + i, 2, PF_R))
			fail("UCS2 input is outside guest memory");
		if (!text[i])
			return i;
	}
	fail("unterminated UCS2 input");
}
static void copy_wide_utf8(char *out, size_t size, const uint16_t *text)
{
	size_t length = wide_length(text);
	if (!mre_utf8_encode(text, length, (unsigned char *)out, size, NULL))
		fail("UCS2 string exceeds UTF-8 adapter buffer");
}
static void copy_wide_ascii(char *out, size_t size, const uint16_t *text)
{
	size_t length = wide_length(text);
	if (length >= size)
		fail("UCS2 string exceeds ASCII adapter buffer");
	for (size_t i = 0; i < length; ++i) {
		if (text[i] > 127)
			fail("UCS2 character cannot be represented in ASCII");
		out[i] = (char)text[i];
	}
	out[length] = 0;
}
int32_t MRE_ABI host_ascii_to_wide(uint16_t *out, int32_t bytes,
				   const char *text)
{
	check_guest_api_stack();
	size_t length = ascii_length(text);
	if (bytes < 0 || (size_t)bytes < (length + 1) * 2)
		return -1;
	if (!guest_data(out, (length + 1) * 2, PF_W))
		fail("ASCII->UCS2 output bounds");
	for (size_t i = 0; i <= length; ++i)
		out[i] = (uint8_t)text[i];
	fprintf(stderr,
		"API vm_ascii_to_ucs2(bytes=%" PRId32 ",text='%s') -> 0\n",
		bytes, text);
	return 0;
}
int32_t MRE_ABI host_gb2312_to_wide(uint16_t *out, int32_t bytes,
				    const char *text)
{
	check_guest_api_stack();
	size_t length = ascii_length(text);
	for (size_t i = 0; i < length; ++i)
		if ((uint8_t)text[i] > 127)
			return -1; /* The GB2312 adapter supports the ASCII subset. */
	return host_ascii_to_wide(out, bytes, text);
}
int32_t MRE_ABI host_wide_to_ascii(char *out, int32_t bytes,
				   const uint16_t *text)
{
	check_guest_api_stack();
	size_t length = wide_length(text);
	if (bytes < 0 || (size_t)bytes < length + 1)
		return -1;
	for (size_t i = 0; i < length; ++i)
		if (text[i] > 127)
			return -1;
	if (!guest_data(out, length + 1, PF_W))
		fail("UCS2->ASCII output bounds");
	copy_wide_ascii(out, length + 1, text);
	fprintf(stderr, "API vm_ucs2_to_ascii(bytes=%" PRId32 ") -> '%s'\n",
		bytes, out);
	return 0;
}
int32_t MRE_ABI host_wstrlen(uint16_t *text)
{
	check_guest_api_stack();
	return (int32_t)wide_length(text);
}
int32_t MRE_ABI host_wstrcmp(const uint16_t *first, const uint16_t *second)
{
	check_guest_api_stack();
	size_t first_length = wide_length(first);
	size_t second_length = wide_length(second);
	size_t length = first_length < second_length ? first_length :
						       second_length;
	for (size_t i = 0; i < length; ++i)
		if (first[i] != second[i])
			return first[i] < second[i] ? -1 : 1;
	if (first_length == second_length)
		return 0;
	return first_length < second_length ? -1 : 1;
}
int32_t MRE_ABI host_safe_wstrcpy(uint16_t *out, int32_t capacity,
				  const uint16_t *text)
{
	check_guest_api_stack();
	if (capacity <= 0)
		return 0;
	if ((uintptr_t)out & 1)
		fail("unaligned UCS2 safe-copy destination");
	size_t length = wide_length(text);
	if (length >= (size_t)capacity)
		length = (size_t)capacity - 1;
	if (!guest_data(out, (length + 1) * 2, PF_W))
		fail("UCS2 safe-copy destination bounds");
	memmove(out, text, length * 2);
	out[length] = 0;
	return (int32_t)length;
}
int32_t MRE_ABI host_wstrncpy(uint16_t *out, const uint16_t *text,
			      int32_t count)
{
	check_guest_api_stack();
	size_t length = wide_length(text);
	if (count < 0 || count >= (int32_t)(FILE_CAP / sizeof(*text)))
		fail("unsupported UCS2 copy count");
	if (length > (size_t)count)
		length = (size_t)count;
	if (!guest_data(out, (length + 1) * 2, PF_W))
		fail("UCS2 copy destination bounds");
	memmove(out, text, length * 2);
	out[length] = 0;
	return (int32_t)length;
}
int32_t MRE_ABI host_wstrcpy(uint16_t *out, const uint16_t *text)
{
	return host_wstrncpy(out, text, (int32_t)wide_length(text));
}
int32_t MRE_ABI host_wstrcat(uint16_t *out, const uint16_t *text)
{
	check_guest_api_stack();
	size_t old_length = wide_length(out), length = wide_length(text);
	if (!guest_data(out, (old_length + length + 1) * 2, PF_W))
		fail("UCS2 append destination bounds");
	memmove(out + old_length, text, (length + 1) * 2);
	return (int32_t)(old_length + length);
}
int32_t MRE_ABI host_exec_filename(uint16_t *out)
{
	check_guest_api_stack();
	size_t bytes = strlen(app.exec_path), units = 0;
	if (!mre_utf8_decode((const unsigned char *)app.exec_path, bytes, NULL,
			     0, &units) ||
	    ((uintptr_t)out & 1) || !guest_data(out, (units + 1) * 2, PF_W) ||
	    !mre_utf8_decode((const unsigned char *)app.exec_path, bytes, out,
			     units + 1, NULL))
		fail("exec filename output bounds/encoding");
	debug_trace("API vm_get_exec_filename() -> '%s'\n", app.exec_path);
	return 0;
}
int32_t MRE_ABI host_editor_title(uint16_t *title)
{
	check_guest_api_stack();
	copy_wide_utf8(app.editor_title, sizeof(app.editor_title), title);
	return 1;
}
int32_t MRE_ABI host_input_text(uint16_t *initial, int32_t max, int32_t method,
				input_fn callback)
{
	check_guest_api_stack();
	if (app.exited || !guest_code((uintptr_t)callback) || max <= 0 ||
	    max >= (int32_t)MRE_EDITOR_UNITS || method < 0 || method > 63 ||
	    app.selector_pending)
		return -1;
	if (app.pending)
		return app.editor_resuming && callback == app.input_callback &&
				       max == app.editor_max &&
				       method == app.editor_method ?
			       0 :
			       -1;
	size_t units = initial ? wide_length(initial) : 0;
	if (units > (size_t)max ||
	    (initial && !mre_utf8_encode(initial, units,
					 (unsigned char *)app.editor_default,
					 sizeof(app.editor_default), NULL)))
		return -1;
	if (!initial)
		app.editor_default[0] = 0;
	app.input_callback = callback;
	app.editor_max = max;
	app.editor_method = method;
	app.pending = 1;
	app.editors++;
	return 0;
}
int32_t MRE_ABI host_removable(void)
{
	check_guest_api_stack();
	return 'E';
}
int32_t MRE_ABI host_system_drive(void)
{
	check_guest_api_stack();
	return 'C';
}
int32_t MRE_ABI host_current_handle(void)
{
	check_guest_api_stack();
	return 1; /* One original application runs in this isolated process. */
}
int32_t MRE_ABI host_process_state(int32_t handle)
{
	check_guest_api_stack();
	if (handle != 1 && handle != 0x0f)
		return 0; /* SDK VM_PMNG_UNLOAD. */
	return app.exited ? 4 : app.visible ? 1 : 5;
}
void MRE_ABI host_volume(int32_t volume)
{
	check_guest_api_stack();
	fprintf(stderr,
		"AUDIO vm_set_volume(%d); isolated engine has no MIDI device\n",
		volume);
}
int32_t MRE_ABI host_midi_play(uint8_t *data, int32_t bytes, int32_t repeat,
			       event_fn callback)
{
	check_guest_api_stack();
	if (bytes <= 0 || bytes > (int32_t)FILE_CAP ||
	    !guest_data(data, (size_t)bytes, PF_R) ||
	    (callback && !guest_code((uintptr_t)callback)))
		return -1;
	fprintf(stderr,
		"AUDIO vm_midi_play_by_bytes(%d,repeat=%d) -> -1 VM_MIDI_FAILED (no MIDI device)\n",
		bytes, repeat);
	return -1; /* SDK VM_MIDI_FAILED, no fabricated handle/callback. */
}
void MRE_ABI host_midi_stop(int32_t handle)
{
	check_guest_api_stack();
	if (handle > 0)
		fail("MIDI stop received an unknown successful device handle");
	fprintf(stderr, "AUDIO vm_midi_stop(%d); no live MIDI handle\n",
		handle);
}
#include "native-format.inc"
int32_t MRE_ABI host_vsprintf(char *out, const char *format, void *args)
{
	check_guest_api_stack();
	return guest_format(out, format, args);
}

void MRE_ABI host_get_filename(uint16_t *path, uint16_t *filename)
{
	check_guest_api_stack();
	size_t length = wide_length(path), start = 0;
	for (size_t i = 0; i < length; ++i)
		if (path[i] == '\\' || path[i] == '/')
			start = i + 1;
	if (!guest_data(filename, (length - start + 1) * 2, PF_W))
		fail("filename destination bounds");
	memmove(filename, path + start, (length - start + 1) * 2);
}
void MRE_ABI host_register_system(event_fn callback)
{
	check_guest_api_stack();
	if (callback && !guest_code((uintptr_t)callback))
		fail("system callback outside guest code");
	app.system_callback = callback;
	fprintf(stderr, "SYSTEM_CALLBACK %p\n", (void *)callback);
}
void MRE_ABI host_register_key(event_fn callback)
{
	check_guest_api_stack();
	if (callback && !guest_code((uintptr_t)callback))
		fail("key callback outside guest code");
	app.key_callback = callback;
	fprintf(stderr, "KEY_CALLBACK %p\n", (void *)callback);
}
void MRE_ABI host_register_pen(pen_fn callback)
{
	check_guest_api_stack();
	if (callback && !guest_code((uintptr_t)callback))
		fail("pen callback outside guest code");
	app.pen_callback = callback;
	const char *available = callback ? "POINTER_AVAILABLE\t1\n" :
					   "POINTER_AVAILABLE\t0\n";
	write_protocol(available, strlen(available));
	fprintf(stderr,
		"PEN_CALLBACK %p (registered; parent provides keypad events)\n",
		(void *)callback);
}
int32_t MRE_ABI host_power(int32_t mode)
{
	check_guest_api_stack();
	return mode == 0 || mode == 1 ? 0 : -1;
}
static int utility_vfs_path(const uint16_t *path, char out[MRE_VFS_PATH_BYTES]);
int32_t MRE_ABI host_selector(int32_t type, const uint16_t *folder,
			      selector_fn callback)
{
	check_guest_api_stack();
	if (app.exited || type < 0 || type > 2 ||
	    !guest_code((uintptr_t)callback) || app.selector_pending ||
	    app.pending)
		return -6;
	if (folder && !utility_vfs_path(folder, app.shared->selector_folder))
		return -6;
	if (!folder)
		app.shared->selector_folder[0] = 0;
	app.shared->selector_type = type;
	app.selector_callback = callback;
	app.selector_pending = 1;
	return 0;
}
int32_t MRE_ABI host_charset(int32_t source, int32_t destination,
			     const char *text, uint16_t *out, int32_t bytes)
{
	check_guest_api_stack();
	return utility_charset(source, destination, text, out, bytes);
}
int32_t MRE_ABI host_screen_width(void)
{
	check_guest_api_stack();
	return guest_screen_width();
}
#include "native-abm.inc"
static int decode_image(const uint8_t *data, size_t bytes,
			struct mre_image *image, uint8_t *transparent_index,
			char *error, size_t error_bytes)
{
	if (transparent_index)
		*transparent_index = 0;
	struct mre_image_limits limits = {
		.max_input_bytes = FILE_CAP,
		.max_width = 320,
		.max_height = 480,
		.max_pixels = 153600,
		.max_working_bytes =
			app.ram_limit - app.memory.bytes - app.canvas_bytes
	};
	if (bytes >= 2 && data[0] == 0x34) {
		int result = decode_guest_abm(data, bytes,
					      limits.max_working_bytes, image);
		if (result && error && error_bytes)
			snprintf(error, error_bytes,
				 "unsupported or truncated packed ABM");
		return result;
	}
	if (bytes >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8))
		return mre_png_decode(data, bytes, &limits, image, error,
				      error_bytes);
	if (bytes >= 2 && data[0] == 0xff && data[1] == 0xd8) {
		limits.max_width = limits.max_height = 800;
		limits.max_pixels = 640000;
		return mre_jpeg_decode(data, bytes, &limits, image, error,
				       error_bytes);
	}
	struct mre_gif_image gif = { 0 };
	if (mre_gif_decode(data, bytes, &gif, error, error_bytes))
		return -1;
	size_t count = (size_t)gif.width * gif.height;
	for (size_t i = 0; i < count; ++i)
		gif.opaque[i] = gif.opaque[i] ? 255 : 0;
	*image = (struct mre_image){ .width = gif.width,
				     .height = gif.height,
				     .pixels = gif.pixels,
				     .alpha = gif.opaque,
				     .transparent_color = gif.transparent_color,
				     .has_transparency = gif.has_transparency };
	if (transparent_index)
		*transparent_index = gif.transparent_index;
	return 0;
}
static int resize_decoded_image(struct mre_image *image, int width, int height)
{
	/* The existing nearest-neighbor resize copies opacity bytes verbatim,
	 * so it also preserves full alpha values from PNG decoding. */
	struct mre_gif_image resized = { .width = image->width,
					 .height = image->height,
					 .pixels = image->pixels,
					 .opaque = image->alpha };
	if (mre_gif_resize(&resized, width, height))
		return -1;
	image->width = resized.width;
	image->height = resized.height;
	image->pixels = resized.pixels;
	image->alpha = resized.opaque;
	return 0;
}
int32_t MRE_ABI host_img_property_ex(uint8_t *data, int32_t bytes,
				     struct sdk_img_property *property)
{
	check_guest_api_stack();
	if (bytes <= 0 || bytes > (int32_t)FILE_CAP)
		return -1;
	if (!guest_data(data, (size_t)bytes, PF_R) ||
	    !guest_data(property, sizeof(*property), PF_W) ||
	    ((uintptr_t)property & 3))
		fail("image property input/output bounds or alignment");
	struct mre_image image = { 0 };
	char error[128];
	if (decode_image(data, (size_t)bytes, &image, NULL, error,
			 sizeof(error))) {
		fprintf(stderr, "IMAGE property decode rejected: %s\n", error);
		return -1;
	}
	property->width = image.width;
	property->height = image.height;
	fprintf(stderr,
		"IMAGE original dimensions %dx%d from %d bytes; property=%p\n",
		image.width, image.height, bytes, (void *)property);
	mre_image_free(&image);
	return 0;
}
static void store16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
}
static void store32(uint8_t *out, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		out[i] = (uint8_t)(value >> (8 * i));
}
static struct sdk_canvas *find_canvas(int32_t handle)
{
	if (handle <= 0)
		return NULL;
	for (struct sdk_canvas *canvas = app.canvases; canvas;
	     canvas = canvas->next)
		if ((uintptr_t)canvas->buffer == (uint32_t)handle)
			return canvas;
	return NULL;
}
static uint8_t *map_canvas(size_t bytes, size_t *mapping_bytes)
{
	*mapping_bytes = (bytes + app.page_size - 1) & ~(app.page_size - 1);
	if (!app.canvas_hint)
		app.canvas_hint = 0x10000000;
	for (unsigned attempt = 0; attempt < 64; ++attempt) {
		if (app.canvas_hint > INT32_MAX - *mapping_bytes)
			return NULL;
		uintptr_t hint = app.canvas_hint;
		app.canvas_hint += *mapping_bytes + app.page_size;
		void *mapping = mmap((void *)hint, *mapping_bytes,
				     PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping == MAP_FAILED)
			return NULL;
		if ((uintptr_t)mapping &&
		    (uintptr_t)mapping <= INT32_MAX - *mapping_bytes)
			return mapping;
		munmap(mapping, *mapping_bytes);
	}
	return NULL;
}
static int32_t load_image_canvas(uint8_t *data, int32_t bytes, int width,
				 int height)
{
	if (bytes <= 0 || bytes > (int32_t)FILE_CAP ||
	    !guest_data(data, (size_t)bytes, PF_R))
		return 0;
	struct mre_image image = { 0 };
	uint8_t transparent_index = 0;
	char error[128];
	if (decode_image(data, (size_t)bytes, &image, &transparent_index, error,
			 sizeof(error))) {
		fprintf(stderr, "IMAGE load decode rejected: %s\n", error);
		return 0;
	}
	if (width && resize_decoded_image(&image, width, height)) {
		mre_image_free(&image);
		return 0;
	}
	size_t pixel_bytes = (size_t)image.width * (size_t)image.height * 2;
	size_t canvas_bytes = 32 + pixel_bytes;
	size_t alpha_bytes = image.has_partial_alpha ? pixel_bytes / 2 : 0;
	if (canvas_bytes + alpha_bytes >
	    app.ram_limit - app.memory.bytes - app.canvas_bytes) {
		mre_image_free(&image);
		return 0;
	}
	struct sdk_canvas *canvas = calloc(1, sizeof(*canvas));
	if (!canvas) {
		mre_image_free(&image);
		return 0;
	}
	canvas->buffer = map_canvas(canvas_bytes, &canvas->mapping_bytes);
	if (!canvas->buffer) {
		free(canvas);
		mre_image_free(&image);
		return 0;
	}
	canvas->bytes = canvas_bytes;
	canvas->alpha_bytes = alpha_bytes;
	if (alpha_bytes) {
		canvas->alpha = image.alpha;
		image.alpha = NULL;
	}
	canvas->property = (struct sdk_frame_property){
		.flag = (uint8_t)image.has_transparency,
		.width = (uint16_t)image.width,
		.height = (uint16_t)image.height,
		.trans_color_index = transparent_index,
		.trans_color = image.transparent_color,
		.offset = (uint32_t)pixel_bytes
	};
	/* The private packed canvas header is separate from the SDK's natural
	 * frame_prop layout. Pixels begin exactly32 bytes after MTKCANVAS. */
	uint8_t *header = canvas->buffer;
	memcpy(header, "MTKCANVAS", 9);
	header[9] = 1;
	header[10] = 0xff;
	header[11] = 0; /* VM_GRAPHIC_COLOR_FORMAT_16. */
	header[12] = (uint8_t)image.has_transparency;
	store16(header + 17, (uint16_t)image.width);
	store16(header + 19, (uint16_t)image.height);
	header[23] = transparent_index;
	store32(header + 24,
		image.has_transparency ? image.transparent_color : UINT32_MAX);
	store32(header + 28, (uint32_t)pixel_bytes);
	memcpy(header + 32, image.pixels, pixel_bytes);
	canvas->next = app.canvases;
	app.canvases = canvas;
	app.canvas_bytes += canvas_bytes + alpha_bytes;
	service_memory_refresh();
	for (size_t i = 0; i < 256; ++i)
		if (app.retired_canvases[i] >= (uintptr_t)header &&
		    app.retired_canvases[i] - (uintptr_t)header < canvas_bytes)
			app.retired_canvases[i] = 0;
	fprintf(stderr,
		"IMAGE load %dx%d RGB565 stride=%d transparency=%d key=%04x handle=%p bytes=%zu alpha_bytes=%zu SDKlive=%zu/%u\n",
		image.width, image.height, image.width * 2,
		image.has_transparency, image.transparent_color, header,
		canvas_bytes, alpha_bytes, app.memory.bytes + app.canvas_bytes,
		app.ram_limit);
	mre_image_free(&image);
	return (int32_t)(uintptr_t)header;
}
int32_t MRE_ABI host_load_image(uint8_t *data, int32_t bytes)
{
	check_guest_api_stack();
	return load_image_canvas(data, bytes, 0, 0);
}
int32_t MRE_ABI host_load_image_resized(uint8_t *data, int32_t bytes,
					int32_t width, int32_t height)
{
	check_guest_api_stack();
	if (width <= 0 || height <= 0 || width > 512 || height > 512)
		return 0;
	return load_image_canvas(data, bytes, width, height);
}
int32_t MRE_ABI host_frame_number(int32_t handle)
{
	check_guest_api_stack();
	struct sdk_canvas *canvas = find_canvas(handle);
	return canvas ? canvas->buffer[9] : 0;
}
struct sdk_frame_property *MRE_ABI host_img_property(int32_t handle,
						     uint8_t frame)
{
	check_guest_api_stack();
	struct sdk_canvas *canvas = find_canvas(handle);
	/* The unchanged original requests1 for its first frame. */
	return canvas && (frame == 0 || frame == 1) ? &canvas->property : NULL;
}
uint8_t *MRE_ABI host_canvas_buffer(int32_t handle)
{
	check_guest_api_stack();
	struct sdk_canvas *canvas = find_canvas(handle);
	return canvas ? canvas->buffer : NULL;
}
static int free_canvas_pointer(void *pointer)
{
	struct sdk_canvas **link = &app.canvases;
	while (*link) {
		struct sdk_canvas *canvas = *link;
		if (canvas->buffer != pointer) {
			link = &canvas->next;
			continue;
		}
		uintptr_t address = (uintptr_t)canvas->buffer;
		*link = canvas->next;
		app.canvas_bytes -= canvas->bytes + canvas->alpha_bytes;
		service_memory_refresh();
		app.retired_canvases[app.retired_canvas_cursor] = address;
		app.retired_canvas_cursor =
			(app.retired_canvas_cursor + 1) % 256;
		if (munmap(canvas->buffer, canvas->mapping_bytes))
			fail("unmapping owned image canvas failed");
		free(canvas->alpha);
		free(canvas);
		fprintf(stderr,
			"IMAGE release canvas=0x%" PRIxPTR " SDKlive=%zu/%u\n",
			address, app.memory.bytes + app.canvas_bytes,
			app.ram_limit);
		return 1;
	}
	for (size_t i = 0; i < 256; ++i)
		if (app.retired_canvases[i] == (uintptr_t)pointer)
			return 1;
	return 0;
}
void MRE_ABI host_release_canvas(int32_t handle)
{
	check_guest_api_stack();
	if (handle && !free_canvas_pointer((void *)(uintptr_t)(uint32_t)handle))
		fail("release_canvas unknown handle=0x%08x", (uint32_t)handle);
}
int32_t MRE_ABI host_screen_height(void)
{
	check_guest_api_stack();
	return guest_screen_height();
}
int32_t MRE_ABI host_create_layer(int32_t x, int32_t y, int32_t width,
				  int32_t height, int32_t transparent)
{
	check_guest_api_stack();
	if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
	    x > guest_screen_width() - width ||
	    y > guest_screen_height() - height || transparent < -1 ||
	    transparent > UINT16_MAX)
		return -1;
	size_t handle;
	for (handle = 0; handle < LAYER_CAP; ++handle)
		if (app.layers[handle].buffer && !app.layers[handle].live)
			break;
	if (handle == LAYER_CAP)
		return -1;
	struct sdk_layer *layer = &app.layers[handle];
	handheld_forget_layer((int32_t)handle);
	memset(layer->buffer, 0, 32 + layer_bytes());
	memcpy(layer->buffer, "MTKCANVAS", 9);
	layer->buffer[9] = 1;
	layer->buffer[10] = 0xff;
	layer->buffer[12] = transparent >= 0;
	store16(layer->buffer + 17, (uint16_t)guest_screen_width());
	store16(layer->buffer + 19, (uint16_t)guest_screen_height());
	store32(layer->buffer + 24, (uint32_t)transparent);
	store32(layer->buffer + 28, (uint32_t)layer_bytes());
	layer->live = 1;
	layer->transparent = transparent;
	layer->opacity = 100;
	if (transparent >= 0)
		for (size_t i = 0; i < layer_bytes() / 2; ++i)
			layer->pixels[i] = (uint16_t)transparent;
	handheld_configure_owned_layer((int32_t)handle, x, y, width, height);
	if (app.active_layer < 0)
		app.active_layer = (int)handle;
	fprintf(stderr,
		"LAYER create %dx%d RGB565 transparent=%d -> %zu buffer=%p\n",
		width, height, transparent, handle, layer->buffer);
	return (int32_t)handle;
}
int32_t MRE_ABI host_delete_layer(int32_t handle)
{
	check_guest_api_stack();
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live)
		return -1;
	app.layers[handle].live = 0;
	handheld_forget_layer(handle);
	if (app.active_layer == handle)
		app.active_layer = -1;
	if (app.alpha_layer == handle)
		app.alpha_layer = -1;
	fprintf(stderr, "LAYER delete original handle=%d\n", handle);
	return 0;
}
int32_t MRE_ABI host_active_layer(int32_t handle)
{
	check_guest_api_stack();
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live)
		return -1;
	app.active_layer = handle;
	return 0;
}
int32_t MRE_ABI host_layer_opacity(int32_t handle, int32_t percent)
{
	check_guest_api_stack();
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live || percent < 0 || percent > 100)
		return -1;
	app.layers[handle].opacity = percent;
	debug_trace("LAYER opacity handle=%d percent=%d\n", handle, percent);
	return 0;
}
uint8_t *MRE_ABI host_layer_buffer(int32_t handle)
{
	check_guest_api_stack();
	uint8_t *external = handheld_layer_buffer(handle);
	if (external)
		return external;
	/* The SDK layer buffer exposes RGB565 pixels directly. The separate
	 * canvas API retains its MTKCANVAS attributes before its pixels. */
	return handle >= 0 && handle < (int32_t)LAYER_CAP &&
			       app.layers[handle].live ?
		       (uint8_t *)app.layers[handle].pixels :
		       NULL;
}
int32_t MRE_ABI host_clear_layer(int32_t handle)
{
	check_guest_api_stack();
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live || app.layers[handle].transparent < 0)
		return -1;
	for (size_t i = 0; i < layer_bytes() / 2; ++i)
		app.layers[handle].pixels[i] =
			(uint16_t)app.layers[handle].transparent;
	return 0;
}
void MRE_ABI host_clip(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
	check_guest_api_stack();
	app.clip_x = x1 > 0 ? x1 : 0;
	app.clip_y = y1 > 0 ? y1 : 0;
	int right = x2 < guest_screen_width() - 1 ? x2 :
						    guest_screen_width() - 1;
	int bottom = y2 < guest_screen_height() - 1 ? y2 :
						      guest_screen_height() - 1;
	app.clip_width = right >= app.clip_x ? right - app.clip_x + 1 : 0;
	app.clip_height = bottom >= app.clip_y ? bottom - app.clip_y + 1 : 0;
}
static void put_pixel(uint16_t *pixels, int x, int y, uint16_t color)
{
	struct pixel_surface surface;
	if (!owned_surface((uint8_t *)pixels, &surface))
		fail("pixel destination is not an owned live surface");
	if (x >= app.clip_x && y >= app.clip_y &&
	    x - app.clip_x < app.clip_width &&
	    y - app.clip_y < app.clip_height && x >= 0 && x < surface.width &&
	    y >= 0 && y < surface.height) {
		size_t pixel = (size_t)y * surface.width + x;
		surface.pixels[pixel] = color;
		if (surface.alpha)
			surface.alpha[pixel] = 255;
	}
}
int32_t MRE_ABI host_set_color(const uint32_t *color)
{
	check_guest_api_stack();
	if (!guest_data(color, sizeof(app.draw_color), PF_R))
		fail("global graphics color outside guest memory");
	memcpy(app.draw_color, color, sizeof(app.draw_color));
	return 0;
}
void MRE_ABI host_fill(uint8_t *buffer, int32_t x, int32_t y, int32_t width,
		       int32_t height, uint16_t line, uint16_t background)
{
	check_guest_api_stack();
	struct pixel_surface surface;
	if (!owned_surface(buffer, &surface))
		fail("graphics destination is not an owned live surface");
	if (width <= 0 || height <= 0)
		return;
	int64_t right = (int64_t)x + width, bottom = (int64_t)y + height;
	int left = x > 0 ? x : 0, top = y > 0 ? y : 0;
	int stop_x = right < surface.width ? (int)right : surface.width;
	int stop_y = bottom < surface.height ? (int)bottom : surface.height;
	for (int yy = top; yy < stop_y; ++yy)
		for (int xx = left; xx < stop_x; ++xx)
			put_pixel(surface.pixels, xx, yy,
				  xx == x || yy == y ||
						  (int64_t)xx == right - 1 ||
						  (int64_t)yy == bottom - 1 ?
					  line :
					  background);
}
void MRE_ABI host_fill_ex(int32_t handle, int32_t x, int32_t y, int32_t width,
			  int32_t height)
{
	check_guest_api_stack();
	uint8_t *buffer = host_layer_buffer(handle);
	if (buffer)
		host_fill(buffer, x, y, width, height,
			  (uint16_t)app.draw_color[0],
			  (uint16_t)app.draw_color[0]);
}
void MRE_ABI host_rect_ex(int32_t handle, int32_t x, int32_t y, int32_t width,
			  int32_t height)
{
	check_guest_api_stack();
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live || width <= 0 || height <= 0)
		return;
	int64_t right = (int64_t)x + width - 1;
	int64_t bottom = (int64_t)y + height - 1;
	for (int yy = 0; yy < guest_screen_height(); ++yy)
		for (int xx = 0; xx < guest_screen_width(); ++xx)
			if (xx >= x && (int64_t)xx <= right && yy >= y &&
			    (int64_t)yy <= bottom &&
			    (xx == x || yy == y || (int64_t)xx == right ||
			     (int64_t)yy == bottom))
				put_pixel(app.layers[handle].pixels, xx, yy,
					  (uint16_t)app.draw_color[0]);
}
void MRE_ABI host_fill_ellipse_ex(int32_t handle, int32_t x, int32_t y,
				  int32_t width, int32_t height)
{
	check_guest_api_stack();
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live || width <= 0 || height <= 0)
		return;
	int64_t right = (int64_t)x + width, bottom = (int64_t)y + height;
	int left = x > 0 ? x : 0, top = y > 0 ? y : 0;
	int stop_x = right < guest_screen_width() ? (int)right :
						    guest_screen_width();
	int stop_y = bottom < guest_screen_height() ? (int)bottom :
						      guest_screen_height();
	for (int yy = top; yy < stop_y; ++yy) {
		double dy = (2.0 * ((double)yy - y) + 1.0 - height) / height;
		for (int xx = left; xx < stop_x; ++xx) {
			double dx =
				(2.0 * ((double)xx - x) + 1.0 - width) / width;
			if (dx * dx + dy * dy <= 1.0)
				put_pixel(app.layers[handle].pixels, xx, yy,
					  (uint16_t)app.draw_color[0]);
		}
	}
}
void MRE_ABI host_rect(uint8_t *buffer, int32_t x, int32_t y, int32_t width,
		       int32_t height, uint16_t color)
{
	check_guest_api_stack();
	if (width <= 0 || height <= 0)
		return;
	struct pixel_surface surface;
	if (!owned_surface(buffer, &surface))
		fail("graphics destination is not an owned live surface");
	int64_t right = (int64_t)x + width - 1;
	int64_t bottom = (int64_t)y + height - 1;
	for (int yy = 0; yy < surface.height; ++yy)
		for (int xx = 0; xx < surface.width; ++xx)
			if (xx >= x && (int64_t)xx <= right && yy >= y &&
			    (int64_t)yy <= bottom &&
			    (xx == x || yy == y || (int64_t)xx == right ||
			     (int64_t)yy == bottom))
				put_pixel(surface.pixels, xx, yy, color);
}
int32_t MRE_ABI host_font_size(int32_t size)
{
	check_guest_api_stack();
	if (size < 0 || size > 2)
		return -16;
	app.font_size = size;
	return 0;
}
static int font_height_for_size(int size)
{
	/* Editor font attributes accept SDK enums or a literal pixel height.
	 * Numeric heights scale the installed PSF raster; they are not enum grades
	 * and do not imply that a vector font backend is present. */
	return size > 2 ? size : (int)app.font.height + size * 4;
}
static int font_width_for_size(int size)
{
	if (size <= 2)
		return (int)app.font.width + size * 2;
	unsigned width =
		(app.font.width * (unsigned)size + app.font.height / 2) /
		app.font.height;
	return width ? (int)width : 1;
}
static int glyph_width(void)
{
	return font_width_for_size(app.font_size);
}
static int glyph_height(void)
{
	return font_height_for_size(app.font_size);
}
static int glyph_advance(void)
{
	return glyph_width() + app.font_bold + (app.font_italic ? 3 : 0);
}
static int glyph_ascent(void)
{
	return glyph_height() * 3 / 4;
}
int32_t MRE_ABI host_character_height(void)
{
	check_guest_api_stack();
	return glyph_height();
}
int32_t MRE_ABI host_character_width(uint16_t character)
{
	check_guest_api_stack();
	(void)character;
	return glyph_advance();
}
int32_t MRE_ABI host_pen_supported(void)
{
	check_guest_api_stack();
	return 1; /* The native parent brokers bounded PEN events. */
}
int32_t MRE_ABI host_rand(void)
{
	check_guest_api_stack();
	return rand();
}
int32_t MRE_ABI host_font_style(int32_t bold, int32_t italic, int32_t underline)
{
	check_guest_api_stack();
	app.font_bold = bold != 0;
	app.font_italic = italic != 0;
	app.font_underline = underline != 0;
	debug_trace(
		"FONT style bold=%d italic=%d underline=%d (scaled PSF raster)\n",
		app.font_bold, app.font_italic, app.font_underline);
	return 0;
}
void MRE_ABI host_set_font(int32_t size)
{
	check_guest_api_stack();
	if (size < 0 || size > 2)
		fail("unsupported SDK font enum %d", size);
	app.font_size = size;
	debug_trace("FONT select enum=%d raster=%dx%d advance=%d ascent=%d\n",
		    size, glyph_width(), glyph_height(), glyph_advance(),
		    glyph_ascent());
}
int32_t MRE_ABI host_string_height(uint16_t *text)
{
	check_guest_api_stack();
	(void)wide_length(text);
	return glyph_height();
}
int32_t MRE_ABI host_string_baseline(uint16_t *text)
{
	check_guest_api_stack();
	(void)wide_length(text);
	return glyph_ascent();
}
int32_t MRE_ABI host_string_width(uint16_t *text)
{
	check_guest_api_stack();
	return (int32_t)(wide_length(text) * (size_t)glyph_advance());
}
static void draw_text(uint8_t *buffer, int32_t x, int32_t y, uint16_t *text,
		      int32_t length, uint16_t color)
{
	struct pixel_surface surface;
	if (!owned_surface(buffer, &surface))
		fail("graphics destination is not an owned live surface");
	if (length < 0 || length > 2048 || ((uintptr_t)text & 1))
		fail("graphics string bounds");
	char trace[256];
	size_t trace_used = 0;
	for (int32_t i = 0; i < length; ++i) {
		if (!guest_data(text + i, 2, PF_R))
			fail("graphics string scalar exceeds guest memory");
		uint16_t code = text[i];
		if (!code)
			break;
		if (trace_used + 3 < sizeof(trace)) {
			if (code < 128)
				trace[trace_used++] = code >= 32 ? (char)code :
								   '?';
			else if (code < 2048) {
				trace[trace_used++] =
					(char)(0xc0 | (code >> 6));
				trace[trace_used++] =
					(char)(0x80 | (code & 63));
			} else {
				trace[trace_used++] =
					(char)(0xe0 | (code >> 12));
				trace[trace_used++] =
					(char)(0x80 | ((code >> 6) & 63));
				trace[trace_used++] =
					(char)(0x80 | (code & 63));
			}
		}
		const unsigned char *glyph =
			fplinux_font_glyph(&app.font, code);
		int width = glyph_width(), height = glyph_height();
		int64_t origin = (int64_t)x + (int64_t)i * glyph_advance();
		if (!glyph || origin < -16 || origin >= surface.width ||
		    y < -32 || y >= surface.height)
			continue;
		for (int row = 0; row < height; ++row) {
			unsigned source_row = (unsigned)row * app.font.height /
					      (unsigned)height;
			int shear = app.font_italic ? (height - 1 - row) * 3 /
							      (height - 1) :
						      0;
			for (int column = 0; column < width; ++column) {
				unsigned source_column = (unsigned)column *
							 app.font.width /
							 (unsigned)width;
				if (!(glyph[source_row * app.font.row_bytes +
					    source_column / 8] &
				      (0x80 >> (source_column & 7))))
					continue;
				put_pixel(surface.pixels,
					  (int)origin + column + shear, y + row,
					  color);
				if (app.font_bold)
					put_pixel(surface.pixels,
						  (int)origin + column + shear +
							  1,
						  y + row, color);
			}
		}
		if (app.font_underline)
			for (int column = 0; column < glyph_advance(); ++column)
				put_pixel(surface.pixels, (int)origin + column,
					  y + height - 2, color);
	}
	trace[trace_used] = 0;
	debug_trace("TEXT x=%d y=%d units=%d RGB565=%04x '%s'\n", x, y, length,
		    color, trace);
}
void MRE_ABI host_textout(uint8_t *buffer, int32_t x, int32_t y, uint16_t *text,
			  int32_t length, uint16_t color)
{
	check_guest_api_stack();
	draw_text(buffer, x, y, text, length, color);
}
int32_t MRE_ABI host_text_to_layer(int32_t handle, int32_t x, int32_t y,
				   uint16_t *text, int32_t length)
{
	check_guest_api_stack();
	uint8_t *buffer = host_layer_buffer(handle);
	if (!buffer)
		return -1;
	draw_text(buffer, x, y, text, length, (uint16_t)app.draw_color[0]);
	return 0;
}
int32_t MRE_ABI host_is_r2l(void)
{
	check_guest_api_stack();
	/* The configured English game locale and PSF renderer use left-to-right. */
	return 0;
}
int32_t MRE_ABI host_alpha_layer(int32_t handle)
{
	check_guest_api_stack();
	if (handle != -1 && (handle < 0 || handle >= (int32_t)LAYER_CAP ||
			     !app.layers[handle].live))
		return -1;
	app.alpha_layer = handle;
	return 0;
}
static int32_t draw_image_memory(int32_t handle, int32_t x, int32_t y,
				 int32_t width, int32_t height,
				 const uint8_t *data, int32_t bytes)
{
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live || bytes <= 0 || bytes > (int32_t)FILE_CAP)
		return -1;
	if (!guest_data(data, (size_t)bytes, PF_R))
		fail("image drawing input outside guest memory");
	struct pixel_surface surface;
	if (!handheld_layer_surface(handle, &surface))
		return -1;
	struct mre_image image = { 0 };
	char error[128];
	if (decode_image(data, (size_t)bytes, &image, NULL, error,
			 sizeof(error))) {
		fprintf(stderr, "IMAGE draw decode rejected: %s\n", error);
		return -1;
	}
	if (!width && !height) {
		width = image.width;
		height = image.height;
	}
	int left = x > 0 ? x : 0, top = y > 0 ? y : 0;
	int64_t right = (int64_t)x + width, bottom = (int64_t)y + height;
	int stop_x = right < surface.width ? (int)right : surface.width;
	int stop_y = bottom < surface.height ? (int)bottom : surface.height;
	for (int dy = top; dy < stop_y; ++dy) {
		size_t sy = (size_t)(((int64_t)dy - y) * image.height / height);
		for (int dx = left; dx < stop_x; ++dx) {
			size_t sx = (size_t)(((int64_t)dx - x) * image.width /
					     width);
			size_t source = sy * (size_t)image.width + sx;
			if (image.alpha[source]) {
				uint16_t color = blend565(
					image.pixels[source],
					surface.pixels[(size_t)dy *
							       surface.width +
						       dx],
					image.alpha[source]);
				put_pixel(surface.pixels, dx, dy, color);
			}
		}
	}
	debug_trace(
		"IMAGE draw %dx%d -> %dx%d at %d,%d layer=%d alpha_layer=%d\n",
		image.width, image.height, width, height, x, y, handle,
		app.alpha_layer);
	mre_image_free(&image);
	return 0;
}
int32_t MRE_ABI host_draw_image(int32_t handle, int32_t x, int32_t y,
				const uint8_t *data, int32_t bytes)
{
	check_guest_api_stack();
	return draw_image_memory(handle, x, y, 0, 0, data, bytes);
}
int32_t MRE_ABI host_draw_resized_image(int32_t handle, int32_t x, int32_t y,
					int32_t width, int32_t height,
					const uint8_t *data, int32_t bytes)
{
	check_guest_api_stack();
	if (width <= 0 || height <= 0 || width > 512 || height > 512)
		return -1;
	return draw_image_memory(handle, x, y, width, height, data, bytes);
}

static int owned_surface(uint8_t *buffer, struct pixel_surface *surface)
{
	for (size_t i = 0; i < LAYER_CAP; ++i) {
		struct sdk_layer *layer = &app.layers[i];
		if (layer->live && (buffer == layer->buffer ||
				    buffer == (uint8_t *)layer->pixels)) {
			return handheld_layer_surface((int32_t)i, surface);
		}
	}
	for (struct sdk_canvas *canvas = app.canvases; canvas;
	     canvas = canvas->next) {
		if (buffer == canvas->buffer || buffer == canvas->buffer + 32) {
			*surface = (struct pixel_surface){
				(uint16_t *)(canvas->buffer + 32),
				canvas->property.width, canvas->property.height,
				canvas->property.flag ?
					canvas->property.trans_color :
					-1,
				canvas->alpha
			};
			return 1;
		}
	}
	return 0;
}
static uint16_t blend565(uint16_t source, uint16_t destination, unsigned alpha)
{
	if (alpha == 255)
		return source;
	unsigned inverse = 255 - alpha;
	unsigned red =
		(((source >> 11) * alpha + (destination >> 11) * inverse) +
		 127) /
		255;
	unsigned green = ((((source >> 5) & 63) * alpha +
			   ((destination >> 5) & 63) * inverse) +
			  127) /
			 255;
	unsigned blue =
		(((source & 31) * alpha + (destination & 31) * inverse) + 127) /
		255;
	return (uint16_t)((red << 11) | (green << 5) | blue);
}
static void surface_blend_pixel(struct pixel_surface *destination, size_t pixel,
				uint16_t color, unsigned alpha)
{
	if (!alpha)
		return;
	if (!destination->alpha) {
		destination->pixels[pixel] =
			blend565(color, destination->pixels[pixel], alpha);
		return;
	}
	unsigned old_alpha = destination->alpha[pixel], inverse = 255 - alpha;
	unsigned denominator = alpha * 255 + old_alpha * inverse;
	unsigned out_alpha = (denominator + 127) / 255;
	uint16_t old = destination->pixels[pixel];
	unsigned red = ((color >> 11) * alpha * 255 +
			(old >> 11) * old_alpha * inverse + denominator / 2) /
		       denominator;
	unsigned green =
		(((color >> 5) & 63) * alpha * 255 +
		 ((old >> 5) & 63) * old_alpha * inverse + denominator / 2) /
		denominator;
	unsigned blue = ((color & 31) * alpha * 255 +
			 (old & 31) * old_alpha * inverse + denominator / 2) /
			denominator;
	destination->pixels[pixel] =
		(uint16_t)((red << 11) | (green << 5) | blue);
	destination->alpha[pixel] = (uint8_t)out_alpha;
}
static void blit_surface(uint8_t *destination, int32_t x, int32_t y,
			 uint8_t *source, int32_t source_x, int32_t source_y,
			 int32_t width, int32_t height, int32_t frame,
			 int32_t alpha)
{
	if (!destination || !source || width <= 0 || height <= 0 ||
	    (frame != 0 && frame != 1) || alpha <= 0 || alpha > 255)
		return;
	struct pixel_surface dest, src;
	if (!owned_surface(destination, &dest) || !owned_surface(source, &src))
		fail("blit source/destination is not an owned graphics buffer");
	for (int dy = 0; dy < dest.height; ++dy) {
		int64_t relative_y = (int64_t)dy - y;
		int64_t sy = relative_y + source_y;
		if (relative_y < 0 || relative_y >= height || sy < 0 ||
		    sy >= src.height || dy < app.clip_y ||
		    dy - app.clip_y >= app.clip_height)
			continue;
		for (int dx = 0; dx < dest.width; ++dx) {
			int64_t relative_x = (int64_t)dx - x;
			int64_t sx = relative_x + source_x;
			if (relative_x < 0 || relative_x >= width || sx < 0 ||
			    sx >= src.width || dx < app.clip_x ||
			    dx - app.clip_x >= app.clip_width)
				continue;
			size_t source_pixel =
				(size_t)sy * (size_t)src.width + (size_t)sx;
			uint16_t color = src.pixels[source_pixel];
			if (src.transparent < 0 ||
			    color != (uint16_t)src.transparent) {
				size_t pixel = (size_t)dy * (size_t)dest.width +
					       (size_t)dx;
				unsigned combined_alpha =
					src.alpha ? (src.alpha[source_pixel] *
							     (unsigned)alpha +
						     127) / 255 :
						    (unsigned)alpha;
				surface_blend_pixel(&dest, pixel, color,
						    combined_alpha);
			}
		}
	}
	debug_trace(
		"BLT %dx%d source=%dx%d offset=%d,%d -> %d,%d frame=%d alpha=%d\n",
		width, height, src.width, src.height, source_x, source_y, x, y,
		frame, alpha);
}
void MRE_ABI host_blt(uint8_t *destination, int32_t x, int32_t y,
		      uint8_t *source, int32_t source_x, int32_t source_y,
		      int32_t width, int32_t height, int32_t frame)
{
	check_guest_api_stack();
	blit_surface(destination, x, y, source, source_x, source_y, width,
		     height, frame, 255);
}
void MRE_ABI host_blt_ex(uint8_t *destination, int32_t x, int32_t y,
			 uint8_t *source, int32_t source_x, int32_t source_y,
			 int32_t width, int32_t height, int32_t frame,
			 int32_t alpha)
{
	check_guest_api_stack();
	blit_surface(destination, x, y, source, source_x, source_y, width,
		     height, frame, alpha);
}
void MRE_ABI host_rotate(uint8_t *destination, int32_t x, int32_t y,
			 uint8_t *source, int32_t frame, int32_t degrees)
{
	check_guest_api_stack();
	if (!destination || !source || (frame != 0 && frame != 1) ||
	    (degrees != 90 && degrees != 180 && degrees != 270))
		return;
	struct pixel_surface dest, src;
	if (!owned_surface(destination, &dest) || !owned_surface(source, &src))
		fail("rotation source/destination is not an owned graphics buffer");
	int width = degrees == 180 ? src.width : src.height;
	int height = degrees == 180 ? src.height : src.width;
	for (int dy = 0; dy < dest.height; ++dy) {
		int64_t ry = (int64_t)dy - y;
		if (ry < 0 || ry >= height || dy < app.clip_y ||
		    dy - app.clip_y >= app.clip_height)
			continue;
		for (int dx = 0; dx < dest.width; ++dx) {
			int64_t rx = (int64_t)dx - x;
			if (rx < 0 || rx >= width || dx < app.clip_x ||
			    dx - app.clip_x >= app.clip_width)
				continue;
			int64_t sx = degrees == 90  ? ry :
				     degrees == 180 ? src.width - 1 - rx :
						      src.width - 1 - ry;
			int64_t sy = degrees == 90  ? src.height - 1 - rx :
				     degrees == 180 ? src.height - 1 - ry :
						      rx;
			size_t source_pixel =
				(size_t)sy * (size_t)src.width + (size_t)sx;
			uint16_t color = src.pixels[source_pixel];
			if (src.transparent < 0 ||
			    color != (uint16_t)src.transparent)
				surface_blend_pixel(
					&dest,
					(size_t)dy * (size_t)dest.width +
						(size_t)dx,
					color,
					src.alpha ? src.alpha[source_pixel] :
						    255);
		}
	}
	debug_trace("ROTATE source=%dx%d -> %d,%d frame=%d degrees=%d\n",
		    src.width, src.height, x, y, frame, degrees);
}
#include "native-graphics.inc"
#include "native-handheld.inc"
void MRE_ABI host_text_baseline(uint8_t *buffer, int32_t x, int32_t y,
				uint16_t *text, int32_t length, uint16_t color,
				int32_t baseline)
{
	check_guest_api_stack();
	if (baseline < 0 || baseline > 32 || y > INT32_MAX - 32)
		fail("graphics baseline range");
	draw_text(buffer, x, y + baseline - glyph_ascent(), text, length,
		  color);
}
static int compose_layers(const int32_t *handles, int32_t count)
{
	if (count < 1 || count > (int32_t)LAYER_CAP)
		return -1;
	for (int32_t i = 0; i < count; ++i)
		if (handles[i] < 0 || handles[i] >= (int32_t)LAYER_CAP ||
		    !app.layers[handles[i]].live)
			return -1;
	/* Snapshot at the flush, rather than at return: unflushed later writes
     * must remain invisible, as on the SDK LCD. */
	handheld_sync_layers();
	memset(app.shared->pixels, 0, sizeof(app.shared->pixels));
	struct display_viewport viewport = display_viewport();
	for (int32_t i = 0; i < count; ++i) {
		struct sdk_layer *layer = &app.layers[handles[i]];
		for (int y = 0; y < viewport.height; ++y) {
			size_t source_row = (size_t)y * guest_screen_height() /
					    viewport.height *
					    guest_screen_width();
			for (int x = 0; x < viewport.width; ++x) {
				size_t source = source_row +
						(size_t)x *
							guest_screen_width() /
							viewport.width;
				uint16_t pixel;
				if (!handheld_screen_pixel(
					    handles[i],
					    (int)(source % guest_screen_width()),
					    (int)(source / guest_screen_width()),
					    &pixel))
					continue;
				if (layer->opacity &&
				    (layer->transparent < 0 ||
				     pixel != (uint16_t)layer->transparent)) {
					uint16_t *output =
						&app.shared->pixels
							 [(viewport.y +
							   y) * MRE_FRAME_WIDTH +
							  viewport.x + x];
					unsigned opacity =
						(unsigned)layer->opacity;
					unsigned inverse = 100 - opacity;
					unsigned red =
						((pixel >> 11) * opacity +
						 (*output >> 11) * inverse +
						 50) /
						100;
					unsigned green =
						(((pixel >> 5) & 63) * opacity +
						 ((*output >> 5) & 63) *
							 inverse +
						 50) /
						100;
					unsigned blue =
						((pixel & 31) * opacity +
						 (*output & 31) * inverse +
						 50) /
						100;
					*output =
						(uint16_t)((red << 11) |
							   (green << 5) | blue);
				}
			}
		}
	}
	app.frame_pending = 1;
	return 0;
}
int32_t MRE_ABI host_flush(int32_t *handles, int32_t count)
{
	check_guest_api_stack();
	if (count < 1 || count > (int32_t)LAYER_CAP ||
	    !guest_data(handles, (size_t)count * 4, PF_R))
		return -1;
	return compose_layers(handles, count);
}
void MRE_ABI host_flush_screen(void)
{
	check_guest_api_stack();
	/* SDK repaint requests run after the requesting guest callback returns. */
	app.paint_pending = 1;
}
int32_t MRE_ABI host_flush_region(int32_t *handles, int32_t count, int32_t x1,
				  int32_t y1, int32_t x2, int32_t y2)
{
	check_guest_api_stack();
	if (x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1 ||
	    x2 >= guest_screen_width() || y2 >= guest_screen_height())
		return -1;
	static uint16_t previous[MRE_FRAME_WIDTH * MRE_FRAME_HEIGHT];
	memcpy(previous, app.shared->pixels, sizeof(previous));
	int result = host_flush(handles, count);
	if (result)
		return result;
	struct display_viewport viewport = display_viewport();
	for (int y = 0; y < MRE_FRAME_HEIGHT; ++y)
		for (int x = 0; x < MRE_FRAME_WIDTH; ++x) {
			int gx = (x - viewport.x) * guest_screen_width() /
				 viewport.width;
			int gy = (y - viewport.y) * guest_screen_height() /
				 viewport.height;
			if (x < viewport.x ||
			    x >= viewport.x + viewport.width ||
			    y < viewport.y ||
			    y >= viewport.y + viewport.height || gx < x1 ||
			    gx > x2 || gy < y1 || gy > y2)
				app.shared->pixels[y * MRE_FRAME_WIDTH + x] =
					previous[y * MRE_FRAME_WIDTH + x];
		}
	return 0;
}
void MRE_ABI host_exit(void)
{
	check_guest_api_stack();
	app.exited++;
	native_http_cancel_all();
	fprintf(stderr,
		"API vm_exit_app() -> exit requested; return to managed host dispatcher\n");
}
static uint32_t call_native_service_provider(uintptr_t callback,
					     int32_t resource_id,
					     int32_t *length)
{
	if (!guest_code(callback) || ((uintptr_t)length & 3) ||
	    !guest_data(length, sizeof(*length), PF_W))
		fail("resource provider callback/output bounds");
	uintptr_t anchor;
	if (guest_uses_ads_abi())
		anchor = ads_active_api_sp;
	else
		__asm__ volatile("mov %0, sp" : "=r"(anchor));
	if (anchor < (uintptr_t)app.stack_low + 4096 + 256 ||
	    anchor > (uintptr_t)app.stack_top)
		fail("resource provider nested guest stack has less than 4096 bytes");
	/* Keep the nested callback below the suspended guest API frame. GCC's
	 * trampoline also saves 104 host bytes on this same managed stack. */
	void *nested_top = (void *)((anchor - 256) & ~(uintptr_t)7);
	uintptr_t outer_api_sp = ads_active_api_sp;
	int outer_phase = app.phase;
	app.phase = 2;
	uint32_t result =
		guest_uses_ads_abi() ?
			ads_guest_call(callback, nested_top, app.ads.data_base,
				       (uint32_t)resource_id,
				       (uint32_t)(uintptr_t)length, 0, 0) :
			arm_guest_call(callback, nested_top,
				       (uint32_t)resource_id,
				       (uint32_t)(uintptr_t)length, 0);
	ads_active_api_sp = outer_api_sp;
	app.phase = outer_phase;
	return result;
}
static uint32_t callback_call_at(uintptr_t callback, void *stack_top,
				 uint32_t first, uint32_t second,
				 uint32_t third, uint32_t fourth)
{
	uintptr_t top = (uintptr_t)stack_top;
	if (!guest_code(callback) || (top & 7) || top < 8 ||
	    (!inside_pointer((void *)(top - 8), 8, app.stack_low, STACK_SIZE) &&
	     !mre_memory_contains(&app.memory, (void *)(top - 8), 8)))
		fail("callback code or owned stack is invalid");
	int outer_phase = app.phase;
	uintptr_t outer_api_sp = ads_active_api_sp;
	app.phase = 2;
	uint32_t result = guest_uses_ads_abi() ?
				  ads_guest_call(callback, stack_top,
						 app.ads.data_base, first,
						 second, third, fourth) :
				  arm_guest_call4(callback, stack_top, first,
						  second, third, fourth);
	ads_active_api_sp = outer_api_sp;
	app.phase = outer_phase;
	return result;
}
static uint32_t callback_call(uintptr_t callback, uint32_t first,
			      uint32_t second, uint32_t third, uint32_t fourth)
{
	return callback_call_at(callback, app.stack_top, first, second, third,
				fourth);
}
#include "native-services.inc"
static uint32_t utility_copy_callback(uintptr_t callback, int32_t stage,
				      uint32_t total, uint32_t done,
				      int32_t handle)
{
	if (!guest_code(callback))
		fail("copy callback outside original executable code");
	uintptr_t anchor;
	if (guest_uses_ads_abi())
		anchor = ads_active_api_sp;
	else
		__asm__ volatile("mov %0, sp" : "=r"(anchor));
	if (anchor < (uintptr_t)app.stack_low + 4096 + 256 ||
	    anchor > (uintptr_t)app.stack_top)
		fail("copy callback nested guest stack has less than 4096 bytes");
	void *nested_top = (void *)((anchor - 256) & ~(uintptr_t)7);
	return callback_call_at(callback, nested_top, (uint32_t)stage, total,
				done, (uint32_t)handle);
}
#include "native-utilities.inc"
#include "native-vfs.inc"
#include "native-http.inc"
#include "native-camera.inc"
#include "native-media.inc"
#include "native-editor.inc"
int32_t MRE_ABI utility_string_equals(const char *first, const char *second)
{
	check_guest_api_stack();
	(void)ascii_length(first);
	(void)ascii_length(second);
	return strcasecmp(first, second);
}
int32_t MRE_ABI utility_ends_with(const char *text, const char *suffix)
{
	check_guest_api_stack();
	size_t length = ascii_length(text), tail = ascii_length(suffix);
	return tail <= length && !memcmp(text + length - tail, suffix, tail) ?
		       0 :
		       1;
}
int32_t MRE_ABI utility_strtoi(char *text)
{
	check_guest_api_stack();
	(void)ascii_length(text);
	char *end;
	errno = 0;
	long value = strtol(text, &end, 10);
	if (end == text || errno == ERANGE || value < INT32_MIN ||
	    value > INT32_MAX)
		return 0;
	return (int32_t)value;
}
int32_t MRE_ABI utility_wide_to_gb2312(char *out, int32_t bytes,
				       const uint16_t *text)
{
	return host_wide_to_ascii(out, bytes, (uint16_t *)text);
}
int32_t MRE_ABI utility_pen_supported(void)
{
	check_guest_api_stack();
	return 0;
}
int32_t MRE_ABI utility_sim_count(void)
{
	check_guest_api_stack();
	return 0; /* The runtime does not expose SIM access to guests. */
}
void MRE_ABI utility_vibrator_once(void)
{
	check_guest_api_stack();
	debug_trace("VIBRATOR guest API is unsupported\n");
}
/* Uniform SDK export table and ABI bridges. */
int32_t MRE_ABI host_res_init_with_language(uint32_t language);
#include "native-api.inc"

#include "native-loader.inc"

static void create_stack(void)
{
	if (guest_uses_ads_abi())
		return;
	app.stack_mapping_size = STACK_SIZE + 2 * app.page_size;
	app.stack_map = mmap(NULL, app.stack_mapping_size, PROT_NONE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (app.stack_map == MAP_FAILED)
		fail("allocating guest stack failed");
	app.stack_low = app.stack_map + app.page_size;
	app.stack_top = app.stack_low + STACK_SIZE;
	if (mprotect(app.stack_low, STACK_SIZE, PROT_READ | PROT_WRITE))
		fail("guest stack mprotect failed");
	fprintf(stderr,
		"GUEST_STACK low=%p top=%p usable=%u guard_pages=2 alignment=%u\n",
		app.stack_low, app.stack_top, STACK_SIZE,
		(unsigned)((uintptr_t)app.stack_top & 7));
}
static void close_inherited_descriptors(int shared, int sealed_original)
{
	/* Linux 4.19 lacks close_range. Scan the inherited hard fd limit before
     * opening the VXP; parent Wayland/device descriptors must not survive. */
	struct rlimit limit;
	if (getrlimit(RLIMIT_NOFILE, &limit) ||
	    limit.rlim_max == RLIM_INFINITY || limit.rlim_max > INT32_MAX)
		fail("cannot determine inherited fd close range");
	int first = sealed_original ? MRE_ORIGINAL_FD + 1 : shared ? 4 : 3;
	for (int fd = first; fd < (int)limit.rlim_max; ++fd)
		close(fd);
}
static void validate_original_descriptor(void)
{
	struct stat status;
	int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK |
			     F_SEAL_SEAL;
	int seals = fcntl(MRE_ORIGINAL_FD, F_GET_SEALS);
	if (fstat(MRE_ORIGINAL_FD, &status) || !S_ISREG(status.st_mode) ||
	    status.st_size <= 0 || status.st_size > FILE_CAP || seals < 0 ||
	    (seals & required_seals) != required_seals)
		fail("original executable descriptor is not a bounded sealed snapshot");
}
static void prepare_shared_io(int shared)
{
	if (!shared)
		fail("native VXP execution requires the parent broker");
	struct stat status;
	if (fstat(MRE_SHARED_FD, &status) || !S_ISREG(status.st_mode) ||
	    status.st_size != (off_t)sizeof(struct mre_shared))
		fail("shared broker memfd size/type mismatch");
	app.shared = mmap(NULL, sizeof(*app.shared), PROT_READ | PROT_WRITE,
			  MAP_SHARED, MRE_SHARED_FD, 0);
	if (app.shared == MAP_FAILED)
		fail("mapping shared broker memfd failed");
	if (close(MRE_SHARED_FD))
		fail("closing mapped broker descriptor failed");
	size_t length =
		strnlen(app.shared->exec_path, sizeof(app.shared->exec_path));
	if (length < 3 || length == sizeof(app.shared->exec_path) ||
	    !mre_utf8_decode((const unsigned char *)app.shared->exec_path,
			     length, NULL, 0, NULL) ||
	    app.shared->exec_path[1] != ':' ||
	    app.shared->exec_path[2] != '\\' ||
	    (app.shared->exec_path[0] != 'E' &&
	     app.shared->exec_path[0] != 'C'))
		fail("selected executable path is not a bounded virtual path");
	memcpy(app.exec_path, app.shared->exec_path, length + 1);
	app.screen_width = app.shared->screen_width == MRE_FRAME_WIDTH ?
				   (int)app.shared->screen_width :
				   MRE_FRAME_WIDTH;
	app.screen_height = app.shared->screen_height == MRE_FRAME_HEIGHT ?
				    (int)app.shared->screen_height :
				    MRE_FRAME_HEIGHT;
	app.visible = 1;
}
static void prepare_display(void)
{
	app.clip_width = guest_screen_width();
	app.clip_height = guest_screen_height();
	app.active_layer = app.alpha_layer = -1;
	memcpy(app.game_language, "en", 3);
	for (size_t i = 0; i < LAYER_CAP; ++i) {
		app.layers[i].buffer = mmap(NULL, 32 + layer_bytes(),
					    PROT_READ | PROT_WRITE,
					    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (app.layers[i].buffer == MAP_FAILED)
			fail("allocating RGB565 layer failed");
		app.layers[i].pixels = (uint16_t *)(app.layers[i].buffer + 32);
	}
	char error[128];
	if (!fplinux_font_open_default(&app.font, MRE_FRAME_WIDTH, error,
				       sizeof(error)))
		fail("default SDK font: %s", error);
}
static void limits(void)
{
	struct rlimit cpu = { 120, 120 },
		      memory = { 64u * 1024u * 1024u, 64u * 1024u * 1024u };
	struct rlimit files = { 16, 16 }, core = { 0, 0 };
	if (setrlimit(RLIMIT_CPU, &cpu) || setrlimit(RLIMIT_AS, &memory) ||
	    setrlimit(RLIMIT_NOFILE, &files) || setrlimit(RLIMIT_CORE, &core) ||
	    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		fail("setting isolated process resource limits failed");
	alarm(900);
}
static void prepare_input_text(void)
{
	app.input_text_bytes =
		(MRE_EDITOR_UNITS + 1U) * sizeof(*app.input_text);
	app.input_text = mmap(NULL, app.input_text_bytes,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (app.input_text == MAP_FAILED)
		fail("allocating bounded SDK input text failed");
}
static void sdk_seccomp(void)
{
#define ALLOW_SYSCALL(number)                              \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, number, 0, 1), \
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
#define ALLOW_STDIO(number)                                          \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, number, 0, 6),           \
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,                   \
			 offsetof(struct seccomp_data, args[0])),    \
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 3, 0),        \
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 2, 0),        \
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 1, 0),        \
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS), \
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
#define ALLOW_NONEXEC_MEMORY(number)                                   \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, number, 0, 4),             \
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,                     \
			 offsetof(struct seccomp_data, args[2])),      \
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1), \
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),   \
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_ARM, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		ALLOW_SYSCALL(__NR_exit) ALLOW_SYSCALL(__NR_exit_group)
			ALLOW_SYSCALL(__NR_rt_sigreturn)
				ALLOW_SYSCALL(__NR_clock_gettime32)
					ALLOW_SYSCALL(__NR_clock_gettime64)
						ALLOW_SYSCALL(__NR_brk)
		/* One stdin poll serves the nearest SDK callback deadline. */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_poll, 0, 4),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, args[1])),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
		/* Guest mappings have already been installed. Runtime allocations
         * cannot acquire executable permission after this filter. */
		ALLOW_NONEXEC_MEMORY(__NR_mmap2) ALLOW_SYSCALL(__NR_munmap)
			ALLOW_NONEXEC_MEMORY(__NR_mprotect) ALLOW_SYSCALL(
				__NR_mremap) ALLOW_STDIO(__NR_read)
				ALLOW_STDIO(__NR_write) ALLOW_STDIO(
					__NR_writev) ALLOW_STDIO(__NR_close)
					BPF_STMT(BPF_RET | BPF_K,
						 SECCOMP_RET_KILL_PROCESS),
	};
#undef ALLOW_SYSCALL
#undef ALLOW_STDIO
#undef ALLOW_NONEXEC_MEMORY
	struct sock_fprog program = {
		(unsigned short)(sizeof(filter) / sizeof(filter[0])), filter
	};
	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program))
		fail("ARM seccomp filter unavailable: %s; original VXP will not execute",
		     strerror(errno));
	fprintf(stderr,
		"SECCOMP installed ARM filter; stdio IPC only; no open/exec/socket/ioctl/clone\n");
}
static int hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}
static int receive_viewer_line(char line[static WIRE_LINE_BYTES])
{
	memset(line, 0, WIRE_LINE_BYTES);
	size_t length = 0;
	for (;;) {
		char byte = 0;
		ssize_t count = read(STDIN_FILENO, &byte, 1);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			fail("reading viewer response: %s", strerror(errno));
		if (!count)
			return 0;
		if (byte == '\n') {
			line[length] = 0;
			return 1;
		}
		if (length == WIRE_LINE_BYTES - 1 || !byte)
			fail("viewer response exceeds bounded line protocol");
		line[length++] = byte;
	}
}
static int receive_viewer_command(char line[WIRE_LINE_BYTES])
{
	if (!app.deferred_count)
		return receive_viewer_line(line);
	memcpy(line, app.deferred[0], sizeof(app.deferred[0]));
	app.deferred_count--;
	memmove(app.deferred, app.deferred + 1,
		app.deferred_count * sizeof(app.deferred[0]));
	return 1;
}
static int utility_queue_http_line(const char *line)
{
	if (strncmp(line, "HTTP_RESPONSE\t", 14))
		return 0;
	unsigned id;
	int used;
	if (sscanf(line + 14, "%u%n", &id, &used) != 1 || line[14 + used])
		fail("invalid HTTP response id");
	(void)native_http_receive_response(id);
	return 1;
}
static void utility_defer_input(const char *line)
{
	if (strncmp(line, "KEY\t", 4) && strncmp(line, "PEN\t", 4) &&
	    strncmp(line, "VISIBILITY\t", 11))
		fail("unexpected command while awaiting a utility broker reply");
	if (app.deferred_count ==
	    sizeof(app.deferred) / sizeof(app.deferred[0]))
		fail("too many events queued while awaiting utility broker reply");
	memcpy(app.deferred[app.deferred_count++], line, strlen(line) + 1);
}
static void utility_http_accept(uint32_t id)
{
	char expected[64], line[WIRE_LINE_BYTES];
	snprintf(expected, sizeof(expected), "HTTP_ACCEPTED\t%u", id);
	/* The parent first owns an immutable request copy. The guest may cancel
	 * or submit again immediately after this API returns, reusing the shared
	 * slot. Queue responses and input here without entering guest callbacks. */
	while (receive_viewer_line(line)) {
		if (!strcmp(line, expected)) {
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			return;
		}
		if (!utility_queue_http_line(line))
			utility_defer_input(line);
	}
	_exit(0);
}
static int utility_vfs_exchange(const struct mre_vfs_request *request,
				struct mre_vfs_response *response)
{
	if (!app.shared)
		return -1;
	uint32_t sequence = ++app.vfs_sequence;
	if (!sequence)
		sequence = ++app.vfs_sequence;
	app.shared->vfs_request = *request;
	app.shared->vfs_request.sequence = sequence;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	char outgoing[64], expected[64], line[WIRE_LINE_BYTES];
	int length = snprintf(outgoing, sizeof(outgoing), "VFS_REQUEST\t%u\n",
			      sequence);
	write_protocol(outgoing, (size_t)length);
	snprintf(expected, sizeof(expected), "VFS_REPLY\t%u", sequence);
	while (receive_viewer_line(line)) {
		if (!strcmp(line, expected)) {
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if (app.shared->vfs_response.sequence != sequence)
				fail("utility VFS shared response sequence mismatch");
			*response = app.shared->vfs_response;
			return 0;
		}
		if (!utility_queue_http_line(line))
			utility_defer_input(line);
	}
	_exit(0); /* No callback may continue after its parent broker disappears. */
}
static int utility_snapshot_exchange(const char *virtual_path)
{
	size_t bytes = strnlen(virtual_path, MRE_VFS_PATH_BYTES);
	if (!app.shared || !bytes || bytes == MRE_VFS_PATH_BYTES)
		return -1;
	char request[2 * MRE_VFS_PATH_BYTES + 16], line[WIRE_LINE_BYTES];
	memcpy(request, "SNAPSHOT\t", 9);
	hex_encode(request + 9, virtual_path, bytes);
	request[9 + 2 * bytes] = '\n';
	write_protocol(request, 10 + 2 * bytes);
	while (receive_viewer_line(line)) {
		if (!strcmp(line, "SNAPSHOT_DONE")) {
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			return 0;
		}
		if (!strcmp(line, "SNAPSHOT_ERROR") ||
		    !strncmp(line, "ERROR\t", 6))
			return -1;
		if (!utility_queue_http_line(line))
			utility_defer_input(line);
	}
	_exit(0);
}
static int publish_frame(void)
{
	if (!app.phase) {
		uint32_t dirty = editor_take_dirty_layers();
		int32_t handles[LAYER_CAP], count = 0;
		for (int32_t i = 0; i < (int32_t)LAYER_CAP; ++i)
			if ((dirty & (UINT32_C(1) << i)) && app.layers[i].live)
				handles[count++] = i;
		if (count)
			(void)compose_layers(handles, count);
	}
	if (!app.frame_pending)
		return 1;
	char line[WIRE_LINE_BYTES], expected[64], outgoing[64];
	app.frame_pending = 0;
	app.shared->frame_sequence = ++app.frame_sequence;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	int length = snprintf(outgoing, sizeof(outgoing), "FRAME\t%u\n",
			      app.frame_sequence);
	write_protocol(outgoing, (size_t)length);
	snprintf(expected, sizeof(expected), "ACK\t%u", app.frame_sequence);
	while (receive_viewer_line(line)) {
		if (!strcmp(line, expected))
			return publish_frame();
		if (!strncmp(line, "HTTP_RESPONSE\t", 14)) {
			unsigned id;
			int used;
			if (sscanf(line + 14, "%u%n", &id, &used) != 1 ||
			    line[14 + used])
				fail("invalid HTTP response id while awaiting frame ACK");
			(void)native_http_receive_response(id);
			continue;
		}
		if (strncmp(line, "KEY\t", 4) &&
		    strncmp(line, "VISIBILITY\t", 11) &&
		    strncmp(line, "PEN\t", 4))
			fail("expected framebuffer ACK before dispatching another guest callback");
		if (app.deferred_count ==
		    sizeof(app.deferred) / sizeof(app.deferred[0]))
			fail("too many input events queued before framebuffer ACK");
		memcpy(app.deferred[app.deferred_count++], line,
		       strlen(line) + 1);
	}
	return 0;
}
static void dispatch_timers(void)
{
	uint64_t now = monotonic_ms();
	for (size_t i = 0; i < ALL_TIMER_CAP && !app.exited; ++i) {
		struct sdk_timer *timer = &app.timers[i];
		if (!timer->callback || timer->next_ms > now)
			continue;
		timer_fn callback = timer->callback;
		/* Coalesce missed intervals and advance before invoking: the callback
		 * may delete or replace this same timer slot. */
		timer->next_ms = now + timer->interval_ms;
		debug_trace("DISPATCH TIMER id=%zu original callback=%p\n",
			    i + 1, (void *)callback);
		app.phase = 2;
		if (guest_uses_ads_abi())
			ads_guest_call((uintptr_t)callback, app.stack_top,
				       app.ads.data_base, (uint32_t)i + 1, 0, 0,
				       0);
		else
			arm_guest_call((uintptr_t)callback, app.stack_top,
				       (uint32_t)i + 1, 0, 0);
		app.phase = 0;
	}
}
static int receive_interactive_command(char line[WIRE_LINE_BYTES])
{
	for (;;) {
		if (app.deferred_count)
			return receive_viewer_command(line);
		if (native_http_response_pending())
			native_http_dispatch_response();
		camera_dispatch_due();
		media_dispatch_due();
		editor_dispatch_due();
		dispatch_timers();
		if (app.exited)
			return 0;
		if (app.visible && app.paint_pending) {
			app.paint_pending = 0;
			call_event(app.system_callback, 1, 0,
				   "requested PAINT");
			if (app.exited)
				return 0;
		}
		if (!publish_frame())
			return 0;
		if (app.deferred_count || (app.visible && app.paint_pending) ||
		    (native_http_response_pending()))
			continue;
		int timeout_ms = -1;
		uint64_t now = monotonic_ms();
		int camera_delay = camera_timeout_ms(now);
		int media_delay = media_timeout_ms(now);
		int editor_delay = editor_timeout_ms(now);
		if (camera_delay >= 0)
			timeout_ms = camera_delay;
		if (media_delay >= 0 &&
		    (timeout_ms < 0 || media_delay < timeout_ms))
			timeout_ms = media_delay;
		if (editor_delay >= 0 &&
		    (timeout_ms < 0 || editor_delay < timeout_ms))
			timeout_ms = editor_delay;
		for (size_t i = 0; i < ALL_TIMER_CAP; ++i) {
			if (!app.timers[i].callback)
				continue;
			uint64_t delay = app.timers[i].next_ms > now ?
						 app.timers[i].next_ms - now :
						 0;
			int delay_ms = delay > INT32_MAX ? INT32_MAX :
							   (int)delay;
			if (timeout_ms < 0 || delay_ms < timeout_ms)
				timeout_ms = delay_ms;
		}
		struct pollfd input = { STDIN_FILENO, POLLIN, 0 };
		int ready = (int)syscall(__NR_poll, &input, 1, timeout_ms);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			fail("polling game input/timers failed: %s",
			     strerror(errno));
		if (ready > 0)
			return receive_viewer_line(line);
	}
}
static void call_event(event_fn callback, int32_t first, int32_t second,
		       const char *kind)
{
	if (!callback || app.exited)
		return;
	fprintf(stderr, "DISPATCH %s first=%d second=%d original callback=%p\n",
		kind, first, second, (void *)callback);
	app.phase = 2;
	if (guest_uses_ads_abi())
		ads_guest_call((uintptr_t)callback, app.stack_top,
			       app.ads.data_base, (uint32_t)first,
			       (uint32_t)second, 0, 0);
	else
		arm_guest_call((uintptr_t)callback, app.stack_top,
			       (uint32_t)first, (uint32_t)second, 0);
	app.phase = 0;
}
#include "native-dispatch.inc"

static void cleanup(void)
{
	camera_cleanup();
	media_cleanup();
	editor_cleanup();
	native_http_cancel_all();
	while (app.canvases)
		free_canvas_pointer(app.canvases->buffer);
	mre_memory_destroy(&app.memory);
	free(app.string_source);
	if (app.stack_map)
		munmap(app.stack_map, app.stack_mapping_size);
	if (app.input_text)
		munmap(app.input_text, app.input_text_bytes);
	if (guest_uses_ads_abi())
		ads_image_unload(&app.ads);
	else {
		free(app.ads.resources);
		if (app.image)
			munmap(app.image, app.image_size);
	}
	for (size_t i = 0; i < LAYER_CAP; ++i)
		if (app.layers[i].buffer)
			munmap(app.layers[i].buffer, 32 + layer_bytes());
	if (app.shared)
		munmap(app.shared, sizeof(*app.shared));
	fplinux_font_close(&app.font);
}
int main(int argc, char **argv)
{
	const char *trace_value = getenv("FPLINUX_MRE_TRACE");
	trace_enabled = trace_value && !strcmp(trace_value, "1");
	setvbuf(stderr, NULL, _IONBF, 0);
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		fail("ignoring SIGPIPE failed");
	if (argc != 3 ||
	    (strcmp(argv[2], "--shared") && strcmp(argv[2], "--validate")) ||
	    strcmp(argv[1], MRE_ORIGINAL_PATH))
		fail("usage: mre-engine /proc/self/fd/4 --shared|--validate");
	close_inherited_descriptors(1, 1);
	validate_original_descriptor();
	/* Validation uses the same native loader without entering guest code. */
	if (!strcmp(argv[2], "--validate")) {
		load_vxp(MRE_ORIGINAL_PATH);
		cleanup();
		return 0;
	}
	prepare_shared_io(1);
	load_vxp(MRE_ORIGINAL_PATH);
	if (close(MRE_ORIGINAL_FD))
		fail("closing loaded original executable descriptor failed");
	create_stack();
	if (mre_memory_init(&app.memory, app.ram_limit))
		fail("preparing bounded SDK allocation registry failed");
	prepare_display();
	prepare_input_text();
	limits();
	prepare_fault_diagnostic();
	sdk_seccomp();
	app.phase = 1;
	if (guest_uses_ads_abi())
		ads_guest_call(app.entry, app.stack_top, app.ads.data_base, 0,
			       0, 0, 0);
	else
		arm_guest_call(app.entry, app.stack_top,
			       (uint32_t)(uintptr_t)host_resolve, 0, 0);
	app.phase = 0;
	dispatch_viewer();
	cleanup();
	return 0;
}
