/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_QUAKE_INTERNAL_H
#define FPLINUX_QUAKE_INTERNAL_H

#define FPLINUX_QUAKE_SOFTWARE_ENGINE "/usr/bin/tyr-quake"
#define FPLINUX_QUAKE_GPU_ENGINE "/usr/bin/tyr-glquake"
#define FPLINUX_QUAKE_FALLBACK_ARGUMENT "-fplinux-software-fallback"

void fplinux_quake_remove_runtime(const char *runtime);
void fplinux_quake_write_controls(const char *directory);
struct fplinux_drm_session;
struct fplinux_drm_session *fplinux_quake_display_session(void);
int fplinux_quake_video_exit_status(void);

#endif
