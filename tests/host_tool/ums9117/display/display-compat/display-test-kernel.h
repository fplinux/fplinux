/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_DISPLAY_TEST_KERNEL_H
#define FPLINUX_DISPLAY_TEST_KERNEL_H

#include <stdio.h>

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include <drm/drm_rect.h>

#define EPROBE_DEFER 517
#define GFP_KERNEL 0
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define GENMASK(high, low) (((~0UL) << (low)) & (~0UL >> (63 - (high))))
#define IS_ALIGNED(value, alignment) (!((value) & ((alignment) - 1)))
#define READ_ONCE(value) (value)
#define msecs_to_jiffies(value) (value)
#define DRM_RECT_INIT(x, y, width, height) \
	{ (x), (y), (x) + (width), (y) + (height) }

/* Initialization is synchronous; these substitutes do not test locking or time. */
#define spin_lock_irqsave(lock, flags) \
	do {                           \
		(void)(lock);          \
		(flags) = 0;           \
	} while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do {                                \
		(void)(lock);               \
		(void)(flags);              \
	} while (0)

static inline void mutex_lock(struct mutex *lock)
{
}

static inline void mutex_unlock(struct mutex *lock)
{
}

static inline void complete(struct completion *completion)
{
}

void reinit_completion(struct completion *completion);
unsigned long wait_for_completion_timeout(struct completion *completion,
					  unsigned long timeout);
void wmb(void);
void usleep_range(unsigned long minimum, unsigned long maximum);
void msleep(unsigned int delay);
u64 ktime_get_ns(void);

typedef int irqreturn_t;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
int platform_get_irq(struct platform_device *pdev, unsigned int index);
int devm_request_irq(struct device *dev, int irq,
		     irqreturn_t (*handler)(int, void *), unsigned long flags,
		     const char *name, void *data);
void synchronize_irq(int irq);
const char *dev_name(const struct device *dev);

struct regmap;
struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name);
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int value);
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value);
int of_property_count_u32_elems(const struct device_node *node,
				const char *name);
int of_property_read_u32(const struct device_node *node, const char *name,
			 u32 *value);
struct property;
struct property *of_find_property(const struct device_node *node,
				  const char *name, int *length);
void *devm_kmalloc_array(struct device *dev, size_t count, size_t size,
			 unsigned int flags);

struct backlight_device;
struct backlight_properties {
	int type;
	int scale;
	int max_brightness;
	int brightness;
	int power;
};
struct backlight_ops {
	int (*update_status)(struct backlight_device *backlight);
	int (*get_brightness)(struct backlight_device *backlight);
};
#define BACKLIGHT_RAW 1
#define BACKLIGHT_SCALE_UNKNOWN 0
#define BACKLIGHT_POWER_ON 0
void *bl_get_data(struct backlight_device *backlight);
int backlight_get_brightness(struct backlight_device *backlight);
struct backlight_device *
devm_backlight_device_register(struct device *dev, const char *name,
			       struct device *parent, void *data,
			       const struct backlight_ops *ops,
			       const struct backlight_properties *properties);

static inline int drm_rect_width(const struct drm_rect *rect)
{
	return rect->x2 - rect->x1;
}

static inline int drm_rect_height(const struct drm_rect *rect)
{
	return rect->y2 - rect->y1;
}

/* Unused framebuffer paths are compiled, then removed by the linker. */
struct drm_format_info {
	u32 format;
	u8 cpp[4];
};
struct drm_framebuffer {
	const struct drm_format_info *format;
	u32 pitches[4];
};
struct drm_plane_state {
	struct drm_framebuffer *fb;
};
dma_addr_t drm_fb_dma_get_gem_addr(struct drm_framebuffer *fb,
				   struct drm_plane_state *state,
				   unsigned int plane);
#define DRM_FORMAT_NV16 0x3631564e

#endif
