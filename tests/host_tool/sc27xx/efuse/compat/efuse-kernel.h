/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_AP_EFUSE_HOST_KERNEL_H
#define FPLINUX_AP_EFUSE_HOST_KERNEL_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((UINT32_MAX >> (31 - (h))) & (UINT32_MAX << (l)))
#define BITS_PER_BYTE 8
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define IS_ERR(ptr) ((uintptr_t)(ptr) >= (uintptr_t)-4095)
#define PTR_ERR(ptr) ((int)(intptr_t)(ptr))
#define dev_err(...) ((void)0)
#define MODULE_DEVICE_TABLE(...)
#define MODULE_AUTHOR(...)
#define MODULE_DESCRIPTION(...)
#define MODULE_LICENSE(...)
#define module_platform_driver(driver) \
	struct platform_driver *test_platform_driver = &(driver)

struct clk {
	int unused;
};
struct hwspinlock {
	int unused;
};
struct mutex {
	bool held;
};
struct device_node {
	int unused;
};
struct device {
	struct device_node *of_node;
};
struct platform_device {
	struct device dev;
};
struct nvmem_device {
	int unused;
};
struct nvmem_config {
	int stride;
	int word_size;
	bool read_only;
	bool root_only;
	const char *name;
	size_t size;
	bool add_legacy_fixed_of_cells;
	int (*reg_read)(void *, u32, void *, size_t);
	int (*reg_write)(void *, u32, void *, size_t);
	void *priv;
	struct device *dev;
};
struct of_device_id {
	const char *compatible;
	const void *data;
};
struct platform_driver {
	int (*probe)(struct platform_device *);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
};

extern struct platform_driver *test_platform_driver;
void mutex_init(struct mutex *mutex);
void mutex_lock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);
int hwspin_lock_timeout_raw(struct hwspinlock *lock, unsigned int timeout);
void hwspin_unlock_raw(struct hwspinlock *lock);
int clk_prepare_enable(struct clk *clk);
void clk_disable_unprepare(struct clk *clk);
u32 readl(const void *address);
void writel(u32 value, void *address);
void usleep_range(unsigned long low, unsigned long high);
void wmb(void);
const void *of_device_get_match_data(struct device *dev);
void *devm_kzalloc(struct device *dev, size_t size, int flags);
void *devm_platform_ioremap_resource(struct platform_device *pdev, int index);
int of_hwspin_lock_get_id(struct device_node *node, int index);
struct hwspinlock *devm_hwspin_lock_request_specific(struct device *dev,
						     int id);
struct clk *devm_clk_get(struct device *dev, const char *name);
struct nvmem_device *devm_nvmem_register(struct device *dev,
					 const struct nvmem_config *config);

#endif
