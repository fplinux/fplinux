/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026, FPLinux contributors. */
#include <arch_helpers.h>
#include <assert.h>
#include <bl31/bl31.h>
#include <common/bl_common.h>
#include <drivers/arm/gic_common.h>
#include <drivers/arm/gicv2.h>
#include <lib/mmio.h>
#include <lib/xlat_tables/xlat_tables.h>
#include <plat/common/platform.h>
#include <platform_def.h>

static entry_point_info_t ns_image;
static const unsigned char power_domains[] = { 1, PLATFORM_CORE_COUNT };

#define QIN_SECURE_IRQ(id, cfg) \
	INTR_PROP_DESC(id, GIC_HIGHEST_SEC_PRIORITY, GICV2_INTR_GROUP0, cfg)

/* Secure SGIs have no configurable trigger. */
static const interrupt_prop_t secure_interrupts[] = {
	QIN_SECURE_IRQ(29, GICV2_PPI_CFG_KEEP),
	QIN_SECURE_IRQ(9, GIC_INTR_CFG_EDGE),
	QIN_SECURE_IRQ(10, GIC_INTR_CFG_EDGE),
	QIN_SECURE_IRQ(11, GIC_INTR_CFG_EDGE),
	QIN_SECURE_IRQ(12, GIC_INTR_CFG_EDGE),
	QIN_SECURE_IRQ(13, GIC_INTR_CFG_EDGE),
	QIN_SECURE_IRQ(15, GIC_INTR_CFG_EDGE),
	QIN_SECURE_IRQ(56, GIC_INTR_CFG_LEVEL),
	QIN_SECURE_IRQ(57, GIC_INTR_CFG_LEVEL),
	QIN_SECURE_IRQ(8, GIC_INTR_CFG_EDGE),
	QIN_SECURE_IRQ(14, GIC_INTR_CFG_EDGE),
};

static const gicv2_driver_data_t gic_data = {
	.gicd_base = QIN_GICD_PHYS,
	.gicc_base = QIN_GICC_PHYS,
	.interrupt_props_num = ARRAY_SIZE(secure_interrupts),
	.interrupt_props = secure_interrupts,
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

void bl31_early_platform_setup2(u_register_t arg0, u_register_t arg1,
				u_register_t arg2, u_register_t arg3)
{
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	SET_PARAM_HEAD(&ns_image, PARAM_EP, VERSION_1, NON_SECURE);
	ns_image.pc = QIN_NS_ENTRY;
	ns_image.spsr = SPSR_MODE32(MODE32_hyp, SPSR_T_ARM, SPSR_E_LITTLE,
				    DAIF_ABT_BIT | DAIF_IRQ_BIT | DAIF_FIQ_BIT);
}

void bl31_plat_arch_setup(void)
{
	mmap_add_region(BL31_BASE, BL31_BASE, BL31_LIMIT - BL31_BASE,
			MT_MEMORY | MT_RW | MT_SECURE);
	mmap_add_region(BL_CODE_BASE, BL_CODE_BASE, BL_CODE_END - BL_CODE_BASE,
			MT_CODE | MT_SECURE);
	mmap_add_region(BL_COHERENT_RAM_BASE, BL_COHERENT_RAM_BASE,
			BL_COHERENT_RAM_END - BL_COHERENT_RAM_BASE,
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
