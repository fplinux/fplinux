/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_BOOTSTRAP_EARLY_INIT_H
#define FPLINUX_UMS9117_BOOTSTRAP_EARLY_INIT_H

#include <stddef.h>
#include <stdint.h>

#define UMS9117_BOOTSTRAP_PIN_GATE_SET_PHYS 0x402e1000U
#define UMS9117_BOOTSTRAP_PIN_APB_GATE_SET_PHYS 0x402e10b0U
#define UMS9117_BOOTSTRAP_PIN_CONTROL2_PHYS 0x402a0018U
#define UMS9117_BOOTSTRAP_ANALOG_BASE_PHYS 0x40608000U

struct ums9117_bootstrap_early_write {
	uint32_t address;
	uint32_t value;
};

void adi_write(uint32_t address, uint32_t value);

static inline void ums9117_bootstrap_early_reg_write(uint32_t address,
						     uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)address = value;
}

static inline void
ums9117_bootstrap_early_init(const struct ums9117_bootstrap_early_write *writes,
			     size_t count)
{
	volatile uint32_t *control2 = (volatile uint32_t *)(uintptr_t)
		UMS9117_BOOTSTRAP_PIN_CONTROL2_PHYS;
	size_t index;

	ums9117_bootstrap_early_reg_write(UMS9117_BOOTSTRAP_PIN_GATE_SET_PHYS,
					  1U << 20);
	ums9117_bootstrap_early_reg_write(
		UMS9117_BOOTSTRAP_PIN_APB_GATE_SET_PHYS, 1U << 12);
	for (index = 0; index < count; index++) {
		uint32_t address = writes[index].address;

		if (address - UMS9117_BOOTSTRAP_ANALOG_BASE_PHYS < 0x1000U)
			adi_write(address, writes[index].value & 0xffffU);
		else
			ums9117_bootstrap_early_reg_write(address,
							  writes[index].value);
	}
	*control2 &= 0xe0000000U;
}

#endif
