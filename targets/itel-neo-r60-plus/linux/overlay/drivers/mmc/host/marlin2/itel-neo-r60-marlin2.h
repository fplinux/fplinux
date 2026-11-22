/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ITEL_NEO_R60_MARLIN2_H
#define ITEL_NEO_R60_MARLIN2_H

#include "marlin2-radio.h"

extern const struct marlin2_radio_board itel_neo_r60_marlin2;

#define ITEL_NEO_R60_MARLIN2_MATCH                      \
	{ .compatible = "fplinux,itel-neo-r60-marlin2", \
	  .data = &itel_neo_r60_marlin2 }

#endif
