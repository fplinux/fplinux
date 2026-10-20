// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <sc27xx-test-kernel.h>

extern struct platform_driver *test_driver_sc2721_regulator_driver;
extern struct platform_driver *test_driver_sc27xx_regulator_driver;

/* The host substitutes the regmap and managed-resource subsystem boundaries. */
struct regmap {
	u32 cells[0x1000];
	int write_error;
	int read_error;
	u32 read_error_reg;
	int update_error;
	u32 update_error_reg;
	unsigned int writes;
	u32 last_update_reg;
	unsigned int sdcore_pd_updates;
};

static unsigned int registration_attempts;
static unsigned int registration_failure;
static int registration_error;
static unsigned int released_resources;
static u32 required_unlock_reg;
static u32 required_unlock_value;

const void *device_get_match_data(const struct device *dev)
{
	return dev->match_data;
}

struct regmap *dev_get_regmap(struct device *dev, const char *name)
{
	assert(name == NULL);
	return dev ? dev->regmap : NULL;
}

int dev_err_probe(const struct device *dev, int error, const char *format, ...)
{
	assert(dev != NULL);
	assert(format != NULL);
	return error;
}

struct regulator_dev *
devm_regulator_register(struct device *dev, const struct regulator_desc *desc,
			const struct regulator_config *config)
{
	struct regulator_dev *rdev;

	registration_attempts++;
	if (registration_attempts == registration_failure)
		return ERR_PTR(registration_error);

	assert(config->dev == dev);
	assert(config->regmap == dev->parent->regmap);
	if (required_unlock_reg)
		assert(config->regmap->cells[required_unlock_reg] ==
		       required_unlock_value);
	assert(dev->num_resources < ARRAY_SIZE(dev->resources));
	rdev = calloc(1, sizeof(*rdev));
	assert(rdev != NULL);
	rdev->desc = desc;
	rdev->config = *config;
	dev->resources[dev->num_resources++] = rdev;
	return rdev;
}

struct regmap *rdev_get_regmap(struct regulator_dev *rdev)
{
	return rdev->config.regmap;
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int value)
{
	assert(reg < ARRAY_SIZE(map->cells));
	map->writes++;
	if (map->write_error)
		return map->write_error;
	map->cells[reg] = value;
	return 0;
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *value)
{
	assert(reg < ARRAY_SIZE(map->cells));
	if (map->read_error &&
	    (!map->read_error_reg || map->read_error_reg == reg))
		return map->read_error;
	*value = map->cells[reg];
	return 0;
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int value)
{
	assert(reg < ARRAY_SIZE(map->cells));
	map->last_update_reg = reg;
	if (reg == 0xd00)
		map->sdcore_pd_updates++;
	if (map->update_error &&
	    (!map->update_error_reg || map->update_error_reg == reg))
		return map->update_error;
	map->cells[reg] = (map->cells[reg] & ~mask) | (value & mask);
	return 0;
}

int regulator_enable_regmap(struct regulator_dev *rdev)
{
	return regmap_update_bits(
		rdev_get_regmap(rdev), rdev->desc->enable_reg,
		rdev->desc->enable_mask,
		rdev->desc->enable_is_inverted ? 0 : rdev->desc->enable_mask);
}

int regulator_disable_regmap(struct regulator_dev *rdev)
{
	return regmap_update_bits(
		rdev_get_regmap(rdev), rdev->desc->enable_reg,
		rdev->desc->enable_mask,
		rdev->desc->enable_is_inverted ? rdev->desc->enable_mask : 0);
}

int regulator_is_enabled_regmap(struct regulator_dev *rdev)
{
	u32 value;
	int ret = regmap_read(rdev_get_regmap(rdev), rdev->desc->enable_reg,
			      &value);

	if (ret)
		return ret;
	return rdev->desc->enable_is_inverted ?
		       !(value & rdev->desc->enable_mask) :
		       !!(value & rdev->desc->enable_mask);
}

int regulator_list_voltage_linear(struct regulator_dev *rdev,
				  unsigned int selector)
{
	if (selector >= rdev->desc->n_voltages)
		return -EINVAL;
	return rdev->desc->min_uV + rdev->desc->uV_step * selector;
}

int regulator_map_voltage_linear(struct regulator_dev *rdev, int min_uV,
				 int max_uV)
{
	int selector = 0;
	int voltage;

	if (min_uV > rdev->desc->min_uV)
		selector = (min_uV - rdev->desc->min_uV + rdev->desc->uV_step -
			    1) /
			   rdev->desc->uV_step;
	voltage = regulator_list_voltage_linear(rdev, selector);
	return voltage < 0 || voltage > max_uV ? -EINVAL : selector;
}

int regulator_get_voltage_sel_regmap(struct regulator_dev *rdev)
{
	u32 value;
	int ret = regmap_read(rdev_get_regmap(rdev), rdev->desc->vsel_reg,
			      &value);

	return ret ? ret : (int)(value & rdev->desc->vsel_mask);
}

int regulator_set_voltage_sel_regmap(struct regulator_dev *rdev,
				     unsigned int selector)
{
	return regmap_update_bits(rdev_get_regmap(rdev), rdev->desc->vsel_reg,
				  rdev->desc->vsel_mask, selector);
}

static void release_resources(struct platform_device *pdev)
{
	while (pdev->dev.num_resources) {
		free(pdev->dev.resources[--pdev->dev.num_resources]);
		released_resources++;
	}
}

static int probe(struct platform_driver *driver, struct platform_device *pdev)
{
	int ret = driver->probe(pdev);

	/* Failed probes unwind devres in the driver framework, outside the driver. */
	if (ret)
		release_resources(pdev);
	return ret;
}

static void match_variant(struct platform_driver *driver,
			  struct platform_device *pdev, const char *compatible)
{
	const struct of_device_id *match = driver->driver.of_match_table;

	for (; match->compatible; match++) {
		if (!strcmp(match->compatible, compatible)) {
			pdev->dev.match_data = match->data;
			assert(pdev->dev.match_data != NULL);
			return;
		}
	}
	assert(!"regulator compatible is unavailable");
}

static struct regulator_dev *find_regulator(struct platform_device *pdev,
					    const char *name)
{
	unsigned int i;

	for (i = 0; i < pdev->dev.num_resources; i++) {
		if (!strcmp(pdev->dev.resources[i]->desc->name, name))
			return pdev->dev.resources[i];
	}
	assert(!"regulator is unavailable");
	return NULL;
}

static void check_sc2721(void)
{
	struct platform_driver *driver = test_driver_sc2721_regulator_driver;
	struct regmap map = {};
	struct device parent = { .regmap = &map };
	struct platform_device pdev = { .dev.parent = &parent };
	struct regulator_dev *core, *sdcore, *sdio;
	const struct regulator_desc *desc;

	assert(driver->driver.probe_type == 0);
	required_unlock_reg = 0xefc;
	required_unlock_value = 0x6e7f;
	match_variant(driver, &pdev, "sprd,sc2721-regulator");
	assert(probe(driver, &pdev) == 0);
	assert(map.writes == 1);
	assert(map.cells[0xefc] == 0x6e7f);
	assert(pdev.dev.num_resources == 5);
	core = find_regulator(&pdev, "vddcore");
	desc = core->desc;
	assert(!strcmp(desc->of_match, "dcdc@1"));
	assert(desc->ops->enable == NULL && desc->ops->disable == NULL);
	assert(desc->ops->set_voltage_sel == NULL);
	assert(desc->ops->get_voltage(core) == 400000);
	map.cells[0xc54] = 0xa5ff;
	assert(desc->ops->get_voltage(core) == 1996875);
	map.cells[0xc1c] = 0x20;
	assert(desc->ops->is_enabled(core) == 0);
	assert(desc->ops->get_voltage(core) == -EIO);
	map.cells[0xc1c] = 0;
	sdcore = find_regulator(&pdev, "vddsdcore");
	sdio = find_regulator(&pdev, "vddsdio");
	desc = sdcore->desc;
	assert(!strcmp(desc->of_match, "ldo@11"));
	assert(!strcmp(desc->regulators_node, "regulators"));
	assert(desc->of_match_full_name);
	assert(desc->id == 11);
	assert(desc->enable_reg == 0xcf4 && desc->enable_mask == 1);
	assert(desc->vsel_reg == 0xcf8 && desc->vsel_mask == 0xff);
	assert(desc->enable_is_inverted);
	assert(desc->n_voltages == 256);
	assert(desc->min_uV == 1200000 && desc->uV_step == 10000);
	assert(desc->ops->list_voltage(sdcore, 0) == 1200000);
	assert(desc->ops->list_voltage(sdcore, 255) == 3750000);
	assert(desc->ops->list_voltage(sdcore, 256) == -EINVAL);
	assert(desc->ops->map_voltage(sdcore, 1800000, 1800000) == 60);
	map.cells[0xcf8] = 0xa500;
	assert(desc->ops->set_voltage_sel(sdcore, 255) == 0);
	assert(map.cells[0xcf8] == 0xa5ff);
	assert(desc->ops->get_voltage_sel(sdcore) == 255);
	map.cells[0xcf4] = 0x81;
	assert(desc->ops->enable(sdcore) == 0);
	assert(map.cells[0xcf4] == 0x80);
	assert(desc->ops->is_enabled(sdcore) == 1);
	assert(desc->ops->disable(sdcore) == 0);
	assert(map.cells[0xcf4] == 0x81);
	assert(desc->ops->is_enabled(sdcore) == 0);

	desc = sdio->desc;
	assert(!strcmp(desc->of_match, "ldo@12"));
	assert(!strcmp(desc->regulators_node, "regulators"));
	assert(desc->of_match_full_name);
	assert(desc->id == 12);
	assert(desc->enable_reg == 0xd00 && desc->enable_mask == 1);
	assert(desc->vsel_reg == 0xd04 && desc->vsel_mask == 0xff);
	assert(desc->enable_is_inverted);
	assert(desc->n_voltages == 256);
	assert(desc->min_uV == 1200000 && desc->uV_step == 10000);
	assert(desc->ops->list_voltage(sdio, 255) == 3750000);
	release_resources(&pdev);
}

static void check_sc2721_connectivity(void)
{
	struct platform_driver *driver = test_driver_sc2721_regulator_driver;
	struct regmap map = {};
	struct device parent = { .regmap = &map };
	struct platform_device pdev = { .dev.parent = &parent };
	struct regulator_dev *wifipa, *con;
	const struct regulator_ops *ops;

	map.cells[0xd10] = 0x81;
	map.cells[0xd14] = 0xa500;
	map.cells[0xd34] = 0x81;
	map.cells[0xd38] = 0xa580;
	match_variant(driver, &pdev, "sprd,sc2721-regulator");
	assert(probe(driver, &pdev) == 0);
	assert(map.writes == 1 && map.cells[0xefc] == 0x6e7f);
	assert(map.cells[0xd10] == 0x81 && map.cells[0xd14] == 0xa500);
	assert(map.cells[0xd34] == 0x81 && map.cells[0xd38] == 0xa580);
	wifipa = find_regulator(&pdev, "vddwifipa");
	assert(!strcmp(wifipa->desc->of_match, "ldo@14"));
	assert(wifipa->desc->id == 14 && wifipa->desc->of_match_full_name);
	ops = wifipa->desc->ops;
	assert(ops->list_voltage(wifipa, 0) == 1200000);
	assert(ops->list_voltage(wifipa, 255) == 3750000);
	assert(ops->list_voltage(wifipa, 256) == -EINVAL);
	assert(ops->map_voltage(wifipa, 3000000, 3000000) == 180);
	assert(ops->set_voltage_sel(wifipa, 180) == 0);
	assert(map.cells[0xd14] == 0xa5b4);
	assert(ops->get_voltage_sel(wifipa) == 180);
	assert(ops->enable(wifipa) == 0 && map.cells[0xd10] == 0x80);
	assert(ops->is_enabled(wifipa) == 1);
	assert(ops->disable(wifipa) == 0 && map.cells[0xd10] == 0x81);

	con = find_regulator(&pdev, "vddcon");
	assert(!strcmp(con->desc->of_match, "ldo@18"));
	assert(con->desc->id == 18 && con->desc->of_match_full_name);
	ops = con->desc->ops;
	assert(ops->list_voltage(con, 0) == 1100000);
	assert(ops->list_voltage(con, 80) == 1600000);
	assert(ops->list_voltage(con, 81) == -EINVAL);
	assert(ops->map_voltage(con, 1200000, 1200000) == 16);
	assert(ops->map_voltage(con, 1606250, 1606250) == -EINVAL);
	assert(ops->set_voltage_sel(con, 80) == 0);
	assert(map.cells[0xd38] == 0xa5d0);
	assert(ops->get_voltage_sel(con) == 80);
	assert(ops->enable(con) == 0 && map.cells[0xd34] == 0x80);
	assert(ops->is_enabled(con) == 1);
	assert(ops->disable(con) == 0 && map.cells[0xd34] == 0x81);
	map.update_error = -ETIMEDOUT;
	assert(ops->enable(con) == -ETIMEDOUT);
	assert(map.cells[0xd34] == 0x81);
	map.read_error = -EIO;
	assert(ops->get_voltage_sel(con) == -EIO);
	assert(ops->is_enabled(con) == -EIO);
	release_resources(&pdev);
}

static void check_sc2731(void)
{
	struct platform_driver *driver = test_driver_sc27xx_regulator_driver;
	struct regmap map = {};
	struct device parent = { .regmap = &map };
	struct platform_device pdev = { .dev.parent = &parent };
	struct regulator_dev *cpu;
	const struct regulator_desc *desc;

	assert(driver->driver.probe_type == PROBE_PREFER_ASYNCHRONOUS);
	required_unlock_reg = 0xf0c;
	required_unlock_value = 0x6e7f;
	match_variant(driver, &pdev, "sprd,sc2731-regulator");
	assert(probe(driver, &pdev) == 0);
	assert(map.writes == 1);
	assert(map.cells[0xf0c] == 0x6e7f);
	cpu = find_regulator(&pdev, "BUCK_CPU0");
	desc = cpu->desc;
	assert(!strcmp(desc->of_match, "BUCK_CPU0"));
	assert(desc->enable_reg == 0xc28 && desc->enable_mask == 0x10);
	assert(desc->vsel_reg == 0xc54 && desc->vsel_mask == 0x1ff);
	assert(desc->enable_is_inverted);
	assert(desc->n_voltages == 512);
	assert(desc->min_uV == 400000 && desc->uV_step == 3125);
	assert(desc->ops->list_voltage(cpu, 511) == 1996875);
	release_resources(&pdev);
}

static void check_sc2720_fixed(void)
{
	struct platform_driver *driver = test_driver_sc27xx_regulator_driver;
	struct regmap map = {};
	struct device parent = { .regmap = &map };
	struct platform_device pdev = { .dev.parent = &parent };
	struct regulator_dev *sdcore, *sdio, *usb;
	const struct regulator_ops *ops;
	unsigned int pd_updates;

	match_variant(driver, &pdev, "sprd,sc2720-regulator");
	assert(probe(driver, &pdev) == 0);
	assert(map.writes == 0);
	sdcore = find_regulator(&pdev, "LDO_SDCORE");
	sdio = find_regulator(&pdev, "LDO_SDIO");
	usb = find_regulator(&pdev, "LDO_USB33");
	assert(sdcore->desc->fixed_uV == 3000000);
	assert(sdcore->desc->n_voltages == 1);
	assert(sdcore->desc->enable_time == 0);
	assert(sdio->desc->fixed_uV == 3000000);
	assert(sdio->desc->enable_time == 300000);
	assert(usb->desc->fixed_uV == 3300000);
	assert(usb->desc->enable_time == 0);

	ops = sdcore->desc->ops;
	map.cells[0xd00] = 0x81;
	map.cells[0xd04] = 0xab80;
	assert(ops->enable(sdcore) == 0);
	assert(map.cells[0xd04] == 0xabd0);
	assert(map.cells[0xd00] == 0x80);
	assert(ops->is_enabled(sdcore) == 1);
	map.cells[0xd04] = 0xabcf;
	assert(ops->is_enabled(sdcore) == -ERANGE);
	assert(ops->disable(sdcore) == 0);
	assert(ops->is_enabled(sdcore) == 0);

	map.update_error = -EIO;
	pd_updates = map.sdcore_pd_updates;
	assert(ops->enable(sdcore) == -EIO);
	assert(map.last_update_reg == 0xd04);
	assert(map.sdcore_pd_updates == pd_updates);
	assert(map.cells[0xd00] == 0x81);
	map.update_error = 0;
	map.update_error_reg = 0xd00;
	map.update_error = -EBUSY;
	assert(ops->enable(sdcore) == -EBUSY);
	assert(map.last_update_reg == 0xd00);
	assert(map.cells[0xd04] == 0xabd0);
	assert(map.cells[0xd00] == 0x81);
	map.update_error = 0;
	map.read_error = -ETIMEDOUT;
	assert(ops->is_enabled(sdcore) == -ETIMEDOUT);
	map.cells[0xd00] = 0x80;
	map.read_error_reg = 0xd04;
	assert(ops->is_enabled(sdcore) == -ETIMEDOUT);
	map.read_error = 0;

	map.cells[0xd0c] = 1;
	map.cells[0xd10] = 0x8000;
	assert(sdio->desc->ops->enable(sdio) == 0);
	assert(map.cells[0xd10] == 0x806f);
	assert(map.cells[0xd0c] == 0);
	assert(sdio->desc->ops->is_enabled(sdio) == 1);
	map.cells[0xd30] = 1;
	assert(usb->desc->ops->enable(usb) == 0);
	assert(map.cells[0xd34] == 0x60);
	assert(map.cells[0xd30] == 0);
	release_resources(&pdev);
}

static void check_probe_errors(void)
{
	struct platform_driver *driver = test_driver_sc2721_regulator_driver;
	struct regmap map = {};
	struct device parent = { .regmap = &map };
	struct platform_device pdev = { .dev.parent = &parent };

	assert(probe(driver, &pdev) == -EINVAL);
	assert(map.writes == 0 && registration_attempts == 0);
	match_variant(driver, &pdev, "sprd,sc2721-regulator");
	parent.regmap = NULL;
	assert(probe(driver, &pdev) == -ENODEV);
	assert(map.writes == 0 && registration_attempts == 0);
	parent.regmap = &map;
	map.cells[0xc1c] = 0x20;
	assert(probe(driver, &pdev) == -EIO);
	assert(map.writes == 0 && registration_attempts == 0);
	map.cells[0xc1c] = 0;
	map.read_error = -ETIMEDOUT;
	assert(probe(driver, &pdev) == -ETIMEDOUT);
	assert(map.writes == 0 && registration_attempts == 0);
	map.read_error = 0;
	map.write_error = -ETIMEDOUT;
	assert(probe(driver, &pdev) == -ETIMEDOUT);
	assert(registration_attempts == 0);
	map.write_error = -EPROBE_DEFER;
	assert(probe(driver, &pdev) == -EPROBE_DEFER);
	map.write_error = 0;
	registration_failure = 2;
	registration_error = -EPROBE_DEFER;
	assert(probe(driver, &pdev) == -EPROBE_DEFER);
	assert(registration_attempts == 2);
	assert(pdev.dev.num_resources == 0);
	assert(released_resources == 1);

	registration_failure = 3;
	registration_error = -ENOMEM;
	assert(probe(driver, &pdev) == -ENOMEM);
	assert(registration_attempts == 3);
	assert(pdev.dev.num_resources == 0);
	assert(released_resources == 1);

	registration_failure = 0;
	assert(probe(driver, &pdev) == 0);
	assert(pdev.dev.num_resources == 5);
	release_resources(&pdev);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "sc2721"))
		check_sc2721();
	else if (!strcmp(argv[1], "sc2721-connectivity"))
		check_sc2721_connectivity();
	else if (!strcmp(argv[1], "sc2731"))
		check_sc2731();
	else if (!strcmp(argv[1], "sc2720-fixed"))
		check_sc2720_fixed();
	else if (!strcmp(argv[1], "probe-errors"))
		check_probe_errors();
	else
		assert(!"unknown regulator scenario");
	return 0;
}
