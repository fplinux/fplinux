// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host oracle for UMS9117 table packing and validation.  The small
 * v4l2_jpeg_parse_header double below represents the external Linux helper's
 * result for the two fixed JPEG vectors; this does not test that helper.
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
	bool has_dri;
	size_t sos_offset;
	const u8 *sof;
	const u8 *sos;
	unsigned int index;

	if (length < 297)
		return -EINVAL;
	has_dri = data[277] == 0xff && data[278] == 0xdd;
	sos_offset = has_dri ? 285 : 279;
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
	header->huffman_tables[0].start = data + 182;
	header->huffman_tables[0].length = huffman_length(data + 182);
	header->huffman_tables[2].start = data + 205;
	header->huffman_tables[2].length = huffman_length(data + 205);
	header->huffman_tables[1].start = data + 227;
	header->huffman_tables[1].length = huffman_length(data + 227);
	header->huffman_tables[3].start = data + 251;
	header->huffman_tables[3].length = huffman_length(data + 251);
	header->restart_interval = has_dri ? get_be16(data + 281) : 0;
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
	 * These sentinels are derived from the fixture DQT/DHT bytes and the
	 * documented UMS9117 coefficient permutation and Huffman lanes.  They
	 * must not be regenerated from the codec implementation under test.
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
			accepted[index].width, accepted[index].height, &config);

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
			rejected[index][0], rejected[index][1], &config);
		failed |= expect(ret == -EINVAL,
				 "invalid encode geometry was accepted");
		failed |= expect(all_zero(&config, sizeof(config)),
				 "failed encode config retained partial state");
	}
	failed |=
		expect(ums9117_jpeg_build_encode_config(16, 8, NULL) == -EINVAL,
		       "NULL encode config was accepted");
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

static int dump_encode_config(void)
{
	struct ums9117_jpeg_encode_config config;
	int ret = ums9117_jpeg_build_encode_config(1200, 32, &config);

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
		return dump_encode_config();
	if (argc != 1) {
		fprintf(stderr, "usage: %s [--dump-encode]\n", argv[0]);
		return 1;
	}
	return test_valid_420() | test_valid_422() |
	       test_semantic_rejections() | test_encode_geometry();
}
