// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <media/v4l2-jpeg.h>

#include "ums9117-jpeg-codec.h"

#define JPEG_MARKER_SOF0 0xc0
#define JPEG_MARKER_SOF1 0xc1
#define JPEG_MARKER_SOI 0xd8
#define JPEG_MARKER_EOI 0xd9
#define JPEG_MARKER_SOS 0xda

#define JPEG_COMPONENTS 3U
#define JPEG_HUFF_DC 0U
#define JPEG_HUFF_AC 2U

static const u8 ums9117_jpeg_zigzag[64] = {
	0,  1,	8,  16, 9,  2,	3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,	7,  14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const u8 ums9117_jpeg_asic_dct[64] = {
	0, 32, 16, 48, 8,  40, 24, 56, 4, 36, 20, 52, 12, 44, 28, 60,
	2, 34, 18, 50, 10, 42, 26, 58, 6, 38, 22, 54, 14, 46, 30, 62,
	1, 33, 17, 49, 9,  41, 25, 57, 5, 37, 21, 53, 13, 45, 29, 61,
	3, 35, 19, 51, 11, 43, 27, 59, 7, 39, 23, 55, 15, 47, 31, 63,
};

static const u8 ums9117_jpeg_dc_offset[16] = {
	0, 2, 6, 14, 26, 38, 50, 62, 74, 86, 98, 110, 122, 134, 146, 158,
};

static u16 ums9117_jpeg_get_be16(const u8 *data)
{
	return ((u16)data[0] << 8) | data[1];
}

static int ums9117_jpeg_audit_markers(const u8 *data, size_t entropy_offset)
{
	size_t offset = 2;
	unsigned int sof_count = 0;
	unsigned int sos_count = 0;

	while (offset < entropy_offset) {
		u16 segment_length;
		u8 marker;

		if (data[offset++] != 0xff)
			return -EINVAL;
		while (offset < entropy_offset && data[offset] == 0xff)
			++offset;
		if (offset >= entropy_offset)
			return -EINVAL;
		marker = data[offset++];
		if (!marker || marker == JPEG_MARKER_SOI ||
		    marker == JPEG_MARKER_EOI || marker == 0x01 ||
		    (marker >= 0xd0 && marker <= 0xd7))
			return -EINVAL;
		if (offset + 2 > entropy_offset)
			return -EINVAL;
		segment_length = ums9117_jpeg_get_be16(data + offset);
		if (segment_length < 2 ||
		    segment_length > entropy_offset - offset)
			return -EINVAL;
		if (marker == JPEG_MARKER_SOF0)
			++sof_count;
		else if (marker == JPEG_MARKER_SOF1)
			return -EOPNOTSUPP;
		if (marker == JPEG_MARKER_SOS) {
			++sos_count;
			if (offset + segment_length != entropy_offset)
				return -EINVAL;
		}
		offset += segment_length;
	}

	return sof_count == 1 && sos_count == 1 && offset == entropy_offset ?
		       0 :
		       -EINVAL;
}

static int ums9117_jpeg_validate_frame(const struct v4l2_jpeg_header *header,
				       struct ums9117_jpeg_frame *frame)
{
	const struct v4l2_jpeg_frame_header *sof = &header->frame;
	const struct v4l2_jpeg_scan_header *sos = header->scan;
	u8 base_id;
	unsigned int index;

	if (sof->precision != 8 || sof->num_components != JPEG_COMPONENTS ||
	    sos->num_components != JPEG_COMPONENTS)
		return -EOPNOTSUPP;
	if (!sof->width || !sof->height)
		return -EINVAL;
	if (sof->width > UMS9117_JPEG_MAX_DIMENSION ||
	    sof->height > UMS9117_JPEG_MAX_DIMENSION)
		return -E2BIG;
	if (header->app14_tf != V4L2_JPEG_APP14_TF_UNKNOWN &&
	    header->app14_tf != V4L2_JPEG_APP14_TF_YCBCR)
		return -EOPNOTSUPP;

	base_id = sof->component[0].component_identifier;
	if (base_id > 2)
		return -EOPNOTSUPP;
	for (index = 0; index < JPEG_COMPONENTS; ++index) {
		const struct v4l2_jpeg_frame_component_spec *component =
			&sof->component[index];
		const struct v4l2_jpeg_scan_component_spec *scan_component =
			&sos->component[index];

		if (component->component_identifier != base_id + index ||
		    scan_component->component_selector !=
			    component->component_identifier)
			return -EOPNOTSUPP;
		if (component->quantization_table_selector >=
			    V4L2_JPEG_MAX_TABLES ||
		    scan_component->dc_entropy_coding_table_selector > 1 ||
		    scan_component->ac_entropy_coding_table_selector > 1)
			return -EOPNOTSUPP;
	}
	if (sof->component[1].horizontal_sampling_factor != 1 ||
	    sof->component[1].vertical_sampling_factor != 1 ||
	    sof->component[2].horizontal_sampling_factor != 1 ||
	    sof->component[2].vertical_sampling_factor != 1)
		return -EOPNOTSUPP;

	if (sof->subsampling == V4L2_JPEG_CHROMA_SUBSAMPLING_420 &&
	    sof->component[0].horizontal_sampling_factor == 2 &&
	    sof->component[0].vertical_sampling_factor == 2) {
		frame->mcu_format = UMS9117_JPEG_MCU_420;
		frame->vertical_subsampling = 2;
	} else if (sof->subsampling == V4L2_JPEG_CHROMA_SUBSAMPLING_422 &&
		   sof->component[0].horizontal_sampling_factor == 2 &&
		   sof->component[0].vertical_sampling_factor == 1) {
		frame->mcu_format = UMS9117_JPEG_MCU_422;
		frame->vertical_subsampling = 1;
	} else {
		return -EOPNOTSUPP;
	}

	frame->width = sof->width;
	frame->height = sof->height;
	frame->padded_width = (frame->width + 15U) & ~15U;
	frame->padded_height =
		(frame->height + 8U * frame->vertical_subsampling - 1U) &
		~(8U * frame->vertical_subsampling - 1U);
	frame->mcu_x = frame->padded_width / 16U;
	frame->mcu_y =
		frame->padded_height / (8U * frame->vertical_subsampling);
	frame->restart_interval = header->restart_interval;
	frame->entropy_offset = header->ecs_offset;

	return 0;
}

static int ums9117_jpeg_validate_sos(const struct v4l2_jpeg_header *header)
{
	const struct v4l2_jpeg_reference *sos = &header->sos;

	if (!sos->start || sos->length < 3)
		return -EINVAL;
	if (sos->start[sos->length - 3] != 0 ||
	    sos->start[sos->length - 2] != 63 ||
	    sos->start[sos->length - 1] != 0)
		return -EOPNOTSUPP;

	return 0;
}

static int
ums9117_jpeg_validate_huffman(const struct v4l2_jpeg_reference *table, bool ac)
{
	u8 seen[256] = { 0 };
	unsigned int index;
	int slots = 1;
	u16 count = 0;

	if (!table->start || table->length < 17)
		return -EINVAL;
	for (index = 0; index < 16; ++index) {
		count += table->start[index];
		slots = slots * 2 - table->start[index];
		if (slots < 0)
			return -EINVAL;
	}
	/* JPEG reserves the all-ones code for entropy-segment padding. */
	if (!count || table->length != 16U + count || !slots)
		return -EINVAL;
	if ((!ac && count > 12) || (ac && count > 162))
		return -EINVAL;
	if (!ac && table->start[15] > 4)
		return -EINVAL;

	for (index = 0; index < count; ++index) {
		u8 value = table->start[16 + index];

		if (seen[value])
			return -EINVAL;
		seen[value] = 1;
		if (!ac) {
			if (value > 11)
				return -EINVAL;
		} else if ((value & 0x0f) > 10 ||
			   (!(value & 0x0f) && value != 0x00 &&
			    value != 0xf0)) {
			return -EINVAL;
		}
	}

	return 0;
}

static bool ums9117_jpeg_table_equal(const struct v4l2_jpeg_reference *left,
				     const struct v4l2_jpeg_reference *right)
{
	return left->start && right->start && left->length == right->length &&
	       !memcmp(left->start, right->start, left->length);
}

static int
ums9117_jpeg_select_tables(const struct v4l2_jpeg_header *header,
			   const struct v4l2_jpeg_reference *quant[2],
			   const struct v4l2_jpeg_reference *dc[2],
			   const struct v4l2_jpeg_reference *ac[2])
{
	const struct v4l2_jpeg_frame_header *sof = &header->frame;
	const struct v4l2_jpeg_scan_header *sos = header->scan;
	const struct v4l2_jpeg_reference *quant_v;
	const struct v4l2_jpeg_reference *dc_v;
	const struct v4l2_jpeg_reference *ac_v;
	unsigned int index;
	int ret;

	quant[0] = &header->quantization_tables
			    [sof->component[0].quantization_table_selector];
	quant[1] = &header->quantization_tables
			    [sof->component[1].quantization_table_selector];
	quant_v = &header->quantization_tables
			   [sof->component[2].quantization_table_selector];
	if (quant[0]->length != 64 || quant[1]->length != 64 ||
	    !ums9117_jpeg_table_equal(quant[1], quant_v))
		return -EINVAL;
	for (index = 0; index < 64; ++index)
		if (!quant[0]->start[index] || !quant[1]->start[index])
			return -EINVAL;

	dc[0] = &header->huffman_tables
			 [JPEG_HUFF_DC +
			  sos->component[0].dc_entropy_coding_table_selector];
	dc[1] = &header->huffman_tables
			 [JPEG_HUFF_DC +
			  sos->component[1].dc_entropy_coding_table_selector];
	dc_v = &header->huffman_tables
			[JPEG_HUFF_DC +
			 sos->component[2].dc_entropy_coding_table_selector];
	ac[0] = &header->huffman_tables
			 [JPEG_HUFF_AC +
			  sos->component[0].ac_entropy_coding_table_selector];
	ac[1] = &header->huffman_tables
			 [JPEG_HUFF_AC +
			  sos->component[1].ac_entropy_coding_table_selector];
	ac_v = &header->huffman_tables
			[JPEG_HUFF_AC +
			 sos->component[2].ac_entropy_coding_table_selector];
	if (!ums9117_jpeg_table_equal(dc[1], dc_v) ||
	    !ums9117_jpeg_table_equal(ac[1], ac_v))
		return -EINVAL;
	for (index = 0; index < 2; ++index) {
		ret = ums9117_jpeg_validate_huffman(dc[index], false);
		if (ret)
			return ret;
		ret = ums9117_jpeg_validate_huffman(ac[index], true);
		if (ret)
			return ret;
	}

	return 0;
}

static void ums9117_jpeg_pack_quant(const struct v4l2_jpeg_reference *table,
				    struct ums9117_jpeg_frame *frame,
				    unsigned int group)
{
	u8 natural[64];
	unsigned int index;

	for (index = 0; index < 64; ++index)
		natural[ums9117_jpeg_zigzag[index]] = table->start[index];
	for (index = 0; index < 64; index += 2) {
		u32 low = natural[ums9117_jpeg_asic_dct[index]];
		u32 high = natural[ums9117_jpeg_asic_dct[index + 1]];

		frame->quant_words[group * 32 + index / 2] = low | (high << 16);
	}
}

static u32 ums9117_jpeg_pack_huffman(const struct v4l2_jpeg_reference *table,
				     struct ums9117_jpeg_frame *frame,
				     unsigned int group, bool ac)
{
	const u8 *bits = table->start;
	const u8 *values = table->start + 16;
	u32 code = 0;
	u32 value_index = 0;
	u32 valid = 0;
	unsigned int length;
	unsigned int meta_base = ac ? 34 + 16 * group : 2 + 16 * group;
	unsigned int value_shift = ac ? 8 * group : 16 + 8 * group;

	for (length = 0; length < 16; ++length) {
		u32 count = bits[length];
		u32 max_code = code;
		unsigned int index;

		if (count) {
			max_code += count - 1;
			valid |= 1U << (15 - length);
		}
		frame->huff_meta_words[meta_base + length] =
			ac ? (max_code << 8) | (count ? value_index : 0) :
			     max_code;
		for (index = 0; index < count; ++index) {
			u32 value = (u32)values[value_index + index]
				    << value_shift;

			if (ac)
				frame->huff_value_words[value_index + index] |=
					value;
			else
				frame->huff_value_words
					[ums9117_jpeg_dc_offset[length] +
					 index] |= value;
		}
		value_index += count;
		code = (code + count) << 1;
	}

	return valid;
}

static int ums9117_jpeg_validate_entropy(const u8 *data, size_t length,
					 struct ums9117_jpeg_frame *frame)
{
	size_t offset = frame->entropy_offset;
	u32 restart_count = 0;
	u32 expected_restarts;
	u32 total_mcus = frame->mcu_x * frame->mcu_y;
	bool saw_eoi = false;

	if (offset >= length)
		return -EINVAL;
	while (offset < length) {
		size_t ff_count = 0;
		u8 marker;

		if (data[offset] != 0xff) {
			++offset;
			continue;
		}
		while (offset < length && data[offset] == 0xff) {
			++ff_count;
			++offset;
		}
		if (offset >= length)
			return -EINVAL;
		marker = data[offset++];
		if (!marker) {
			if (ff_count != 1)
				return -EINVAL;
			continue;
		}
		if (marker >= 0xd0 && marker <= 0xd7) {
			if (!frame->restart_interval ||
			    marker != 0xd0 + (restart_count & 7))
				return -EINVAL;
			++restart_count;
			continue;
		}
		if (marker == JPEG_MARKER_EOI && offset == length) {
			saw_eoi = true;
			break;
		}
		return -EOPNOTSUPP;
	}
	if (!saw_eoi)
		return -EINVAL;
	expected_restarts = frame->restart_interval ?
				    (total_mcus - 1) / frame->restart_interval :
				    0;
	if (restart_count != expected_restarts)
		return -EINVAL;
	frame->entropy_length = length - frame->entropy_offset;

	return frame->entropy_length > 2 ? 0 : -EINVAL;
}

int ums9117_jpeg_parse(const void *data, size_t length,
		       struct ums9117_jpeg_frame *frame)
{
	struct v4l2_jpeg_scan_header scan = { 0 };
	struct v4l2_jpeg_reference quant_tables[V4L2_JPEG_MAX_TABLES] = { 0 };
	struct v4l2_jpeg_reference huffman_tables[V4L2_JPEG_MAX_TABLES] = { 0 };
	struct v4l2_jpeg_header header = {
		.scan = &scan,
		.quantization_tables = quant_tables,
		.huffman_tables = huffman_tables,
	};
	const struct v4l2_jpeg_reference *quant[2];
	const struct v4l2_jpeg_reference *dc[2];
	const struct v4l2_jpeg_reference *ac[2];
	u32 dc_valid_y;
	u32 dc_valid_c;
	u32 ac_valid_y;
	u32 ac_valid_c;
	int ret;

	if (!data || !frame)
		return -EINVAL;
	memset(frame, 0, sizeof(*frame));
	if (length < 4 || length > UMS9117_JPEG_MAX_INPUT_SIZE)
		return length > UMS9117_JPEG_MAX_INPUT_SIZE ? -E2BIG : -EINVAL;

	ret = v4l2_jpeg_parse_header((void *)data, length, &header);
	if (ret)
		return ret;
	ret = ums9117_jpeg_audit_markers(data, header.ecs_offset);
	if (ret)
		return ret;
	ret = ums9117_jpeg_validate_frame(&header, frame);
	if (ret)
		return ret;
	ret = ums9117_jpeg_validate_sos(&header);
	if (ret)
		return ret;
	ret = ums9117_jpeg_select_tables(&header, quant, dc, ac);
	if (ret)
		return ret;
	ret = ums9117_jpeg_validate_entropy(data, length, frame);
	if (ret)
		return ret;

	ums9117_jpeg_pack_quant(quant[0], frame, 0);
	ums9117_jpeg_pack_quant(quant[1], frame, 1);
	dc_valid_y = ums9117_jpeg_pack_huffman(dc[0], frame, 0, false);
	dc_valid_c = ums9117_jpeg_pack_huffman(dc[1], frame, 1, false);
	ac_valid_y = ums9117_jpeg_pack_huffman(ac[0], frame, 0, true);
	ac_valid_c = ums9117_jpeg_pack_huffman(ac[1], frame, 1, true);
	frame->huff_meta_words[0] = (dc_valid_y << 16) | dc_valid_c;
	frame->huff_meta_words[1] = (ac_valid_y << 16) | ac_valid_c;

	return 0;
}

struct ums9117_jpeg_writer {
	u8 *data;
	size_t capacity;
	size_t position;
	int error;
};

static void ums9117_jpeg_put_u8(struct ums9117_jpeg_writer *writer, u8 value)
{
	if (writer->error)
		return;
	if (writer->position >= writer->capacity) {
		writer->error = -ENOSPC;
		return;
	}
	writer->data[writer->position++] = value;
}

static void ums9117_jpeg_put_be16(struct ums9117_jpeg_writer *writer, u16 value)
{
	ums9117_jpeg_put_u8(writer, value >> 8);
	ums9117_jpeg_put_u8(writer, value);
}

static void ums9117_jpeg_put_bytes(struct ums9117_jpeg_writer *writer,
				   const u8 *data, size_t length)
{
	if (writer->error)
		return;
	if (length > writer->capacity - writer->position) {
		writer->error = -ENOSPC;
		return;
	}
	memcpy(writer->data + writer->position, data, length);
	writer->position += length;
}

static void ums9117_jpeg_put_marker(struct ums9117_jpeg_writer *writer,
				    u8 marker)
{
	ums9117_jpeg_put_u8(writer, 0xff);
	ums9117_jpeg_put_u8(writer, marker);
}

static u8 ums9117_jpeg_quality85_quant(u8 reference)
{
	u32 value = DIV_ROUND_CLOSEST((u32)reference * 30U, 100U);

	return clamp_t(u32, value, 1, 255);
}

static unsigned int ums9117_jpeg_reverse6(unsigned int value)
{
	unsigned int reversed = 0;
	unsigned int bit;

	for (bit = 0; bit < 6; ++bit) {
		reversed = (reversed << 1) | (value & 1);
		value >>= 1;
	}
	return reversed;
}

static u16 ums9117_jpeg_encode_qbuf_half(u8 quantum)
{
	u32 exponent = fls((unsigned int)quantum - 1);
	u32 shift = 11 + exponent;
	u32 reciprocal = ((1U << shift) + quantum / 2) / quantum;

	return (reciprocal << 4) | exponent;
}

static void ums9117_jpeg_encode_qbuf(struct ums9117_jpeg_encode_config *config)
{
	unsigned int component;

	for (component = 0; component < 2; ++component) {
		unsigned int upload;

		for (upload = 0; upload < UMS9117_JPEG_BLOCK_COEFFICIENTS;
		     upload += 2) {
			u16 low = ums9117_jpeg_encode_qbuf_half(
				config->quant[component]
					     [ums9117_jpeg_reverse6(upload)]);
			u16 high = ums9117_jpeg_encode_qbuf_half(
				config->quant[component][ums9117_jpeg_reverse6(
					upload + 1)]);

			config->qbuf_words[component * 32 + upload / 2] =
				low | ((u32)high << 16);
		}
	}
}

static int ums9117_jpeg_canonical_codes(const u8 *table, size_t table_size,
					u16 codes[256], bool present[256])
{
	u32 code = 0;
	size_t position = 16;
	unsigned int bit_length;

	if (table_size < 16)
		return -EINVAL;
	memset(codes, 0, 256 * sizeof(*codes));
	memset(present, 0, 256 * sizeof(*present));
	for (bit_length = 1; bit_length <= 16; ++bit_length) {
		u32 count = table[bit_length - 1];
		u32 index;

		if (count > table_size - position ||
		    code + count > (1U << bit_length))
			return -EINVAL;
		for (index = 0; index < count; ++index) {
			u8 symbol = table[position++];

			if (present[symbol])
				return -EINVAL;
			present[symbol] = true;
			codes[symbol] = code++ << (16 - bit_length);
		}
		code <<= 1;
	}
	return position == table_size ? 0 : -EINVAL;
}

static int ums9117_jpeg_encode_ac_lut(struct ums9117_jpeg_encode_config *config)
{
	u16 codes[256];
	bool present[256];
	unsigned int run;
	int ret;

	ret = ums9117_jpeg_canonical_codes(
		v4l2_jpeg_ref_table_luma_ac_ht,
		ARRAY_SIZE(v4l2_jpeg_ref_table_luma_ac_ht), codes, present);
	if (ret)
		return ret;

	for (run = 0; run < 16; ++run) {
		unsigned int size;

		for (size = 1; size <= 10; ++size) {
			u8 symbol = (run << 4) | size;
			unsigned int index = run * 10 + size - 1;

			if (!present[symbol])
				return -EINVAL;
			config->ac_lut_words[index] = codes[symbol];
		}
	}

	ret = ums9117_jpeg_canonical_codes(
		v4l2_jpeg_ref_table_chroma_ac_ht,
		ARRAY_SIZE(v4l2_jpeg_ref_table_chroma_ac_ht), codes, present);
	if (ret)
		return ret;

	for (run = 0; run < 16; ++run) {
		unsigned int size;

		for (size = 1; size <= 10; ++size) {
			u8 symbol = (run << 4) | size;
			unsigned int index = run * 10 + size - 1;

			if (!present[symbol])
				return -EINVAL;
			config->ac_lut_words[index] |= (u32)codes[symbol] << 16;
		}
	}
	/* The hardware table has two unused trailing words. */
	config->ac_lut_words[160] = 0;
	config->ac_lut_words[161] = 0;
	return 0;
}

static void
ums9117_jpeg_encode_dqt(struct ums9117_jpeg_writer *writer,
			const u8 quant[UMS9117_JPEG_BLOCK_COEFFICIENTS],
			u8 destination)
{
	unsigned int index;

	ums9117_jpeg_put_marker(writer, 0xdb);
	ums9117_jpeg_put_be16(writer, 67);
	ums9117_jpeg_put_u8(writer, destination);
	for (index = 0; index < UMS9117_JPEG_BLOCK_COEFFICIENTS; ++index) {
		ums9117_jpeg_put_u8(writer,
				    quant[v4l2_jpeg_zigzag_scan_index[index]]);
	}
}

static void ums9117_jpeg_encode_dht(struct ums9117_jpeg_writer *writer,
				    const u8 *table, size_t table_size,
				    u8 destination)
{
	if (table_size > U16_MAX - 3) {
		writer->error = -EINVAL;
		return;
	}
	ums9117_jpeg_put_marker(writer, 0xc4);
	ums9117_jpeg_put_be16(writer, 3 + table_size);
	ums9117_jpeg_put_u8(writer, destination);
	ums9117_jpeg_put_bytes(writer, table, table_size);
}

static int ums9117_jpeg_encode_header(struct ums9117_jpeg_encode_config *config)
{
	static const u8 app0[] = {
		'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
	};
	struct ums9117_jpeg_writer writer = {
		.data = config->header,
		.capacity = sizeof(config->header),
	};

	ums9117_jpeg_put_marker(&writer, 0xd8);
	ums9117_jpeg_put_marker(&writer, 0xe0);
	ums9117_jpeg_put_be16(&writer, 16);
	ums9117_jpeg_put_bytes(&writer, app0, sizeof(app0));
	ums9117_jpeg_encode_dqt(&writer, config->quant[0], 0);
	ums9117_jpeg_encode_dqt(&writer, config->quant[1], 1);

	ums9117_jpeg_put_marker(&writer, 0xc0);
	ums9117_jpeg_put_be16(&writer, 17);
	ums9117_jpeg_put_u8(&writer, 8);
	ums9117_jpeg_put_be16(&writer, config->height);
	ums9117_jpeg_put_be16(&writer, config->width);
	{
		static const u8 components[] = {
			3, 1, 0x21, 0, 2, 0x11, 1, 3, 0x11, 1,
		};

		ums9117_jpeg_put_bytes(&writer, components, sizeof(components));
	}

	ums9117_jpeg_encode_dht(&writer, v4l2_jpeg_ref_table_luma_dc_ht,
				ARRAY_SIZE(v4l2_jpeg_ref_table_luma_dc_ht),
				0x00);
	ums9117_jpeg_encode_dht(&writer, v4l2_jpeg_ref_table_luma_ac_ht,
				ARRAY_SIZE(v4l2_jpeg_ref_table_luma_ac_ht),
				0x10);
	ums9117_jpeg_encode_dht(&writer, v4l2_jpeg_ref_table_chroma_dc_ht,
				ARRAY_SIZE(v4l2_jpeg_ref_table_chroma_dc_ht),
				0x01);
	ums9117_jpeg_encode_dht(&writer, v4l2_jpeg_ref_table_chroma_ac_ht,
				ARRAY_SIZE(v4l2_jpeg_ref_table_chroma_ac_ht),
				0x11);

	ums9117_jpeg_put_marker(&writer, 0xdd);
	ums9117_jpeg_put_be16(&writer, 4);
	ums9117_jpeg_put_be16(&writer, config->restart_interval);

	ums9117_jpeg_put_marker(&writer, 0xda);
	ums9117_jpeg_put_be16(&writer, 12);
	{
		static const u8 scan[] = {
			3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0,
		};

		ums9117_jpeg_put_bytes(&writer, scan, sizeof(scan));
	}
	if (writer.error)
		return writer.error;
	return writer.position == UMS9117_JPEG_ENCODE_HEADER_SIZE ? 0 : -EINVAL;
}

int ums9117_jpeg_build_encode_config(u32 width, u32 height,
				     struct ums9117_jpeg_encode_config *config)
{
	unsigned int index;
	int ret;

	if (!config)
		return -EINVAL;
	memset(config, 0, sizeof(*config));
	if (!width || !height || width > UMS9117_JPEG_MAX_DIMENSION ||
	    height > UMS9117_JPEG_MAX_DIMENSION || !IS_ALIGNED(width, 16) ||
	    !IS_ALIGNED(height, 8))
		return -EINVAL;
	config->width = width;
	config->height = height;
	config->mcu_x = width / 16;
	config->mcu_y = height / 8;
	config->restart_interval =
		config->mcu_x * (UMS9117_JPEG_ENCODE_STRIP_ROWS / 8);

	for (index = 0; index < V4L2_JPEG_PIXELS_IN_BLOCK; ++index) {
		config->quant[0][index] = ums9117_jpeg_quality85_quant(
			v4l2_jpeg_ref_table_luma_qt[index]);
		config->quant[1][index] = ums9117_jpeg_quality85_quant(
			v4l2_jpeg_ref_table_chroma_qt[index]);
	}
	ums9117_jpeg_encode_qbuf(config);
	ret = ums9117_jpeg_encode_ac_lut(config);
	if (!ret)
		ret = ums9117_jpeg_encode_header(config);
	if (ret)
		memset(config, 0, sizeof(*config));
	return ret;
}
