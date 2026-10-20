/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026, FPLinux contributors. */
#include <arch_helpers.h>
#include <assert.h>
#include <bl31.h>
#include <bl_common.h>
#include <console.h>
#include <gicv2.h>
#include <mmio.h>
#include <platform.h>
#include <platform_def.h>
#include <xlat_tables.h>

static entry_point_info_t ns_image;
static const unsigned char power_domains[] = { 1, PLATFORM_CORE_COUNT };

/* Preserve secure interrupt routing during the RAM handoff. */
static const unsigned int secure_interrupts[] = { 29, 9,  10, 11, 12, 13,
						  15, 56, 57, 8,  14 };

static const gicv2_driver_data_t gic_data = {
	.gicd_base = QIN_GICD_PHYS,
	.gicc_base = QIN_GICC_PHYS,
	.g0_interrupt_num = ARRAY_SIZE(secure_interrupts),
	.g0_interrupt_array = secure_interrupts,
};

static const mmap_region_t device_regions[] = {
	MAP_REGION_FLAT(0x12000000, 0x10000, MT_DEVICE | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(QIN_COUNTER_PHYS, 0x1000,
			MT_DEVICE | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(0x40200000, 0x400000, MT_DEVICE | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(QIN_AP_APB_PHYS, 0x500000,
			MT_DEVICE | MT_RW | MT_SECURE),
	{ 0 },
};

static void cold_power_setup(void)
{
	unsigned int core;
	uint32_t chip_id;
	uintptr_t warm_entry = (uintptr_t)bl31_warm_entrypoint;

	for (core = 0; core < PLATFORM_CORE_COUNT; core++)
		mmio_write_32(QIN_AON_APB_PHYS + 0x528 + core * 4,
			      (uint32_t)(warm_entry >> 2));
	for (core = 0; core < 4; core++)
		mmio_setbits_32(QIN_AP_APB_PHYS, 1U << (19 + core));
	mmio_clrbits_32(QIN_AON_APB_PHYS + 0xb0, 1U << 15);
	mmio_setbits_32(QIN_PMU_PHYS + 0x230, 1U);
	mmio_setbits_32(QIN_PMU_PHYS + 0xc8, 0xfU);
	mmio_setbits_32(QIN_AON_APB_PHYS + 0x590, 1U);
	mmio_setbits_32(QIN_PMU_PHYS + 0x230, 1U << 3);

	chip_id = mmio_read_32(QIN_AON_APB_PHYS + 0xfc) & 0xfU;
	if (chip_id == 2)
		mmio_setbits_32(QIN_AON_APB_PHYS + 0x140, 1U << 16);
	else
		mmio_clrbits_32(QIN_AON_APB_PHYS + 0x140, 1U << 16);

	/* Preserve the board's cold clock state at these two offsets. */
	mmio_write_32(QIN_PRE_DIV_CLK_PHYS + 0x274, 0);
	mmio_write_32(QIN_PRE_DIV_CLK_PHYS + 0x290, 1);
	mmio_write_32(QIN_PMU_PHYS + 0x1018, 1U << 24);
	mmio_write_32(QIN_PMU_PHYS + 0x2018, 1U << 25);
}

void bl31_early_platform_setup(bl31_params_t *from_bl2, void *plat_params)
{
	(void)from_bl2;
	(void)plat_params;
	SET_PARAM_HEAD(&ns_image, PARAM_EP, VERSION_1, NON_SECURE);
	ns_image.pc = QIN_NS_ENTRY;
	ns_image.spsr = SPSR_MODE32(MODE32_hyp, SPSR_T_ARM, SPSR_E_LITTLE,
				    DAIF_ABT_BIT | DAIF_IRQ_BIT | DAIF_FIQ_BIT);
}

void bl31_plat_arch_setup(void)
{
	uintptr_t ro_start = (uintptr_t)&__RO_START__;
	uintptr_t ro_end = (uintptr_t)&__RO_END__;
	uintptr_t coherent_start = (uintptr_t)&__COHERENT_RAM_START__;
	uintptr_t coherent_end = (uintptr_t)&__COHERENT_RAM_END__;

	mmap_add_region(BL31_BASE, BL31_BASE, BL31_LIMIT - BL31_BASE,
			MT_MEMORY | MT_RW | MT_SECURE);
	mmap_add_region(ro_start, ro_start, ro_end - ro_start,
			MT_MEMORY | MT_RO | MT_SECURE);
	mmap_add_region(coherent_start, coherent_start,
			coherent_end - coherent_start,
			MT_DEVICE | MT_RW | MT_SECURE);
	mmap_add(device_regions);
	init_xlat_tables();
	enable_mmu_el3(0);
}

void bl31_platform_setup(void)
{
	gicv2_driver_init(&gic_data);
	gicv2_distif_init();
	gicv2_pcpu_distif_init();
	gicv2_cpuif_enable();
	mmio_write_32(QIN_COUNTER_PHYS, 1);
	cold_power_setup();
}

entry_point_info_t *bl31_plat_get_next_image_ep_info(uint32_t security_state)
{
	assert(sec_state_is_valid(security_state));
	return security_state == NON_SECURE ? &ns_image : NULL;
}

unsigned int plat_get_syscnt_freq2(void)
{
	return QIN_COUNTER_HZ;
}

const unsigned char *plat_get_power_domain_tree_desc(void)
{
	return power_domains;
}

int plat_core_pos_by_mpidr(u_register_t mpidr)
{
	u_register_t affinity = mpidr & MPIDR_AFFINITY_MASK;

	if (affinity >= PLATFORM_CORE_COUNT)
		return -1;
	return (int)affinity;
}

/* This stage has no configured UART console. */
int console_putc(int character)
{
	(void)character;
	return -1;
}

void console_uninit(void)
{
}
