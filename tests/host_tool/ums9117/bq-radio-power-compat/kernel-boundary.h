/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_BQ_RADIO_HOST_BOUNDARY_H
#define FPLINUX_BQ_RADIO_HOST_BOUNDARY_H

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int irqreturn_t;
typedef pthread_mutex_t spinlock_t;

#define BIT(bit) (1UL << (bit))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define GFP_KERNEL 0
#define GPIOD_ASIS 0
#define CLK_IGNORE_UNUSED 1
#define IRQ_HANDLED 1
#define IRQF_TRIGGER_HIGH 1
#define IRQF_ONESHOT 2
#define ETH_ALEN 6
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
#define IS_ENABLED(option) option
#define CONFIG_BQ2830L_WLAN 1
#define CONFIG_BQ2830L_BLUETOOTH 0
#define CONFIG_BQ2830L_FM 0
#define max_t(type, left, right) \
	((type)(left) > (type)(right) ? (type)(left) : (type)(right))
#define msecs_to_jiffies(milliseconds) (milliseconds)
#define time_after_eq(left, right) ((long)((left) - (right)) >= 0)
#define __init
#define __exit
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_LICENSE(license)
#define module_init(function)        \
	int bq_radio_host_init(void) \
	{                            \
		return function();   \
	}
#define module_exit(function)         \
	void bq_radio_host_exit(void) \
	{                             \
		function();           \
	}

struct device_node {
	int unused;
};
struct device {
	struct device *parent;
	struct device_node *of_node;
	void *driver_data;
};
struct platform_device {
	struct device dev;
};
struct of_device_id {
	const char *compatible;
};
struct platform_driver {
	int (*probe)(struct platform_device *pdev);
	void (*shutdown)(struct platform_device *pdev);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
};
struct attribute {
	const char *name;
};
struct device_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *, struct device_attribute *, char *);
	ssize_t (*store)(struct device *, struct device_attribute *,
			 const char *, size_t);
};
struct attribute_group {
	struct attribute **attrs;
};
#define DEVICE_ATTR_RO(name)                                     \
	struct device_attribute dev_attr_##name = { { #name },   \
						    name##_show, \
						    NULL }
#define DEVICE_ATTR_WO(name)                                   \
	struct device_attribute dev_attr_##name = { { #name }, \
						    NULL,      \
						    name##_store }
struct mutex {
	pthread_mutex_t native;
};
struct gpio_desc {
	int direction, value, irq;
};
struct regmap {
	unsigned int words[512];
};
struct regulator {
	unsigned int enables;
	int voltage_uv;
	bool exclusive;
};
struct clk_hw;
struct clk_ops {
	int (*prepare)(struct clk_hw *);
	void (*unprepare)(struct clk_hw *);
};
struct clk_init_data {
	const char *name;
	const struct clk_ops *ops;
	unsigned long flags;
};
struct clk_hw {
	const struct clk_init_data *init;
};
struct clk {
	struct clk_hw *hw;
};
struct completion {
	bool done;
};
struct work_struct {
	void (*function)(struct work_struct *);
};
struct delayed_work {
	struct work_struct work;
	bool pending;
};
struct workqueue_struct {
	int unused;
};
struct mmc_host {
	unsigned int max_req_size, max_seg_size, max_blk_count;
	struct device *parent;
};
struct mmc_card {
	struct mmc_host *host;
	struct {
		bool multi_block;
	} cccr;
};
struct sdio_func {
	struct mmc_card *card;
	unsigned int num, cur_blksize;
	u16 vendor, device, max_blksize;
	u8 class;
};
struct sdio_driver {
	int unused;
};
struct firmware {
	const u8 *data;
	size_t size;
};

extern unsigned long jiffies;
extern struct workqueue_struct *system_percpu_wq;

static inline void mutex_init(struct mutex *lock)
{
	assert(!pthread_mutex_init(&lock->native, NULL));
}
static inline void mutex_lock(struct mutex *lock)
{
	assert(!pthread_mutex_lock(&lock->native));
}
static inline void mutex_unlock(struct mutex *lock)
{
	assert(!pthread_mutex_unlock(&lock->native));
}
#define spin_lock_init(lock) assert(!pthread_mutex_init((lock), NULL))
#define spin_lock_irqsave(lock, flags)             \
	do {                                       \
		(flags) = 0;                       \
		assert(!pthread_mutex_lock(lock)); \
	} while (0)
#define spin_unlock_irqrestore(lock, flags)          \
	do {                                         \
		(void)(flags);                       \
		assert(!pthread_mutex_unlock(lock)); \
	} while (0)
static inline void *kmalloc(size_t bytes, int flags)
{
	return malloc(bytes);
}
static inline void *kzalloc(size_t bytes, int flags)
{
	return calloc(1, bytes);
}
#define kzalloc_obj(object, flags) kzalloc(sizeof(object), flags)
static inline void kfree(void *pointer)
{
	free(pointer);
}
static inline void *dev_get_drvdata(struct device *dev)
{
	return dev->driver_data;
}
static inline void platform_set_drvdata(struct platform_device *pdev,
					void *data)
{
	pdev->dev.driver_data = data;
}
static inline void *platform_get_drvdata(struct platform_device *pdev)
{
	return dev_get_drvdata(&pdev->dev);
}
static inline void init_completion(struct completion *done)
{
	done->done = false;
}
static inline void reinit_completion(struct completion *done)
{
	done->done = false;
}
static inline void complete(struct completion *done)
{
	done->done = true;
}
static inline unsigned long wait_for_completion_timeout(struct completion *done,
							unsigned long timeout)
{
	return done->done ? 1 : 0;
}
static inline void msleep(unsigned int milliseconds)
{
	jiffies += milliseconds;
}
static inline void fsleep(unsigned int microseconds)
{
	jiffies += microseconds / 1000;
}
#define INIT_DELAYED_WORK(delayed, callback) \
	((delayed)->work.function = (callback))
#define to_delayed_work(work) container_of(work, struct delayed_work, work)
static inline void cancel_delayed_work_sync(struct delayed_work *work)
{
	work->pending = false;
}
static inline bool sysfs_streq(const char *left, const char *right)
{
	return !strcmp(left, right);
}
static inline bool is_valid_ether_addr(const u8 *mac)
{
	return !(mac[0] & 1) && memcmp(mac, "\0\0\0\0\0\0", 6);
}
static inline void ether_addr_copy(u8 *destination, const u8 *source)
{
	memcpy(destination, source, 6);
}
static inline int gpiod_to_irq(struct gpio_desc *gpio)
{
	return gpio->irq;
}
static inline int gpiod_get_direction(struct gpio_desc *gpio)
{
	return gpio->direction;
}
static inline int gpiod_get_raw_value_cansleep(struct gpio_desc *gpio)
{
	return gpio->value;
}
static inline int gpiod_direction_input(struct gpio_desc *gpio)
{
	gpio->direction = 1;
	return 0;
}
static inline int gpiod_direction_output_raw(struct gpio_desc *gpio, int value)
{
	gpio->direction = 0;
	gpio->value = value;
	return 0;
}
#define gpiod_direction_output gpiod_direction_output_raw
static inline void gpiod_set_value_cansleep(struct gpio_desc *gpio, int value)
{
	gpio->value = value;
}

void *devm_kzalloc(struct device *, size_t, int);
int devm_add_action_or_reset(struct device *, void (*)(void *), void *);
void devm_release_action(struct device *, void (*)(void *), void *);
void devm_remove_action(struct device *, void (*)(void *), void *);
int platform_driver_register(struct platform_driver *);
void platform_driver_unregister(struct platform_driver *);
int sdio_register_driver(struct sdio_driver *);
void sdio_unregister_driver(struct sdio_driver *);
struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *,
					       const char *);
int regmap_read(struct regmap *, unsigned int, unsigned int *);
int regmap_write(struct regmap *, unsigned int, unsigned int);
struct gpio_desc *devm_gpiod_get(struct device *, const char *, int);
struct regulator *devm_regulator_get_exclusive(struct device *, const char *);
int regulator_get_voltage(struct regulator *);
int regulator_set_voltage(struct regulator *, int, int);
int regulator_enable(struct regulator *);
int regulator_disable(struct regulator *);
int devm_clk_hw_register(struct device *, struct clk_hw *);
struct clk *devm_clk_hw_get_clk(struct device *, struct clk_hw *, const char *);
int clk_prepare_enable(struct clk *);
void clk_disable_unprepare(struct clk *);
int of_platform_populate(struct device_node *, const void *, const void *,
			 struct device *);
void of_platform_depopulate(struct device *);
bool of_device_is_compatible(struct device_node *, const char *);
bool mod_delayed_work(struct workqueue_struct *, struct delayed_work *,
		      unsigned long);
int device_add_group(struct device *, const struct attribute_group *);
void device_remove_group(struct device *, const struct attribute_group *);
int device_property_read_string(struct device *, const char *, const char **);
int request_firmware_direct(const struct firmware **, const char *,
			    struct device *);
void release_firmware(const struct firmware *);
ssize_t sysfs_emit(char *, const char *, ...);
ssize_t sysfs_emit_at(char *, ssize_t, const char *, ...);
int dev_err_probe(struct device *, int, const char *, ...);
void bq_radio_host_log(const void *, const char *, ...);
#define dev_err bq_radio_host_log
#define dev_info bq_radio_host_log
int request_threaded_irq(int, void *, irqreturn_t (*)(int, void *),
			 unsigned long, const char *, void *);
void free_irq(int, void *);
void disable_irq_nosync(int);
void sdio_claim_host(struct sdio_func *);
void sdio_release_host(struct sdio_func *);
int sdio_readsb(struct sdio_func *, void *, unsigned int, size_t);
int sdio_writesb(struct sdio_func *, unsigned int, const void *, size_t);
int bq_radio_host_init(void);
void bq_radio_host_exit(void);

#endif
