/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_DCAM_HOST_H
#define FPLINUX_TEST_DCAM_HOST_H

#include <assert.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_VIDEO_UMS9117_DCAM_PARALLEL 0
#define CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0 1
#define CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0_ONE_BIT 1
#define CONFIG_VIDEO_UMS9117_DCAM_SERIAL_G0_TWO_BIT 0
#define IS_ENABLED(option) (option)
#define BIT_ULL(bit) (1ULL << (bit))
#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, new_value) ((value) = (new_value))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((long)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define kzalloc_obj(object, flags) calloc(1, sizeof(object))
#define kfree(pointer) free(pointer)
#define dev_err(device, ...) ((void)(device))

typedef uintptr_t dma_addr_t;
typedef unsigned int spinlock_t;

struct device_node {
	int unused;
};

struct fwnode_handle {
	int unused;
};

struct device {
	struct device_node *of_node;
};

struct mutex {
	bool locked;
};

static inline void mutex_init(struct mutex *lock)
{
	lock->locked = false;
}

static inline void mutex_lock(struct mutex *lock)
{
	assert(!lock->locked);
	lock->locked = true;
}

static inline void mutex_unlock(struct mutex *lock)
{
	assert(lock->locked);
	lock->locked = false;
}

#define spin_lock_init(lock) (*(lock) = 0)
#define spin_lock_irqsave(lock, flags) \
	do {                           \
		(flags) = 0;           \
		assert(!*(lock));      \
		*(lock) = 1;           \
	} while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do {                                \
		(void)(flags);              \
		assert(*(lock));            \
		*(lock) = 0;                \
	} while (0)

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *head)
{
	head->next = head;
	head->prev = head;
}

static inline bool list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline void list_add_tail(struct list_head *item, struct list_head *head)
{
	item->next = head;
	item->prev = head->prev;
	head->prev->next = item;
	head->prev = item;
}

static inline void list_del(struct list_head *item)
{
	item->prev->next = item->next;
	item->next->prev = item->prev;
}

#define list_first_entry(head, type, member) \
	container_of((head)->next, type, member)

struct work_struct {
	void (*function)(struct work_struct *work);
	bool pending;
};

struct delayed_work {
	struct work_struct work;
	unsigned long delay;
};

#define to_delayed_work(item) container_of(item, struct delayed_work, work)
#define system_percpu_wq NULL

void dcam_test_init_work(struct work_struct *work,
			 void (*function)(struct work_struct *work));
void dcam_test_init_delayed_work(struct delayed_work *work,
				 void (*function)(struct work_struct *work));
#define INIT_WORK(item, callback) dcam_test_init_work(item, callback)
#define INIT_DELAYED_WORK(item, callback) \
	dcam_test_init_delayed_work(item, callback)
bool schedule_work(struct work_struct *work);
bool cancel_work_sync(struct work_struct *work);
bool mod_delayed_work(void *queue, struct delayed_work *work,
		      unsigned long delay);
bool cancel_delayed_work(struct delayed_work *work);
bool cancel_delayed_work_sync(struct delayed_work *work);

extern unsigned long jiffies;
#define msecs_to_jiffies(milliseconds) (milliseconds)
#define time_after_eq(left, right) ((long)((left) - (right)) >= 0)
u64 ktime_get_ns(void);

struct device_node *of_graph_get_endpoint_by_regs(struct device_node *node,
						  int port, int endpoint);
struct fwnode_handle *of_fwnode_handle(struct device_node *node);
struct fwnode_handle *
fwnode_graph_get_remote_endpoint(struct fwnode_handle *endpoint);
void fwnode_handle_put(struct fwnode_handle *node);
void of_node_put(struct device_node *node);
bool of_property_present(const struct device_node *node, const char *name);
int of_property_read_u32(const struct device_node *node, const char *name,
			 u32 *value);

static inline size_t strscpy(char *destination, const char *source, size_t size)
{
	size_t length = strlen(source);
	size_t copied = length < size - 1 ? length : size - 1;

	memcpy(destination, source, copied);
	destination[copied] = '\0';
	return copied;
}

#endif
