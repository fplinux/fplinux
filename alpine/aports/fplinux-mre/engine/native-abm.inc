/* SPDX-License-Identifier: 0BSD */
/* Single-frame packed RGB565 ABM decoder. The format header and the caller's
 * memory budget determine supported images, independently of their source. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

struct sdk_abm_image {
	unsigned width, height;
	uint16_t *pixels;
	uint8_t *alpha;
	int has_transparency;
};

static uint32_t little(const uint8_t *p, unsigned count)
{
	uint32_t value = 0;
	for (unsigned i = 0; i < count; ++i)
		value |= (uint32_t)p[i] << (8 * i);
	return value;
}

struct bits {
	const uint8_t *data;
	size_t length, position;
};

static int read_bits(struct bits *stream, unsigned count, unsigned *value)
{
	if (stream->position > stream->length * 8 ||
	    count > stream->length * 8 - stream->position)
		return -1;
	*value = 0;
	for (unsigned i = 0; i < count; ++i) {
		size_t bit = stream->position++;
		*value |= ((stream->data[bit / 8] >> (bit % 8)) & 1u) << i;
	}
	return 0;
}

void sdk_abm_release(struct sdk_abm_image *image)
{
	free(image->pixels);
	free(image->alpha);
	*image = (struct sdk_abm_image){ 0 };
}

/* Returns 0 on success; output must be empty on entry. Alpha combines intrinsic
 * transparent index 0 with the caller's chroma key. Use -1 to disable chroma.
 */
int sdk_abm_decode(const uint8_t *data, size_t length, int chroma_key,
		   struct sdk_abm_image *output)
{
	if (!data || !output || length < 20 || length > SIZE_MAX / 8 ||
	    data[0] != 0x34 || data[1] != 1 ||
	    little(data + 2, 3) + 8 != length)
		return -1;
	uint32_t outer = little(data + 5, 3), dimensions = little(data + 8, 4);
	unsigned width = dimensions & 4095, height = (dimensions >> 12) & 4095;
	unsigned colors = little(data + 16, 4), index_bits = 0;
	if (!width || !height || width != outer >> 12 ||
	    height != (outer & 4095) || dimensions >> 24 ||
	    little(data + 12, 4) != 0x11000000 || !colors || colors > 65535 ||
	    20 + 2 * (size_t)colors > length)
		return -1;
	for (unsigned n = colors; n; n >>= 1)
		++index_bits;
	size_t offset = 20 + 2 * (size_t)colors, count = (size_t)width * height;
	struct sdk_abm_image image = { width, height, malloc(count * 2),
				       malloc(count), 0 };
	if (!image.pixels || !image.alpha)
		goto fail;
	struct bits stream = { data + offset, length - offset, 0 };
	for (size_t position = 0; position < count;) {
		unsigned control, index = 0;
		if (read_bits(&stream, 8, &control))
			goto fail;
		unsigned run = (control & 127) + 1;
		if (run > count - position)
			goto fail;
		if (!(control & 128) && read_bits(&stream, index_bits, &index))
			goto fail;
		for (unsigned i = 0; i < run; ++i) {
			if ((control & 128) &&
			    read_bits(&stream, index_bits, &index))
				goto fail;
			if (index > colors)
				goto fail;
			uint16_t color =
				index ? (uint16_t)little(
						data + 20 + 2 * (index - 1),
						2) :
					0;
			image.pixels[position] = color;
			image.has_transparency |= !index ||
						  (int)color == chroma_key;
			image.alpha[position++] =
				index && (int)color != chroma_key ? 255 : 0;
		}
	}
	if (stream.length * 8 - stream.position >= 16)
		goto fail;
	while (stream.position < stream.length * 8) {
		unsigned padding;
		if (read_bits(&stream, 1, &padding) || padding)
			goto fail;
	}
	*output = image;
	return 0;
fail:
	sdk_abm_release(&image);
	return -1;
}

static int decode_guest_abm(const uint8_t *data, size_t bytes, size_t budget,
			    struct mre_image *out)
{
	if (!data || !out || bytes < 20 || bytes > FILE_CAP || data[0] != 0x34)
		return -1;
	uint32_t dimensions = little(data + 8, 4);
	unsigned width = dimensions & 4095, height = (dimensions >> 12) & 4095;
	size_t pixels = (size_t)width * height;
	if (!width || !height || pixels > budget / 3)
		return -1;
	struct sdk_abm_image image = { 0 };
	if (sdk_abm_decode(data, bytes, -1, &image))
		return -1;
	int transparent = image.has_transparency;
	*out = (struct mre_image){ .width = (int)image.width,
				   .height = (int)image.height,
				   .pixels = image.pixels,
				   .alpha = image.alpha,
				   .has_transparency = transparent,
				   .has_partial_alpha = transparent };
	return 0;
}
