/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_RTC_TEST_KERNEL_H
#define FPLINUX_RTC_TEST_KERNEL_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef int64_t time64_t;
typedef uint64_t timeu64_t;
typedef int irqreturn_t;

#define BIT(n) (1U << (n))
#define GENMASK(high, low) ((~0U >> (31 - (high))) & (~0U << (low)))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define GFP_KERNEL 0
#define EPROBE_DEFER 517
#define IRQ_HANDLED 1
#define IRQF_ONESHOT 1
#define IRQF_EARLY_RESUME 2
#define RTC_AF 0x20
#define RTC_IRQF 0x80
#define RTC_TIMESTAMP_END_2099 4102444799LL
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_LICENSE(license)
#define MODULE_DESCRIPTION(description)
#define MODULE_AUTHOR(author)
#define module_platform_driver(driver) \
	struct platform_driver *test_driver_##driver = &(driver)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)

struct mutex {
	bool locked;
};

static inline void mutex_init(struct mutex *mutex)
{
	mutex->locked = false;
}

static inline void mutex_lock(struct mutex *mutex)
{
	assert(!mutex->locked);
	mutex->locked = true;
}

static inline void mutex_unlock(struct mutex *mutex)
{
	assert(mutex->locked);
	mutex->locked = false;
}

struct regmap;
struct rtc_device;

struct device_node {
	u32 base;
};

struct device {
	struct device *parent;
	struct device_node *of_node;
	const void *match_data;
	struct regmap *regmap;
	void *drvdata;
	void *resources[4];
	unsigned int num_resources;
	struct rtc_device *rtc;
	irqreturn_t (*irq_thread)(int irq, void *data);
	void *irq_data;
	bool wakeup;
};

struct platform_device {
	struct device dev;
	const char *name;
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
	void (*shutdown)(struct platform_device *pdev);
};

struct rtc_time {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

struct rtc_wkalrm {
	unsigned char enabled;
	unsigned char pending;
	struct rtc_time time;
};

struct rtc_class_ops {
	int (*read_time)(struct device *dev, struct rtc_time *tm);
	int (*set_time)(struct device *dev, struct rtc_time *tm);
	int (*read_alarm)(struct device *dev, struct rtc_wkalrm *alarm);
	int (*set_alarm)(struct device *dev, struct rtc_wkalrm *alarm);
	int (*alarm_irq_enable)(struct device *dev, unsigned int enabled);
};

struct rtc_device {
	const struct rtc_class_ops *ops;
	time64_t range_min;
	timeu64_t range_max;
	struct {
		struct {
			time64_t expires;
		} node;
		bool enabled;
	} aie_timer;
	unsigned int irq_count;
	unsigned int irq_events;
	bool registered;
};

void *devm_kzalloc(struct device *dev, size_t size, int flags);
void *dev_get_drvdata(struct device *dev);
void platform_set_drvdata(struct platform_device *pdev, void *data);
void *platform_get_drvdata(struct platform_device *pdev);
const void *of_device_get_match_data(struct device *dev);
int of_property_read_u32(struct device_node *node, const char *name,
			 u32 *value);
struct regmap *dev_get_regmap(struct device *dev, const char *name);
int platform_get_irq(struct platform_device *pdev, unsigned int index);
struct rtc_device *devm_rtc_allocate_device(struct device *dev);
int devm_rtc_register_device(struct rtc_device *rtc);
int devm_request_threaded_irq(struct device *dev, int irq,
			      irqreturn_t (*handler)(int, void *),
			      irqreturn_t (*thread)(int, void *), int flags,
			      const char *name, void *data);
void device_init_wakeup(struct device *dev, bool enabled);
int dev_err_probe(struct device *dev, int error, const char *format, ...);
time64_t rtc_tm_to_time64(const struct rtc_time *tm);
void rtc_time64_to_tm(time64_t secs, struct rtc_time *tm);
struct rtc_time rtc_ktime_to_tm(time64_t secs);
time64_t rtc_tm_sub(const struct rtc_time *left, const struct rtc_time *right);
void rtc_update_irq(struct rtc_device *rtc, unsigned int count,
		    unsigned int events);
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int value);
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value);
int regmap_set_bits(struct regmap *map, unsigned int reg, unsigned int mask);

static inline time64_t div_s64_rem(time64_t value, int divisor, int *remainder)
{
	*remainder = value % divisor;
	return value / divisor;
}

/* Replace elapsed hardware time with bounded deterministic register polling. */
#define regmap_read_poll_timeout(map, reg, value, condition, delay, timeout) \
	({                                                                   \
		unsigned int test_attempt;                                   \
		int test_ret = -ETIMEDOUT;                                   \
		for (test_attempt = 0; test_attempt <= (timeout) / (delay);  \
		     test_attempt++) {                                       \
			test_ret = regmap_read(map, reg, &(value));          \
			if (test_ret || (condition))                         \
				break;                                       \
			test_ret = -ETIMEDOUT;                               \
		}                                                            \
		test_ret;                                                    \
	})

#endif
