// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only handoff diagnostics for the Nokia TA-1618 RAM-boot target.
 *
 * The bootstrap records a before/after USB transaction and quiesces the
 * inherited Spreadtrum DMA channels before Linux starts.  This file only
 * publishes that immutable record and safe, non-clear-on-read live fields.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "ta1618-hwdiag.h"

#define TA1618_DIAG_BASE 0x83ef0000u
#define TA1618_GICD_BASE 0x12001000u
#define TA1618_MUSB_BASE 0x20200000u

#define GICD_CTLR 0x000u
#define GICD_TYPER 0x004u
#define GICD_IIDR 0x008u
#define GICD_ISENABLER 0x100u
#define GICD_ISPENDR 0x200u

#define MUSB_FADDR 0x000u
#define MUSB_POWER 0x001u
#define MUSB_FRAME 0x00cu
#define MUSB_INDEX 0x00eu
#define MUSB_DEVCTL 0x060u
#define MUSB_HWVERS 0x06cu
#define MUSB_EPINFO 0x078u
#define MUSB_RAMINFO 0x079u
#define MUSB_LINKINFO 0x07au
#define MUSB_VPLEN 0x07bu
#define MUSB_DMA_REQ_STATUS 0x1010u
#define MUSB_DMA_EN_STATUS 0x1014u
#define MUSB_DMA_DEBUG_STATUS 0x1018u

static struct ta1618_boot_diag boot_diag;
static bool boot_diag_valid;

static void show_gic_snapshot(struct seq_file *m, const char *name,
			      const struct ta1618_gic_snapshot *snapshot)
{
	unsigned int i;

	seq_printf(m, "gic.%s.ctlr=0x%08x\n", name, snapshot->ctlr);
	seq_printf(m, "gic.%s.typer=0x%08x\n", name, snapshot->typer);
	seq_printf(m, "gic.%s.iidr=0x%08x\n", name, snapshot->iidr);
	seq_printf(m, "gic.%s.words=%u\n", name, snapshot->words);
	for (i = 0; i < snapshot->words && i < TA1618_GIC_WORDS; ++i)
		seq_printf(
			m,
			"gic.%s.bank%u=group:0x%08x enabled:0x%08x pending:0x%08x active:0x%08x\n",
			name, i, snapshot->group[i], snapshot->enabled[i],
			snapshot->pending[i], snapshot->active[i]);
}

static void show_musb_snapshot(struct seq_file *m, const char *name,
			       const struct ta1618_musb_snapshot *snapshot)
{
	unsigned int i;

	seq_printf(
		m,
		"musb.%s.core=faddr:0x%02x power:0x%02x devctl:0x%02x frame:0x%04x index:%u hwvers:0x%04x\n",
		name, snapshot->faddr, snapshot->power, snapshot->devctl,
		snapshot->frame, snapshot->index, snapshot->hwvers);
	seq_printf(
		m,
		"musb.%s.cap=epinfo:0x%02x raminfo:0x%02x linkinfo:0x%02x vplen:0x%02x\n",
		name, snapshot->epinfo, snapshot->raminfo, snapshot->linkinfo,
		snapshot->vplen);
	seq_printf(
		m,
		"musb.%s.irq=usb:0x%02x/0x%02x tx:0x%04x/0x%04x rx:0x%04x/0x%04x\n",
		name, snapshot->intrusb, snapshot->intrusbe, snapshot->intrtx,
		snapshot->intrtxe, snapshot->intrrx, snapshot->intrrxe);
	seq_printf(
		m,
		"musb.%s.dma=raw:0x%08x masked:0x%08x req:0x%08x enabled:0x%08x debug:0x%08x\n",
		name, snapshot->dma_raw_status, snapshot->dma_mask_status,
		snapshot->dma_req_status, snapshot->dma_enable_status,
		snapshot->dma_debug_status);
	for (i = 0; i < 8; ++i)
		seq_printf(m, "musb.%s.dma5.r%u=0x%08x\n", name, i,
			   snapshot->dma_channel5[i]);
	for (i = 0; i < 8; ++i)
		seq_printf(m, "musb.%s.dma21.r%u=0x%08x\n", name, i,
			   snapshot->dma_channel21[i]);
}

static void show_live_safe_registers(struct seq_file *m)
{
	void __iomem *gic;
	void __iomem *musb;
	u32 typer;
	unsigned int words;
	unsigned int i;

	gic = ioremap(TA1618_GICD_BASE, 0x1000);
	musb = ioremap(TA1618_MUSB_BASE, 0x2000);
	if (!gic || !musb) {
		seq_puts(m, "live.error=ioremap-failed\n");
		goto out;
	}

	typer = readl_relaxed(gic + GICD_TYPER);
	words = min_t(unsigned int, (typer & 0x1f) + 1, TA1618_GIC_WORDS);
	seq_printf(m, "live.gic.ctlr=0x%08x\n", readl_relaxed(gic + GICD_CTLR));
	seq_printf(m, "live.gic.typer=0x%08x\n", typer);
	seq_printf(m, "live.gic.iidr=0x%08x\n", readl_relaxed(gic + GICD_IIDR));
	for (i = 0; i < words; ++i)
		seq_printf(m, "live.gic.bank%u=enabled:0x%08x pending:0x%08x\n",
			   i, readl_relaxed(gic + GICD_ISENABLER + i * 4),
			   readl_relaxed(gic + GICD_ISPENDR + i * 4));

	/*
	 * Do not read MUSB interrupt status here: several implementations use
	 * clear-on-read semantics and Linux USB may already own the device.
	 */
	seq_printf(
		m,
		"live.musb.safe=faddr:0x%02x power:0x%02x devctl:0x%02x frame:0x%04x index:%u hwvers:0x%04x\n",
		readb_relaxed(musb + MUSB_FADDR),
		readb_relaxed(musb + MUSB_POWER),
		readb_relaxed(musb + MUSB_DEVCTL),
		readw_relaxed(musb + MUSB_FRAME),
		readb_relaxed(musb + MUSB_INDEX),
		readw_relaxed(musb + MUSB_HWVERS));
	seq_printf(
		m,
		"live.musb.cap=epinfo:0x%02x raminfo:0x%02x linkinfo:0x%02x vplen:0x%02x\n",
		readb_relaxed(musb + MUSB_EPINFO),
		readb_relaxed(musb + MUSB_RAMINFO),
		readb_relaxed(musb + MUSB_LINKINFO),
		readb_relaxed(musb + MUSB_VPLEN));
	seq_printf(m, "live.musb.dma=req:0x%08x enabled:0x%08x debug:0x%08x\n",
		   readl_relaxed(musb + MUSB_DMA_REQ_STATUS),
		   readl_relaxed(musb + MUSB_DMA_EN_STATUS),
		   readl_relaxed(musb + MUSB_DMA_DEBUG_STATUS));

out:
	if (musb)
		iounmap(musb);
	if (gic)
		iounmap(gic);
}

static int ta1618_hwdiag_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "boot.valid=%u\n", boot_diag_valid);
	if (!boot_diag_valid)
		return 0;

	seq_printf(m, "boot.version=%u\n", boot_diag.version);
	seq_printf(m, "boot.struct_bytes=%u\n", boot_diag.struct_bytes);
	seq_printf(m, "boot.flags=0x%08x\n", boot_diag.flags);
	seq_printf(m, "boot.stage=%u\n", boot_diag.stage);
	seq_printf(m, "boot.error=%u\n", boot_diag.error);
	seq_printf(m, "boot.last_message=%.*s\n",
		   (int)sizeof(boot_diag.last_message), boot_diag.last_message);
	seq_printf(m, "usb.spi=%u\n", boot_diag.usb_spi);
	seq_printf(m, "usb.intid=%u\n", boot_diag.usb_intid);
	seq_printf(m, "usb.pending.before=%u\n", boot_diag.usb_pending_before);
	seq_printf(m, "usb.pending.after_clear=%u\n",
		   boot_diag.usb_pending_after_clear);
	seq_printf(m, "usb.pending.after_tx=%u\n",
		   boot_diag.usb_pending_after_tx);
	seq_printf(m, "usb.dma5_quiesce=0x%08x\n",
		   boot_diag.dma5_quiesce_status);
	seq_printf(m, "usb.dma21_quiesce=0x%08x\n",
		   boot_diag.dma21_quiesce_status);

	show_gic_snapshot(m, "before_clear", &boot_diag.gic_before_clear);
	show_gic_snapshot(m, "after_clear", &boot_diag.gic_after_clear);
	show_gic_snapshot(m, "after_usb_tx", &boot_diag.gic_after_usb_tx);
	show_musb_snapshot(m, "before_usb_tx", &boot_diag.musb_before_usb_tx);
	show_musb_snapshot(m, "after_usb_tx", &boot_diag.musb_after_usb_tx);
	show_musb_snapshot(m, "after_quiesce", &boot_diag.musb_after_quiesce);
	show_live_safe_registers(m);
	return 0;
}

static int __init ta1618_hwdiag_init(void)
{
	void __iomem *record;
	struct proc_dir_entry *entry;

	record = ioremap(TA1618_DIAG_BASE, TA1618_DIAG_BYTES);
	if (!record)
		return -ENOMEM;
	memcpy_fromio(&boot_diag, record, sizeof(boot_diag));
	iounmap(record);

	boot_diag_valid = boot_diag.magic == TA1618_DIAG_MAGIC &&
			  boot_diag.version == TA1618_DIAG_VERSION &&
			  boot_diag.struct_bytes == sizeof(boot_diag) &&
			  boot_diag.usb_spi == TA1618_USB_SPI &&
			  boot_diag.usb_intid == TA1618_USB_INTID;
	if (!boot_diag_valid) {
		pr_warn("TA1618 hwdiag: invalid handoff record magic=%08x version=%u bytes=%u\n",
			boot_diag.magic, boot_diag.version,
			boot_diag.struct_bytes);
		return 0;
	}

	entry = proc_create_single("ta1618-hwdiag", 0444, NULL,
				   ta1618_hwdiag_show);
	if (!entry)
		return -ENOMEM;

	pr_info("TA1618 hwdiag: USB SPI %u pending %u -> %u -> %u, DMA5=%#x DMA21=%#x\n",
		boot_diag.usb_spi, boot_diag.usb_pending_before,
		boot_diag.usb_pending_after_clear,
		boot_diag.usb_pending_after_tx, boot_diag.dma5_quiesce_status,
		boot_diag.dma21_quiesce_status);
	return 0;
}
device_initcall(ta1618_hwdiag_init);
