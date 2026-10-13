/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_OWNER_HOST_BOUNDARY_H
#define FPLINUX_MARLIN2_OWNER_HOST_BOUNDARY_H

/* Only kernel allocation, locking, GPIO, IRQ and SDIO boundaries are replaced. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int irqreturn_t;

#define GFP_KERNEL 0
#define IRQ_HANDLED 1
#define IRQF_TRIGGER_HIGH 1
#define IRQF_ONESHOT 2
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define PTR_ERR(pointer) ((long)(intptr_t)(pointer))
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_RELAXED)
#define WRITE_ONCE(value, data) \
	__atomic_store_n(&(value), (data), __ATOMIC_RELAXED)
#define smp_load_acquire(pointer) __atomic_load_n((pointer), __ATOMIC_ACQUIRE)
#define smp_store_release(pointer, data) \
	__atomic_store_n((pointer), (data), __ATOMIC_RELEASE)
#define WARN_ON(condition) (!!(condition))
/* Logging is an external sink, not the subject. */
static inline void marlin2_host_log(const void *device, const char *format, ...)
{
	(void)device;
	(void)format;
}
#define dev_info(device, format, ...) \
	marlin2_host_log(device, format, ##__VA_ARGS__)
#define dev_err(device, format, ...) \
	marlin2_host_log(device, format, ##__VA_ARGS__)

struct device {
	int unused;
};
struct gpio_desc {
	int irq;
};
struct mutex {
	pthread_mutex_t native;
};
struct mmc_host {
	unsigned int max_req_size;
	unsigned int max_seg_size;
	unsigned int max_blk_count;
};
struct mmc_card {
	struct mmc_host *host;
	struct {
		bool multi_block;
	} cccr;
};
struct sdio_func {
	struct mmc_card *card;
	unsigned int num;
	unsigned int cur_blksize;
};

static inline void mutex_init(struct mutex *mutex)
{
	assert(!pthread_mutex_init(&mutex->native, NULL));
}
static inline void mutex_lock(struct mutex *mutex)
{
	assert(!pthread_mutex_lock(&mutex->native));
}
static inline void mutex_unlock(struct mutex *mutex)
{
	assert(!pthread_mutex_unlock(&mutex->native));
}
static inline void *kmalloc(size_t bytes, int flags)
{
	(void)flags;
	return malloc(bytes);
}
static inline void *kzalloc(size_t bytes, int flags)
{
	(void)flags;
	return calloc(1, bytes);
}
#define kzalloc_obj(object, flags) kzalloc(sizeof(object), flags)
static inline void kfree(void *pointer)
{
	free(pointer);
}
static inline int gpiod_to_irq(struct gpio_desc *gpio)
{
	return gpio->irq;
}

int request_threaded_irq(int irq, void *primary,
			 irqreturn_t (*thread)(int, void *),
			 unsigned long flags, const char *name, void *context);
void free_irq(int irq, void *context);
void disable_irq_nosync(int irq);
void sdio_claim_host(struct sdio_func *func);
void sdio_release_host(struct sdio_func *func);
int sdio_readsb(struct sdio_func *func, void *buffer, unsigned int address,
		size_t bytes);
int sdio_writesb(struct sdio_func *func, unsigned int address,
		 const void *buffer, size_t bytes);

#endif
