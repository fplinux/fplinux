/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QIN1SPLUS_SIPC_LAYOUT_H
#define QIN1SPLUS_SIPC_LAYOUT_H

#include <linux/io.h>
#include <linux/types.h>

#define QIN_SIPC_OWNER_COUNT 7
#define QIN_SIPC_SMEM_BYTES 0x100000
#define QIN_SIPC_CP_BASE 0x180000

struct qin_sipc_owner {
	u8 channel;
	bool block;
	u32 count;
	u32 tx_size;
	u32 rx_count;
	u32 rx_size;
	u32 offset;
	u32 bytes;
};

/* All words in the shared headers and descriptors are little endian. */
struct qin_sbuf_ring {
	__le32 tx_addr;
	__le32 tx_size;
	__le32 tx_read;
	__le32 tx_write;
	__le32 rx_addr;
	__le32 rx_size;
	__le32 rx_read;
	__le32 rx_write;
};

struct qin_swcn_ring {
	__le32 tx_addr;
	__le32 tx_count;
	__le32 tx_size;
	__le32 tx_desc;
	__le32 tx_read;
	__le32 tx_write;
	__le32 rx_addr;
	__le32 rx_count;
	__le32 rx_size;
	__le32 rx_desc;
	__le32 rx_read;
	__le32 rx_write;
};

struct qin_swcn_header {
	struct qin_swcn_ring ring;
	struct qin_swcn_ring pool;
};

struct qin_swcn_desc {
	__le32 address;
	__le32 length;
};

int qin_sipc_layout(struct qin_sipc_owner *owners);
void qin_sipc_init_owner(void __iomem *base,
			 const struct qin_sipc_owner *owner);
/* The caller must revoke and quiesce any data borrower before recovery. */
void qin_sipc_recover_owner(void __iomem *base,
			    const struct qin_sipc_owner *owner);
/* Fixed channel-4 rings; caller serializes AP access and owns the session. */
int qin_sipc_bt_write_ring(void __iomem *base,
			   const struct qin_sipc_owner *owner, const void *data,
			   size_t bytes, bool *notify);
int qin_sipc_bt_read_ring(void __iomem *base,
			  const struct qin_sipc_owner *owner, void *data,
			  size_t capacity, bool *notify);
int qin_sipc_bt_idle(void __iomem *base, const struct qin_sipc_owner *owner);

#endif
