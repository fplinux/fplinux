/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_JPEG_HW_H
#define FPLINUX_UMS9117_JPEG_HW_H

#include <linux/types.h>

struct platform_device;
struct ums9117_jpeg_encode_config;
struct ums9117_jpeg_frame;
struct ums9117_jpeg_hw;

enum ums9117_jpeg_scale_profile {
	UMS9117_JPEG_SCALE_640X480_TO_320X240,
	UMS9117_JPEG_SCALE_320X240_TO_160X120,
	UMS9117_JPEG_SCALE_PROFILE_COUNT,
};

struct ums9117_jpeg_hw *ums9117_jpeg_hw_create(struct platform_device *pdev);
void ums9117_jpeg_hw_destroy(struct ums9117_jpeg_hw *hw);
void ums9117_jpeg_hw_prepare(struct ums9117_jpeg_hw *hw);
void ums9117_jpeg_hw_cancel(struct ums9117_jpeg_hw *hw);
int ums9117_jpeg_hw_decode(struct ums9117_jpeg_hw *hw,
			   const struct ums9117_jpeg_frame *frame,
			   const u8 *entropy, u8 *output[2],
			   const size_t capacity[2], unsigned int factor);
int ums9117_jpeg_hw_encode(struct ums9117_jpeg_hw *hw,
			   const struct ums9117_jpeg_encode_config *config,
			   const u8 *input[2], const size_t input_size[2],
			   u8 *output, size_t capacity, size_t *written);
int ums9117_jpeg_hw_scale(struct ums9117_jpeg_hw *hw,
			  enum ums9117_jpeg_scale_profile profile,
			  const u8 *input[2], const size_t input_size[2],
			  u8 *output[2], const size_t capacity[2]);
bool ums9117_jpeg_hw_failed(struct ums9117_jpeg_hw *hw);

#endif
