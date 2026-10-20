// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rtc-test-kernel.h>

extern struct platform_driver *test_driver_sprd_rtc_driver;

/* The fake PMIC supplies counter latches, W1C status and update acknowledgements. */
struct regmap {
	u32 cells[0x1000 / 4];
	unsigned int counter_writes;
	unsigned int power_status_reads;
	unsigned int power_status_writes;
	unsigned int error_reg;
	int read_error;
	int write_error;
	bool block_time_ack;
	bool block_spg_ack;
};

struct fixture {
	struct regmap map;
	struct device parent;
	struct device_node node;
	struct platform_device pdev;
};

static u32 *cell(struct regmap *map, unsigned int reg)
{
	assert(reg < 0x1000 && !(reg & 3));
	return &map->cells[reg / 4];
}

void *devm_kzalloc(struct device *dev, size_t size, int flags)
{
	void *resource = calloc(1, size);

	assert(resource != NULL);
	assert(dev->num_resources < ARRAY_SIZE(dev->resources));
	dev->resources[dev->num_resources++] = resource;
	return resource;
}

void *dev_get_drvdata(struct device *dev)
{
	return dev->drvdata;
}

void platform_set_drvdata(struct platform_device *pdev, void *data)
{
	pdev->dev.drvdata = data;
}

void *platform_get_drvdata(struct platform_device *pdev)
{
	return pdev->dev.drvdata;
}

const void *of_device_get_match_data(struct device *dev)
{
	return dev->match_data;
}

int of_property_read_u32(struct device_node *node, const char *name, u32 *value)
{
	assert(!strcmp(name, "reg"));
	*value = node->base;
	return 0;
}

struct regmap *dev_get_regmap(struct device *dev, const char *name)
{
	assert(name == NULL);
	return dev->regmap;
}

int platform_get_irq(struct platform_device *pdev, unsigned int index)
{
	assert(index == 0);
	return 7;
}

struct rtc_device *devm_rtc_allocate_device(struct device *dev)
{
	dev->rtc = devm_kzalloc(dev, sizeof(*dev->rtc), GFP_KERNEL);
	return dev->rtc;
}

int devm_rtc_register_device(struct rtc_device *rtc)
{
	rtc->registered = true;
	return 0;
}

int devm_request_threaded_irq(struct device *dev, int irq,
			      irqreturn_t (*handler)(int, void *),
			      irqreturn_t (*thread)(int, void *), int flags,
			      const char *name, void *data)
{
	assert(handler == NULL && thread != NULL);
	dev->irq_thread = thread;
	dev->irq_data = data;
	return 0;
}

void device_init_wakeup(struct device *dev, bool enabled)
{
	dev->wakeup = enabled;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	return error;
}

time64_t rtc_tm_to_time64(const struct rtc_time *tm)
{
	struct tm calendar = {
		.tm_sec = tm->tm_sec,
		.tm_min = tm->tm_min,
		.tm_hour = tm->tm_hour,
		.tm_mday = tm->tm_mday,
		.tm_mon = tm->tm_mon,
		.tm_year = tm->tm_year,
	};

	return timegm(&calendar);
}

void rtc_time64_to_tm(time64_t secs, struct rtc_time *tm)
{
	time_t timestamp = secs;
	struct tm calendar;

	assert(gmtime_r(&timestamp, &calendar) != NULL);
	*tm = (struct rtc_time){
		.tm_sec = calendar.tm_sec,
		.tm_min = calendar.tm_min,
		.tm_hour = calendar.tm_hour,
		.tm_mday = calendar.tm_mday,
		.tm_mon = calendar.tm_mon,
		.tm_year = calendar.tm_year,
		.tm_wday = calendar.tm_wday,
		.tm_yday = calendar.tm_yday,
	};
}

struct rtc_time rtc_ktime_to_tm(time64_t secs)
{
	struct rtc_time tm;

	rtc_time64_to_tm(secs, &tm);
	return tm;
}

time64_t rtc_tm_sub(const struct rtc_time *left, const struct rtc_time *right)
{
	return rtc_tm_to_time64(left) - rtc_tm_to_time64(right);
}

void rtc_update_irq(struct rtc_device *rtc, unsigned int count,
		    unsigned int events)
{
	rtc->irq_count += count;
	rtc->irq_events |= events;
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value)
{
	if (reg == 0x25c)
		map->power_status_reads++;
	if (map->read_error && reg == map->error_reg)
		return map->read_error;
	*value = *cell(map, reg);
	return 0;
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int value)
{
	if (reg == 0x258)
		map->power_status_writes++;
	if (map->write_error && reg == map->error_reg)
		return map->write_error;
	if (reg == 0x238) {
		*cell(map, 0x234) &= ~value;
		return 0;
	}
	*cell(map, reg) = value;
	if (reg >= 0x210 && reg <= 0x21c) {
		map->counter_writes++;
		*cell(map, reg - 0x10) = value;
		if (!map->block_time_ack)
			*cell(map, 0x234) |= 1U << (8 + (reg - 0x210) / 4);
	}
	if (reg >= 0x220 && reg <= 0x22c) {
		*cell(map, reg + 0x20) = value;
		*cell(map, 0x234) |= 1U << (12 + (reg - 0x220) / 4);
	}
	if (reg == 0x254) {
		*cell(map, 0x250) = value;
		if (!map->block_spg_ack)
			*cell(map, 0x234) |= 0x80;
	}
	return 0;
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value)
{
	u32 current;
	int ret = regmap_read(map, reg, &current);

	if (ret)
		return ret;
	return regmap_write(map, reg, (current & ~mask) | (value & mask));
}

int regmap_set_bits(struct regmap *map, unsigned int reg, unsigned int mask)
{
	return regmap_update_bits(map, reg, mask, mask);
}

static void prepare(struct fixture *fixture, const char *compatible, u32 spg)
{
	const struct of_device_id *match =
		test_driver_sprd_rtc_driver->driver.of_match_table;

	memset(fixture, 0, sizeof(*fixture));
	fixture->parent.regmap = &fixture->map;
	fixture->node.base = 0x200;
	fixture->pdev.dev.parent = &fixture->parent;
	fixture->pdev.dev.of_node = &fixture->node;
	fixture->pdev.name = "rtc-test";
	*cell(&fixture->map, 0x250) = spg;
	for (; match->compatible; match++) {
		if (!strcmp(match->compatible, compatible)) {
			fixture->pdev.dev.match_data = match->data;
			return;
		}
	}
	assert(!"RTC compatible is unavailable");
}

static void release(struct fixture *fixture)
{
	struct device *dev = &fixture->pdev.dev;

	while (dev->num_resources)
		free(dev->resources[--dev->num_resources]);
}

static struct rtc_device *probe(struct fixture *fixture)
{
	assert(test_driver_sprd_rtc_driver->probe(&fixture->pdev) == 0);
	assert(fixture->pdev.dev.rtc->registered);
	return fixture->pdev.dev.rtc;
}

static void assert_date(const struct rtc_time *tm, int year, int month, int day,
			int hour, int minute, int second)
{
	assert(tm->tm_year == year - 1900 && tm->tm_mon == month - 1);
	assert(tm->tm_mday == day && tm->tm_hour == hour);
	assert(tm->tm_min == minute && tm->tm_sec == second);
}

static struct rtc_time sample_date(void)
{
	return (struct rtc_time){
		.tm_year = 126,
		.tm_mon = 9,
		.tm_mday = 15,
		.tm_hour = 12,
		.tm_min = 34,
		.tm_sec = 56,
	};
}

static void check_sc2721_time(void)
{
	struct fixture fixture;
	struct rtc_device *rtc;
	struct rtc_time tm = sample_date();

	prepare(&fixture, "sprd,sc2721-rtc", 0x2a5);
	*cell(&fixture.map, 0x230) = 0xffff;
	rtc = probe(&fixture);
	assert(*cell(&fixture.map, 0x230) == 0x20);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == 0);
	assert_date(&tm, 2019, 1, 1, 0, 0, 0);
	tm = sample_date();
	assert(rtc->ops->set_time(&fixture.pdev.dev, &tm) == 0);
	assert(!(*cell(&fixture.map, 0x230) & 0xff80));
	assert(*cell(&fixture.map, 0x200) == 56);
	assert(*cell(&fixture.map, 0x204) == 34);
	assert(*cell(&fixture.map, 0x208) == 12);
	assert(*cell(&fixture.map, 0x20c) == 2844);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == 0);
	assert_date(&tm, 2026, 10, 15, 12, 34, 56);
	tm = (struct rtc_time){ .tm_year = 118, .tm_mon = 11, .tm_mday = 31 };
	assert(rtc->ops->set_time(&fixture.pdev.dev, &tm) == -ERANGE);
	assert(fixture.map.power_status_reads == 0);
	assert(fixture.map.power_status_writes == 0);
	release(&fixture);
}

static void check_sc2721_invalid(void)
{
	struct fixture fixture;
	struct rtc_device *rtc;
	struct rtc_time tm = sample_date();

	prepare(&fixture, "sprd,sc2721-rtc", 0x14a5);
	rtc = probe(&fixture);
	assert(fixture.map.counter_writes == 0);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == -EINVAL);
	assert(rtc->ops->set_time(&fixture.pdev.dev, &tm) == 0);
	assert(*cell(&fixture.map, 0x250) == 0x16a5);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == 0);
	assert_date(&tm, 2026, 10, 15, 12, 34, 56);
	assert(fixture.map.power_status_reads == 0);
	assert(fixture.map.power_status_writes == 0);
	release(&fixture);
}

static void check_sc2721_alarm(void)
{
	struct fixture fixture;
	struct rtc_device *rtc;
	struct rtc_wkalrm alarm = {
		.enabled = 1,
		.time = { .tm_year = 119, .tm_mday = 2, .tm_sec = 5 },
	};

	prepare(&fixture, "sprd,sc2721-rtc", 0x43a5);
	*cell(&fixture.map, 0x230) = 0xffff;
	*cell(&fixture.map, 0x234) = 0xffff;
	*cell(&fixture.map, 0x260) = 41;
	rtc = probe(&fixture);
	assert(*cell(&fixture.map, 0x230) == 0x20);
	assert(*cell(&fixture.map, 0x234) == 0xffa0);
	assert(rtc->ops->set_alarm(&fixture.pdev.dev, &alarm) == 0);
	assert(*cell(&fixture.map, 0x230) == 0x30);
	assert(*cell(&fixture.map, 0x240) == 5);
	assert(*cell(&fixture.map, 0x24c) == 1);
	assert(*cell(&fixture.map, 0x260) == 41);
	assert(*cell(&fixture.map, 0x250) == 0x42a5);
	assert(rtc->ops->read_alarm(&fixture.pdev.dev, &alarm) == 0);
	assert(alarm.enabled && !alarm.pending);
	assert_date(&alarm.time, 2019, 1, 2, 0, 0, 5);
	*cell(&fixture.map, 0x234) |= 0x10;
	assert(fixture.pdev.dev.irq_thread(7, fixture.pdev.dev.irq_data) == 1);
	assert(!(*cell(&fixture.map, 0x230) & 0x10));
	assert(!(*cell(&fixture.map, 0x234) & 0x10));
	assert(rtc->irq_count == 1 && rtc->irq_events == 0xa0);
	assert(rtc->ops->alarm_irq_enable(&fixture.pdev.dev, 1) == 0);
	test_driver_sprd_rtc_driver->shutdown(&fixture.pdev);
	assert(!(*cell(&fixture.map, 0x230) & 0x10));
	assert(*cell(&fixture.map, 0x250) == 0x425a);
	release(&fixture);
}

static void check_sc2721_errors(void)
{
	struct fixture fixture;
	struct rtc_device *rtc;
	struct rtc_time tm = sample_date();

	prepare(&fixture, "sprd,sc2721-rtc", 0xa5);
	fixture.map.error_reg = 0x250;
	fixture.map.read_error = -EPROBE_DEFER;
	assert(test_driver_sprd_rtc_driver->probe(&fixture.pdev) ==
	       -EPROBE_DEFER);
	assert(fixture.pdev.dev.irq_thread == NULL);
	release(&fixture);

	prepare(&fixture, "sprd,sc2721-rtc", 0xa5);
	rtc = probe(&fixture);
	fixture.map.block_time_ack = true;
	*cell(&fixture.map, 0x234) = 0xf00;
	assert(rtc->ops->set_time(&fixture.pdev.dev, &tm) == -ETIMEDOUT);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == -EINVAL);
	fixture.map.block_time_ack = false;
	fixture.map.block_spg_ack = true;
	*cell(&fixture.map, 0x234) = 0x80;
	assert(rtc->ops->set_time(&fixture.pdev.dev, &tm) == -ETIMEDOUT);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == -EINVAL);
	fixture.map.block_spg_ack = false;
	fixture.map.error_reg = 0x254;
	fixture.map.write_error = -EIO;
	assert(rtc->ops->set_time(&fixture.pdev.dev, &tm) == -EIO);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == -EINVAL);
	fixture.map.write_error = 0;
	assert(rtc->ops->set_time(&fixture.pdev.dev, &tm) == 0);
	fixture.map.error_reg = 0x204;
	fixture.map.read_error = -EBUSY;
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == -EBUSY);
	fixture.map.read_error = 0;
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == 0);
	release(&fixture);
}

static void check_sc2720(void)
{
	struct fixture fixture;
	struct rtc_device *rtc;
	struct rtc_time tm;

	prepare(&fixture, "sprd,sc2720-rtc", 0xa7);
	rtc = probe(&fixture);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == 0);
	assert_date(&tm, 1980, 1, 1, 0, 0, 0);
	assert(fixture.map.power_status_reads == 1);
	assert(rtc->ops->alarm_irq_enable(&fixture.pdev.dev, 1) == 0);
	assert(*cell(&fixture.map, 0x250) == 0xa5);
	assert(rtc->ops->alarm_irq_enable(&fixture.pdev.dev, 0) == 0);
	assert(*cell(&fixture.map, 0x250) == 0xa7);
	release(&fixture);

	prepare(&fixture, "sprd,sc2720-rtc", 0xa7);
	*cell(&fixture.map, 0x25c) = 0x96;
	rtc = probe(&fixture);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == -EINVAL);
	tm = (struct rtc_time){ .tm_year = 80, .tm_mday = 2 };
	assert(rtc->ops->set_time(&fixture.pdev.dev, &tm) == 0);
	assert(fixture.map.power_status_writes == 2);
	assert((*cell(&fixture.map, 0x258) & 0xffff) == 0x6900);
	assert(*cell(&fixture.map, 0x20c) == 1);
	assert(rtc->ops->read_time(&fixture.pdev.dev, &tm) == 0);
	assert_date(&tm, 1980, 1, 2, 0, 0, 0);
	release(&fixture);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "sc2721-time"))
		check_sc2721_time();
	else if (!strcmp(argv[1], "sc2721-invalid"))
		check_sc2721_invalid();
	else if (!strcmp(argv[1], "sc2721-alarm"))
		check_sc2721_alarm();
	else if (!strcmp(argv[1], "sc2721-errors"))
		check_sc2721_errors();
	else if (!strcmp(argv[1], "sc2720"))
		check_sc2720();
	else
		assert(!"unknown RTC scenario");
	return 0;
}
