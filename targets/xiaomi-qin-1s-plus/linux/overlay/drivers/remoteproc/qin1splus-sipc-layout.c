// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2015 Spreadtrum Communications Inc. */
#include <linux/build_bug.h>
#include <linux/kernel.h>
#include <linux/sizes.h>

#include "qin1splus-sipc-layout.h"

static const struct qin_sipc_owner qin_owners[QIN_SIPC_OWNER_COUNT] = {
	{ .channel = 5, .count = 1, .tx_size = 0x8000, .rx_size = 0x30000 },
	{ .channel = 11, .count = 1, .tx_size = 0x400, .rx_size = 0x400 },
	{ .channel = 12, .count = 1, .tx_size = 0x400, .rx_size = 0x400 },
	{ .channel = 4, .count = 16, .tx_size = 0x1000, .rx_size = 0x1000 },
	{ .channel = 7,
	  .block = true,
	  .count = 4,
	  .tx_size = 2048,
	  .rx_count = 16,
	  .rx_size = 2048 },
	{ .channel = 8,
	  .block = true,
	  .count = 64,
	  .tx_size = 1664,
	  .rx_count = 128,
	  .rx_size = 1664 },
	{ .channel = 9, .block = true, .count = 64, .tx_size = 1664 },
};

int qin_sipc_layout(struct qin_sipc_owner *owners)
{
	u32 cursor = 0;
	int i;

	static_assert(sizeof(struct qin_sbuf_ring) == 32);
	static_assert(sizeof(struct qin_swcn_header) == 96);
	static_assert(sizeof(struct qin_swcn_desc) == 8);
	for (i = 0; i < QIN_SIPC_OWNER_COUNT; i++) {
		struct qin_sipc_owner *owner = &owners[i];

		*owner = qin_owners[i];
		if (owner->block)
			owner->bytes = sizeof(struct qin_swcn_header) +
				       owner->count * owner->tx_size +
				       owner->rx_count * owner->rx_size +
				       2 * (owner->count + owner->rx_count) *
					       sizeof(struct qin_swcn_desc);
		else
			owner->bytes =
				sizeof(__le32) +
				owner->count *
					(sizeof(struct qin_sbuf_ring) +
					 owner->tx_size + owner->rx_size);
		if (owner->bytes > QIN_SIPC_SMEM_BYTES - cursor)
			return -ENOSPC;
		owner->offset = cursor;
		/* Fixed 4 KiB granularity matches the current ARM32 pool. */
		cursor += ALIGN(owner->bytes, SZ_4K);
	}
	return 0;
}

static void init_sbuf(void __iomem *base, const struct qin_sipc_owner *owner)
{
	u32 cp_base = QIN_SIPC_CP_BASE + owner->offset;
	u32 header_bytes = 4 + owner->count * sizeof(struct qin_sbuf_ring);
	u32 i;

	writel(owner->count, base);
	for (i = 0; i < owner->count; i++) {
		struct qin_sbuf_ring __iomem *ring =
			base + 4 + i * sizeof(*ring);
		u32 tx = cp_base + header_bytes +
			 i * (owner->tx_size + owner->rx_size);

		writel(tx, &ring->tx_addr);
		writel(owner->tx_size, &ring->tx_size);
		writel(tx + owner->tx_size, &ring->rx_addr);
		writel(owner->rx_size, &ring->rx_size);
	}
}

static void fill_pool(void __iomem *base, u32 desc_offset, u32 cp_address,
		      u32 count, u32 bytes)
{
	struct qin_swcn_desc __iomem *descs = base + desc_offset;
	u32 i;

	for (i = 0; i < count; i++) {
		writel(cp_address + i * bytes, &descs[i].address);
		writel(bytes, &descs[i].length);
	}
}

static void init_swcn(void __iomem *base, const struct qin_sipc_owner *owner)
{
	struct qin_swcn_header __iomem *header = base;
	u32 cp_base = QIN_SIPC_CP_BASE + owner->offset;
	u32 tx = cp_base + sizeof(*header);
	u32 rx = tx + owner->count * owner->tx_size;
	u32 ring_tx = rx + owner->rx_count * owner->rx_size;
	u32 ring_rx = ring_tx + owner->count * sizeof(struct qin_swcn_desc);
	u32 pool_tx = ring_rx + owner->rx_count * sizeof(struct qin_swcn_desc);
	u32 pool_rx = pool_tx + owner->count * sizeof(struct qin_swcn_desc);
	struct qin_swcn_ring __iomem *ring = &header->ring;
	struct qin_swcn_ring __iomem *pool = &header->pool;

	writel(tx, &ring->tx_addr);
	writel(owner->count, &ring->tx_count);
	writel(owner->tx_size, &ring->tx_size);
	writel(ring_tx, &ring->tx_desc);
	writel(rx, &ring->rx_addr);
	writel(owner->rx_count, &ring->rx_count);
	writel(owner->rx_size, &ring->rx_size);
	writel(ring_rx, &ring->rx_desc);
	memcpy_toio(pool,
		    &((struct qin_swcn_ring){
			    .tx_addr = cpu_to_le32(tx),
			    .tx_count = cpu_to_le32(owner->count),
			    .tx_size = cpu_to_le32(owner->tx_size),
			    .tx_desc = cpu_to_le32(pool_tx),
			    .rx_addr = cpu_to_le32(rx),
			    .rx_count = cpu_to_le32(owner->rx_count),
			    .rx_size = cpu_to_le32(owner->rx_size),
			    .rx_desc = cpu_to_le32(pool_rx),
		    }),
		    sizeof(*pool));
	fill_pool(base, pool_tx - cp_base, tx, owner->count, owner->tx_size);
	fill_pool(base, pool_rx - cp_base, rx, owner->rx_count, owner->rx_size);
	/* Publish descriptors before offering their producer counters. */
	wmb();
	writel(owner->count, &pool->tx_write);
	writel(owner->rx_count, &pool->rx_write);
}

void qin_sipc_init_owner(void __iomem *base, const struct qin_sipc_owner *owner)
{
	base += owner->offset;
	memset_io(base, 0, owner->bytes);
	if (owner->block)
		init_swcn(base, owner);
	else
		init_sbuf(base, owner);
}

void qin_sipc_recover_owner(void __iomem *base,
			    const struct qin_sipc_owner *owner)
{
	u32 cp_base = QIN_SIPC_CP_BASE + owner->offset;
	u32 i;

	base += owner->offset;
	if (owner->block) {
		struct qin_swcn_header __iomem *header = base;
		struct qin_swcn_ring __iomem *ring = &header->ring;
		struct qin_swcn_ring __iomem *pool = &header->pool;
		u32 tx_desc, rx_desc;
		u32 tx_write = readl(&pool->tx_write);
		u32 rx_read = readl(&pool->rx_read);

		/* Use the AP's fixed layout, never a CP-modified address. */
		tx_desc = sizeof(*header) + owner->count * owner->tx_size +
			  owner->rx_count * owner->rx_size +
			  (owner->count + owner->rx_count) * 8;
		rx_desc = tx_desc + owner->count * 8;
		writel(readl(&ring->tx_read), &ring->tx_write);
		writel(readl(&ring->rx_write), &ring->rx_read);
		writel(tx_write, &pool->tx_read);
		writel(rx_read, &pool->rx_write);
		fill_pool(base, tx_desc, cp_base + sizeof(*header),
			  owner->count, owner->tx_size);
		fill_pool(base, rx_desc,
			  cp_base + sizeof(*header) +
				  owner->count * owner->tx_size,
			  owner->rx_count, owner->rx_size);
		/* Rebuilt descriptors precede publication of their free counts. */
		wmb();
		writel(tx_write + owner->count, &pool->tx_write);
		writel(rx_read + owner->rx_count, &pool->rx_write);
	} else {
		for (i = 0; i < owner->count; i++) {
			struct qin_sbuf_ring __iomem *ring =
				base + 4 + i * sizeof(*ring);

			writel(readl(&ring->rx_write), &ring->rx_read);
		}
	}
	/* Recovery pointer updates must precede the subsequent OPEN reply. */
	wmb();
}

static bool bt_owner_valid(const struct qin_sipc_owner *owner)
{
	return owner->channel == 4 && !owner->block && owner->count == 16 &&
	       owner->tx_size == 4096 && owner->rx_size == 4096 &&
	       owner->offset <= QIN_SIPC_SMEM_BYTES &&
	       owner->bytes <= QIN_SIPC_SMEM_BYTES - owner->offset &&
	       owner->bytes == 4 + 16 * (32 + 8192);
}

int qin_sipc_bt_idle(void __iomem *base, const struct qin_sipc_owner *owner)
{
	struct qin_sbuf_ring __iomem *tx, *rx;
	u32 pending_tx, pending_rx;

	if (!bt_owner_valid(owner))
		return -EINVAL;
	base += owner->offset;
	tx = base + 4 + 11 * sizeof(*tx);
	rx = base + 4 + 10 * sizeof(*rx);
	pending_tx = readl(&tx->tx_write) - readl(&tx->tx_read);
	pending_rx = readl(&rx->rx_write) - readl(&rx->rx_read);
	if (pending_tx > 4096 || pending_rx > 4096)
		return -EPROTO;
	return pending_tx || pending_rx ? -EBUSY : 0;
}

int qin_sipc_bt_write_ring(void __iomem *base,
			   const struct qin_sipc_owner *owner, const void *data,
			   size_t bytes, bool *notify)
{
	struct qin_sbuf_ring __iomem *ring;
	void __iomem *payload;
	u32 read, written, used, first;

	*notify = false;
	if (!bt_owner_valid(owner) || !bytes || bytes > 4096)
		return -EINVAL;
	base += owner->offset;
	ring = base + 4 + 11 * sizeof(*ring);
	payload = base + 4 + 16 * sizeof(*ring) + 11 * 8192;
	written = readl(&ring->tx_write);
	read = readl(&ring->tx_read);
	used = written - read;
	if (used > 4096)
		return -EPROTO;
	if (bytes > 4096 - used)
		return -EAGAIN;
	/* The peer's read counter precedes reuse of its former bytes. */
	rmb();
	first = min_t(size_t, bytes, 4096 - written % 4096);
	memcpy_toio(payload + written % 4096, data, first);
	memcpy_toio(payload, data + first, bytes - first);
	/* Payload bytes must reach the peer before its producer counter. */
	wmb();
	writel(written + bytes, &ring->tx_write);
	/* Match the peer protocol: notify only an empty-to-nonempty edge. */
	*notify = written + (u32)bytes - readl(&ring->tx_read) == bytes;
	return bytes;
}

int qin_sipc_bt_read_ring(void __iomem *base,
			  const struct qin_sipc_owner *owner, void *data,
			  size_t capacity, bool *notify)
{
	struct qin_sbuf_ring __iomem *ring;
	void __iomem *payload;
	u32 read, written, available, bytes, first;

	*notify = false;
	if (!bt_owner_valid(owner) || !capacity)
		return -EINVAL;
	base += owner->offset;
	ring = base + 4 + 10 * sizeof(*ring);
	payload = base + 4 + 16 * sizeof(*ring) + 10 * 8192 + 4096;
	read = readl(&ring->rx_read);
	written = readl(&ring->rx_write);
	available = written - read;
	if (available > 4096)
		return -EPROTO;
	if (!available)
		return -EAGAIN;
	/* Observe the peer's payload only after its producer counter. */
	rmb();
	bytes = min_t(size_t, available, capacity);
	first = min(bytes, 4096 - read % 4096);
	memcpy_fromio(data, payload + read % 4096, first);
	memcpy_fromio(data + first, payload, bytes - first);
	/* Finish copying before granting the peer permission to overwrite. */
	mb();
	writel(read + bytes, &ring->rx_read);
	*notify = readl(&ring->rx_write) - (read + bytes) == 4096 - bytes;
	return bytes;
}
