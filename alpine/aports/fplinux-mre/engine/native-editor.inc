/* SPDX-License-Identifier: 0BSD */
/* Classic editor boundary. The host edits the caller's UTF16 buffer, delivers
 * text updates outside guest entry, and requests the original PAINT callback.
 * Rendering runs only on a checked guest API stack. */
#define INLINE_EDITOR_CAP 2u
#define INLINE_EDITOR_BYTES FILE_CAP
#define INLINE_EDITOR_SOFTKEY_HEIGHT 24u
#define INLINE_EDITOR_IME_CAP 16u
#define INLINE_EDITOR_TAP_MS 800u

typedef void(MRE_ABI *editor_softkey_fn)(void);
typedef uint32_t(MRE_ABI *editor_ime_fn)(int32_t, void *);
typedef void(MRE_ABI *editor_update_fn)(uint8_t *, uint8_t *, int32_t);
struct editor_font {
	uint8_t bold, italic, underline, size;
};
struct editor_message {
	uint32_t message_id;
	int32_t x, y, width, height;
};
_Static_assert(sizeof(struct editor_font) == 4, "SDK editor font by-value ABI");
_Static_assert(sizeof(struct editor_message) == 20,
	       "SDK editor IME message ABI");

struct inline_editor {
	int32_t handle, x, y, width, height, layer;
	uint16_t *text;
	size_t buffer_bytes, cursor, first_row;
	uint32_t input_type;
	uint16_t input_mode;
	uint8_t uppercase, disable_softkey, border;
	uint16_t normal_border, focused_border;
	struct editor_font font;
	struct {
		uint16_t *label;
		editor_softkey_fn callback;
	} softkeys[3];
	editor_ime_fn ime;
	editor_update_fn update;
	uint8_t update_pending;
	int tap_key;
	unsigned tap_index;
	size_t tap_cursor;
	uint64_t tap_until;
};
static struct {
	struct inline_editor slots[INLINE_EDITOR_CAP];
	int32_t next_handle, active_handle;
	uint32_t dirty_layers;
	struct {
		int32_t handle;
		uint32_t message;
	} ime[INLINE_EDITOR_IME_CAP];
	size_t ime_count;
} inline_editors;

static struct inline_editor *editor_find(int32_t handle)
{
	if (handle <= 0)
		return NULL;
	for (size_t i = 0; i < INLINE_EDITOR_CAP; ++i)
		if (inline_editors.slots[i].handle == handle)
			return &inline_editors.slots[i];
	return NULL;
}

/* A byte capacity always includes the terminating UTF16 word. This helper
 * never scans beyond the supplied buffer or repairs an unterminated string. */
static int editor_text_length(const uint16_t *text, size_t bytes,
			      uint32_t flags, size_t *length)
{
	if (!text || ((uintptr_t)text & 1) || bytes < 2 || (bytes & 1) ||
	    bytes > INLINE_EDITOR_BYTES || !guest_data(text, bytes, flags))
		return 0;
	for (size_t i = 0; i < bytes / 2; ++i)
		if (!text[i]) {
			*length = i;
			return 1;
		}
	return 0;
}

static int editor_live_text(struct inline_editor *editor, size_t *length)
{
	if (!editor || !editor_text_length(editor->text, editor->buffer_bytes,
					   PF_R | PF_W, length))
		return 0;
	if (editor->cursor > *length)
		editor->cursor = *length;
	return 1;
}

static int editor_high_surrogate(uint16_t word)
{
	return word >= 0xd800 && word <= 0xdbff;
}
static int editor_low_surrogate(uint16_t word)
{
	return word >= 0xdc00 && word <= 0xdfff;
}
static size_t editor_next(const uint16_t *text, size_t length, size_t cursor)
{
	if (cursor >= length)
		return length;
	return cursor +
	       (editor_high_surrogate(text[cursor]) && cursor + 1 < length &&
				editor_low_surrogate(text[cursor + 1]) ?
			2 :
			1);
}
static size_t editor_previous(const uint16_t *text, size_t cursor)
{
	if (!cursor)
		return 0;
	return cursor -
	       (cursor >= 2 && editor_low_surrogate(text[cursor - 1]) &&
				editor_high_surrogate(text[cursor - 2]) ?
			2 :
			1);
}
static void editor_commit_tap(struct inline_editor *editor)
{
	editor->tap_key = 0;
	editor->tap_until = 0;
}
static void editor_queue_ime(struct inline_editor *editor, uint32_t message)
{
	if (!editor->ime)
		return;
	if (inline_editors.ime_count == INLINE_EDITOR_IME_CAP)
		fail("inline editor IME queue bound");
	inline_editors.ime[inline_editors.ime_count].handle = editor->handle;
	inline_editors.ime[inline_editors.ime_count++].message = message;
}

static int editor_rect_valid(int32_t x, int32_t y, int32_t width,
			     int32_t height)
{
	return x >= 0 && y >= 0 && x <= guest_screen_width() &&
	       y <= guest_screen_height() && width >= 0 && height >= 0 &&
	       width <= guest_screen_width() - x &&
	       height <= guest_screen_height() - y;
}

int32_t MRE_ABI host_editor_create(int32_t type, int32_t x, int32_t y,
				   int32_t width, int32_t height,
				   uint16_t *text, int32_t bytes,
				   int32_t disable_softkey, int32_t layer)
{
	check_guest_api_stack();
	size_t length;
	if (app.exited || type < 0 || type > 1 || bytes < 2 ||
	    !editor_text_length(text, (size_t)bytes, PF_R | PF_W, &length) ||
	    !editor_rect_valid(x, y, width, height) ||
	    (disable_softkey != 0 && disable_softkey != 1) || layer < 0 ||
	    layer >= (int32_t)LAYER_CAP || !app.layers[layer].live ||
	    inline_editors.next_handle == INT32_MAX)
		return 0;
	for (size_t i = 0; i < INLINE_EDITOR_CAP; ++i) {
		struct inline_editor *editor = &inline_editors.slots[i];
		if (editor->handle)
			continue;
		memset(editor, 0, sizeof(*editor));
		editor->handle = ++inline_editors.next_handle;
		editor->x = x;
		editor->y = y;
		editor->width = width;
		editor->height = height;
		editor->layer = layer;
		editor->text = text;
		editor->buffer_bytes = (size_t)bytes;
		editor->cursor = length;
		editor->disable_softkey = (uint8_t)disable_softkey;
		debug_trace(
			"EDITOR create handle=%d rect=%d,%d,%d,%d bytes=%d layer=%d\n",
			editor->handle, x, y, width, height, bytes, layer);
		return editor->handle;
	}
	return 0;
}

void MRE_ABI host_editor_close(int32_t handle)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (!editor)
		return;
	if (inline_editors.active_handle == handle)
		inline_editors.active_handle = 0;
	/* The caller owns the original text allocation. */
	memset(editor, 0, sizeof(*editor));
}

void MRE_ABI host_editor_set_ime(int32_t handle, uint32_t input_type,
				 const uint16_t *modes, int32_t default_mode,
				 editor_ime_fn callback)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (!editor || default_mode < 0 || default_mode > UINT16_MAX ||
	    (callback && !guest_code((uintptr_t)callback)))
		return;
	/* The original's eight-byte mode array is {7,1,2,0}: 16-bit enums. */
	if (modes) {
		if ((uintptr_t)modes & 1)
			return;
		size_t i;
		for (i = 0; i < 10; ++i) {
			if (!guest_data(modes + i, 2, PF_R))
				return;
			if (!modes[i])
				break;
		}
		if (i == 10)
			return;
	}
	editor->input_type = input_type;
	editor->input_mode = (uint16_t)default_mode;
	editor->uppercase = default_mode == 3 || default_mode == 5;
	editor->ime = callback;
}

void MRE_ABI host_editor_set_softkey(int32_t handle, uint16_t *label,
				     int32_t key, editor_softkey_fn callback)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (!editor || key < 0 || key > 2 ||
	    (callback && !guest_code((uintptr_t)callback)))
		return;
	if (label) {
		size_t length = wide_length(label);
		if (length > 63)
			return;
	}
	editor->softkeys[key].label = label;
	editor->softkeys[key].callback = callback;
}

uint32_t MRE_ABI host_editor_softkey_height(void)
{
	check_guest_api_stack();
	return INLINE_EDITOR_SOFTKEY_HEIGHT;
}

void MRE_ABI host_editor_font(int32_t handle, struct editor_font font)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (editor)
		editor->font = font;
}
void MRE_ABI host_editor_border(int32_t handle, int32_t border, uint16_t normal,
				uint16_t focused)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (!editor || border < 0 || border > 2)
		return;
	editor->border = (uint8_t)border;
	editor->normal_border = normal;
	editor->focused_border = focused;
}
void MRE_ABI host_editor_pos(int32_t handle, int32_t x, int32_t y)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (editor && editor_rect_valid(x, y, editor->width, editor->height)) {
		editor->x = x;
		editor->y = y;
	}
}
void MRE_ABI host_editor_size(int32_t handle, int32_t width, int32_t height)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (editor && editor_rect_valid(editor->x, editor->y, width, height)) {
		editor->width = width;
		editor->height = height;
	}
}

void MRE_ABI host_editor_set_text(int32_t handle, uint16_t *text, int32_t bytes)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	size_t length;
	if (!editor || bytes < 2 ||
	    !editor_text_length(text, (size_t)bytes, PF_R | PF_W, &length))
		return;
	/* Like create, set_text attaches the caller's preallocated writable
	 * buffer. The caller may release the previous allocation after return. */
	editor->text = text;
	editor->buffer_bytes = (size_t)bytes;
	editor->cursor = length;
	editor->first_row = 0;
	editor_commit_tap(editor);
}

void MRE_ABI host_editor_set_update_text(int32_t handle,
					 editor_update_fn callback)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (!editor || (callback && !guest_code((uintptr_t)callback)))
		return;
	editor->update = callback;
	if (!callback)
		editor->update_pending = 0;
}

int32_t MRE_ABI host_editor_cursor(int32_t handle)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	size_t length;
	return editor_live_text(editor, &length) ? (int32_t)editor->cursor : -1;
}

void MRE_ABI host_editor_save(int32_t handle)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	size_t length;
	if (!editor_live_text(editor, &length))
		return;
	/* Keypad characters are committed to the caller buffer immediately.
	 * Settle the only transient IME state; cursor, scroll and input mode stay
	 * attached to this handle across deactivate/activate. This is editor
	 * history, not an application file save. */
	editor_commit_tap(editor);
}

int32_t MRE_ABI host_editor_get_text(int32_t handle, uint16_t *out,
				     int32_t bytes)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	size_t length;
	if (bytes < 2 || (bytes & 1) || bytes > (int32_t)INLINE_EDITOR_BYTES ||
	    ((uintptr_t)out & 1) || !guest_data(out, (size_t)bytes, PF_W) ||
	    !editor_live_text(editor, &length))
		return -1;
	size_t capacity = (size_t)bytes / 2 - 1;
	if (length > capacity) {
		length = capacity;
		if (length && editor_high_surrogate(editor->text[length - 1]) &&
		    editor_low_surrogate(editor->text[length]))
			--length;
	}
	memmove(out, editor->text, length * 2);
	out[length] = 0;
	return (int32_t)length;
}

void MRE_ABI host_editor_activate(int32_t handle, int32_t virtual_keyboard)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	(void)virtual_keyboard; /* Text entry uses the physical keypad. */
	if (!editor || inline_editors.active_handle == handle)
		return;
	struct inline_editor *previous =
		editor_find(inline_editors.active_handle);
	if (previous) {
		editor_commit_tap(previous);
		editor_queue_ime(previous, 3);
	}
	inline_editors.active_handle = handle;
	editor_queue_ime(editor, 2);
	app.paint_pending = 1;
}
void MRE_ABI host_editor_deactivate(int32_t handle)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	if (!editor || inline_editors.active_handle != handle)
		return;
	editor_commit_tap(editor);
	inline_editors.active_handle = 0;
	editor_queue_ime(editor, 3);
}

/* Layout follows UTF16 buffer indices, including explicit newlines and wrap.
 * Surrogate pairs remain indivisible for movement and deletion. */
static size_t editor_line_end(const uint16_t *text, size_t length, size_t start,
			      size_t columns)
{
	size_t cursor = start;
	while (cursor < length && text[cursor] != '\n' &&
	       text[cursor] != '\r') {
		size_t next = editor_next(text, length, cursor);
		if (next - start > columns && cursor > start)
			break;
		cursor = next;
	}
	return cursor;
}
static size_t editor_after_line(const uint16_t *text, size_t length, size_t end)
{
	if (end < length && text[end] == '\r') {
		++end;
		if (end < length && text[end] == '\n')
			++end;
	} else if (end < length && text[end] == '\n')
		++end;
	return end;
}
static size_t editor_columns(const struct inline_editor *editor)
{
	int advance = font_width_for_size(editor->font.size) +
		      (editor->font.bold != 0) + (editor->font.italic ? 3 : 0);
	int width = editor->width - 8;
	return advance > 0 && width >= advance ? (size_t)(width / advance) : 1;
}
static void editor_cursor_location(const struct inline_editor *editor,
				   size_t length, size_t columns, size_t *row,
				   size_t *column)
{
	size_t start = 0;
	*row = 0;
	for (;;) {
		size_t end =
			editor_line_end(editor->text, length, start, columns);
		if (editor->cursor < end || end == length ||
		    (editor->cursor == end && end < length &&
		     (editor->text[end] == '\n' ||
		      editor->text[end] == '\r'))) {
			*column = editor->cursor >= start ?
					  editor->cursor - start :
					  0;
			return;
		}
		start = editor_after_line(editor->text, length, end);
		++*row;
	}
}
static void editor_move_vertical(struct inline_editor *editor, size_t length,
				 int direction)
{
	size_t columns = editor_columns(editor), row, column;
	editor_cursor_location(editor, length, columns, &row, &column);
	if (direction < 0 && !row)
		return;
	size_t wanted = direction < 0 ? row - 1 : row + 1, start = 0;
	for (size_t current = 0;; ++current) {
		size_t end =
			editor_line_end(editor->text, length, start, columns);
		if (current == wanted) {
			size_t target = start + column;
			if (target > end)
				target = end;
			if (target > start && target < end &&
			    editor_low_surrogate(editor->text[target]) &&
			    editor_high_surrogate(editor->text[target - 1]))
				--target;
			editor->cursor = target;
			return;
		}
		if (end == length)
			return;
		start = editor_after_line(editor->text, length, end);
	}
}

static void editor_draw_softkeys(struct inline_editor *editor, uint8_t *buffer)
{
	if (editor->disable_softkey)
		return;
	int old_clip[4] = { app.clip_x, app.clip_y, app.clip_width,
			    app.clip_height };
	int old_font[4] = { app.font_size, app.font_bold, app.font_italic,
			    app.font_underline };
	app.font_size = editor->font.size;
	app.font_bold = editor->font.bold != 0;
	app.font_italic = editor->font.italic != 0;
	app.font_underline = editor->font.underline != 0;
	int advance = glyph_advance();
	app.clip_x = 0;
	app.clip_y = guest_screen_height() - INLINE_EDITOR_SOFTKEY_HEIGHT;
	app.clip_width = guest_screen_width();
	app.clip_height = INLINE_EDITOR_SOFTKEY_HEIGHT;
	host_fill(buffer, 0, app.clip_y, app.clip_width, app.clip_height,
		  0xbdf7, 0xe71c);
	for (int key = 0; key < 3; ++key) {
		uint16_t *label = editor->softkeys[key].label;
		if (!label)
			continue;
		size_t units = wide_length(label);
		if (units > 63)
			continue;
		int x = key == 0 ?
				4 :
			key == 1 ?
				guest_screen_width() - 4 -
					(int)units * advance :
				(guest_screen_width() - (int)units * advance) /
					2;
		host_textout(buffer, x, app.clip_y + 4, label, (int32_t)units,
			     0);
	}
	app.clip_x = old_clip[0];
	app.clip_y = old_clip[1];
	app.clip_width = old_clip[2];
	app.clip_height = old_clip[3];
	app.font_size = old_font[0];
	app.font_bold = old_font[1];
	app.font_italic = old_font[2];
	app.font_underline = old_font[3];
}

void MRE_ABI host_editor_redraw_ime(void)
{
	check_guest_api_stack();
	struct inline_editor *editor =
		editor_find(inline_editors.active_handle);
	if (!editor || editor->disable_softkey || editor->layer < 0 ||
	    editor->layer >= (int32_t)LAYER_CAP ||
	    !app.layers[editor->layer].live)
		return;
	if (editor->update_pending) {
		app.paint_pending = 1;
		return;
	}
	/* The physical-keypad IME has a fixed softkey strip and no floating UI. */
	editor_draw_softkeys(editor,
			     (uint8_t *)app.layers[editor->layer].pixels);
	inline_editors.dirty_layers |= UINT32_C(1) << editor->layer;
}

void MRE_ABI host_editor_show(int32_t handle)
{
	check_guest_api_stack();
	struct inline_editor *editor = editor_find(handle);
	size_t length;
	if (!editor_live_text(editor, &length) || !editor->width ||
	    !editor->height || editor->layer < 0 ||
	    editor->layer >= (int32_t)LAYER_CAP ||
	    !app.layers[editor->layer].live)
		return;
	if (editor->update_pending) {
		app.paint_pending = 1;
		return;
	}
	int old_clip[4] = { app.clip_x, app.clip_y, app.clip_width,
			    app.clip_height };
	int old_font[4] = { app.font_size, app.font_bold, app.font_italic,
			    app.font_underline };
	app.font_size = editor->font.size;
	app.font_bold = editor->font.bold != 0;
	app.font_italic = editor->font.italic != 0;
	app.font_underline = editor->font.underline != 0;
	int active = inline_editors.active_handle == handle;
	uint8_t *buffer = (uint8_t *)app.layers[editor->layer].pixels;
	app.clip_x = editor->x;
	app.clip_y = editor->y;
	app.clip_width = editor->width;
	app.clip_height = editor->height;
	uint16_t border = active ? editor->focused_border :
				   editor->normal_border;
	host_fill(buffer, editor->x, editor->y, editor->width, editor->height,
		  editor->border ? border : 0xffff, 0xffff);
	if (editor->border == 2 && editor->width > 2 && editor->height > 2)
		host_fill(buffer, editor->x + 1, editor->y + 1,
			  editor->width - 2, editor->height - 2, border,
			  0xffff);
	int advance = glyph_advance(), line_height = glyph_height() + 2;
	size_t columns = editor_columns(editor), cursor_row, cursor_column;
	size_t visible_rows =
		editor->height > 8 && line_height > 0 ?
			(size_t)((editor->height - 8) / line_height) :
			1;
	if (!visible_rows)
		visible_rows = 1;
	editor_cursor_location(editor, length, columns, &cursor_row,
			       &cursor_column);
	if (cursor_row < editor->first_row)
		editor->first_row = cursor_row;
	else if (cursor_row >= editor->first_row + visible_rows)
		editor->first_row = cursor_row - visible_rows + 1;
	size_t start = 0, row = 0;
	for (;;) {
		size_t end =
			editor_line_end(editor->text, length, start, columns);
		if (row >= editor->first_row &&
		    row - editor->first_row < visible_rows)
			host_textout(buffer, editor->x + 4,
				     editor->y + 4 +
					     (int)(row - editor->first_row) *
						     line_height,
				     editor->text + start,
				     (int32_t)(end - start), 0);
		if (end == length || row >= editor->first_row + visible_rows)
			break;
		start = editor_after_line(editor->text, length, end);
		++row;
	}
	if (active)
		host_fill(buffer, editor->x + 4 + (int)cursor_column * advance,
			  editor->y + 4 +
				  (int)(cursor_row - editor->first_row) *
					  line_height,
			  1, glyph_height(), 0, 0);
	if (active)
		editor_draw_softkeys(editor, buffer);
	app.clip_x = old_clip[0];
	app.clip_y = old_clip[1];
	app.clip_width = old_clip[2];
	app.clip_height = old_clip[3];
	app.font_size = old_font[0];
	app.font_bold = old_font[1];
	app.font_italic = old_font[2];
	app.font_underline = old_font[3];
	/* Original PAINT/History/Input callbacks do not call graphic_flush_layer.
	 * The runtime may compose this editor-owned dirty layer after guest return;
	 * show itself keeps the SDK's drawing-only contract. */
	inline_editors.dirty_layers |= UINT32_C(1) << editor->layer;
}

static uint32_t editor_take_dirty_layers(void)
{
	if (app.phase)
		fail("inline editor dirty layers consumed during guest invocation");
	/* A timer may flush before requested PAINT draws newer editor pixels.
	 * Publish that explicit snapshot first; retain the editor update for the
	 * next frame rather than overriding or discarding either snapshot. */
	if (app.frame_pending)
		return 0;
	uint32_t dirty = inline_editors.dirty_layers;
	inline_editors.dirty_layers = 0;
	return dirty;
}

static int editor_insert(struct inline_editor *editor, size_t length,
			 uint16_t character)
{
	if (length + 1 >= editor->buffer_bytes / 2)
		return 0;
	memmove(editor->text + editor->cursor + 1,
		editor->text + editor->cursor,
		(length - editor->cursor + 1) * 2);
	editor->text[editor->cursor++] = character;
	return 1;
}
static uint16_t editor_input_case(const struct inline_editor *editor,
				  size_t position, uint16_t character)
{
	int first_upper = editor->input_mode == 7 &&
			  (!position || editor->text[position - 1] == '\n' ||
			   editor->text[position - 1] == '\r');
	if ((editor->uppercase || first_upper) && character >= 'a' &&
	    character <= 'z')
		character -= 'a' - 'A';
	return character;
}

static int editor_updates_pending(void)
{
	for (size_t i = 0; i < INLINE_EDITOR_CAP; ++i)
		if (inline_editors.slots[i].update_pending)
			return 1;
	return 0;
}

static void editor_dispatch_updates(void)
{
	if (app.phase)
		fail("inline editor text update attempted guest reentry");
	int32_t due[INLINE_EDITOR_CAP];
	for (size_t i = 0; i < INLINE_EDITOR_CAP; ++i)
		due[i] = inline_editors.slots[i].update_pending ?
				 inline_editors.slots[i].handle :
				 0;
	for (size_t i = 0; i < INLINE_EDITOR_CAP && !app.exited; ++i) {
		struct inline_editor *editor = editor_find(due[i]);
		size_t length;
		if (!editor || !editor->update_pending)
			continue;
		/* Retire before entry: the callback can unregister, replace the text
		 * allocation, close this handle or activate another editor. */
		editor->update_pending = 0;
		editor_update_fn callback = editor->update;
		if (!callback || !guest_code((uintptr_t)callback) ||
		    !editor_live_text(editor, &length))
			continue;
		uint16_t *text = editor->text, *cursor = text + editor->cursor;
		debug_trace(
			"EDITOR update handle=%d words=%zu cursor=%zu original=%p\n",
			editor->handle, length, editor->cursor,
			(void *)callback);
		app.phase = 2;
		callback_call((uintptr_t)callback, (uint32_t)(uintptr_t)text,
			      (uint32_t)(uintptr_t)cursor, (uint32_t)length, 0);
		app.phase = 0;
	}
}

/* Host dispatcher hook: one DOWN event is one physical press. UP never
 * triggers a softkey callback, so Send is not duplicated by the key release. */
static int editor_key(int32_t event, int32_t key)
{
	if (app.exited || app.phase)
		return 0;
	/* A burst of deferred keypad events must observe each preceding edit
	 * before the next edit or a Save softkey can consume callback state. */
	editor_dispatch_updates();
	if (app.exited)
		return 0;
	struct inline_editor *editor =
		editor_find(inline_editors.active_handle);
	size_t length;
	if (!editor_live_text(editor, &length))
		return 0;
	if (event != 2)
		return 1;
	int softkey = key == -6 ? 0 : key == -7 ? 1 : key == -5 ? 2 : -1;
	if (softkey >= 0 && !editor->disable_softkey) {
		editor_commit_tap(editor);
		editor_softkey_fn callback = editor->softkeys[softkey].callback;
		if (callback && guest_code((uintptr_t)callback)) {
			debug_trace(
				"EDITOR softkey handle=%d key=%d original=%p\n",
				editor->handle, softkey, (void *)callback);
			app.phase = 2;
			callback_call((uintptr_t)callback, 0, 0, 0, 0);
			app.phase = 0;
		}
		app.paint_pending = !app.exited;
		return 1;
	}
	int changed = 0;
	if (key >= -4 && key <= -1) {
		editor_commit_tap(editor);
		if (key == -3)
			editor->cursor =
				editor_previous(editor->text, editor->cursor);
		else if (key == -4)
			editor->cursor = editor_next(editor->text, length,
						     editor->cursor);
		else
			editor_move_vertical(editor, length,
					     key == -1 ? -1 : 1);
	} else if (key == '*') {
		editor_commit_tap(editor);
		size_t previous = editor_previous(editor->text, editor->cursor);
		memmove(editor->text + previous, editor->text + editor->cursor,
			(length - editor->cursor + 1) * 2);
		changed = previous != editor->cursor;
		editor->cursor = previous;
	} else if (key == '#') {
		editor_commit_tap(editor);
		editor->uppercase = !editor->uppercase;
	} else if (key >= '0' && key <= '9') {
		static const char *const choices[10] = {
			" 0",	".,?!1\n", "abc2",  "def3", "ghi4",
			"jkl5", "mno6",	   "pqrs7", "tuv8", "wxyz9"
		};
		const char *letters = choices[key - '0'];
		uint64_t now = monotonic_ms();
		int numeric = editor->input_mode == 1 ||
			      editor->input_mode == 2 ||
			      (editor->input_type & 0x1f);
		if (!numeric && editor->tap_key == key &&
		    now < editor->tap_until &&
		    editor->cursor == editor->tap_cursor + 1) {
			editor->tap_index =
				(editor->tap_index + 1) % strlen(letters);
			uint16_t character = editor_input_case(
				editor, editor->tap_cursor,
				(unsigned char)letters[editor->tap_index]);
			editor->text[editor->tap_cursor] = character;
			editor->tap_until = now + INLINE_EDITOR_TAP_MS;
			changed = 1;
		} else {
			editor_commit_tap(editor);
			uint16_t character =
				numeric ? (uint16_t)key :
					  (uint16_t)(unsigned char)letters[0];
			character = editor_input_case(editor, editor->cursor,
						      character);
			size_t before = editor->cursor;
			changed = editor_insert(editor, length, character);
			if (changed && !numeric) {
				editor->tap_key = key;
				editor->tap_index = 0;
				editor->tap_cursor = before;
				editor->tap_until = now + INLINE_EDITOR_TAP_MS;
			}
		}
	} else
		return 0;
	if (changed && editor->update)
		editor->update_pending = 1;
	app.paint_pending = 1;
	return 1;
}

/* Raw TCP is unavailable. A positive
 * pending handle lets the original yield; one deferred SDK failure then ends
 * its TLS retry loop. No socket, TLS success or fabricated reply is produced. */
static struct {
	int32_t next_handle, handle, port;
	char host[256];
	event_fn callback;
	int failure_pending;
} editor_tcp;

int32_t MRE_ABI host_editor_tcp_connect(const char *host, int32_t port,
					int32_t apn, event_fn callback)
{
	check_guest_api_stack();
	if (app.exited || editor_tcp.handle ||
	    editor_tcp.next_handle == INT32_MAX || port <= 0 || port > 65535 ||
	    apn < 1 || apn > 6 || !guest_code((uintptr_t)callback))
		return -2;
	char copied[sizeof(editor_tcp.host)];
	size_t length;
	for (length = 0; length < sizeof(copied); ++length) {
		if (!guest_data(host + length, 1, PF_R))
			return -2;
		unsigned char byte = (unsigned char)host[length];
		copied[length] = (char)byte;
		if (!byte)
			break;
		if (byte < 33 || byte > 126)
			return -2;
	}
	if (!length || length == sizeof(copied))
		return -2;
	memcpy(editor_tcp.host, copied, length + 1);
	editor_tcp.port = port;
	editor_tcp.handle = ++editor_tcp.next_handle;
	editor_tcp.callback = callback;
	editor_tcp.failure_pending = 1;
	fprintf(stderr,
		"TCP backend unavailable: pending handle=%d; deferred SDK PIPE_BROKEN (4)\n",
		editor_tcp.handle);
	return editor_tcp.handle;
}
void MRE_ABI host_editor_tcp_close(int32_t handle)
{
	check_guest_api_stack();
	if (handle != editor_tcp.handle)
		return;
	editor_tcp.handle = 0;
	editor_tcp.callback = NULL;
	editor_tcp.failure_pending = 0;
	memset(editor_tcp.host, 0, sizeof(editor_tcp.host));
	editor_tcp.port = 0;
}
int32_t MRE_ABI host_editor_tcp_read(int32_t handle, void *buffer,
				     int32_t bytes)
{
	check_guest_api_stack();
	if (handle <= 0 || handle != editor_tcp.handle || bytes < 0 ||
	    bytes > (int32_t)FILE_CAP ||
	    (bytes && !guest_data(buffer, (size_t)bytes, PF_W)))
		return -2;
	return -1;
}
int32_t MRE_ABI host_editor_tcp_write(int32_t handle, const void *buffer,
				      int32_t bytes)
{
	check_guest_api_stack();
	if (handle <= 0 || handle != editor_tcp.handle || bytes < 0 ||
	    bytes > (int32_t)FILE_CAP ||
	    (bytes && !guest_data(buffer, (size_t)bytes, PF_R)))
		return -2;
	return -1;
}

static void editor_dispatch_due(void)
{
	if (app.phase)
		fail("inline editor dispatcher attempted guest reentry");
	if (app.exited)
		return;
	editor_dispatch_updates();
	/* Retire each event before entry; callbacks can close or switch editors. */
	size_t due = inline_editors.ime_count;
	for (size_t i = 0; i < due && inline_editors.ime_count && !app.exited;
	     ++i) {
		int32_t handle = inline_editors.ime[0].handle;
		uint32_t message = inline_editors.ime[0].message;
		memmove(inline_editors.ime, inline_editors.ime + 1,
			--inline_editors.ime_count *
				sizeof(inline_editors.ime[0]));
		struct inline_editor *editor = editor_find(handle);
		if (!editor || !editor->ime ||
		    !guest_code((uintptr_t)editor->ime))
			continue;
		/* Scratch lives above the callback stack, inside the managed mapping. */
		struct editor_message *guest_message =
			(void *)(app.stack_top - 64);
		*guest_message = (struct editor_message){ message, editor->x,
							  editor->y,
							  editor->width,
							  editor->height };
		app.phase = 2;
		callback_call_at((uintptr_t)editor->ime, app.stack_top - 128,
				 (uint32_t)handle,
				 (uint32_t)(uintptr_t)guest_message, 0, 0);
		app.phase = 0;
	}
	if (editor_tcp.failure_pending && !app.exited) {
		int32_t handle = editor_tcp.handle;
		event_fn callback = editor_tcp.callback;
		/* The original's early TLS error path does not call vm_tcp_close.
		 * Retire before delivery so its next Send can acquire a new handle. */
		editor_tcp.handle = 0;
		editor_tcp.callback = NULL;
		editor_tcp.failure_pending = 0;
		memset(editor_tcp.host, 0, sizeof(editor_tcp.host));
		editor_tcp.port = 0;
		if (handle && callback && guest_code((uintptr_t)callback)) {
			debug_trace(
				"DISPATCH TCP handle=%d event=4 backend unavailable\n",
				handle);
			app.phase = 2;
			callback_call((uintptr_t)callback, (uint32_t)handle, 4,
				      0, 0);
			app.phase = 0;
		}
	}
}
static int editor_timeout_ms(uint64_t now)
{
	(void)now;
	return !app.exited && (inline_editors.ime_count ||
			       editor_tcp.failure_pending ||
			       inline_editors.dirty_layers ||
			       editor_updates_pending()) ?
		       0 :
		       -1;
}
static void editor_cleanup(void)
{
	memset(&inline_editors, 0, sizeof(inline_editors));
	memset(&editor_tcp, 0, sizeof(editor_tcp));
}

static uintptr_t editor_resolve(const char *name)
{
#define EDITOR_EXPORT(symbol, function) \
	if (!strcmp(name, symbol))      \
		return (uintptr_t)function;
	EDITOR_EXPORT("vm_editor_create", host_editor_create)
	EDITOR_EXPORT("vm_editor_close", host_editor_close)
	EDITOR_EXPORT("vm_editor_set_IME", host_editor_set_ime)
	EDITOR_EXPORT("vm_editor_set_softkey", host_editor_set_softkey)
	EDITOR_EXPORT("vm_editor_get_softkey_height",
		      host_editor_softkey_height)
	EDITOR_EXPORT("vm_editor_show", host_editor_show)
	EDITOR_EXPORT("vm_editor_set_multiline_text_font", host_editor_font)
	EDITOR_EXPORT("vm_editor_set_bg_border_style", host_editor_border)
	EDITOR_EXPORT("vm_editor_set_pos", host_editor_pos)
	EDITOR_EXPORT("vm_editor_set_size", host_editor_size)
	EDITOR_EXPORT("vm_editor_set_text", host_editor_set_text)
	EDITOR_EXPORT("vm_editor_get_text", host_editor_get_text)
	EDITOR_EXPORT("vm_editor_set_update_text_callback",
		      host_editor_set_update_text)
	EDITOR_EXPORT("vm_editor_get_cursor_index", host_editor_cursor)
	EDITOR_EXPORT("vm_editor_save", host_editor_save)
	EDITOR_EXPORT("vm_editor_redraw_ime_screen", host_editor_redraw_ime)
	EDITOR_EXPORT("vm_editor_activate", host_editor_activate)
	EDITOR_EXPORT("vm_editor_deactivate", host_editor_deactivate)
	EDITOR_EXPORT("vm_tcp_connect", host_editor_tcp_connect)
	EDITOR_EXPORT("vm_tcp_close", host_editor_tcp_close)
	EDITOR_EXPORT("vm_tcp_read", host_editor_tcp_read)
	EDITOR_EXPORT("vm_tcp_write", host_editor_tcp_write)
#undef EDITOR_EXPORT
	return 0;
}
