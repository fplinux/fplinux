/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SC9820E_GPIO_HOST_KERNEL_H
#define FPLINUX_SC9820E_GPIO_HOST_KERNEL_H

#include "../gate-compat/gate-kernel.h"

typedef uint16_t u16;
typedef unsigned int raw_spinlock_t;

#define GENMASK(high, low) ((~0UL << (low)) & (~0UL >> (63 - (high))))
#define IRQ_TYPE_NONE 0
#define IRQ_TYPE_EDGE_RISING 1
#define IRQ_TYPE_EDGE_FALLING 2
#define IRQ_TYPE_EDGE_BOTH 3
#define IRQ_TYPE_LEVEL_HIGH 4
#define IRQ_TYPE_LEVEL_LOW 8
#define IRQCHIP_SKIP_SET_WAKE BIT(0)
#define IRQCHIP_IMMUTABLE BIT(1)
#define GPIOCHIP_IRQ_RESOURCE_HELPERS .irq_request_resources = NULL
#define for_each_set_bit(bit, address, count)     \
	for ((bit) = 0; (bit) < (count); (bit)++) \
		if (*(address) & BIT(bit))
#define raw_spin_lock_irqsave(lock, flags) \
	do {                               \
		(void)(lock);              \
		(flags) = 0;               \
	} while (0)
#define raw_spin_unlock_irqrestore(lock, flags) \
	do {                                    \
		(void)(lock);                   \
		(void)(flags);                  \
	} while (0)
#define raw_spin_lock_init(lock) (*(lock) = 0)

struct gpio_chip;
struct irq_data {
	struct gpio_chip *chip;
	unsigned int hwirq;
};
struct irq_desc;
struct irq_chip {
	const char *name;
	void (*irq_ack)(struct irq_data *data);
	void (*irq_mask)(struct irq_data *data);
	void (*irq_unmask)(struct irq_data *data);
	int (*irq_set_type)(struct irq_data *data, unsigned int type);
	int (*irq_request_resources)(struct irq_data *data);
	unsigned long flags;
};
struct irq_desc {
	struct gpio_chip *chip;
	struct irq_chip *irqchip;
};
struct gpio_irq_chip {
	const struct irq_chip *chip;
	void (*handler)(struct irq_desc *desc);
	unsigned int default_type;
	void (*parent_handler)(struct irq_desc *desc);
	void *parent_handler_data;
	unsigned int num_parents;
	int *parents;
	void *domain;
};
struct gpio_chip {
	const char *label;
	unsigned int ngpio;
	int base;
	struct device *parent;
	int (*request)(struct gpio_chip *chip, unsigned int offset);
	void (*free)(struct gpio_chip *chip, unsigned int offset);
	int (*get)(struct gpio_chip *chip, unsigned int offset);
	int (*set)(struct gpio_chip *chip, unsigned int offset, int value);
	int (*direction_input)(struct gpio_chip *chip, unsigned int offset);
	int (*direction_output)(struct gpio_chip *chip, unsigned int offset,
				int value);
	struct gpio_irq_chip irq;
	void *data;
};
struct clk {
	struct clk_hw *hw;
};

struct clk *devm_clk_get_optional_enabled(struct device *dev, const char *name);
int platform_get_irq(struct platform_device *pdev, unsigned int index);
int device_property_read_u32(struct device *dev, const char *name, u32 *value);
const char *dev_name(const struct device *dev);
int devm_gpiochip_add_data(struct device *dev, struct gpio_chip *chip,
			   void *data);
u32 readl_relaxed(const void *address);
void writel_relaxed(u32 value, void *address);
void gpiochip_disable_irq(struct gpio_chip *chip, unsigned int offset);
void gpiochip_enable_irq(struct gpio_chip *chip, unsigned int offset);

static inline void *gpiochip_get_data(struct gpio_chip *chip)
{
	return chip->data;
}

static inline struct gpio_chip *
irq_data_get_irq_chip_data(struct irq_data *data)
{
	return data->chip;
}

static inline unsigned int irqd_to_hwirq(struct irq_data *data)
{
	return data->hwirq;
}

static inline struct gpio_chip *irq_desc_get_handler_data(struct irq_desc *desc)
{
	return desc->chip;
}

static inline struct irq_chip *irq_desc_get_chip(struct irq_desc *desc)
{
	return desc->irqchip;
}

static inline void irq_set_handler_locked(struct irq_data *data,
					  void (*handler)(struct irq_desc *))
{
	(void)data;
	(void)handler;
}

static inline void handle_bad_irq(struct irq_desc *desc)
{
	(void)desc;
}

static inline void handle_edge_irq(struct irq_desc *desc)
{
	(void)desc;
}

static inline void handle_level_irq(struct irq_desc *desc)
{
	(void)desc;
}

static inline void chained_irq_enter(struct irq_chip *chip,
				     struct irq_desc *desc)
{
	(void)chip;
	(void)desc;
}

static inline void chained_irq_exit(struct irq_chip *chip,
				    struct irq_desc *desc)
{
	(void)chip;
	(void)desc;
}

static inline int generic_handle_domain_irq(void *domain, unsigned int irq)
{
	(void)domain;
	(void)irq;
	return 0;
}

static inline void gpio_irq_chip_set_chip(struct gpio_irq_chip *irq,
					  const struct irq_chip *chip)
{
	irq->chip = chip;
}

extern struct platform_driver *host_sprd_gpio_driver;

#endif
