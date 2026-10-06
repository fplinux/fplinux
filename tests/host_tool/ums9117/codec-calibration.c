// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/regmap.h>

#include "ums9117-sc2720-calibration.h"

/* The fake models masked register writes and controlled status/I/O faults. */
struct regmap {
	unsigned int registers[32];
	unsigned int snapshot_error_register;
	unsigned int write_error_register;
	unsigned int status_reads;
	unsigned int writes;
	int status_error;
	int cdc2_restore_error;
	int cdc3_restore_error;
	bool dc_never_ready;
	bool unrelated_change;
};

struct device {
	unsigned int errors;
};

static unsigned int *register_value(struct regmap *map, unsigned int reg)
{
	assert(reg >= 0x0700 && reg <= 0x077c && !(reg & 3));
	return &map->registers[(reg - 0x0700) / 4];
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value)
{
	if (reg == map->snapshot_error_register)
		return -5;
	if (reg == 0x0764) {
		map->status_reads++;
		if (!map->unrelated_change) {
			/* Changes outside the route masks must survive restoration. */
			*register_value(map, 0x0724) |= 0x4000;
			*register_value(map, 0x0728) ^= 0x0001;
			map->unrelated_change = true;
		}
		if (map->status_error)
			return map->status_error;
		*value = map->dc_never_ready ? 0 : 0x1430;
		return 0;
	}
	*value = *register_value(map, reg);
	return 0;
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int value)
{
	map->writes++;
	if (reg == map->write_error_register)
		return -6;
	*register_value(map, reg) = value;
	return 0;
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value)
{
	unsigned int *stored = register_value(map, reg);

	map->writes++;
	if (reg == 0x0724 && mask == 0x3fff && value == 0xa155 &&
	    map->cdc2_restore_error)
		return map->cdc2_restore_error;
	if (reg == 0x0728 && mask == 0x13c0 && value == 0xb3a5 &&
	    map->cdc3_restore_error)
		return map->cdc3_restore_error;
	*stored = (*stored & ~mask) | (value & mask);
	return 0;
}

int regmap_write_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		      unsigned int value)
{
	return regmap_update_bits(map, reg, mask, value);
}

/* Delays and logging replace host-inapplicable kernel boundaries only. */
void usleep_range(unsigned long minimum, unsigned long maximum)
{
	(void)minimum;
	(void)maximum;
}

void msleep(unsigned int milliseconds)
{
	(void)milliseconds;
}

void dev_err(struct device *dev, const char *format, ...)
{
	(void)format;
	dev->errors++;
}

static struct regmap new_regmap(void)
{
	struct regmap map = { 0 };

	*register_value(&map, 0x0700) = 0x0042;
	*register_value(&map, 0x0724) = 0xa155;
	*register_value(&map, 0x0728) = 0xb3a5;
	*register_value(&map, 0x072c) = 0xab20;
	return map;
}

static void success_preserves_routes_and_unrelated_changes(void)
{
	struct device dev = { 0 };
	struct regmap map = new_regmap();

	assert(ums9117_sc2720_calibrate_headphones(&dev, &map) == 0);
	assert(*register_value(&map, 0x0724) == 0xe155);
	assert(*register_value(&map, 0x0728) == 0xb3a4);
	assert(*register_value(&map, 0x072c) == 0xabff);
	assert(*register_value(&map, 0x0700) == 0x1842);
}

static void snapshot_failure_has_no_register_effects(void)
{
	const unsigned int registers[] = { 0x0724, 0x0728 };
	unsigned int index;

	for (index = 0; index < 2; index++) {
		struct device dev = { 0 };
		struct regmap map = new_regmap();
		struct regmap before = map;

		map.snapshot_error_register = registers[index];
		assert(ums9117_sc2720_calibrate_headphones(&dev, &map) == -5);
		assert(memcmp(map.registers, before.registers,
			      sizeof(map.registers)) == 0);
		assert(map.writes == 0);
	}
}

static void operation_failure_restores_both_routes(void)
{
	struct device dev = { 0 };
	struct regmap map = new_regmap();

	map.write_error_register = 0x0754;
	assert(ums9117_sc2720_calibrate_headphones(&dev, &map) == -6);
	assert(*register_value(&map, 0x0724) == 0xa155);
	assert(*register_value(&map, 0x0728) == 0xb3a5);
	assert(*register_value(&map, 0x072c) == 0xabff);
}

static void status_timeout_is_bounded_and_restores_routes(void)
{
	struct device dev = { 0 };
	struct regmap map = new_regmap();

	map.dc_never_ready = true;
	assert(ums9117_sc2720_calibrate_headphones(&dev, &map) == -110);
	assert(map.status_reads > 0 && map.status_reads <= 20);
	assert(*register_value(&map, 0x0724) == 0xe155);
	assert(*register_value(&map, 0x0728) == 0xb3a4);
}

static void primary_error_survives_restore_failure(void)
{
	struct device dev = { 0 };
	struct regmap map = new_regmap();

	map.status_error = -5;
	map.cdc2_restore_error = -11;
	assert(ums9117_sc2720_calibrate_headphones(&dev, &map) == -5);
	assert(*register_value(&map, 0x0728) == 0xb3a4);
	assert(dev.errors > 0);
}

static void first_restore_error_wins_and_other_route_is_attempted(void)
{
	struct device dev = { 0 };
	struct regmap map = new_regmap();

	map.cdc2_restore_error = -11;
	assert(ums9117_sc2720_calibrate_headphones(&dev, &map) == -11);
	assert(*register_value(&map, 0x0724) == 0xc000);
	assert(*register_value(&map, 0x0728) == 0xb3a4);

	map = new_regmap();
	map.cdc2_restore_error = -11;
	map.cdc3_restore_error = -6;
	assert(ums9117_sc2720_calibrate_headphones(&dev, &map) == -11);
	assert(*register_value(&map, 0x0724) == 0xc000);
	assert(*register_value(&map, 0x0728) == 0xa024);

	map = new_regmap();
	map.cdc3_restore_error = -6;
	assert(ums9117_sc2720_calibrate_headphones(&dev, &map) == -6);
	assert(*register_value(&map, 0x0724) == 0xe155);
	assert(*register_value(&map, 0x0728) == 0xa024);
}

int main(void)
{
	success_preserves_routes_and_unrelated_changes();
	snapshot_failure_has_no_register_effects();
	operation_failure_restores_both_routes();
	status_timeout_is_bounded_and_restores_routes();
	primary_error_survives_restore_failure();
	first_restore_error_wins_and_other_route_is_attempted();
	puts("codec calibration host component cases passed");
	return 0;
}
