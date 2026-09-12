/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_MEDIA_V4L2_JPEG_H
#define FPLINUX_TEST_MEDIA_V4L2_JPEG_H

#include <linux/types.h>

#define V4L2_JPEG_MAX_COMPONENTS 4
#define V4L2_JPEG_MAX_TABLES 4
#define V4L2_JPEG_PIXELS_IN_BLOCK 64
#define V4L2_JPEG_REF_HT_DC_LEN 28
#define V4L2_JPEG_REF_HT_AC_LEN 178

enum v4l2_jpeg_chroma_subsampling {
	V4L2_JPEG_CHROMA_SUBSAMPLING_444 = 0,
	V4L2_JPEG_CHROMA_SUBSAMPLING_422 = 1,
	V4L2_JPEG_CHROMA_SUBSAMPLING_420 = 2,
	V4L2_JPEG_CHROMA_SUBSAMPLING_411 = 3,
	V4L2_JPEG_CHROMA_SUBSAMPLING_410 = 4,
	V4L2_JPEG_CHROMA_SUBSAMPLING_GRAY = 5,
};

enum v4l2_jpeg_app14_tf {
	V4L2_JPEG_APP14_TF_CMYK_RGB = 0,
	V4L2_JPEG_APP14_TF_YCBCR = 1,
	V4L2_JPEG_APP14_TF_YCCK = 2,
	V4L2_JPEG_APP14_TF_UNKNOWN = -1,
};

struct v4l2_jpeg_reference {
	u8 *start;
	size_t length;
};

struct v4l2_jpeg_frame_component_spec {
	u8 component_identifier;
	u8 horizontal_sampling_factor;
	u8 vertical_sampling_factor;
	u8 quantization_table_selector;
};

struct v4l2_jpeg_frame_header {
	u16 height;
	u16 width;
	u8 precision;
	u8 num_components;
	struct v4l2_jpeg_frame_component_spec
		component[V4L2_JPEG_MAX_COMPONENTS];
	enum v4l2_jpeg_chroma_subsampling subsampling;
};

struct v4l2_jpeg_scan_component_spec {
	u8 component_selector;
	u8 dc_entropy_coding_table_selector;
	u8 ac_entropy_coding_table_selector;
};

struct v4l2_jpeg_scan_header {
	u8 num_components;
	struct v4l2_jpeg_scan_component_spec component[V4L2_JPEG_MAX_COMPONENTS];
};

struct v4l2_jpeg_header {
	struct v4l2_jpeg_reference sof;
	struct v4l2_jpeg_reference sos;
	unsigned int num_dht;
	struct v4l2_jpeg_reference dht[V4L2_JPEG_MAX_TABLES];
	unsigned int num_dqt;
	struct v4l2_jpeg_reference dqt[V4L2_JPEG_MAX_TABLES];
	struct v4l2_jpeg_frame_header frame;
	struct v4l2_jpeg_scan_header *scan;
	struct v4l2_jpeg_reference *quantization_tables;
	struct v4l2_jpeg_reference *huffman_tables;
	u16 restart_interval;
	size_t ecs_offset;
	enum v4l2_jpeg_app14_tf app14_tf;
};

extern const u8 v4l2_jpeg_zigzag_scan_index[V4L2_JPEG_PIXELS_IN_BLOCK];
extern const u8 v4l2_jpeg_ref_table_luma_qt[V4L2_JPEG_PIXELS_IN_BLOCK];
extern const u8 v4l2_jpeg_ref_table_chroma_qt[V4L2_JPEG_PIXELS_IN_BLOCK];
extern const u8 v4l2_jpeg_ref_table_luma_dc_ht[V4L2_JPEG_REF_HT_DC_LEN];
extern const u8 v4l2_jpeg_ref_table_luma_ac_ht[V4L2_JPEG_REF_HT_AC_LEN];
extern const u8 v4l2_jpeg_ref_table_chroma_dc_ht[V4L2_JPEG_REF_HT_DC_LEN];
extern const u8 v4l2_jpeg_ref_table_chroma_ac_ht[V4L2_JPEG_REF_HT_AC_LEN];

int v4l2_jpeg_parse_header(void *buf, size_t len, struct v4l2_jpeg_header *out);

#endif
