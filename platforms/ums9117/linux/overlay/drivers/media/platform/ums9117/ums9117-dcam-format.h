/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_DCAM_FORMAT_H
#define FPLINUX_UMS9117_DCAM_FORMAT_H

#include <linux/types.h>

struct ums9117_dcam_format {
	u32 y_bytes;
	u32 source_size;
	u32 capture_start;
	u32 capture_end;
	u32 capture_control;
};

int ums9117_dcam_format_configure(u32 width, u32 height, u32 mbus_code,
				  unsigned int serial_data_width,
				  struct ums9117_dcam_format *format);

#endif
