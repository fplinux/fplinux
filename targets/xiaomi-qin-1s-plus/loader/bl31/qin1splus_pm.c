/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026, FPLinux contributors. */
#include <arch_helpers.h>
#include <assert.h>
#include <bakery_lock.h>
#include <bl31.h>
#include <debug.h>
#include <gicv2.h>
#include <mmio.h>
#include <platform.h>
#include <platform_def.h>
#include <psci.h>

static bakery_lock_t power_lock __attribute__((section("tzfw_coherent_mem")));

static void cpu_standby(plat_local_state_t state)
{
	assert(state == PLAT_MAX_RET_STATE);
	dsb();
	wfi();
}

static int power_domain_on(u_register_t mpidr)
{
	unsigned int core = (unsigned int)plat_core_pos_by_mpidr(mpidr);
	unsigned int shift = (core + 1) * 5;
	uint32_t state;

	assert(core < PLATFORM_CORE_COUNT);
	do {
		bakery_lock_get(&power_lock);
		state = (mmio_read_32(QIN_PMU_PHYS + 0xbc) >> shift) & 0x1fU;
		bakery_lock_release(&power_lock);
	} while (state != 7);

	bakery_lock_get(&power_lock);
	mmio_setbits_32(QIN_PMU_PHYS + 0x254, 1U);
	mmio_clrbits_32(QIN_PMU_PHYS + 4 + core * 4, 1U << 24);
	mmio_clrbits_32(QIN_PMU_PHYS + 4 + core * 4, 1U << 25);
	mmio_clrbits_32(QIN_PMU_PHYS + 0x174, 1U << (8 + core));
	mmio_clrbits_32(QIN_PMU_PHYS + 0x348 + core * 4, 1U);
	bakery_lock_release(&power_lock);
	return PSCI_E_SUCCESS;
}

static void power_domain_off(const psci_power_state_t *target_state)
{
	unsigned int core = plat_my_core_pos();
	unsigned int controller;
	uint32_t enabled[4];

	assert(target_state->pwr_domain_state[0] == PLAT_MAX_OFF_STATE);
	assert(target_state->pwr_domain_state[1] == PSCI_LOCAL_STATE_RUN);
	bakery_lock_get(&power_lock);
	mmio_setbits_32(QIN_PMU_PHYS + 0x1bc + core * 4, 1U);
	bakery_lock_release(&power_lock);
	gicv2_cpuif_disable();

	bakery_lock_get(&power_lock);
	for (controller = 0; controller < 4; controller++)
		mmio_write_32(QIN_INTC0_PHYS + controller * QIN_INTC_STRIDE +
				      0xc,
			      0xffffffffU);
	for (controller = 0; controller < 4; controller++)
		enabled[controller] =
			mmio_read_32(QIN_GICD_PHYS + 0x104 + controller * 4);
	for (controller = 0; controller < 4; controller++)
		mmio_write_32(QIN_INTC0_PHYS + controller * QIN_INTC_STRIDE + 8,
			      enabled[controller]);
	mmio_setbits_32(QIN_PMU_PHYS + 4 + core * 4, 1U << 24);
	mmio_clrbits_32(QIN_PMU_PHYS + 4 + core * 4, 1U << 27);
	mmio_clrbits_32(QIN_PMU_PHYS + 0x174, 1U << (8 + core));
	mmio_setbits_32(QIN_PMU_PHYS + 0x348 + core * 4, 1U);
	mmio_setbits_32(QIN_PMU_PHYS + 0x1bc + core * 4, 1U);
	bakery_lock_release(&power_lock);
}

static void power_domain_on_finish(const psci_power_state_t *target_state)
{
	assert(target_state->pwr_domain_state[0] == PLAT_MAX_OFF_STATE);
	assert(target_state->pwr_domain_state[1] == PSCI_LOCAL_STATE_RUN);
	gicv2_pcpu_distif_init();
	gicv2_cpuif_enable();
}

static int validate_power_state(unsigned int power_state,
				psci_power_state_t *requested)
{
	/* Only CPU standby is supported; power-down suspend is unavailable. */
	if (power_state != 0)
		return PSCI_E_INVALID_PARAMS;
	requested->pwr_domain_state[0] = PLAT_MAX_RET_STATE;
	return PSCI_E_SUCCESS;
}

static int validate_ns_entrypoint(uintptr_t entrypoint)
{
	if (entrypoint < QIN_DRAM_BASE || entrypoint >= QIN_DRAM_LIMIT ||
	    (entrypoint >= BL31_BASE && entrypoint < QIN_SECURE_LIMIT))
		return PSCI_E_INVALID_ADDRESS;
	return PSCI_E_SUCCESS;
}

/* v1.3 requires both hooks to expose the CPU_SUSPEND standby fast path. */
static void unreachable_power_down_suspend(const psci_power_state_t *state)
{
	(void)state;
	panic();
}

static const plat_psci_ops_t power_ops = {
	.cpu_standby = cpu_standby,
	.pwr_domain_on = power_domain_on,
	.pwr_domain_off = power_domain_off,
	.pwr_domain_suspend = unreachable_power_down_suspend,
	.pwr_domain_on_finish = power_domain_on_finish,
	.pwr_domain_suspend_finish = unreachable_power_down_suspend,
	.validate_power_state = validate_power_state,
	.validate_ns_entrypoint = validate_ns_entrypoint,
};

int plat_setup_psci_ops(uintptr_t secure_entrypoint,
			const plat_psci_ops_t **operations)
{
	assert(secure_entrypoint >= BL31_BASE &&
	       secure_entrypoint < BL31_LIMIT);
	assert((secure_entrypoint & 3U) == 0);
	assert(secure_entrypoint == (uintptr_t)bl31_warm_entrypoint);
	bakery_lock_init(&power_lock);
	dsb();
	*operations = &power_ops;
	return PSCI_E_SUCCESS;
}
