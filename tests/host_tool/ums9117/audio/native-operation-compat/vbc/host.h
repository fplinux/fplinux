/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_VBC_HOST_COMPAT_H
#define FPLINUX_VBC_HOST_COMPAT_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <linux/types.h>

#define __iomem
#define GFP_KERNEL 0
#define S16_MAX 32767
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((~0UL >> (63 - (h))) & (~0UL << (l)))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctzl(mask))
#define FIELD_PREP(mask, value) (((value) << __builtin_ctzl(mask)) & (mask))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define ERR_CAST(pointer) ((void *)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define dev_err(dev, ...) ((void)(dev), fprintf(stderr, __VA_ARGS__))
#define dev_warn(dev, ...) dev_err(dev, __VA_ARGS__)
#define dev_err_ratelimited(dev, ...) dev_err(dev, __VA_ARGS__)
#define readl_poll_timeout(address, value, condition, delay, timeout) \
	({                                                            \
		(value) = readl(address);                             \
		(condition) ? 0 : -ETIMEDOUT;                         \
	})

struct device {
	void *of_node;
};
struct platform_device {
	struct device dev;
};
struct pinctrl {
	int unused;
};
struct pinctrl_state {
	int unused;
};
struct regmap {
	u32 words[9];
};

u32 readl(const volatile void *address);
void writel(u32 value, volatile void *address);
void udelay(unsigned int delay);
void usleep_range(unsigned int minimum, unsigned int maximum);
void *devm_kzalloc(struct device *dev, size_t size, int flags);
int dev_err_probe(struct device *dev, int error, const char *format, ...);
void *devm_platform_ioremap_resource_byname(struct platform_device *pdev,
					    const char *name);
bool device_property_read_bool(struct device *dev, const char *name);
struct pinctrl *devm_pinctrl_get(struct device *dev);
struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *control,
					   const char *name);
int pinctrl_select_state(struct pinctrl *control, struct pinctrl_state *state);
struct regmap *syscon_regmap_lookup_by_phandle(void *node, const char *name);
int regmap_read(struct regmap *map, unsigned int offset, unsigned int *value);
int regmap_write(struct regmap *map, unsigned int offset, unsigned int value);

#endif
