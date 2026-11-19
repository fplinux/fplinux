// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_PM_FILES_H
#define FPLINUX_PM_FILES_H

struct fplinux_volume_snapshot;

void fplinux_pm_files_volumes(const struct fplinux_volume_snapshot *snapshot);
void fplinux_pm_files_cleanup(void);

#endif
