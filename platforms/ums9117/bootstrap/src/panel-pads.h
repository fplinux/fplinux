/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_BOOTSTRAP_PANEL_PADS_H
#define FPLINUX_UMS9117_BOOTSTRAP_PANEL_PADS_H

#include <stddef.h>
#include <stdint.h>

#define UMS9117_BOOTSTRAP_PANEL_PADS_PHYS 0x402a0000U

struct ums9117_bootstrap_panel_pad {
	uint32_t offset;
	uint32_t value;
};

static inline void ums9117_bootstrap_prepare_panel_pads(
	const struct ums9117_bootstrap_panel_pad *pads, size_t count)
{
	size_t index;

	for (index = 0; index < count; index++) {
		uintptr_t address =
			UMS9117_BOOTSTRAP_PANEL_PADS_PHYS + pads[index].offset;

		*(volatile uint32_t *)address = pads[index].value;
	}
}

#endif
