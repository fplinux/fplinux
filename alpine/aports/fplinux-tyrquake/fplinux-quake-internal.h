/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_QUAKE_INTERNAL_H
#define FPLINUX_QUAKE_INTERNAL_H

void fplinux_quake_remove_runtime(const char *runtime);
void fplinux_quake_write_controls(const char *directory);
struct fplinux_drm_session;
struct fplinux_drm_session *fplinux_quake_display_session(void);

#endif
