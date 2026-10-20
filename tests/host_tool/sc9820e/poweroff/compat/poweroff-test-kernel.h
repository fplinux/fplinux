/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_POWEROFF_TEST_KERNEL_H
#define FPLINUX_POWEROFF_TEST_KERNEL_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint32_t __be32;
typedef unsigned long resource_size_t;
typedef unsigned int raw_spinlock_t;

#define __iomem
#define __acquire(lock) ((void)0)
#define __release(lock) ((void)0)
#define BIT(bit) (1U << (bit))
#define GENMASK(high, low) ((~0U >> (31 - (high))) & (~0U << (low)))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define IS_ALIGNED(value, alignment) (!((value) & ((alignment) - 1)))
#define IS_ENABLED(option) (option)
#define CONFIG_HWSPINLOCK 1
#define CONFIG_SPRD_WATCHDOG 0
#define U32_MAX UINT32_MAX
#define SZ_4K 4096U
#define SZ_16K 16384U
#define SZ_128K 131072U
#define SPI_CONTROLLER_HALF_DUPLEX 1U
#define NOTIFY_DONE 0
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define fallthrough __attribute__((fallthrough))
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_AUTHOR(author)
#define MODULE_LICENSE(license)
#define EXPORT_SYMBOL_GPL(symbol)
#define module_platform_driver(driver) \
	struct platform_driver *test_driver_##driver = &(driver)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_emerg(...) ((void)0)
#define pr_err(...) ((void)0)
#define raw_spin_lock_init(lock) (*(lock) = 0)
#define raw_spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define raw_spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))

struct device_node {
	const char *compatible;
};

struct device {
	struct device *parent;
	struct device_node *of_node;
	const void *match_data;
	void *drvdata;
};

struct platform_device {
	struct device dev;
	int id;
};

struct resource {
	resource_size_t start;
	resource_size_t end;
};

struct spi_device;
struct spi_transfer {
	void *rx_buf;
	const void *tx_buf;
};

struct spi_controller {
	struct device dev;
	void *private;
	int bus_num;
	u16 num_chipselect;
	u32 flags;
	u32 bits_per_word_mask;
	int (*transfer_one)(struct spi_controller *ctlr, struct spi_device *spi,
			    struct spi_transfer *transfer);
};

struct spi_device {
	struct device dev;
	struct spi_controller *controller;
};

struct of_device_id {
	const char *compatible;
	const void *data;
};

struct platform_driver {
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
	int (*probe)(struct platform_device *pdev);
};

struct hwspinlock {
	bool held;
};

struct clk;
struct sys_off_data {
	void *cb_data;
	unsigned long mode;
	const char *cmd;
};

const void *of_device_get_match_data(const struct device *dev);
int of_alias_get_id(struct device_node *node, const char *alias);
unsigned int of_get_child_count(struct device_node *node);
const void *of_get_property(struct device_node *node, const char *name,
			    int *size);
bool of_device_is_compatible(const struct device_node *node,
			     const char *compatible);
u32 be32_to_cpu(__be32 value);
struct spi_controller *devm_spi_alloc_host(struct device *dev,
					   size_t private_size);
void *spi_controller_get_devdata(struct spi_controller *controller);
void dev_set_drvdata(struct device *dev, void *data);
void *devm_platform_get_and_ioremap_resource(struct platform_device *pdev,
					     unsigned int index,
					     struct resource **resource);
resource_size_t resource_size(const struct resource *resource);
int of_hwspin_lock_get_id(struct device_node *node, unsigned int index);
struct hwspinlock *devm_hwspin_lock_request_specific(struct device *dev,
						     int id);
int devm_spi_register_controller(struct device *dev,
				 struct spi_controller *controller);
struct clk *devm_clk_get_enabled(struct device *dev, const char *name);
int devm_register_restart_handler(struct device *dev,
				  int (*callback)(struct sys_off_data *),
				  void *data);
int dev_err_probe(struct device *dev, int error, const char *format, ...);
int hwspin_lock_timeout_irqsave(struct hwspinlock *lock, unsigned int timeout,
				unsigned long *flags);
void hwspin_unlock_irqrestore(struct hwspinlock *lock, unsigned long *flags);
int hwspin_lock_timeout_in_atomic(struct hwspinlock *lock,
				  unsigned int timeout);
void hwspin_unlock_in_atomic(struct hwspinlock *lock);
bool irqs_disabled(void);
u32 readl_relaxed(const void *address);
void writel_relaxed(u32 value, void *address);
void udelay(unsigned long delay);
void mdelay(unsigned long delay);
void cpu_relax(void);

#define readl(address) readl_relaxed(address)
#define writel(value, address) writel_relaxed(value, address)

/* Host polling uses a finite iteration budget instead of hardware elapsed time. */
#define read_poll_timeout_atomic(op, value, condition, delay, timeout, before, \
				 ...)                                          \
	({                                                                     \
		unsigned long test_elapsed;                                    \
		unsigned long test_step = (delay) ? (delay) : 1;               \
		int test_ret = -ETIMEDOUT;                                     \
		assert(!(before));                                             \
		for (test_elapsed = 0; test_elapsed <= (timeout);              \
		     test_elapsed += test_step) {                              \
			(value) = op(__VA_ARGS__);                             \
			if (condition) {                                       \
				test_ret = 0;                                  \
				break;                                         \
			}                                                      \
			udelay(test_step);                                     \
		}                                                              \
		test_ret;                                                      \
	})
#define readl_poll_timeout_atomic(address, value, condition, delay, timeout) \
	read_poll_timeout_atomic(readl, value, condition, delay, timeout,    \
				 false, address)

#endif
