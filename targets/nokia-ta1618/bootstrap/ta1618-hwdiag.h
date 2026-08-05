/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TA1618_HWDIAG_H
#define TA1618_HWDIAG_H

/*
 * Shared, fixed-layout handoff record.  Both producers and consumers are
 * ARM32 little-endian code, so unsigned int is deliberately used as the
 * 32-bit wire type without pulling Linux headers into the bootstrap.
 */
#define TA1618_DIAG_MAGIC 0x54413138u /* "TA18" */
#define TA1618_DIAG_VERSION 1u
#define TA1618_DIAG_BYTES 0x00010000u
#define TA1618_GIC_WORDS 8u
#define TA1618_USB_SPI 55u
#define TA1618_USB_INTID (TA1618_USB_SPI + 32u)

#define TA1618_DIAG_F_GIC_SNAPSHOTS (1u << 0)
#define TA1618_DIAG_F_USB_TX_PROBE (1u << 1)
#define TA1618_DIAG_F_DMA5_QUIESCED (1u << 2)
#define TA1618_DIAG_F_DMA21_QUIESCED (1u << 3)
#define TA1618_DIAG_F_SOFT_DISCONNECT (1u << 4)

struct ta1618_gic_snapshot {
	unsigned int ctlr;
	unsigned int typer;
	unsigned int iidr;
	unsigned int words;
	unsigned int group[TA1618_GIC_WORDS];
	unsigned int enabled[TA1618_GIC_WORDS];
	unsigned int pending[TA1618_GIC_WORDS];
	unsigned int active[TA1618_GIC_WORDS];
};

struct ta1618_musb_snapshot {
	unsigned int faddr;
	unsigned int power;
	unsigned int intrtx;
	unsigned int intrrx;
	unsigned int intrtxe;
	unsigned int intrrxe;
	unsigned int intrusb;
	unsigned int intrusbe;
	unsigned int frame;
	unsigned int index;
	unsigned int testmode;
	unsigned int devctl;
	unsigned int babble_ctl;
	unsigned int txfifosz;
	unsigned int rxfifosz;
	unsigned int txfifoadd;
	unsigned int rxfifoadd;
	unsigned int hwvers;
	unsigned int epinfo;
	unsigned int raminfo;
	unsigned int linkinfo;
	unsigned int vplen;
	unsigned int dma_raw_status;
	unsigned int dma_mask_status;
	unsigned int dma_req_status;
	unsigned int dma_enable_status;
	unsigned int dma_debug_status;
	unsigned int dma_channel5[8];
	unsigned int dma_channel21[8];
};

struct ta1618_boot_diag {
	unsigned int magic;
	unsigned int version;
	unsigned int stage;
	unsigned int error;
	unsigned int ram_bytes;
	unsigned int zimage_addr;
	unsigned int zimage_bytes;
	unsigned int dtb_addr;
	unsigned int dtb_bytes;
	unsigned int framebuffer_addr;
	unsigned int framebuffer_bytes;
	char last_message[84];

	unsigned int struct_bytes;
	unsigned int flags;
	unsigned int usb_spi;
	unsigned int usb_intid;
	unsigned int usb_pending_before;
	unsigned int usb_pending_after_clear;
	unsigned int usb_pending_after_tx;
	unsigned int dma5_quiesce_status;
	unsigned int dma21_quiesce_status;

	struct ta1618_gic_snapshot gic_before_clear;
	struct ta1618_gic_snapshot gic_after_clear;
	struct ta1618_gic_snapshot gic_after_usb_tx;
	struct ta1618_musb_snapshot musb_before_usb_tx;
	struct ta1618_musb_snapshot musb_after_usb_tx;
	struct ta1618_musb_snapshot musb_after_quiesce;
};

#endif
