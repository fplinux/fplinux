/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PCM_RESET_KERNEL_H
#define FPLINUX_PCM_RESET_KERNEL_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/types.h>

#define CONFIG_SND_UMS9117_HEADPHONE_FED_SPEAKER 0
#define CONFIG_SND_UMS9117_SPEAKER_VIBRATOR 0
#define IS_ENABLED(option) (option)
#define BIT(bit) (1UL << (bit))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((long)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, replacement) ((value) = (replacement))
#define min(left, right) ((left) < (right) ? (left) : (right))
#define max_t(type, left, right) \
	((type)(left) > (type)(right) ? (type)(left) : (type)(right))
#define min_t(type, left, right) \
	((type)(left) < (type)(right) ? (type)(left) : (type)(right))
#define DIV_ROUND_UP(value, divisor) (((value) + (divisor) - 1) / (divisor))
#define cpu_to_le16(value) ((u16)(value))
#define le16_to_cpu(value) ((u16)(value))
#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define NSEC_PER_MSEC 1000000UL
#define MSEC_PER_SEC 1000UL
#define CLOCK_MONOTONIC 1
#define strscpy(destination, source) \
	snprintf(destination, sizeof(destination), "%s", source)

struct mutex {
	bool locked;
};
typedef struct {
	bool locked;
} spinlock_t;

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

#define spin_lock_init(lock) ((lock)->locked = false)
#define spin_lock(lock)                  \
	do {                             \
		assert(!(lock)->locked); \
		(lock)->locked = true;   \
	} while (0)
#define spin_unlock(lock)               \
	do {                            \
		assert((lock)->locked); \
		(lock)->locked = false; \
	} while (0)
#define spin_lock_irqsave(lock, flags) \
	do {                           \
		(flags) = 0;           \
		spin_lock(lock);       \
	} while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do {                                \
		(void)(flags);              \
		spin_unlock(lock);          \
	} while (0)

struct device {
	void *driver_data;
};
struct platform_device {
	struct device dev;
};
struct of_device_id {
	const char *compatible;
};
struct dev_pm_ops {
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
};
struct platform_driver {
	int (*probe)(struct platform_device *pdev);
	void (*remove)(struct platform_device *pdev);
	void (*shutdown)(struct platform_device *pdev);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
		const struct dev_pm_ops *pm;
	} driver;
};
#define module_platform_driver(driver) \
	struct platform_driver *pcm_reset_driver = &(driver)
#define DEFINE_SIMPLE_DEV_PM_OPS(name, suspend_fn, resume_fn) \
	const struct dev_pm_ops name = { suspend_fn, resume_fn }
#define pm_sleep_ptr(pointer) (pointer)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_DESCRIPTION(description)
#define MODULE_LICENSE(license)
void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags);
void *dev_get_drvdata(struct device *dev);
void platform_set_drvdata(struct platform_device *pdev, void *data);
void *platform_get_drvdata(struct platform_device *pdev);
const char *dev_driver_string(struct device *dev);
int dev_err_probe(struct device *dev, int error, const char *format, ...);
void pcm_reset_log(const char *format, ...);
#define dev_err(dev, ...) pcm_reset_log(__VA_ARGS__)
#define dev_err_ratelimited(dev, ...) pcm_reset_log(__VA_ARGS__)
#define dev_info_once(dev, ...) ((void)(dev))
#define dev_dbg(dev, ...) ((void)(dev))
bool device_property_read_bool(struct device *dev, const char *name);
bool device_property_present(struct device *dev, const char *name);
int device_property_read_string(struct device *dev, const char *name,
				const char **value);

extern unsigned long jiffies;
#define msecs_to_jiffies(milliseconds) (milliseconds)
#define time_after(left, right) ((long)((left) - (right)) > 0)
#define time_before(left, right) ((long)((left) - (right)) < 0)
typedef u64 ktime_t;
static inline ktime_t ns_to_ktime(u64 nanoseconds)
{
	return nanoseconds;
}
enum hrtimer_restart {
	HRTIMER_NORESTART,
	HRTIMER_RESTART,
};
#define HRTIMER_MODE_REL 0
struct hrtimer {
	enum hrtimer_restart (*function)(struct hrtimer *timer);
	bool active;
};
void hrtimer_setup(struct hrtimer *timer,
		   enum hrtimer_restart (*function)(struct hrtimer *timer),
		   int clock, int mode);
void hrtimer_start(struct hrtimer *timer, ktime_t delay, int mode);
int hrtimer_cancel(struct hrtimer *timer);
void hrtimer_forward_now(struct hrtimer *timer, ktime_t delay);
typedef struct {
	unsigned int wakes;
} wait_queue_head_t;
#define init_waitqueue_head(queue) ((queue)->wakes = 0)
#define wake_up(queue) ((queue)->wakes++)
void pcm_reset_wait(wait_queue_head_t *queue, bool ready);
#define wait_event_interruptible(queue, condition) \
	pcm_reset_wait(&(queue), (condition))
struct task_struct {
	int (*function)(void *data);
	void *data;
	bool stop_requested;
	bool stop_when_idle;
};
struct task_struct *kthread_run(int (*function)(void *data), void *data,
				const char *name);
bool kthread_should_stop(void);
int kthread_stop(struct task_struct *task);
void sched_set_fifo_low(struct task_struct *task);

/* Disabled input/work paths still compile without scheduling them. */
struct work_struct {
	void (*function)(struct work_struct *work);
};
struct delayed_work {
	struct work_struct work;
};
#define INIT_WORK(work, callback) ((work)->function = (callback))
#define INIT_DELAYED_WORK(item, callback) INIT_WORK(&(item)->work, callback)
#define to_delayed_work(pointer) \
	container_of(pointer, struct delayed_work, work)
#define system_percpu_wq NULL
bool schedule_work(struct work_struct *work);
bool cancel_work_sync(struct work_struct *work);
bool cancel_delayed_work(struct delayed_work *work);
bool cancel_delayed_work_sync(struct delayed_work *work);
bool mod_delayed_work(void *queue, struct delayed_work *work,
		      unsigned long delay);
struct input_dev {
	const char *name;
	const char *phys;
	struct {
		unsigned int bustype;
	} id;
	void (*close)(struct input_dev *input);
	void *driver_data;
};
struct ff_effect {
	union {
		struct {
			u16 strong_magnitude;
			u16 weak_magnitude;
		} rumble;
	} u;
};
#define BUS_HOST 0x19
#define EV_FF 0x15
#define FF_RUMBLE 0x50
struct input_dev *devm_input_allocate_device(struct device *dev);
void *input_get_drvdata(struct input_dev *input);
void input_set_drvdata(struct input_dev *input, void *data);
void input_set_capability(struct input_dev *input, unsigned int type,
			  unsigned int code);
int input_ff_create_memless(struct input_dev *input, void *data,
			    int (*play)(struct input_dev *, void *,
					struct ff_effect *));
int input_register_device(struct input_dev *input);

#endif
