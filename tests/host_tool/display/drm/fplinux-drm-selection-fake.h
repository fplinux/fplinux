/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_DRM_SELECTION_FAKE_H
#define FPLINUX_DRM_SELECTION_FAKE_H

#include <stdbool.h>
#include <stdint.h>

bool fplinux_drm_selection_setup(const char *mode);
bool fplinux_drm_selection_open(const char *path, int flags, int *result);
const char *fplinux_drm_selection_path(void);
uint32_t fplinux_drm_selection_format(void);
bool fplinux_drm_selection_check(int error);

#endif
