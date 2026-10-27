/* SPDX-License-Identifier: 0BSD */
/* Included after guest memory helpers. Service methods use plain AAPCS;
 * SDK-visible object/vtable bytes live in the guest pool. Lifecycle state and
 * copied filenames stay in the host, independently of executable identity. */
static int utility_vfs_path(const uint16_t *, char out[MRE_VFS_PATH_BYTES]);
static int utility_snapshot_exchange(const char *virtual_path);
struct utility_viewer;
struct utility_viewer_vtable {
	int32_t(MRE_ABI *add_ref)(struct utility_viewer *);
	int32_t(MRE_ABI *release)(struct utility_viewer *);
	int32_t(MRE_ABI *query_interface)(struct utility_viewer *, int32_t,
					  void **);
	void(MRE_ABI *close)(struct utility_viewer *);
	void(MRE_ABI *create)(struct utility_viewer *);
	void(MRE_ABI *run)(struct utility_viewer *);
	void(MRE_ABI *set_title)(struct utility_viewer *, uint16_t *);
	void(MRE_ABI *set_direction)(struct utility_viewer *, int32_t);
	void(MRE_ABI *set_filename)(struct utility_viewer *, char *);
};
struct utility_viewer {
	struct utility_viewer_vtable *vtable;
};
_Static_assert(sizeof(struct utility_viewer) == 4 &&
		       sizeof(struct utility_viewer_vtable) == 36 &&
		       offsetof(struct utility_viewer_vtable, release) == 4 &&
		       offsetof(struct utility_viewer_vtable, create) == 16 &&
		       offsetof(struct utility_viewer_vtable, run) == 20 &&
		       offsetof(struct utility_viewer_vtable, set_filename) ==
			       32,
	       "SDK ARM32 image viewer ABI");
static struct {
	struct utility_viewer *object;
	struct utility_viewer_vtable *vtable;
	char filename[MRE_VFS_PATH_BYTES];
	uint16_t title[256];
	int32_t references, direction;
	int created, filename_set, ran;
} utility_viewer_state;

static void utility_viewer_check(struct utility_viewer *self)
{
	check_guest_api_stack();
	if (!self || self != utility_viewer_state.object ||
	    !mre_memory_contains(&app.memory, self, sizeof(*self)) ||
	    self->vtable != utility_viewer_state.vtable ||
	    !mre_memory_contains(&app.memory, self->vtable,
				 sizeof(*self->vtable)))
		fail("image viewer object is not live");
}
int32_t MRE_ABI utility_viewer_add_ref(struct utility_viewer *self)
{
	utility_viewer_check(self);
	if (utility_viewer_state.references == INT32_MAX)
		return -1;
	return ++utility_viewer_state.references;
}
int32_t MRE_ABI utility_viewer_release(struct utility_viewer *self)
{
	utility_viewer_check(self);
	if (--utility_viewer_state.references)
		return utility_viewer_state.references;
	struct utility_viewer_vtable *vtable = utility_viewer_state.vtable;
	int accepted = utility_viewer_state.ran;
	/* The accepted frame belongs to shared memory, not the service object. */
	memset(&utility_viewer_state, 0, sizeof(utility_viewer_state));
	host_free(vtable);
	host_free(self);
	fprintf(stderr, "VIEWER release original object; frame_accepted=%d\n",
		accepted);
	return 0;
}
int32_t MRE_ABI utility_viewer_query_interface(struct utility_viewer *self,
					       int32_t iid, void **out)
{
	utility_viewer_check(self);
	if (((uintptr_t)out & 3) || !guest_data(out, sizeof(*out), PF_W))
		return -1;
	*out = NULL;
	if ((iid != 1 && iid != 0x1009) ||
	    utility_viewer_state.references == INT32_MAX)
		return -1;
	++utility_viewer_state.references;
	*out = self;
	return 0;
}
void MRE_ABI utility_viewer_close(struct utility_viewer *self)
{
	utility_viewer_check(self);
	utility_viewer_state.created = 0;
	utility_viewer_state.ran = 0;
}
void MRE_ABI utility_viewer_create(struct utility_viewer *self)
{
	utility_viewer_check(self);
	utility_viewer_state.created = 1;
	utility_viewer_state.ran = 0;
	debug_trace("VIEWER create service\n");
}
void MRE_ABI utility_viewer_set_title(struct utility_viewer *self,
				      uint16_t *title)
{
	utility_viewer_check(self);
	if (!title) {
		utility_viewer_state.title[0] = 0;
		return;
	}
	size_t units = wide_length(title);
	if (units >= sizeof(utility_viewer_state.title) /
			     sizeof(*utility_viewer_state.title))
		return;
	memcpy(utility_viewer_state.title, title, (units + 1) * sizeof(*title));
}
void MRE_ABI utility_viewer_set_direction(struct utility_viewer *self,
					  int32_t direction)
{
	utility_viewer_check(self);
	if (direction == 0 || direction == 1)
		utility_viewer_state.direction = direction;
}
void MRE_ABI utility_viewer_set_filename(struct utility_viewer *self,
					 char *filename)
{
	utility_viewer_check(self);
	utility_viewer_state.filename_set =
		filename &&
		utility_vfs_path((const uint16_t *)filename,
				 utility_viewer_state.filename) &&
		utility_viewer_state.filename[0];
	utility_viewer_state.ran = 0;
}
void MRE_ABI utility_viewer_run(struct utility_viewer *self)
{
	utility_viewer_check(self);
	if (!utility_viewer_state.created ||
	    !utility_viewer_state.filename_set || app.exited || !app.shared)
		return;
	if (utility_snapshot_exchange(utility_viewer_state.filename) ||
	    !app.shared->file_bytes ||
	    app.shared->file_bytes > MRE_FILE_BYTES) {
		debug_trace("VIEWER image snapshot unavailable\n");
		return;
	}
	struct mre_image_limits limits = {
		.max_input_bytes = MRE_FILE_BYTES,
		.max_width = 800,
		.max_height = 800,
		.max_pixels = 640000,
		/* External decoder storage is bounded separately from guest heap.
		 * Object/vtable allocations still count against the guest RAM tag. */
		.max_working_bytes = 4u * 1024u * 1024u
	};
	struct mre_image image = { 0 };
	char error[192] = { 0 };
	if (mre_jpeg_decode(app.shared->file_data, app.shared->file_bytes,
			    &limits, &image, error, sizeof(error))) {
		fprintf(stderr, "VIEWER JPEG backend: %s\n",
			error[0] ? error : "decoding failed");
		return;
	}
	int width = MRE_FRAME_WIDTH;
	int height = image.height * width / image.width;
	if (height > MRE_FRAME_HEIGHT) {
		height = MRE_FRAME_HEIGHT;
		width = image.width * height / image.height;
	}
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;
	int left = (MRE_FRAME_WIDTH - width) / 2;
	int top = (MRE_FRAME_HEIGHT - height) / 2;
	write_protocol("VIEW_PICTURE\n", sizeof("VIEW_PICTURE\n") - 1);
	memset(app.shared->pixels, 0, sizeof(app.shared->pixels));
	for (int y = 0; y < height; ++y) {
		int source_y = y * image.height / height;
		for (int x = 0; x < width; ++x) {
			int source_x = x * image.width / width;
			app.shared->pixels[(top + y) * MRE_FRAME_WIDTH + left +
					   x] =
				image.pixels[source_y * image.width + source_x];
		}
	}
	fprintf(stderr,
		"VIEWER decoded JPEG %dx%d -> fit %dx%d at %d,%d RGB565\n",
		image.width, image.height, width, height, left, top);
	mre_image_free(&image);
	utility_viewer_state.ran = 1;
	app.frame_pending = 1;
}
int32_t MRE_ABI utility_create_service(int32_t clsid, int32_t iid, void **out)
{
	check_guest_api_stack();
	if (((uintptr_t)out & 3) || !guest_data(out, sizeof(*out), PF_W))
		fail("vm_create_service output bounds/alignment");
	*out = NULL;
	if (app.exited || clsid != 0x1008 || (iid != 1 && iid != 0x1009) ||
	    utility_viewer_state.object)
		return -1;
	struct utility_viewer *object = host_calloc(sizeof(*object));
	if (!object)
		return -1;
	struct utility_viewer_vtable *vtable = host_calloc(sizeof(*vtable));
	if (!vtable) {
		host_free(object);
		return -1;
	}
	*vtable = (struct utility_viewer_vtable){
		utility_viewer_add_ref,		utility_viewer_release,
		utility_viewer_query_interface, utility_viewer_close,
		utility_viewer_create,		utility_viewer_run,
		utility_viewer_set_title,	utility_viewer_set_direction,
		utility_viewer_set_filename
	};
#define VIEWER_ADAPT(member)                                               \
	do {                                                               \
		uintptr_t address =                                        \
			adapt_api_address((uintptr_t)vtable->member);      \
		memcpy(&vtable->member, &address, sizeof(vtable->member)); \
	} while (0)
	VIEWER_ADAPT(add_ref);
	VIEWER_ADAPT(release);
	VIEWER_ADAPT(query_interface);
	VIEWER_ADAPT(close);
	VIEWER_ADAPT(create);
	VIEWER_ADAPT(run);
	VIEWER_ADAPT(set_title);
	VIEWER_ADAPT(set_direction);
	VIEWER_ADAPT(set_filename);
#undef VIEWER_ADAPT
	object->vtable = vtable;
	utility_viewer_state.object = object;
	utility_viewer_state.vtable = vtable;
	utility_viewer_state.references = 1;
	*out = object;
	fprintf(stderr,
		"VIEWER service clsid=0x1008 iid=0x1009 object=%p vtable=%p\n",
		(void *)object, (void *)vtable);
	return 0;
}

enum {
	UTILITY_CHSET_ASCII = 1,
	UTILITY_CHSET_UTF16LE = 35,
	UTILITY_CHSET_UTF16BE = 36,
	UTILITY_CHSET_UTF8 = 37,
	UTILITY_CHSET_UCS2 = 38
};
static int utility_charset_supported(int32_t charset)
{
	return charset == UTILITY_CHSET_ASCII ||
	       (charset >= UTILITY_CHSET_UTF16LE &&
		charset <= UTILITY_CHSET_UCS2);
}
static int utility_charset_wide(int32_t charset)
{
	return charset == UTILITY_CHSET_UTF16LE ||
	       charset == UTILITY_CHSET_UTF16BE ||
	       charset == UTILITY_CHSET_UCS2;
}
static int utility_charset_byte(const unsigned char *text, size_t offset,
				unsigned char *byte)
{
	if (offset >= FILE_CAP || (uintptr_t)text > UINTPTR_MAX - offset ||
	    !guest_data(text + offset, 1, PF_R))
		return 0;
	*byte = text[offset];
	return 1;
}
static int utility_charset_word(const unsigned char *text, size_t offset,
				int32_t charset, uint16_t *word)
{
	unsigned char first, second;
	if (!utility_charset_byte(text, offset, &first) ||
	    !utility_charset_byte(text, offset + 1, &second))
		return 0;
	*word = charset == UTILITY_CHSET_UTF16BE ?
			(uint16_t)((uint16_t)first << 8 | second) :
			(uint16_t)((uint16_t)second << 8 | first);
	return 1;
}
/* Decode a complete scalar before advancing. Memory faults are parameter
 * errors; malformed encoding is the SDK conversion failure. */
static int utility_charset_next(const unsigned char *text, int32_t charset,
				size_t *cursor, uint32_t *scalar)
{
	if (utility_charset_wide(charset)) {
		uint16_t first;
		if (!utility_charset_word(text, *cursor, charset, &first))
			return -1;
		*cursor += 2;
		*scalar = first;
		if (first >= 0xd800 && first <= 0xdfff) {
			uint16_t second;
			if (charset == UTILITY_CHSET_UCS2 || first > 0xdbff)
				return -4;
			if (!utility_charset_word(text, *cursor, charset,
						  &second))
				return -1;
			if (second < 0xdc00 || second > 0xdfff)
				return -4;
			*cursor += 2;
			*scalar = 0x10000u +
				  ((uint32_t)(first - 0xd800) << 10) +
				  (uint32_t)(second - 0xdc00);
		}
		return 0;
	}
	unsigned char first;
	if (!utility_charset_byte(text, *cursor, &first))
		return -1;
	unsigned count = 1;
	uint32_t minimum = 0;
	*scalar = first;
	if (first >= 0x80) {
		if (charset == UTILITY_CHSET_ASCII)
			return -4;
		if (first >= 0xc2 && first <= 0xdf) {
			count = 2;
			minimum = 0x80;
			*scalar = first & 0x1f;
		} else if (first >= 0xe0 && first <= 0xef) {
			count = 3;
			minimum = 0x800;
			*scalar = first & 0x0f;
		} else if (first >= 0xf0 && first <= 0xf4) {
			count = 4;
			minimum = 0x10000;
			*scalar = first & 7;
		} else
			return -4;
	}
	for (unsigned i = 1; i < count; ++i) {
		unsigned char next;
		if (!utility_charset_byte(text, *cursor + i, &next))
			return -1;
		if ((next & 0xc0) != 0x80)
			return -4;
		*scalar = (*scalar << 6) | (next & 0x3f);
	}
	if (*scalar < minimum || *scalar > 0x10ffff ||
	    (*scalar >= 0xd800 && *scalar <= 0xdfff))
		return -4;
	*cursor += count;
	return 0;
}
static void utility_charset_store_word(unsigned char *out, uint16_t word,
				       int32_t charset)
{
	if (charset == UTILITY_CHSET_UTF16BE) {
		out[0] = (unsigned char)(word >> 8);
		out[1] = (unsigned char)word;
	} else {
		out[0] = (unsigned char)word;
		out[1] = (unsigned char)(word >> 8);
	}
}
static int32_t utility_charset(int32_t source, int32_t destination,
			       const char *text, uint16_t *out, int32_t bytes)
{
	if (!utility_charset_supported(source))
		return -2;
	if (!utility_charset_supported(destination))
		return -3;
	size_t terminator = utility_charset_wide(destination) ? 2 : 1;
	if (!text || !out || bytes < (int32_t)terminator ||
	    bytes > (int32_t)FILE_CAP)
		return -1;
	unsigned char *encoded = malloc((size_t)bytes);
	if (!encoded)
		return -4;
	size_t cursor = 0, used = 0;
	int32_t result = 0;
	for (;;) {
		uint32_t scalar;
		result = utility_charset_next((const unsigned char *)text,
					      source, &cursor, &scalar);
		if (result || !scalar)
			break;
		unsigned char next[4];
		size_t count;
		if (utility_charset_wide(destination)) {
			if (scalar <= 0xffff) {
				utility_charset_store_word(
					next, (uint16_t)scalar, destination);
				count = 2;
			} else {
				if (destination == UTILITY_CHSET_UCS2) {
					result = -4;
					break;
				}
				scalar -= 0x10000;
				utility_charset_store_word(
					next,
					(uint16_t)(0xd800 + (scalar >> 10)),
					destination);
				utility_charset_store_word(
					next + 2,
					(uint16_t)(0xdc00 + (scalar & 0x3ff)),
					destination);
				count = 4;
			}
		} else if (scalar < 0x80) {
			next[0] = (unsigned char)scalar;
			count = 1;
		} else if (destination == UTILITY_CHSET_ASCII) {
			result = -4;
			break;
		} else {
			count = scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
			uint32_t remaining = scalar;
			for (size_t i = count - 1; i; --i) {
				next[i] = (unsigned char)(0x80 |
							  (remaining & 0x3f));
				remaining >>= 6;
			}
			next[0] = (unsigned char)((count == 2 ? 0xc0 :
						   count == 3 ? 0xe0 :
								0xf0) |
						  remaining);
		}
		if (count > (size_t)bytes - terminator - used) {
			result = -1;
			break;
		}
		memcpy(encoded + used, next, count);
		used += count;
	}
	if (!result) {
		memset(encoded + used, 0, terminator);
		if (!guest_data(out, used + terminator, PF_W))
			result = -1;
		else
			memmove(out, encoded, used + terminator);
	}
	free(encoded);
	return result;
}
