// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef FPLINUX_QUAKE_STATS_H
#define FPLINUX_QUAKE_STATS_H

#include <stdbool.h>
#include <stdint.h>

void fplinux_quake_stats_init(void);
bool fplinux_quake_stats_enabled(void);
void fplinux_quake_stats_draw(void);
void fplinux_quake_stats_gpu_sample(uint64_t elapsed_ns);

#endif
