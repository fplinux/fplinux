// SPDX-License-Identifier: GPL-2.0-only
#include <linux/compiler_attributes.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/media-bus-format.h>

#include "ums9117-dcam-format.h"

#define DCAM_CAP_CCIR656 (1U << 0)
#define DCAM_CAP_HSYNC_HIGH (1U << 3)
#define DCAM_CAP_TWO_BIT (2U << 9)
#define DCAM_CAP_ONE_BIT (3U << 9)
#define DCAM_G0_YUYV_WIDTH 640U
#define DCAM_G0_COUNTED_HEIGHT 476U
#define DCAM_G0_FULL_FRAME_HEIGHT 480U
#define DCAM_G0_LINE_COUNT_BYTES 2U

int ums9117_dcam_format_configure(u32 width, u32 height,
				  u32 mbus_code __maybe_unused,
				  unsigned int serial_data_width,
				  struct ums9117_dcam_format *format)
{
	u32 control;
	u32 capture_start = 0;

	switch (serial_data_width) {
#if IS_ENABLED(CONFIG_VIDEO_UMS9117_DCAM_PARALLEL)
	case 0:
		if (mbus_code != MEDIA_BUS_FMT_YUYV8_2X8 ||
		    !((width == 800 && height == 600) ||
		      (width == 1600 && height == 1200)))
			return -EINVAL;
		control = DCAM_CAP_HSYNC_HIGH;
		break;
#endif
#if IS_ENABLED(CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0_ONE_BIT)
	case 1:
		if (mbus_code != MEDIA_BUS_FMT_YUYV8_2X8 || width != 240 ||
		    height != 320)
			return -EINVAL;
		control = DCAM_CAP_CCIR656 | DCAM_CAP_ONE_BIT;
		break;
#endif
#if IS_ENABLED(CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0_TWO_BIT)
	case 2:
		if (mbus_code != MEDIA_BUS_FMT_YUYV8_2X8 ||
		    width != DCAM_G0_YUYV_WIDTH)
			return -EINVAL;
		control = DCAM_CAP_CCIR656 | DCAM_CAP_TWO_BIT;
		/* The fixed frame sizes select distinct serial line encodings. */
		if (height == DCAM_G0_COUNTED_HEIGHT) {
			control |= DCAM_CAP_HSYNC_HIGH;
			/* Exclude the serial line count from the pixel window. */
			capture_start = DCAM_G0_LINE_COUNT_BYTES;
		} else if (height != DCAM_G0_FULL_FRAME_HEIGHT) {
			return -EINVAL;
		}
		break;
#endif
	default:
		return -EINVAL;
	}
	format->y_bytes = width * height;
	format->source_size = (height << 16) | width;
	format->capture_start = capture_start;
	format->capture_end = ((height - 1) << 16) |
			      (capture_start + 2 * width - 1);
	format->capture_control = control;
	return 0;
}
