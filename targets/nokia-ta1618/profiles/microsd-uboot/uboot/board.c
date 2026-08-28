// SPDX-License-Identifier: GPL-2.0-only
#include <asm/global_data.h>
#include <hang.h>
#include <init.h>

#include <fplinux-boot-layout.h>

#include "stage0-handoff.h"
#include "ta1618-mmc.h"

#define TA1618_RAM_BASE FPLINUX_BOOT_LAYOUT_RAM_BASE_PHYS
#define TA1618_RAM_BYTES FPLINUX_BOOT_LAYOUT_RAM_REQUIRED_BYTES
#define TA1618_RESIDENT_LIMIT FPLINUX_BOOT_LAYOUT_RESIDENT_LIMIT_PHYS
#define TA1618_UBOOT_RAM_TOP FPLINUX_BOOT_LAYOUT_ZIMAGE_PHYS

DECLARE_GLOBAL_DATA_PTR;

uint32_t fplinux_uboot_handoff_phys = FPLINUX_BOOT_INVALID_PHYS;

static bool range_valid(uint32_t start, uint32_t end)
{
	return start >= TA1618_RAM_BASE && end > start &&
	       end <= TA1618_RESIDENT_LIMIT;
}

const struct fplinux_uboot_handoff *fplinux_uboot_handoff(void)
{
	const struct fplinux_uboot_handoff *handoff;
	uint32_t end;

	if (fplinux_uboot_handoff_phys & (FPLINUX_BOOT_ABI_ALIGNMENT - 1U))
		return NULL;
	if (fplinux_uboot_handoff_phys < TA1618_RAM_BASE ||
	    fplinux_uboot_handoff_phys >
		    TA1618_RESIDENT_LIMIT - sizeof(*handoff))
		return NULL;
	handoff = (const void *)(uintptr_t)fplinux_uboot_handoff_phys;
	if (handoff->magic != FPLINUX_UBOOT_HANDOFF_MAGIC ||
	    handoff->size != sizeof(*handoff))
		return NULL;
	if (!range_valid(handoff->resident_start, handoff->resident_end))
		return NULL;
	end = fplinux_uboot_handoff_phys + sizeof(*handoff);
	if (end < fplinux_uboot_handoff_phys ||
	    fplinux_uboot_handoff_phys < handoff->resident_start ||
	    end > handoff->resident_end)
		return NULL;
	return handoff;
}

const struct fplinux_stage0_ops *fplinux_stage0_ops(void)
{
	const struct fplinux_uboot_handoff *handoff = fplinux_uboot_handoff();
	const struct fplinux_stage0_ops *ops;
	uint32_t end;

	if (!handoff ||
	    (handoff->stage0_ops_phys & (FPLINUX_BOOT_ABI_ALIGNMENT - 1U)))
		return NULL;
	if (handoff->resident_end - handoff->resident_start < sizeof(*ops))
		return NULL;
	if (handoff->stage0_ops_phys < handoff->resident_start ||
	    handoff->stage0_ops_phys > handoff->resident_end - sizeof(*ops))
		return NULL;
	ops = (const void *)(uintptr_t)handoff->stage0_ops_phys;
	end = handoff->stage0_ops_phys + sizeof(*ops);
	if (end < handoff->stage0_ops_phys || end > handoff->resident_end)
		return NULL;
	if (ops->magic != FPLINUX_STAGE0_OPS_MAGIC ||
	    ops->size != sizeof(*ops) || !ops->checkpoint || !ops->fail ||
	    !ops->finalize_and_boot || !ops->console_putc || !ops->timer_ms)
		return NULL;
	return ops;
}

int arch_cpu_init(void)
{
	if (!fplinux_stage0_ops())
		hang();
	return 0;
}

int board_init(void)
{
	return 0;
}

int dram_init(void)
{
	gd->ram_size = TA1618_RAM_BYTES;
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = TA1618_RAM_BASE;
	gd->bd->bi_dram[0].size = TA1618_RAM_BYTES;
	return 0;
}

phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	(void)total_size;
	return TA1618_UBOOT_RAM_TOP;
}

void reset_cpu(void)
{
	const struct fplinux_stage0_ops *ops = fplinux_stage0_ops();

	(void)ta1618_mmc_release();
	if (ops)
		ops->fail(FPLINUX_STAGE0_FAILURE_UBOOT, 0U);
	hang();
}
