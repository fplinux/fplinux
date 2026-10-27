/* SPDX-License-Identifier: 0BSD */
/* RGB565 layer geometry, canvas attachments and handheld SDK APIs.
 * Include after native-graphics.inc. Pixel buffers use their own dimensions;
 * the LCD compositor translates screen coordinates at flush time. */
struct handheld_external_layer {
	uint8_t *source;
	int32_t x, y, width, height;
	int owns_canvas;
};
static struct handheld_external_layer handheld_layers[LAYER_CAP];
static uint8_t handheld_keypad_mode;

static void handheld_forget_layer(int32_t handle)
{
	if (handle < 0 || handle >= (int32_t)LAYER_CAP)
		return;
	struct handheld_external_layer old = handheld_layers[handle];
	memset(&handheld_layers[handle], 0, sizeof(handheld_layers[handle]));
	if (old.owns_canvas && old.source)
		(void)free_canvas_pointer(old.source - 32);
}
static struct sdk_canvas *handheld_canvas_for_buffer(uint8_t *buffer)
{
	for (struct sdk_canvas *canvas = app.canvases; canvas;
	     canvas = canvas->next)
		/* Layer composition supports opaque RGB565 plus a chroma key;
		 * alpha-backed images remain usable through the blit APIs. */
		if (buffer == canvas->buffer + 32 && !canvas->alpha &&
		    canvas->buffer[11] == 0 && canvas->property.width &&
		    canvas->property.height &&
		    canvas->bytes ==
			    32 + (size_t)canvas->property.width *
					    (size_t)canvas->property.height * 2)
			return canvas;
	return NULL;
}
static void handheld_configure_owned_layer(int32_t handle, int32_t x, int32_t y,
					   int32_t width, int32_t height)
{
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live || x < 0 || y < 0 || width <= 0 ||
	    height <= 0 || (int64_t)x + width > guest_screen_width() ||
	    (int64_t)y + height > guest_screen_height())
		fail("layer geometry outside the guest screen");
	handheld_layers[handle] = (struct handheld_external_layer){
		.x = x, .y = y, .width = width, .height = height
	};
	uint8_t *header = app.layers[handle].buffer;
	store16(header + 17, (uint16_t)width);
	store16(header + 19, (uint16_t)height);
	store32(header + 28, (uint32_t)((size_t)width * (size_t)height * 2));
}
static uint8_t *handheld_layer_buffer(int32_t handle)
{
	if (handle < 0 || handle >= (int32_t)LAYER_CAP)
		return NULL;
	if (!app.layers[handle].live) {
		handheld_forget_layer(handle);
		return NULL;
	}
	uint8_t *source = handheld_layers[handle].source;
	if (source) {
		const struct sdk_canvas *canvas =
			handheld_canvas_for_buffer(source);
		if (!canvas ||
		    canvas->property.width != handheld_layers[handle].width ||
		    canvas->property.height != handheld_layers[handle].height)
			fail("external layer references a released or incompatible canvas");
	}
	return source;
}
static int handheld_layer_surface(int32_t handle, struct pixel_surface *surface)
{
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live)
		return 0;
	uint8_t *source = handheld_layer_buffer(handle);
	const struct handheld_external_layer *geometry =
		&handheld_layers[handle];
	*surface = (struct pixel_surface){
		.pixels = source ? (uint16_t *)source :
				   app.layers[handle].pixels,
		.width = geometry->width ? geometry->width :
					   guest_screen_width(),
		.height = geometry->height ? geometry->height :
					     guest_screen_height(),
		.transparent = app.layers[handle].transparent,
		.alpha = source ? handheld_canvas_for_buffer(source)->alpha :
				  NULL
	};
	return 1;
}
static int handheld_pixel_visible(int32_t handle, int32_t x, int32_t y)
{
	if (handle < 0 || handle >= (int32_t)LAYER_CAP)
		return 0;
	const struct handheld_external_layer *external =
		&handheld_layers[handle];
	int32_t width = external->width ? external->width :
					  guest_screen_width();
	int32_t height = external->height ? external->height :
					    guest_screen_height();
	return x >= external->x && y >= external->y &&
	       (int64_t)x - external->x < width &&
	       (int64_t)y - external->y < height;
}
static int handheld_screen_pixel(int32_t handle, int32_t x, int32_t y,
				 uint16_t *pixel)
{
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live || !handheld_pixel_visible(handle, x, y))
		return 0;
	const struct handheld_external_layer *geometry =
		&handheld_layers[handle];
	size_t width = geometry->width ? (size_t)geometry->width :
					 (size_t)guest_screen_width();
	*pixel = app.layers[handle].pixels[(size_t)(y - geometry->y) * width +
					   (size_t)(x - geometry->x)];
	return 1;
}
static void handheld_sync_layers(void)
{
	for (int32_t handle = 0; handle < (int32_t)LAYER_CAP; ++handle) {
		uint8_t *source = handheld_layer_buffer(handle);
		if (!source)
			continue;
		const struct handheld_external_layer *external =
			&handheld_layers[handle];
		size_t bytes =
			(size_t)external->width * (size_t)external->height * 2;
		if (!guest_data(source, bytes, PF_R))
			fail("external layer canvas is outside owned guest memory");
		memcpy(app.layers[handle].pixels, source, bytes);
	}
}
int32_t MRE_ABI host_handheld_create_canvas_cf(uint8_t format, int32_t width,
					       int32_t height)
{
	check_guest_api_stack();
	if (format != 0)
		return -2; /* VM_GDI_ERR_WRONG_PARAM. */
	return host_create_canvas(width, height);
}
int32_t MRE_ABI host_handheld_canvas_buffer_size(int32_t handle)
{
	check_guest_api_stack();
	const struct sdk_canvas *canvas = find_canvas(handle);
	if (!canvas || canvas->bytes < 32 || canvas->bytes - 32 > INT32_MAX)
		return -2;
	return (int32_t)(canvas->bytes - 32);
}
static int32_t handheld_attach_canvas(int32_t x, int32_t y, int32_t width,
				      int32_t height, int32_t transparent,
				      uint8_t *buffer, int32_t bytes,
				      int owns_canvas)
{
	struct sdk_canvas *canvas = handheld_canvas_for_buffer(buffer);
	if (width <= 0 || height <= 0 || x < 0 || y < 0 ||
	    (int64_t)x + width > guest_screen_width() ||
	    (int64_t)y + height > guest_screen_height() || bytes < 0 ||
	    ((uintptr_t)buffer & 1) || !canvas ||
	    width != canvas->property.width ||
	    height != canvas->property.height ||
	    (size_t)bytes != canvas->bytes - 32 ||
	    !guest_data(buffer, (size_t)bytes, PF_R | PF_W))
		return -2;
	for (int32_t handle = 0; handle < (int32_t)LAYER_CAP; ++handle) {
		if (!app.layers[handle].live)
			handheld_forget_layer(handle);
		else if (handheld_layers[handle].source == buffer)
			return -2; /* SDK forbids attaching the same canvas twice. */
	}
	int32_t handle = host_create_layer(x, y, width, height, transparent);
	if (handle < 0)
		return handle;
	handheld_configure_owned_layer(handle, x, y, width, height);
	handheld_layers[handle].source = buffer;
	handheld_layers[handle].owns_canvas = owns_canvas;
	fprintf(stderr,
		"GRAPHICS external RGB565 %dx%d at %d,%d layer=%d canvas_pixels=%p bytes=%d\n",
		width, height, x, y, handle, buffer, bytes);
	return handle;
}
int32_t MRE_ABI host_handheld_create_layer_ex(int32_t x, int32_t y,
					      int32_t width, int32_t height,
					      int32_t transparent, int32_t mode,
					      uint8_t *buffer)
{
	check_guest_api_stack();
	if (transparent < -1 || transparent > UINT16_MAX || x < 0 || y < 0 ||
	    width <= 0 || height <= 0 ||
	    (int64_t)x + width > guest_screen_width() ||
	    (int64_t)y + height > guest_screen_height())
		return -2;
	if (mode ==
	    1) { /* VM_BASE_LAYER replaces the base layer's attributes. */
		if (x || y || width != guest_screen_width() ||
		    height != guest_screen_height())
			return -2;
		if (app.layers[0].live)
			(void)host_delete_layer(0);
		return host_create_layer(x, y, width, height, transparent);
	}
	if (mode == 3) { /* VM_BUF binds an existing SDK canvas. */
		struct sdk_canvas *canvas = handheld_canvas_for_buffer(buffer);
		if (!canvas || canvas->bytes - 32 > INT32_MAX)
			return -2;
		return handheld_attach_canvas(x, y, width, height, transparent,
					      buffer,
					      (int32_t)(canvas->bytes - 32), 0);
	}
	if (mode != 4)
		return -2; /* VM_FAST_LAYER and other modes are not implemented. */
	int32_t canvas_handle = host_create_canvas(width, height);
	if (canvas_handle < 0)
		return canvas_handle;
	struct sdk_canvas *canvas = find_canvas(canvas_handle);
	int32_t handle = handheld_attach_canvas(
		x, y, width, height, transparent, canvas->buffer + 32,
		(int32_t)(canvas->bytes - 32), 1);
	if (handle < 0)
		(void)free_canvas_pointer(canvas->buffer);
	return handle;
}
int32_t MRE_ABI host_handheld_create_layer_cf(
	uint8_t format, int32_t x, int32_t y, int32_t width, int32_t height,
	const void *transparent, int32_t mode, uint8_t *buffer, int32_t bytes)
{
	check_guest_api_stack();
	/* The API passes nine AAPCS words: format/x/y/width
	 * in registers, then height/trans_color/mode/buffer/bytes on its stack. */
	if (format != 0)
		return -2;
	int32_t chroma = -1;
	if (transparent && (uintptr_t)transparent != (uintptr_t)(intptr_t)-1) {
		if (!guest_data(transparent, 4, PF_R))
			return -2;
		const uint8_t *argb = transparent;
		chroma = (argb[1] >> 3) << 11 | (argb[2] >> 2) << 5 |
			 (argb[3] >> 3);
	}
	if (mode == 3)
		return handheld_attach_canvas(x, y, width, height, chroma,
					      buffer, bytes, 0);
	return host_handheld_create_layer_ex(x, y, width, height, chroma, mode,
					     buffer);
}
int32_t MRE_ABI host_handheld_kbd_mode(uint8_t mode)
{
	check_guest_api_stack();
	if (mode > 1)
		return -2; /* VM_KBD_MODE_ERR_PARAM; numeric modes only. */
	handheld_keypad_mode = mode;
	fprintf(stderr, "HANDHELD keypad numeric concurrent mode=%u\n", mode);
	return 0;
}
static int handheld_two_key_mode(void)
{
	return handheld_keypad_mode == 1;
}
int32_t MRE_ABI host_handheld_utc(uint32_t *seconds)
{
	check_guest_api_stack();
	if (((uintptr_t)seconds & 3) ||
	    !guest_data(seconds, sizeof(*seconds), PF_W))
		fail("vm_get_curr_utc output bounds/alignment");
	struct timespec now;
	if (clock_gettime(CLOCK_REALTIME, &now) || now.tv_sec < 0 ||
	    (uint64_t)now.tv_sec > UINT32_MAX)
		return -1;
	*seconds = (uint32_t)now.tv_sec;
	debug_trace("TIME vm_get_curr_utc RTCseconds=%" PRIu32 "\n", *seconds);
	return 0;
}
static int handheld_ellipse_contains(int64_t x, int64_t y, int32_t width,
				     int32_t height)
{
	if (x < 0 || y < 0 || x >= width || y >= height)
		return 0;
	double dx = (2.0 * (double)x + 1.0 - width) / width;
	double dy = (2.0 * (double)y + 1.0 - height) / height;
	return dx * dx + dy * dy <= 1.0;
}
void MRE_ABI host_handheld_ellipse(uint8_t *buffer, int32_t x, int32_t y,
				   int32_t width, int32_t height,
				   uint16_t color)
{
	check_guest_api_stack();
	if (width <= 0 || height <= 0)
		return;
	struct pixel_surface surface = graphics_surface(buffer);
	for (int yy = 0; yy < surface.height; ++yy)
		for (int xx = 0; xx < surface.width; ++xx) {
			int64_t rx = (int64_t)xx - x, ry = (int64_t)yy - y;
			if (handheld_ellipse_contains(rx, ry, width, height) &&
			    (!handheld_ellipse_contains(rx - 1, ry, width,
							height) ||
			     !handheld_ellipse_contains(rx + 1, ry, width,
							height) ||
			     !handheld_ellipse_contains(rx, ry - 1, width,
							height) ||
			     !handheld_ellipse_contains(rx, ry + 1, width,
							height)))
				surface_pixel(&surface, xx, yy, color);
		}
}
static uintptr_t handheld_resolve(const char *name)
{
	if (!strcmp(name, "vm_graphic_create_canvas_cf"))
		return (uintptr_t)host_handheld_create_canvas_cf;
	if (!strcmp(name, "vm_graphic_create_layer_cf"))
		return (uintptr_t)host_handheld_create_layer_cf;
	if (!strcmp(name, "vm_graphic_create_layer_ex"))
		return (uintptr_t)host_handheld_create_layer_ex;
	if (!strcmp(name, "vm_graphic_get_canvas_buffer_size"))
		return (uintptr_t)host_handheld_canvas_buffer_size;
	if (!strcmp(name, "vm_kbd_set_mode"))
		return (uintptr_t)host_handheld_kbd_mode;
	if (!strcmp(name, "rand"))
		return (uintptr_t)host_rand;
	if (!strcmp(name, "vm_get_curr_utc"))
		return (uintptr_t)host_handheld_utc;
	if (!strcmp(name, "vm_graphic_ellipse"))
		return (uintptr_t)host_handheld_ellipse;
	if (!strcmp(name, "vm_res_init"))
		return (uintptr_t)host_res_init;
	return 0;
}
