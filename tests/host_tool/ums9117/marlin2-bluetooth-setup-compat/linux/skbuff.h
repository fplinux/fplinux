/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_SETUP_HOST_SKBUFF_H
#define FPLINUX_MARLIN2_SETUP_HOST_SKBUFF_H

#include "../../bluetooth-compat/linux/skbuff.h"

static inline void skb_queue_tail(struct sk_buff_head *queue,
				  struct sk_buff *skb)
{
	__skb_queue_tail(queue, skb);
}
static inline void skb_queue_purge(struct sk_buff_head *queue)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(queue)))
		kfree_skb(skb);
}
static inline void *skb_put(struct sk_buff *skb, size_t bytes)
{
	void *tail = skb->data + skb->len;

	assert(bytes <= skb->capacity - skb->len);
	skb->len += bytes;
	return tail;
}
static inline void skb_pull(struct sk_buff *skb, size_t bytes)
{
	assert(bytes <= skb->len);
	skb->len -= bytes;
	memmove(skb->data, skb->data + bytes, skb->len);
}

#endif
