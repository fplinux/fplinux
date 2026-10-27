/* SPDX-License-Identifier: 0BSD */
/* fplinux-check: package-embedded
 * PNG/JPEG headers and libraries are supplied by this aport. */
#include "image-decode.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>

#if defined(__arm__) && defined(WITH_SIMD) && WITH_SIMD
#error "MRE requires an ARM libjpeg-turbo backend built with WITH_SIMD=OFF"
#endif

struct decode_budget {
	size_t used;
	size_t limit;
};

struct png_operation {
	png_structp decoder;
	png_infop info;
	jmp_buf jump;
	struct decode_budget budget;
	const uint8_t *input;
	size_t input_size;
	size_t input_offset;
	uint8_t *rgba;
	png_bytep *rows;
	int succeeded;
	char message[128];
};

union png_allocation {
	max_align_t alignment;
	struct {
		size_t bytes;
	} allocation;
};

struct jpeg_allocation {
	struct jpeg_allocation *next;
};

struct jpeg_operation {
	struct jpeg_decompress_struct decoder;
	struct jpeg_error_mgr error;
	jmp_buf jump;
	struct decode_budget budget;
	struct jpeg_allocation *allocations;
	uint8_t *row;
	int succeeded;
	char message[JMSG_LENGTH_MAX];
};

/* The supported libjpeg-turbo create path allocates one memory manager and
 * small permanent objects before allocation callbacks can be replaced.
 * Its 1600-byte first-pool reserve and initialization objects fit below this
 * conservative allowance; all subsequent allocations use our callbacks.
 */
#define JPEG_INITIAL_BYTES (64U * 1024U)
#define JPEG_ALIGNMENT 32U

static void set_error(char *error, size_t size, const char *message)
{
	if (error && size)
		snprintf(error, size, "%s", message);
}

static int reserve_bytes(struct decode_budget *budget, size_t bytes)
{
	if (bytes > budget->limit - budget->used)
		return -1;
	budget->used += bytes;
	return 0;
}

static int checked_product(size_t left, size_t right, size_t *product)
{
	if (right && left > SIZE_MAX / right)
		return -1;
	*product = left * right;
	return 0;
}

static int checked_sum(size_t left, size_t right, size_t *sum)
{
	if (left > SIZE_MAX - right)
		return -1;
	*sum = left + right;
	return 0;
}

static uint16_t rgb565(const uint8_t *rgb)
{
	return ((uint16_t)(rgb[0] >> 3) << 11) |
	       ((uint16_t)(rgb[1] >> 2) << 5) | (rgb[2] >> 3);
}

void mre_image_free(struct mre_image *image)
{
	if (!image)
		return;
	free(image->pixels);
	free(image->alpha);
	memset(image, 0, sizeof(*image));
}

static int prepare_output(const void *data, size_t size,
			  const struct mre_image_limits *limits,
			  struct mre_image *image, char *error,
			  size_t error_size)
{
	set_error(error, error_size, "");
	if (!image) {
		set_error(error, error_size, "Missing image output");
		return -1;
	}
	memset(image, 0, sizeof(*image));
	if (!limits || !limits->max_input_bytes || !limits->max_width ||
	    !limits->max_height || !limits->max_pixels ||
	    !limits->max_working_bytes) {
		set_error(error, error_size, "Missing image limits");
		return -1;
	}
	if (!data || !size || size > limits->max_input_bytes) {
		set_error(error, error_size, "Image input exceeds byte limit");
		return -1;
	}
	return 0;
}

static int image_buffers(struct mre_image *image, unsigned int width,
			 unsigned int height,
			 const struct mre_image_limits *limits,
			 struct decode_budget *budget, char *error,
			 size_t error_size)
{
	size_t pixels, pixel_bytes;

	if (!width || !height || width > limits->max_width ||
	    height > limits->max_height || width > INT_MAX ||
	    height > INT_MAX || checked_product(width, height, &pixels) ||
	    pixels > limits->max_pixels ||
	    checked_product(pixels, sizeof(*image->pixels), &pixel_bytes)) {
		set_error(error, error_size, "Image dimensions exceed limits");
		return -1;
	}
	if (reserve_bytes(budget, pixel_bytes) ||
	    reserve_bytes(budget, pixels)) {
		set_error(error, error_size,
			  "Image buffers exceed working limit");
		return -1;
	}
	image->pixels = malloc(pixel_bytes);
	image->alpha = malloc(pixels);
	if (!image->pixels || !image->alpha) {
		set_error(error, error_size, "Cannot allocate image buffers");
		return -1;
	}
	image->width = (int)width;
	image->height = (int)height;
	image->transparent_color = 0xffff;
	return 0;
}

static int set_transparent_color(struct mre_image *image,
				 struct decode_budget *budget, char *error,
				 size_t error_size)
{
	size_t count = (size_t)image->width * image->height;
	uint8_t *used_colors;
	unsigned int candidate;
	int has_zero_alpha = 0;

	for (size_t i = 0; i < count; ++i) {
		if (image->alpha[i] < 255)
			image->has_transparency = 1;
		if (image->alpha[i] && image->alpha[i] < 255)
			image->has_partial_alpha = 1;
		if (!image->alpha[i])
			has_zero_alpha = 1;
	}
	if (!has_zero_alpha)
		return 0;
	if (reserve_bytes(budget, 65536 / 8)) {
		set_error(error, error_size,
			  "PNG color key exceeds working limit");
		return -1;
	}
	used_colors = calloc(65536 / 8, 1);
	if (!used_colors) {
		set_error(error, error_size,
			  "Cannot allocate PNG color key map");
		return -1;
	}
	for (size_t i = 0; i < count; ++i) {
		uint16_t color = image->pixels[i];

		if (image->alpha[i])
			used_colors[color / 8] |= (uint8_t)(1U << (color % 8));
	}
	for (candidate = 0; candidate < 65536; ++candidate) {
		uint16_t color = (uint16_t)(0xf81fU + candidate);

		if (!(used_colors[color / 8] & (1U << (color % 8)))) {
			image->transparent_color = color;
			break;
		}
	}
	free(used_colors);
	budget->used -= 65536 / 8;
	if (candidate == 65536) {
		set_error(error, error_size,
			  "PNG has no unused RGB565 color key");
		return -1;
	}
	for (size_t i = 0; i < count; ++i)
		if (!image->alpha[i])
			image->pixels[i] = image->transparent_color;
	return 0;
}

static void png_error_callback(png_structp decoder, png_const_charp message)
{
	struct png_operation *operation = png_get_error_ptr(decoder);

	snprintf(operation->message, sizeof(operation->message), "%s", message);
	longjmp(operation->jump, 1);
}

static void png_read_callback(png_structp decoder, png_bytep output,
			      png_size_t bytes)
{
	struct png_operation *operation = png_get_io_ptr(decoder);

	if (bytes > operation->input_size - operation->input_offset)
		png_error(decoder, "Truncated PNG input");
	memcpy(output, operation->input + operation->input_offset, bytes);
	operation->input_offset += bytes;
}

static png_voidp png_allocate(png_structp decoder, png_alloc_size_t bytes)
{
	struct png_operation *operation = png_get_mem_ptr(decoder);
	union png_allocation *allocation;
	size_t total;

	if (bytes > SIZE_MAX - sizeof(*allocation))
		return NULL;
	total = bytes + sizeof(*allocation);
	if (reserve_bytes(&operation->budget, total))
		return NULL;
	allocation = malloc(total);
	if (!allocation) {
		operation->budget.used -= total;
		return NULL;
	}
	allocation->allocation.bytes = total;
	return allocation + 1;
}

static void png_release(png_structp decoder, png_voidp pointer)
{
	struct png_operation *operation = png_get_mem_ptr(decoder);
	union png_allocation *allocation = pointer;

	if (!allocation)
		return;
	--allocation;
	operation->budget.used -= allocation->allocation.bytes;
	free(allocation);
}

int mre_png_decode(const void *data, size_t size,
		   const struct mre_image_limits *limits,
		   struct mre_image *image, char *error, size_t error_size)
{
	struct png_operation *operation;
	size_t rgba_bytes, row_pointer_bytes;
	png_uint_32 width, height;
	int color_type, bit_depth;
	int result;

	if (prepare_output(data, size, limits, image, error, error_size))
		return -1;
	if (size < 33 || png_sig_cmp(data, 0, 8)) {
		set_error(error, error_size, "Invalid PNG signature or header");
		return -1;
	}
	if (limits->max_working_bytes < sizeof(*operation)) {
		set_error(error, error_size,
			  "PNG operation exceeds working limit");
		return -1;
	}
	operation = calloc(1, sizeof(*operation));
	if (!operation) {
		set_error(error, error_size, "Cannot allocate PNG operation");
		return -1;
	}
	operation->budget = (struct decode_budget){ sizeof(*operation),
						    limits->max_working_bytes };
	operation->input = data;
	operation->input_size = size;
	/* Cleanup owners live on the heap: longjmp makes modified automatic
	 * variables indeterminate, including pointers assigned after setjmp.
	 */
	if (setjmp(operation->jump)) {
		set_error(error, error_size, operation->message);
		goto done;
	}
	operation->decoder = png_create_read_struct_2(
		PNG_LIBPNG_VER_STRING, operation, png_error_callback,
		png_error_callback, operation, png_allocate, png_release);
	if (!operation->decoder) {
		set_error(error, error_size,
			  "Cannot allocate PNG decoder within limit");
		goto done;
	}
	operation->info = png_create_info_struct(operation->decoder);
	if (!operation->info)
		png_error(operation->decoder,
			  "Cannot allocate PNG information");
	png_set_read_fn(operation->decoder, operation, png_read_callback);
	png_set_user_limits(operation->decoder, limits->max_width,
			    limits->max_height);
	png_set_chunk_malloc_max(operation->decoder, limits->max_working_bytes);
	png_set_keep_unknown_chunks(operation->decoder, PNG_HANDLE_CHUNK_NEVER,
				    NULL, 0);
	png_set_crc_action(operation->decoder, PNG_CRC_ERROR_QUIT,
			   PNG_CRC_ERROR_QUIT);
	png_read_info(operation->decoder, operation->info);
	width = png_get_image_width(operation->decoder, operation->info);
	height = png_get_image_height(operation->decoder, operation->info);
	if (image_buffers(image, width, height, limits, &operation->budget,
			  error, error_size))
		goto done;
	color_type = png_get_color_type(operation->decoder, operation->info);
	bit_depth = png_get_bit_depth(operation->decoder, operation->info);
	if (bit_depth == 16)
		png_set_strip_16(operation->decoder);
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(operation->decoder);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(operation->decoder);
	if (png_get_valid(operation->decoder, operation->info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(operation->decoder);
	else if (!(color_type & PNG_COLOR_MASK_ALPHA))
		png_set_add_alpha(operation->decoder, 255, PNG_FILLER_AFTER);
	if (color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(operation->decoder);
	png_set_interlace_handling(operation->decoder);
	png_read_update_info(operation->decoder, operation->info);
	if (png_get_channels(operation->decoder, operation->info) != 4 ||
	    png_get_bit_depth(operation->decoder, operation->info) != 8 ||
	    checked_product(width, 4, &rgba_bytes) ||
	    png_get_rowbytes(operation->decoder, operation->info) !=
		    rgba_bytes ||
	    checked_product(rgba_bytes, height, &rgba_bytes) ||
	    checked_product(height, sizeof(*operation->rows),
			    &row_pointer_bytes))
		png_error(operation->decoder,
			  "Invalid PNG transformed row size");
	if (reserve_bytes(&operation->budget, rgba_bytes) ||
	    reserve_bytes(&operation->budget, row_pointer_bytes))
		png_error(operation->decoder, "PNG rows exceed working limit");
	operation->rgba = malloc(rgba_bytes);
	operation->rows = malloc(row_pointer_bytes);
	if (!operation->rgba || !operation->rows)
		png_error(operation->decoder, "Cannot allocate PNG rows");
	for (size_t row = 0; row < height; ++row)
		operation->rows[row] =
			operation->rgba + row * (size_t)width * 4;
	png_read_image(operation->decoder, operation->rows);
	png_read_end(operation->decoder, operation->info);
	for (size_t i = 0; i < (size_t)width * height; ++i) {
		image->pixels[i] = rgb565(operation->rgba + i * 4);
		image->alpha[i] = operation->rgba[i * 4 + 3];
	}
	if (set_transparent_color(image, &operation->budget, error, error_size))
		goto done;
	operation->succeeded = 1;
done:
	if (operation->decoder)
		png_destroy_read_struct(&operation->decoder, &operation->info,
					NULL);
	result = operation->succeeded ? 0 : -1;
	free(operation->rgba);
	free(operation->rows);
	free(operation);
	if (result)
		mre_image_free(image);
	return result;
}

static void jpeg_error_callback(j_common_ptr decoder)
{
	struct jpeg_operation *operation = decoder->client_data;

	decoder->err->format_message(decoder, operation->message);
	longjmp(operation->jump, 1);
}

static void jpeg_message_callback(j_common_ptr decoder, int level)
{
	/* libjpeg's truncated-input recovery inserts EOI and produces pixels.
	 * An incomplete service image must fail rather than report success.
	 */
	if (level < 0)
		jpeg_error_callback(decoder);
}

static _Noreturn void jpeg_reject(struct jpeg_operation *operation,
				  const char *message)
{
	snprintf(operation->message, sizeof(operation->message), "%s", message);
	longjmp(operation->jump, 1);
}

static void *jpeg_allocate(j_common_ptr decoder, int pool, size_t bytes)
{
	struct jpeg_operation *operation = decoder->client_data;
	struct jpeg_allocation *allocation;
	size_t overhead = sizeof(*allocation) + JPEG_ALIGNMENT - 1;
	size_t total;
	uintptr_t address;

	(void)pool;
	if (checked_sum(bytes, JPEG_ALIGNMENT - 1, &bytes))
		jpeg_reject(operation, "JPEG aligned allocation size overflow");
	bytes &= ~(size_t)(JPEG_ALIGNMENT - 1);
	if (bytes > SIZE_MAX - overhead)
		jpeg_reject(operation, "JPEG allocation size overflow");
	total = bytes + overhead;
	if (reserve_bytes(&operation->budget, total))
		jpeg_reject(operation, "JPEG allocations exceed working limit");
	allocation = malloc(total);
	if (!allocation)
		jpeg_reject(operation, "Cannot allocate JPEG decoder buffer");
	allocation->next = operation->allocations;
	operation->allocations = allocation;
	address = (uintptr_t)(allocation + 1);
	address = (address + JPEG_ALIGNMENT - 1) &
		  ~(uintptr_t)(JPEG_ALIGNMENT - 1);
	return (void *)address;
}

static JSAMPARRAY jpeg_sample_rows(j_common_ptr decoder, int pool,
				   JDIMENSION width, JDIMENSION height)
{
	struct jpeg_operation *operation = decoder->client_data;
	size_t stride, data_bytes, pointer_bytes;
	JSAMPARRAY rows;
	JSAMPROW data;

	/* libjpeg-turbo SIMD may read padding at either end of a row. Its
	 * standard memory manager aligns sample strides to twice 32 bytes.
	 */
	if (checked_sum(width, 2 * JPEG_ALIGNMENT - 1, &stride))
		jpeg_reject(operation, "JPEG row stride overflow");
	stride &= ~(size_t)(2 * JPEG_ALIGNMENT - 1);
	if (!stride || !height ||
	    checked_product(stride, height, &data_bytes) ||
	    checked_product(height, sizeof(*rows), &pointer_bytes))
		jpeg_reject(operation, "JPEG row allocation overflow");
	rows = jpeg_allocate(decoder, pool, pointer_bytes);
	data = jpeg_allocate(decoder, pool, data_bytes);
	for (size_t row = 0; row < height; ++row)
		rows[row] = data + row * stride;
	return rows;
}

static JBLOCKARRAY jpeg_block_rows(j_common_ptr decoder, int pool,
				   JDIMENSION width, JDIMENSION height)
{
	struct jpeg_operation *operation = decoder->client_data;
	size_t row_bytes, data_bytes, pointer_bytes;
	JBLOCKARRAY rows;
	JBLOCKROW data;

	if (!width || !height ||
	    checked_product(width, sizeof(*data), &row_bytes) ||
	    checked_product(row_bytes, height, &data_bytes) ||
	    checked_product(height, sizeof(*rows), &pointer_bytes))
		jpeg_reject(operation, "JPEG block allocation overflow");
	rows = jpeg_allocate(decoder, pool, pointer_bytes);
	data = jpeg_allocate(decoder, pool, data_bytes);
	for (size_t row = 0; row < height; ++row)
		rows[row] = data + row * width;
	return rows;
}

static jvirt_sarray_ptr jpeg_virtual_samples(j_common_ptr decoder, int pool,
					     boolean zero, JDIMENSION width,
					     JDIMENSION height,
					     JDIMENSION access)
{
	(void)pool;
	(void)zero;
	(void)width;
	(void)height;
	(void)access;
	jpeg_reject(decoder->client_data,
		    "JPEG virtual sample arrays unsupported");
	return NULL;
}

static jvirt_barray_ptr jpeg_virtual_blocks(j_common_ptr decoder, int pool,
					    boolean zero, JDIMENSION width,
					    JDIMENSION height,
					    JDIMENSION access)
{
	(void)pool;
	(void)zero;
	(void)width;
	(void)height;
	(void)access;
	jpeg_reject(decoder->client_data,
		    "JPEG virtual block arrays unsupported");
	return NULL;
}

int mre_jpeg_decode(const void *data, size_t size,
		    const struct mre_image_limits *limits,
		    struct mre_image *image, char *error, size_t error_size)
{
	const uint8_t *input = data;
	struct jpeg_operation *operation;
	size_t row_bytes;
	int result;

	if (prepare_output(data, size, limits, image, error, error_size))
		return -1;
	if (size < 4 || size > ULONG_MAX || input[0] != 0xff ||
	    input[1] != 0xd8 || input[2] != 0xff) {
		set_error(error, error_size, "Invalid JPEG signature");
		return -1;
	}
	if (limits->max_working_bytes <
	    sizeof(*operation) + JPEG_INITIAL_BYTES) {
		set_error(error, error_size,
			  "JPEG operation exceeds working limit");
		return -1;
	}
	operation = calloc(1, sizeof(*operation));
	if (!operation) {
		set_error(error, error_size, "Cannot allocate JPEG operation");
		return -1;
	}
	operation->budget =
		(struct decode_budget){ sizeof(*operation) + JPEG_INITIAL_BYTES,
					limits->max_working_bytes };
	operation->decoder.err = jpeg_std_error(&operation->error);
	operation->decoder.client_data = operation;
	operation->error.error_exit = jpeg_error_callback;
	operation->error.emit_message = jpeg_message_callback;
	if (setjmp(operation->jump)) {
		set_error(error, error_size, operation->message);
		goto done;
	}
	jpeg_create_decompress(&operation->decoder);
	operation->decoder.mem->alloc_small = jpeg_allocate;
	operation->decoder.mem->alloc_large = jpeg_allocate;
	operation->decoder.mem->alloc_sarray = jpeg_sample_rows;
	operation->decoder.mem->alloc_barray = jpeg_block_rows;
	operation->decoder.mem->request_virt_sarray = jpeg_virtual_samples;
	operation->decoder.mem->request_virt_barray = jpeg_virtual_blocks;
	jpeg_mem_src(&operation->decoder, input, (unsigned long)size);
	if (jpeg_read_header(&operation->decoder, TRUE) != JPEG_HEADER_OK)
		jpeg_reject(operation, "Missing JPEG image header");
	if (operation->decoder.data_precision != 8 ||
	    operation->decoder.progressive_mode ||
	    operation->decoder.arith_code || operation->decoder.Ss != 0 ||
	    operation->decoder.Se != 63 || operation->decoder.Ah != 0 ||
	    operation->decoder.Al != 0 ||
	    operation->decoder.comps_in_scan !=
		    operation->decoder.num_components ||
	    (operation->decoder.jpeg_color_space != JCS_GRAYSCALE &&
	     operation->decoder.jpeg_color_space != JCS_RGB &&
	     operation->decoder.jpeg_color_space != JCS_YCbCr))
		jpeg_reject(operation,
			    "Unsupported JPEG coding or color space");
	if (image_buffers(image, operation->decoder.image_width,
			  operation->decoder.image_height, limits,
			  &operation->budget, error, error_size))
		goto done;
	operation->decoder.out_color_space = JCS_RGB;
	operation->decoder.dct_method = JDCT_ISLOW;
	if (!jpeg_start_decompress(&operation->decoder) ||
	    operation->decoder.output_width != (unsigned int)image->width ||
	    operation->decoder.output_height != (unsigned int)image->height ||
	    operation->decoder.output_components != 3 ||
	    checked_product(image->width, 3, &row_bytes))
		jpeg_reject(operation, "Invalid JPEG output dimensions");
	operation->row = jpeg_allocate((j_common_ptr)&operation->decoder,
				       JPOOL_IMAGE, row_bytes);
	while (operation->decoder.output_scanline <
	       operation->decoder.output_height) {
		size_t row = operation->decoder.output_scanline;
		JSAMPROW rows[] = { operation->row };

		if (jpeg_read_scanlines(&operation->decoder, rows, 1) != 1)
			jpeg_reject(operation, "Incomplete JPEG scanline");
		for (size_t column = 0; column < (size_t)image->width; ++column)
			image->pixels[row * image->width + column] =
				rgb565(operation->row + column * 3);
	}
	if (!jpeg_finish_decompress(&operation->decoder))
		jpeg_reject(operation, "Incomplete JPEG image");
	memset(image->alpha, 255, (size_t)image->width * image->height);
	operation->succeeded = 1;
done:
	/* mem is initialized before jpeg_create_decompress can fail. */
	if (operation->decoder.mem)
		jpeg_destroy_decompress(&operation->decoder);
	while (operation->allocations) {
		struct jpeg_allocation *allocation = operation->allocations;

		operation->allocations = allocation->next;
		free(allocation);
	}
	result = operation->succeeded ? 0 : -1;
	free(operation);
	if (result)
		mre_image_free(image);
	return result;
}
