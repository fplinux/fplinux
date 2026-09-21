/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_JPEG_CODEC_H
#define FPLINUX_UMS9117_JPEG_CODEC_H

#include <linux/types.h>

#define UMS9117_JPEG_MAX_INPUT_SIZE (1024U * 1024U)
#define UMS9117_JPEG_MAX_DIMENSION 2048U

#define UMS9117_JPEG_QUANT_WORDS 64U
#define UMS9117_JPEG_HUFF_META_WORDS 66U
#define UMS9117_JPEG_HUFF_VALUE_WORDS 162U
#define UMS9117_JPEG_BLOCK_COEFFICIENTS 64U

#define UMS9117_JPEG_ENCODE_QUALITY 85U
#define UMS9117_JPEG_ENCODE_STRIP_ROWS 16U
#define UMS9117_JPEG_ENCODE_HEADER_SIZE 629U
#define UMS9117_JPEG_ENCODE_QBUF_WORDS 64U
#define UMS9117_JPEG_ENCODE_AC_LUT_WORDS 162U

enum ums9117_jpeg_mcu_format {
	UMS9117_JPEG_MCU_420 = 0,
	UMS9117_JPEG_MCU_422 = 3,
};

struct ums9117_jpeg_frame {
	u32 width;
	u32 height;
	u32 padded_width;
	u32 padded_height;
	u32 mcu_x;
	u32 mcu_y;
	u32 mcu_format;
	u32 vertical_subsampling;
	u32 restart_interval;
	u32 entropy_offset;
	u32 entropy_length;
	u32 quant_words[UMS9117_JPEG_QUANT_WORDS];
	u32 huff_meta_words[UMS9117_JPEG_HUFF_META_WORDS];
	u32 huff_value_words[UMS9117_JPEG_HUFF_VALUE_WORDS];
};

struct ums9117_jpeg_encode_config {
	u32 width;
	u32 height;
	u32 mcu_x;
	u32 mcu_y;
	u32 restart_interval;
	u8 quant[2][UMS9117_JPEG_BLOCK_COEFFICIENTS];
	u8 header[UMS9117_JPEG_ENCODE_HEADER_SIZE];
	u32 qbuf_words[UMS9117_JPEG_ENCODE_QBUF_WORDS];
	u32 ac_lut_words[UMS9117_JPEG_ENCODE_AC_LUT_WORDS];
};

int ums9117_jpeg_parse(const void *data, size_t length,
		       struct ums9117_jpeg_frame *frame);
int ums9117_jpeg_build_encode_config(u32 width, u32 height,
				     struct ums9117_jpeg_encode_config *config);

#endif
