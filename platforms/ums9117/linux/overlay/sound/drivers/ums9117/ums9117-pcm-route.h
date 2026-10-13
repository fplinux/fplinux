/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_PCM_ROUTE_H
#define FPLINUX_UMS9117_PCM_ROUTE_H

#include "ums9117-sc2720-codec.h"

#define UMS9117_PCM_OUTPUTS_BOTH \
	(UMS9117_SC2720_OUTPUT_HEADPHONES | UMS9117_SC2720_OUTPUT_SPEAKER)

/* Headfree gains remain selected by the requested set, independently of this route. */
static inline unsigned int
ums9117_pcm_effective_outputs(unsigned int requested,
			      bool headfree_headphones_only)
{
	if (headfree_headphones_only && requested == UMS9117_PCM_OUTPUTS_BOTH)
		return UMS9117_SC2720_OUTPUT_HEADPHONES;
	return requested;
}

#endif
