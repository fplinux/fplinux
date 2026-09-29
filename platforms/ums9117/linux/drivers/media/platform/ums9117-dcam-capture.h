/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_DCAM_CAPTURE_H
#define FPLINUX_UMS9117_DCAM_CAPTURE_H

struct device;
struct mutex;
struct ums9117_dcam_capture;
struct ums9117_jpeg_hw;
struct v4l2_device;

struct ums9117_dcam_capture *
ums9117_dcam_capture_create(struct device *dev, struct v4l2_device *v4l2,
			    struct ums9117_jpeg_hw *hw, struct mutex *lock);
void ums9117_dcam_capture_destroy(struct ums9117_dcam_capture *capture);

#endif
