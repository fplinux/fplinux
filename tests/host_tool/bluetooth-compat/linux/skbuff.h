/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_SKBUFF_H
#define BLUETOOTH_HOST_SKBUFF_H

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct sk_buff {
	struct sk_buff *next;
	size_t len;
	size_t capacity;
	u8 type;
	u8 data[];
};
struct sk_buff_head {
	spinlock_t lock;
	struct sk_buff *head;
	struct sk_buff *tail;
};

static inline void skb_queue_head_init(struct sk_buff_head *queue)
{
	memset(queue, 0, sizeof(*queue));
}
static inline bool skb_queue_empty(const struct sk_buff_head *queue)
{
	return !queue->head;
}
static inline void __skb_queue_tail(struct sk_buff_head *queue,
				    struct sk_buff *skb)
{
	assert(!skb->next);
	if (queue->tail)
		queue->tail->next = skb;
	else
		queue->head = skb;
	queue->tail = skb;
}
static inline struct sk_buff *__skb_dequeue(struct sk_buff_head *queue)
{
	struct sk_buff *skb = queue->head;

	if (!skb)
		return NULL;
	queue->head = skb->next;
	if (!queue->head)
		queue->tail = NULL;
	skb->next = NULL;
	return skb;
}
static inline struct sk_buff *skb_dequeue(struct sk_buff_head *queue)
{
	struct sk_buff *skb;

	mutex_lock(&queue->lock);
	skb = __skb_dequeue(queue);
	mutex_unlock(&queue->lock);
	return skb;
}
static inline void kfree_skb(struct sk_buff *skb)
{
	free(skb);
}
static inline int skb_copy_bits(const struct sk_buff *skb, size_t offset,
				void *output, size_t bytes)
{
	assert(offset <= skb->len && bytes <= skb->len - offset);
	memcpy(output, skb->data + offset, bytes);
	return 0;
}
static inline void skb_put_data(struct sk_buff *skb, const void *data,
				size_t bytes)
{
	assert(bytes <= skb->capacity - skb->len);
	memcpy(skb->data + skb->len, data, bytes);
	skb->len += bytes;
}
static inline struct sk_buff *bt_skb_alloc(size_t bytes, int flags)
{
	struct sk_buff *skb = calloc(1, sizeof(*skb) + bytes);

	(void)flags;
	assert(skb);
	skb->capacity = bytes;
	return skb;
}

#define GFP_KERNEL 0
#define hci_skb_pkt_type(skb) ((skb)->type)

#endif
