/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BQ2830L_MARLIN2_H
#define BQ2830L_MARLIN2_H

#include "marlin2-radio.h"

extern const struct marlin2_radio_board bq2830l_marlin2;

#define BQ2830L_MARLIN2_MATCH \
	{ .compatible = "bq,2830l-radio", .data = &bq2830l_marlin2 }

#endif
