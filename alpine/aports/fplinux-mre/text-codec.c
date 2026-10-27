/* SPDX-License-Identifier: 0BSD */
#include "text-codec.h"

#include <errno.h>

static bool text_unit_valid(uint16_t value)
{
	return value &&
	       (value >= 0x20 || value == '\t' || value == '\n' ||
		value == '\r') &&
	       (value < 0x7f || value > 0x9f) &&
	       (value < 0xd800 || value > 0xdfff);
}

bool mre_utf8_decode(const unsigned char *input, size_t bytes, uint16_t *output,
		     size_t capacity, size_t *units)
{
	size_t count = 0;

	if (!input && bytes) {
		errno = EINVAL;
		return false;
	}
	for (size_t offset = 0; offset < bytes;) {
		unsigned char first = input[offset++];
		uint16_t value;

		if (first < 0x80) {
			value = first;
		} else if (first >= 0xc2 && first <= 0xdf) {
			if (offset >= bytes || (input[offset] & 0xc0) != 0x80)
				goto invalid;
			value = (uint16_t)((first & 0x1f) << 6) |
				(input[offset++] & 0x3f);
		} else if (first >= 0xe0 && first <= 0xef) {
			unsigned char second;

			if (bytes - offset < 2)
				goto invalid;
			second = input[offset];
			if ((second & 0xc0) != 0x80 ||
			    (input[offset + 1] & 0xc0) != 0x80 ||
			    (first == 0xe0 && second < 0xa0) ||
			    (first == 0xed && second >= 0xa0))
				goto invalid;
			value = (uint16_t)((first & 0x0f) << 12) |
				(uint16_t)((second & 0x3f) << 6) |
				(input[offset + 1] & 0x3f);
			offset += 2;
		} else {
			goto invalid;
		}
		if (!text_unit_valid(value))
			goto invalid;
		if (output) {
			if (count >= capacity || capacity - count <= 1) {
				errno = ENOSPC;
				return false;
			}
			output[count] = value;
		}
		++count;
	}
	if (output) {
		if (count >= capacity) {
			errno = ENOSPC;
			return false;
		}
		output[count] = 0;
	}
	if (units)
		*units = count;
	return true;

invalid:
	errno = EINVAL;
	return false;
}

bool mre_utf8_encode(const uint16_t *input, size_t units, unsigned char *output,
		     size_t capacity, size_t *bytes)
{
	size_t required = 0;
	size_t offset = 0;

	if ((!input && units) || !output) {
		errno = EINVAL;
		return false;
	}
	for (size_t i = 0; i < units; ++i) {
		uint16_t value = input[i];
		size_t length = value < 0x80 ? 1 : value < 0x800 ? 2 : 3;

		if (!text_unit_valid(value)) {
			errno = EINVAL;
			return false;
		}
		if (required > SIZE_MAX - length) {
			errno = ENOSPC;
			return false;
		}
		required += length;
	}
	if (required >= capacity) {
		errno = ENOSPC;
		return false;
	}
	for (size_t i = 0; i < units; ++i) {
		uint16_t value = input[i];

		if (value < 0x80) {
			output[offset++] = (unsigned char)value;
		} else if (value < 0x800) {
			output[offset++] = 0xc0 | (value >> 6);
			output[offset++] = 0x80 | (value & 0x3f);
		} else {
			output[offset++] = 0xe0 | (value >> 12);
			output[offset++] = 0x80 | ((value >> 6) & 0x3f);
			output[offset++] = 0x80 | (value & 0x3f);
		}
	}
	output[offset] = 0;
	if (bytes)
		*bytes = offset;
	return true;
}

bool mre_text_save_fits(size_t bytes, size_t units)
{
	/* bytes + NUL <= 2 * (units + NUL), without overflowing the product. */
	return bytes <= 1999 && units >= bytes / 2;
}
