/* SPDX-License-Identifier: 0BSD */
/* Included after owned_surface(): both layers and owned canvases carry their
 * actual dimensions here, independently of the LCD's dimensions. */
struct sdk_graphic_point {
	int16_t x, y;
};
_Static_assert(sizeof(struct sdk_graphic_point) == 4,
	       "SDK vm_graphic_point ABI");

static struct pixel_surface graphics_surface(uint8_t *buffer)
{
	struct pixel_surface surface;
	if (!owned_surface(buffer, &surface) || surface.width <= 0 ||
	    surface.height <= 0 ||
	    (size_t)surface.width > SIZE_MAX / 2 / (size_t)surface.height)
		fail("graphics buffer is not an owned live surface");
	return surface;
}
static int graphics_layer(int32_t handle, struct pixel_surface *surface)
{
	if (handle < 0 || handle >= (int32_t)LAYER_CAP ||
	    !app.layers[handle].live)
		return 0;
	*surface = graphics_surface(app.layers[handle].buffer);
	return 1;
}
static void surface_pixel(struct pixel_surface *surface, int64_t x, int64_t y,
			  uint16_t color)
{
	if (x >= 0 && x < surface->width && y >= 0 && y < surface->height &&
	    x >= app.clip_x && x - app.clip_x < app.clip_width &&
	    y >= app.clip_y && y - app.clip_y < app.clip_height) {
		size_t pixel = (size_t)y * (size_t)surface->width + (size_t)x;
		surface->pixels[pixel] = color;
		if (surface->alpha)
			surface->alpha[pixel] = 255;
	}
}
uint16_t MRE_ABI host_get_pixel(uint8_t *buffer, int32_t x, int32_t y)
{
	check_guest_api_stack();
	struct pixel_surface surface = graphics_surface(buffer);
	if (x < 0 || x >= surface.width || y < 0 || y >= surface.height)
		return 0;
	return surface.pixels[(size_t)y * (size_t)surface.width + (size_t)x];
}
void MRE_ABI host_set_pixel(uint8_t *buffer, int32_t x, int32_t y,
			    uint16_t color)
{
	check_guest_api_stack();
	struct pixel_surface surface = graphics_surface(buffer);
	surface_pixel(&surface, x, y, color);
}
void MRE_ABI host_set_pixel_ex(int32_t handle, int32_t x, int32_t y)
{
	check_guest_api_stack();
	struct pixel_surface surface;
	if (graphics_layer(handle, &surface))
		surface_pixel(&surface, x, y, (uint16_t)app.draw_color[0]);
}
static void surface_line(struct pixel_surface *surface, int32_t x0, int32_t y0,
			 int32_t x1, int32_t y1, uint16_t color)
{
	int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
	uint64_t span_x = (uint64_t)(dx < 0 ? -dx : dx);
	uint64_t span_y = (uint64_t)(dy < 0 ? -dy : dy);
	int horizontal = span_x >= span_y;
	int64_t first = horizontal ? x0 : y0;
	int64_t last = horizontal ? x1 : y1;
	int64_t minor_first = horizontal ? y0 : x0;
	int64_t minor_last = horizontal ? y1 : x1;
	if (first > last) {
		int64_t swap = first;
		first = last;
		last = swap;
		swap = minor_first;
		minor_first = minor_last;
		minor_last = swap;
	}
	uint64_t major_span = (uint64_t)(last - first);
	int64_t minor_delta = minor_last - minor_first;
	uint64_t minor_span =
		(uint64_t)(minor_delta < 0 ? -minor_delta : minor_delta);
	int limit = horizontal ? surface->width : surface->height;
	int64_t start = first > 0 ? first : 0;
	int64_t stop = last < limit - 1 ? last : limit - 1;
	/* Iterate only the visible major axis. The unsigned product fits even
	 * for opposite int32 endpoints, and preserves the unclipped raster phase. */
	for (int64_t major = start; major <= stop; ++major) {
		uint64_t offset = (uint64_t)(major - first);
		uint64_t step = major_span ?
					(offset * minor_span + major_span / 2) /
						major_span :
					0;
		int64_t minor =
			minor_first +
			(minor_delta < 0 ? -(int64_t)step : (int64_t)step);
		surface_pixel(surface, horizontal ? major : minor,
			      horizontal ? minor : major, color);
	}
}
void MRE_ABI host_line(uint8_t *buffer, int32_t x0, int32_t y0, int32_t x1,
		       int32_t y1, uint16_t color)
{
	check_guest_api_stack();
	struct pixel_surface surface = graphics_surface(buffer);
	surface_line(&surface, x0, y0, x1, y1, color);
}
void MRE_ABI host_line_ex(int32_t handle, int32_t x0, int32_t y0, int32_t x1,
			  int32_t y1)
{
	check_guest_api_stack();
	struct pixel_surface surface;
	if (graphics_layer(handle, &surface))
		surface_line(&surface, x0, y0, x1, y1,
			     (uint16_t)app.draw_color[0]);
}
static int polygon_points(const struct sdk_graphic_point *points, int32_t count)
{
	if (count < 2)
		return 0;
	if ((size_t)count > SIZE_MAX / sizeof(*points) ||
	    ((uintptr_t)points & 1) ||
	    !guest_data(points, (size_t)count * sizeof(*points), PF_R))
		fail("polygon points bounds or alignment");
	return 1;
}
static void surface_polygon(struct pixel_surface *surface,
			    const struct sdk_graphic_point *points,
			    int32_t count, uint16_t color)
{
	for (int32_t i = 0; i < count; ++i) {
		int32_t next = i + 1 < count ? i + 1 : 0;
		surface_line(surface, points[i].x, points[i].y, points[next].x,
			     points[next].y, color);
	}
}
void MRE_ABI host_polygon(int32_t handle,
			  const struct sdk_graphic_point *points, int32_t count)
{
	check_guest_api_stack();
	struct pixel_surface surface;
	if (graphics_layer(handle, &surface) && polygon_points(points, count))
		surface_polygon(&surface, points, count,
				(uint16_t)app.draw_color[0]);
}
static int compare_intersections(const void *left, const void *right)
{
	int64_t a = *(const int64_t *)left, b = *(const int64_t *)right;
	return (a > b) - (a < b);
}
void MRE_ABI host_fill_polygon(int32_t handle,
			       const struct sdk_graphic_point *points,
			       int32_t count)
{
	check_guest_api_stack();
	struct pixel_surface surface;
	if (count < 3 || !graphics_layer(handle, &surface) ||
	    !polygon_points(points, count))
		return;
	if ((size_t)count > SIZE_MAX / sizeof(int64_t))
		return;
	int64_t *intersections = malloc((size_t)count * sizeof(*intersections));
	if (!intersections)
		return;
	uint16_t color = (uint16_t)app.draw_color[0];
	for (int y = 0; y < surface.height; ++y) {
		if (y < app.clip_y ||
		    (int64_t)y - app.clip_y >= app.clip_height)
			continue;
		size_t used = 0;
		for (int32_t i = 0; i < count; ++i) {
			int32_t next = i + 1 < count ? i + 1 : 0;
			int x0 = points[i].x, y0 = points[i].y;
			int x1 = points[next].x, y1 = points[next].y;
			if (!((y0 <= y && y < y1) || (y1 <= y && y < y0)))
				continue;
			/* SDK points are int16: the 16.16 numerator fits int64. */
			intersections[used++] = (int64_t)x0 * 65536 +
						(int64_t)(y - y0) * (x1 - x0) *
							65536 / (y1 - y0);
		}
		qsort(intersections, used, sizeof(*intersections),
		      compare_intersections);
		for (size_t i = 0; i + 1 < used; i += 2) {
			int64_t left = intersections[i],
				right = intersections[i + 1];
			left = left >= 0 ? (left + 65535) / 65536 :
					   left / 65536;
			right = right >= 0 ? right / 65536 :
					     (right - 65535) / 65536;
			if (left < 0)
				left = 0;
			if (right >= surface.width)
				right = surface.width - 1;
			for (int64_t x = left; x <= right; ++x)
				surface_pixel(&surface, x, y, color);
		}
	}
	free(intersections);
	surface_polygon(&surface, points, count, color);
}
static int rounded_contains(int64_t x, int64_t y, int32_t width, int32_t height,
			    int32_t radius)
{
	if (x < 0 || x >= width || y < 0 || y >= height)
		return 0;
	int64_t dx = x < radius		 ? radius - x :
		     x >= width - radius ? x - (width - radius - 1) :
					   0;
	int64_t dy = y < radius		  ? radius - y :
		     y >= height - radius ? y - (height - radius - 1) :
					    0;
	return dx * dx + dy * dy <= (int64_t)radius * radius;
}
static void surface_roundrect(struct pixel_surface *surface, int32_t x,
			      int32_t y, int32_t width, int32_t height,
			      int32_t radius, uint16_t color, int fill)
{
	if (width <= 0 || height <= 0 || radius < 0 ||
	    (radius && (int64_t)radius * 2 >= width))
		return;
	if (radius > height / 2)
		radius = height / 2;
	int64_t right = (int64_t)x + width, bottom = (int64_t)y + height;
	int left = x > 0 ? x : 0, top = y > 0 ? y : 0;
	int stop_x = right < surface->width ? (int)right : surface->width;
	int stop_y = bottom < surface->height ? (int)bottom : surface->height;
	for (int yy = top; yy < stop_y; ++yy)
		for (int xx = left; xx < stop_x; ++xx) {
			int64_t rx = (int64_t)xx - x, ry = (int64_t)yy - y;
			if (rounded_contains(rx, ry, width, height, radius) &&
			    (fill ||
			     !rounded_contains(rx - 1, ry, width, height,
					       radius) ||
			     !rounded_contains(rx + 1, ry, width, height,
					       radius) ||
			     !rounded_contains(rx, ry - 1, width, height,
					       radius) ||
			     !rounded_contains(rx, ry + 1, width, height,
					       radius)))
				surface_pixel(surface, xx, yy, color);
		}
}
void MRE_ABI host_roundrect(uint8_t *buffer, int32_t x, int32_t y,
			    int32_t width, int32_t height, int32_t radius,
			    uint16_t color)
{
	check_guest_api_stack();
	struct pixel_surface surface = graphics_surface(buffer);
	surface_roundrect(&surface, x, y, width, height, radius, color, 0);
}
void MRE_ABI host_fill_roundrect(uint8_t *buffer, int32_t x, int32_t y,
				 int32_t width, int32_t height, int32_t radius,
				 uint16_t color)
{
	check_guest_api_stack();
	struct pixel_surface surface = graphics_surface(buffer);
	surface_roundrect(&surface, x, y, width, height, radius, color, 1);
}
void MRE_ABI host_roundrect_ex(int32_t handle, int32_t x, int32_t y,
			       int32_t width, int32_t height, int32_t radius)
{
	check_guest_api_stack();
	struct pixel_surface surface;
	if (graphics_layer(handle, &surface))
		surface_roundrect(&surface, x, y, width, height, radius,
				  (uint16_t)app.draw_color[0], 0);
}
void MRE_ABI host_fill_roundrect_ex(int32_t handle, int32_t x, int32_t y,
				    int32_t width, int32_t height,
				    int32_t radius)
{
	check_guest_api_stack();
	struct pixel_surface surface;
	if (graphics_layer(handle, &surface))
		surface_roundrect(&surface, x, y, width, height, radius,
				  (uint16_t)app.draw_color[0], 1);
}
int32_t MRE_ABI host_create_canvas(int32_t width, int32_t height)
{
	check_guest_api_stack();
	if (width <= 0 || height <= 0 || width > UINT16_MAX ||
	    height > UINT16_MAX ||
	    (size_t)width > (SIZE_MAX - 32) / 2 / (size_t)height)
		return -2; /* VM_GDI_ERR_WRONG_PARAM. */
	size_t pixel_bytes = (size_t)width * (size_t)height * 2;
	size_t bytes = 32 + pixel_bytes;
	if (app.memory.bytes > app.ram_limit ||
	    app.canvas_bytes > app.ram_limit - app.memory.bytes ||
	    bytes > app.ram_limit - app.memory.bytes - app.canvas_bytes)
		return -7; /* VM_GDI_ERR_MALLOC_FAILED. */
	struct sdk_canvas *canvas = calloc(1, sizeof(*canvas));
	if (!canvas)
		return -7;
	canvas->buffer = map_canvas(bytes, &canvas->mapping_bytes);
	if (!canvas->buffer) {
		free(canvas);
		return -7;
	}
	canvas->bytes = bytes;
	canvas->property =
		(struct sdk_frame_property){ .width = (uint16_t)width,
					     .height = (uint16_t)height,
					     .offset = (uint32_t)pixel_bytes };
	uint8_t *header = canvas->buffer;
	memset(header, 0, bytes);
	memcpy(header, "MTKCANVAS", 9);
	header[9] = 1;
	header[10] = 0xff;
	store16(header + 17, (uint16_t)width);
	store16(header + 19, (uint16_t)height);
	store32(header + 24, UINT32_MAX);
	store32(header + 28, (uint32_t)pixel_bytes);
	canvas->next = app.canvases;
	app.canvases = canvas;
	app.canvas_bytes += bytes;
	service_memory_refresh();
	for (size_t i = 0;
	     i < sizeof(app.retired_canvases) / sizeof(app.retired_canvases[0]);
	     ++i)
		if (app.retired_canvases[i] >= (uintptr_t)header &&
		    app.retired_canvases[i] - (uintptr_t)header < bytes)
			app.retired_canvases[i] = 0;
	return (int32_t)(uintptr_t)header;
}
int32_t MRE_ABI host_canvas_set_trans_color(int32_t handle, int32_t color)
{
	check_guest_api_stack();
	struct sdk_canvas *canvas = find_canvas(handle);
	if (!canvas || color < -1 || color > UINT16_MAX)
		return -2;
	canvas->property.flag = color >= 0;
	canvas->property.trans_color = color >= 0 ? (uint16_t)color : 0;
	canvas->buffer[12] = color >= 0;
	store32(canvas->buffer + 24, (uint32_t)color);
	return 0;
}
void MRE_ABI host_mirror(uint8_t *buffer, int32_t x, int32_t y, uint8_t *source,
			 int32_t frame, int32_t direction)
{
	check_guest_api_stack();
	if ((frame != 0 && frame != 1) || (direction != 0 && direction != 1))
		return;
	struct pixel_surface dest = graphics_surface(buffer);
	struct pixel_surface src = graphics_surface(source);
	size_t source_bytes = (size_t)src.width * (size_t)src.height * 2;
	size_t dest_bytes = (size_t)dest.width * (size_t)dest.height * 2;
	uintptr_t source_address = (uintptr_t)src.pixels;
	uintptr_t dest_address = (uintptr_t)dest.pixels;
	int overlap = source_address <= dest_address ?
			      dest_address - source_address < source_bytes :
			      source_address - dest_address < dest_bytes;
	uint16_t *snapshot = NULL;
	if (overlap) {
		size_t alpha_bytes = src.alpha ? source_bytes / 2 : 0;
		if (source_bytes > SIZE_MAX - alpha_bytes)
			return;
		snapshot = malloc(source_bytes + alpha_bytes);
		if (!snapshot)
			return;
		memcpy(snapshot, src.pixels, source_bytes);
		src.pixels = snapshot;
		if (src.alpha) {
			uint8_t *alpha_copy =
				(uint8_t *)snapshot + source_bytes;
			memcpy(alpha_copy, src.alpha, alpha_bytes);
			src.alpha = alpha_copy;
		}
	}
	for (int dy = 0; dy < dest.height; ++dy) {
		int64_t ry = (int64_t)dy - y;
		if (ry < 0 || ry >= src.height)
			continue;
		for (int dx = 0; dx < dest.width; ++dx) {
			int64_t rx = (int64_t)dx - x;
			if (rx < 0 || rx >= src.width)
				continue;
			size_t sx = (size_t)(direction == 0 ?
						     src.width - 1 - rx :
						     rx);
			size_t sy = (size_t)(direction == 1 ?
						     src.height - 1 - ry :
						     ry);
			size_t source_pixel = sy * (size_t)src.width + sx;
			uint16_t color = src.pixels[source_pixel];
			unsigned alpha = src.alpha ? src.alpha[source_pixel] :
						     255;
			if (alpha &&
			    (src.transparent < 0 ||
			     color != (uint16_t)src.transparent) &&
			    dx >= app.clip_x &&
			    (int64_t)dx - app.clip_x < app.clip_width &&
			    dy >= app.clip_y &&
			    (int64_t)dy - app.clip_y < app.clip_height) {
				size_t dest_pixel =
					(size_t)dy * (size_t)dest.width +
					(size_t)dx;
				surface_blend_pixel(&dest, dest_pixel, color,
						    alpha);
			}
		}
	}
	free(snapshot);
}
