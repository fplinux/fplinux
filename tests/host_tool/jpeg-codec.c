// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host oracle for UMS9117 table packing and validation.  The small
 * v4l2_jpeg_parse_header double below represents the external Linux helper's
 * result for the fixed JPEG vectors; this does not test that helper.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <linux/kernel.h>
#include <media/v4l2-jpeg.h>

#include "ums9117-jpeg-codec.h"
#include "jpeg-codec-fixtures.h"

static u16 get_be16(const u8 *data)
{
	return ((u16)data[0] << 8) | data[1];
}

static size_t huffman_length(const u8 *bits)
{
	size_t length = 16;
	unsigned int index;

	for (index = 0; index < 16; ++index)
		length += bits[index];
	return length;
}

int v4l2_jpeg_parse_header(void *buffer, size_t length,
			   struct v4l2_jpeg_header *header)
{
	u8 *data = buffer;
	struct v4l2_jpeg_scan_header *scan = header->scan;
	size_t offset = 177;
	size_t sos_offset = 0;
	const u8 *sof;
	const u8 *sos;
	unsigned int index;

	if (length < 193)
		return -EINVAL;
	while (offset + 4 <= length) {
		size_t segment_length = get_be16(data + offset + 2);
		u8 marker = data[offset + 1];

		if (data[offset] != 0xff || segment_length < 2 ||
		    segment_length > length - offset - 2)
			return -EINVAL;
		if (marker == 0xda) {
			if (segment_length != 12)
				return -EINVAL;
			sos_offset = offset + 2;
			break;
		}
		if (marker == 0xc4) {
			size_t table_offset = offset + 4;
			size_t end = offset + 2 + segment_length;

			++header->num_dht;
			if (segment_length < 19)
				return -EINVAL;
			while (table_offset < end) {
				u8 selector = data[table_offset++];
				size_t table_length;
				unsigned int table_index;

				if (end - table_offset < 16 ||
				    (selector >> 4) > 1 || (selector & 15) > 1)
					return -EINVAL;
				table_length =
					huffman_length(data + table_offset);
				if (table_length > end - table_offset)
					return -EINVAL;
				table_index =
					2 * (selector >> 4) + (selector & 15);
				header->huffman_tables[table_index].start =
					data + table_offset;
				header->huffman_tables[table_index].length =
					table_length;
				table_offset += table_length;
			}
		} else if (marker == 0xdd) {
			if (segment_length != 4)
				return -EINVAL;
			header->restart_interval = get_be16(data + offset + 4);
		}
		offset += 2 + segment_length;
	}
	if (!sos_offset)
		return -EINVAL;
	sof = data + 162;
	sos = data + sos_offset + 2;

	header->sof.start = data + 160;
	header->sof.length = 17;
	header->sos.start = data + sos_offset;
	header->sos.length = get_be16(header->sos.start);
	header->frame.precision = sof[0];
	header->frame.height = get_be16(sof + 1);
	header->frame.width = get_be16(sof + 3);
	header->frame.num_components = sof[5];
	for (index = 0; index < 3; ++index) {
		struct v4l2_jpeg_frame_component_spec *component =
			&header->frame.component[index];
		u8 sampling = sof[7 + 3 * index];

		component->component_identifier = sof[6 + 3 * index];
		component->horizontal_sampling_factor = sampling >> 4;
		component->vertical_sampling_factor = sampling & 0x0f;
		component->quantization_table_selector = sof[8 + 3 * index];
	}
	switch (sof[7]) {
	case 0x22:
		header->frame.subsampling = V4L2_JPEG_CHROMA_SUBSAMPLING_420;
		break;
	case 0x21:
		header->frame.subsampling = V4L2_JPEG_CHROMA_SUBSAMPLING_422;
		break;
	default:
		header->frame.subsampling = V4L2_JPEG_CHROMA_SUBSAMPLING_444;
		break;
	}

	scan->num_components = sos[0];
	for (index = 0; index < 3; ++index) {
		u8 selector = sos[2 + 2 * index];

		scan->component[index].component_selector = sos[1 + 2 * index];
		scan->component[index].dc_entropy_coding_table_selector =
			selector >> 4;
		scan->component[index].ac_entropy_coding_table_selector =
			selector & 0x0f;
	}

	header->quantization_tables[0].start = data + 25;
	header->quantization_tables[0].length = 64;
	header->quantization_tables[1].start = data + 94;
	header->quantization_tables[1].length = 64;
	header->ecs_offset = sos_offset + header->sos.length;
	header->app14_tf = V4L2_JPEG_APP14_TF_UNKNOWN;
	return 0;
}

static int expect(bool condition, const char *message)
{
	if (condition)
		return 0;
	fprintf(stderr, "%s\n", message);
	return 1;
}

static int test_valid_420(void)
{
	struct ums9117_jpeg_frame frame;
	int failed = 0;
	int ret = ums9117_jpeg_parse(jpeg_420, sizeof(jpeg_420), &frame);

	failed |= expect(ret == 0, "4:2:0 fixture was rejected");
	if (ret)
		return failed;
	failed |= expect(frame.width == 32 && frame.height == 16 &&
				 frame.padded_width == 32 &&
				 frame.padded_height == 16,
			 "4:2:0 geometry changed");
	failed |= expect(frame.mcu_x == 2 && frame.mcu_y == 1 &&
				 frame.mcu_format == 0 &&
				 frame.vertical_subsampling == 2,
			 "4:2:0 MCU description changed");
	failed |= expect(frame.restart_interval == 1 &&
				 frame.entropy_offset == 297 &&
				 frame.entropy_length == 31,
			 "4:2:0 entropy description changed");
	/*
	 * These sentinels are derived by hand from the fixture DQT/DHT bytes
	 * and the register layout the driver programs: natural-order
	 * coefficients in 6-bit bit-reversed order and per-length Huffman
	 * lanes.  They characterize that layout; a host run cannot show that
	 * the hardware accepts it.  They must not be regenerated from the
	 * codec implementation under test.
	 * For example, Y DQT zigzag entries 0/10 are 4/5 and feed ASIC natural
	 * slots 0/32, giving word 0x00050004.  Y-DC length counts 1,1 and C-DC
	 * counts 1,1,1 give valid masks 0xc000/0xe000 and maxima 0,2/0,2,6.
	 * C-AC counts at lengths 2..5 are 1,3,5,1, giving mask 0x7800 and the
	 * checked (max, base) pairs (4,1), (14,4), and (30,9).
	 */
	failed |= expect(frame.quant_words[0] == 0x00050004U &&
				 frame.quant_words[1] == 0x000d0004U &&
				 frame.quant_words[31] == 0x001a0010U &&
				 frame.quant_words[32] == 0x001a0004U &&
				 frame.quant_words[33] == 0x001a0006U &&
				 frame.quant_words[63] == 0x001a001aU,
			 "selected quantization register slots changed");
	failed |= expect(frame.huff_meta_words[0] == 0xc000e000U &&
				 frame.huff_meta_words[1] == 0x80007800U &&
				 frame.huff_meta_words[3] == 0x00000002U &&
				 frame.huff_meta_words[19] == 0x00000002U &&
				 frame.huff_meta_words[20] == 0x00000006U &&
				 frame.huff_meta_words[52] == 0x00000401U &&
				 frame.huff_meta_words[53] == 0x00000e04U &&
				 frame.huff_meta_words[54] == 0x00001e09U,
			 "selected Huffman metadata register slots changed");
	failed |= expect(frame.huff_value_words[0] == 0x04070000U &&
				 frame.huff_value_words[1] == 0x00000700U &&
				 frame.huff_value_words[2] == 0x01084500U &&
				 frame.huff_value_words[6] == 0x06004300U &&
				 frame.huff_value_words[9] == 0x0000c200U,
			 "selected 4:2:0 Huffman value slots changed");
	return failed;
}

static int test_valid_422(void)
{
	struct ums9117_jpeg_frame frame;
	int failed = 0;
	int ret = ums9117_jpeg_parse(jpeg_422, sizeof(jpeg_422), &frame);

	failed |= expect(ret == 0, "4:2:2 fixture was rejected");
	if (ret)
		return failed;
	failed |= expect(frame.width == 32 && frame.height == 8 &&
				 frame.padded_width == 32 &&
				 frame.padded_height == 8,
			 "4:2:2 geometry changed");
	failed |= expect(frame.mcu_x == 2 && frame.mcu_y == 1 &&
				 frame.mcu_format == 3 &&
				 frame.vertical_subsampling == 1,
			 "4:2:2 MCU description changed");
	failed |= expect(frame.restart_interval == 0 &&
				 frame.entropy_offset == 291 &&
				 frame.entropy_length == 24,
			 "4:2:2 entropy description changed");
	failed |= expect(frame.quant_words[0] == 0x00050004U &&
				 frame.quant_words[32] == 0x001a0004U &&
				 frame.quant_words[63] == 0x001a001aU,
			 "selected 4:2:2 quantization register slots changed");
	failed |= expect(frame.huff_meta_words[0] == 0xc000e000U &&
				 frame.huff_meta_words[1] == 0x80007800U &&
				 frame.huff_meta_words[20] == 0x00000006U &&
				 frame.huff_meta_words[54] == 0x00001e09U,
			 "selected 4:2:2 Huffman metadata slots changed");
	failed |= expect(frame.huff_value_words[0] == 0x06070000U &&
				 frame.huff_value_words[1] == 0x00000700U &&
				 frame.huff_value_words[2] == 0x03084500U &&
				 frame.huff_value_words[6] == 0x04004300U &&
				 frame.huff_value_words[9] == 0x0000c200U,
			 "selected 4:2:2 Huffman value slots changed");
	return failed;
}

static int test_mjpeg_without_dht(void)
{
	static const struct {
		const u8 *data;
		size_t length;
		u32 height;
		u32 mcu_format;
		u32 vertical_subsampling;
	} cases[] = {
		{ mjpeg_420, sizeof(mjpeg_420), 16, 0, 2 },
		{ mjpeg_422, sizeof(mjpeg_422), 8, 3, 1 },
	};
	struct ums9117_jpeg_frame frame;
	unsigned int index;
	int failed = 0;

	for (index = 0; index < ARRAY_SIZE(cases); ++index) {
		int ret = ums9117_jpeg_parse(cases[index].data,
					     cases[index].length, &frame);

		failed |= expect(ret == 0, "MJPEG without DHT was rejected");
		if (ret)
			continue;
		failed |= expect(
			frame.width == 32 &&
				frame.height == cases[index].height &&
				frame.padded_width == 32 &&
				frame.padded_height == cases[index].height &&
				frame.mcu_x == 2 && frame.mcu_y == 1 &&
				frame.mcu_format == cases[index].mcu_format &&
				frame.vertical_subsampling ==
					cases[index].vertical_subsampling,
			"MJPEG without DHT has incorrect geometry");
		failed |= expect(
			frame.entropy_offset == 191 &&
				frame.entropy_length ==
					cases[index].length - 191 &&
				frame.restart_interval == 0,
			"MJPEG without DHT has incorrect entropy bounds");
		/*
		 * Annex K DC lengths are 2..9 (Y) and 2..11 (C); AC lengths
		 * are 2..12,15,16 (Y) and 2..12,14..16 (C). The literal
		 * sentinels characterize packing of those specification tables,
		 * independently of the arrays supplied to the codec object.
		 */
		failed |= expect(frame.huff_meta_words[0] == 0x7f807fe0U &&
					 frame.huff_meta_words[1] ==
						 0x7ff37ff7U &&
					 frame.huff_meta_words[4] == 6 &&
					 frame.huff_meta_words[19] == 2 &&
					 frame.huff_meta_words[20] == 6 &&
					 frame.huff_meta_words[35] == 0x100 &&
					 frame.huff_meta_words[36] == 0x402 &&
					 frame.huff_meta_words[51] == 0x100 &&
					 frame.huff_meta_words[52] == 0x402,
				 "MJPEG default Huffman metadata is incorrect");
		failed |= expect(
			frame.huff_value_words[0] == 0x00000001U &&
				frame.huff_value_words[1] == 0x00000102U &&
				frame.huff_value_words[3] == 0x01000300U &&
				frame.huff_value_words[6] == 0x03010505U &&
				frame.huff_value_words[161] == 0x0000fafaU,
			"MJPEG default Huffman symbols are incorrect");
	}
	return failed;
}

static int test_mjpeg_dht_rejections(void)
{
	static const struct {
		size_t offset;
		u8 selector;
	} unsupported[] = {
		{ 183, 0x10 }, { 183, 0x01 }, { 183, 0x11 }, { 185, 0x00 },
		{ 185, 0x01 }, { 185, 0x10 }, { 185, 0x22 }, { 187, 0x00 },
	};
	u8 changed[sizeof(mjpeg_420)];
	u8 partial[sizeof(jpeg_420) - 77];
	u8 empty[sizeof(mjpeg_420) + 4];
	struct ums9117_jpeg_frame frame;
	unsigned int index;
	int failed = 0;

	for (index = 0; index < ARRAY_SIZE(unsupported); ++index) {
		memcpy(changed, mjpeg_420, sizeof(changed));
		changed[unsupported[index].offset] =
			unsupported[index].selector;
		failed |= expect(
			ums9117_jpeg_parse(changed, sizeof(changed), &frame) ==
				-EOPNOTSUPP,
			"MJPEG without DHT accepted unsupported table selectors");
	}
	/* Keep only the first DHT segment: Y-DC cannot supply the other tables. */
	memcpy(partial, jpeg_420, 200);
	memcpy(partial + 200, jpeg_420 + 277, sizeof(jpeg_420) - 277);
	failed |= expect(ums9117_jpeg_parse(partial, sizeof(partial), &frame) ==
				 -EINVAL,
			 "partial DHT was completed with defaults");
	/* An empty DHT is malformed, not an abbreviated frame. */
	memcpy(empty, mjpeg_420, 177);
	empty[177] = 0xff;
	empty[178] = 0xc4;
	empty[179] = 0;
	empty[180] = 2;
	memcpy(empty + 181, mjpeg_420 + 177, sizeof(mjpeg_420) - 177);
	failed |= expect(ums9117_jpeg_parse(empty, sizeof(empty), &frame) ==
				 -EINVAL,
			 "empty DHT was completed with defaults");
	return failed;
}

static int expect_mutation_error(size_t offset, u8 value, int error,
				 const char *message)
{
	u8 copy[sizeof(jpeg_420)];
	struct ums9117_jpeg_frame frame;

	memcpy(copy, jpeg_420, sizeof(copy));
	copy[offset] = value;
	return expect(ums9117_jpeg_parse(copy, sizeof(copy), &frame) == error,
		      message);
}

static int test_semantic_rejections(void)
{
	u8 trailing[sizeof(jpeg_420) + 1];
	struct ums9117_jpeg_frame frame;
	int failed = 0;

	failed |= expect_mutation_error(159, 0xc1, -EOPNOTSUPP,
					"SOF1 was accepted as baseline");
	failed |= expect_mutation_error(169, 0x11, -EOPNOTSUPP,
					"unsupported 4:4:4 was accepted");
	failed |= expect_mutation_error(165, 0x08, -E2BIG,
					"oversized width was accepted");
	failed |= expect_mutation_error(25, 0x00, -EINVAL,
					"zero quantizer was accepted");
	failed |= expect_mutation_error(
		176, 0x00, -EINVAL, "different Cb/Cr quantizers were accepted");
	failed |= expect_mutation_error(
		293, 0x00, -EINVAL,
		"different Cb/Cr Huffman tables were accepted");
	failed |= expect_mutation_error(
		227, 0x03, -EINVAL, "oversubscribed Huffman tree was accepted");
	failed |= expect_mutation_error(198, 0x0c, -EINVAL,
					"out-of-domain DC symbol was accepted");
	failed |= expect_mutation_error(221, 0x0b, -EINVAL,
					"out-of-domain AC symbol was accepted");
	failed |= expect_mutation_error(
		294, 0x01, -EOPNOTSUPP,
		"non-baseline scan parameters were accepted");
	failed |= expect_mutation_error(310, 0xd1, -EINVAL,
					"wrong restart sequence was accepted");
	failed |= expect(ums9117_jpeg_parse(jpeg_420, sizeof(jpeg_420) - 2,
					    &frame) == -EINVAL,
			 "missing EOI was accepted");
	memcpy(trailing, jpeg_420, sizeof(jpeg_420));
	trailing[sizeof(jpeg_420)] = 0;
	failed |= expect(ums9117_jpeg_parse(trailing, sizeof(trailing),
					    &frame) == -EOPNOTSUPP,
			 "bytes after EOI were accepted");
	return failed;
}

static bool all_zero(const void *buffer, size_t size)
{
	const u8 *bytes = buffer;
	size_t index;

	for (index = 0; index < size; ++index)
		if (bytes[index])
			return false;
	return true;
}

static int test_encode_geometry(void)
{
	static const struct {
		u32 width;
		u32 height;
		u32 mcu_x;
		u32 mcu_y;
		u32 restart_interval;
	} accepted[] = {
		{ 16, 8, 1, 1, 2 },
		{ 1200, 32, 75, 4, 150 },
		{ 2048, 2048, 128, 256, 256 },
	};
	static const u32 rejected[][2] = {
		{ 0, 8 },  { 16, 0 },	{ 15, 8 },
		{ 16, 7 }, { 2064, 8 }, { 16, 2056 },
	};
	struct ums9117_jpeg_encode_config config;
	unsigned int index;
	int failed = 0;

	for (index = 0; index < ARRAY_SIZE(accepted); ++index) {
		int ret = ums9117_jpeg_build_encode_config(
			accepted[index].width, accepted[index].height, 85,
			&config);

		failed |=
			expect(ret == 0, "valid encode geometry was rejected");
		if (ret)
			continue;
		failed |= expect(
			config.width == accepted[index].width &&
				config.height == accepted[index].height &&
				config.mcu_x == accepted[index].mcu_x &&
				config.mcu_y == accepted[index].mcu_y &&
				config.restart_interval ==
					accepted[index].restart_interval,
			"encode geometry or strip DRI changed");
	}
	for (index = 0; index < ARRAY_SIZE(rejected); ++index) {
		int ret;

		memset(&config, 0xa5, sizeof(config));
		ret = ums9117_jpeg_build_encode_config(
			rejected[index][0], rejected[index][1], 85, &config);
		failed |= expect(ret == -EINVAL,
				 "invalid encode geometry was accepted");
		failed |= expect(all_zero(&config, sizeof(config)),
				 "failed encode config retained partial state");
	}
	failed |= expect(ums9117_jpeg_build_encode_config(16, 8, 85, NULL) ==
				 -EINVAL,
			 "NULL encode config was accepted");
	failed |= expect(
		ums9117_jpeg_build_encode_config(16, 16, 0, &config) == -EINVAL,
		"encode quality below the supported range was accepted");
	failed |=
		expect(ums9117_jpeg_build_encode_config(16, 16, 101, &config) ==
			       -EINVAL,
		       "encode quality above the supported range was accepted");
	return failed;
}

static void print_bytes(const char *name, const u8 *data, size_t size)
{
	size_t index;

	printf("%s ", name);
	for (index = 0; index < size; ++index)
		printf("%02x", (unsigned int)data[index]);
	putchar('\n');
}

static void print_words(const char *name, const u32 *data, size_t count)
{
	size_t index;

	printf("%s", name);
	for (index = 0; index < count; ++index)
		printf(" %08x", (unsigned int)data[index]);
	putchar('\n');
}

static int dump_encode_config(u32 quality)
{
	struct ums9117_jpeg_encode_config config;
	int ret = ums9117_jpeg_build_encode_config(1200, 32, quality, &config);

	if (ret) {
		fprintf(stderr, "encode config builder returned %d\n", ret);
		return 1;
	}
	printf("meta %u %u %u %u %u\n", (unsigned int)config.width,
	       (unsigned int)config.height, (unsigned int)config.mcu_x,
	       (unsigned int)config.mcu_y,
	       (unsigned int)config.restart_interval);
	print_bytes("quant", (const u8 *)config.quant, sizeof(config.quant));
	print_bytes("header", config.header, sizeof(config.header));
	print_words("qbuf", config.qbuf_words, ARRAY_SIZE(config.qbuf_words));
	print_words("ac", config.ac_lut_words, ARRAY_SIZE(config.ac_lut_words));
	return ferror(stdout) ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--dump-encode"))
		return dump_encode_config(85);
	if (argc == 2 && !strcmp(argv[1], "--dump-quality-1"))
		return dump_encode_config(1);
	if (argc == 2 && !strcmp(argv[1], "--dump-quality-50"))
		return dump_encode_config(50);
	if (argc == 2 && !strcmp(argv[1], "--dump-quality-100"))
		return dump_encode_config(100);
	if (argc != 1) {
		fprintf(stderr, "usage: %s [--dump-encode]\n", argv[0]);
		return 1;
	}
	return test_valid_420() | test_valid_422() | test_mjpeg_without_dht() |
	       test_mjpeg_dht_rejections() | test_semantic_rejections() |
	       test_encode_geometry();
}
