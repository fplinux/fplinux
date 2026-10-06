// SPDX-License-Identifier: GPL-2.0-only
#include "charger-compat/charger-kernel.h"
#include <assert.h>
#include <stdio.h>

/* The fake replaces ADI access and class registration, not driver decoding. */
struct regmap {
	unsigned int status;
	unsigned int reads;
	unsigned int writes;
	int error;
};
static struct power_supply registered_supply;

struct regmap *dev_get_regmap(struct device *dev, const char *name)
{
	(void)name;
	return dev->regmap;
}

struct fwnode_handle *dev_fwnode(struct device *dev)
{
	(void)dev;
	return NULL;
}

void *power_supply_get_drvdata(struct power_supply *supply)
{
	return supply->drv_data;
}

struct power_supply *
devm_power_supply_register(struct device *dev,
			   const struct power_supply_desc *desc,
			   const struct power_supply_config *config)
{
	(void)dev;
	registered_supply.desc = desc;
	registered_supply.drv_data = config->drv_data;
	return &registered_supply;
}

int regmap_read(struct regmap *regmap, unsigned int offset, unsigned int *value)
{
	assert(offset == 0xe14);
	regmap->reads++;
	if (regmap->error)
		return regmap->error;
	*value = regmap->status;
	return 0;
}

int regmap_write(struct regmap *regmap, unsigned int offset, unsigned int value)
{
	(void)offset;
	(void)value;
	regmap->writes++;
	return 0;
}

int regmap_update_bits(struct regmap *regmap, unsigned int offset,
		       unsigned int mask, unsigned int value)
{
	(void)mask;
	return regmap_write(regmap, offset, value);
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

static int read_property(struct regmap *regmap,
			 enum power_supply_property property,
			 union power_supply_propval *value)
{
	unsigned int reads = regmap->reads;
	int ret = registered_supply.desc->get_property(&registered_supply,
						       property, value);

	assert(regmap->reads == reads + 1);
	assert(regmap->writes == 0);
	return ret;
}

static void test_status_snapshots(struct regmap *regmap)
{
	static const struct {
		unsigned int status;
		int online;
		enum power_supply_usb_type usb_type;
	} cases[] = {
		/* Literal SC2720 charger-status examples. */
		{ 0x0000, 0, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0008, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0088, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0028, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0048, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0880, 0, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0820, 0, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0840, 0, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0808, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0888, 1, POWER_SUPPLY_USB_TYPE_SDP },
		{ 0x0828, 1, POWER_SUPPLY_USB_TYPE_CDP },
		{ 0x0848, 1, POWER_SUPPLY_USB_TYPE_DCP },
		{ 0x0868, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x08a8, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x08c8, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x08e8, 1, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		/* Other status/control fields cannot change classification. */
		{ 0x288a, 1, POWER_SUPPLY_USB_TYPE_SDP },
		{ 0x282a, 1, POWER_SUPPLY_USB_TYPE_CDP },
		{ 0x284a, 1, POWER_SUPPLY_USB_TYPE_DCP },
	};
	union power_supply_propval value;
	size_t index;

	for (index = 0; index < ARRAY_SIZE(cases); index++) {
		regmap->status = cases[index].status;
		assert(read_property(regmap, POWER_SUPPLY_PROP_ONLINE,
				     &value) == 0);
		assert(value.intval == cases[index].online);
		assert(read_property(regmap, POWER_SUPPLY_PROP_USB_TYPE,
				     &value) == 0);
		assert(value.intval == (int)cases[index].usb_type);
	}
}

static void test_errors_and_read_only_interface(struct regmap *regmap)
{
	const struct power_supply_desc *desc = registered_supply.desc;
	union power_supply_propval value = { .intval = 1234 };
	unsigned int reads;
	size_t index;
	int online_count = 0;
	int usb_type_count = 0;

	/* The class must advertise each observed property and all four types. */
	for (index = 0; index < desc->num_properties; index++) {
		online_count += desc->properties[index] ==
				POWER_SUPPLY_PROP_ONLINE;
		usb_type_count += desc->properties[index] ==
				  POWER_SUPPLY_PROP_USB_TYPE;
	}
	assert(online_count == 1 && usb_type_count == 1);
	assert(desc->usb_types == 0x0f);
	assert(desc->set_property == NULL);
	assert(desc->property_is_writeable == NULL);

	regmap->error = -EIO;
	assert(read_property(regmap, POWER_SUPPLY_PROP_ONLINE, &value) == -EIO);
	assert(value.intval == 1234);
	assert(read_property(regmap, POWER_SUPPLY_PROP_USB_TYPE, &value) ==
	       -EIO);
	assert(value.intval == 1234);

	reads = regmap->reads;
	assert(desc->get_property(&registered_supply,
				  POWER_SUPPLY_PROP_VOLTAGE_NOW,
				  &value) == -EINVAL);
	assert(regmap->reads == reads);
	assert(value.intval == 1234);
	assert(regmap->writes == 0);
}

int main(void)
{
	struct regmap regmap = {};
	struct device parent = { .regmap = &regmap };
	struct platform_device device = { .dev = { .parent = &parent } };

	assert(charger_host_driver->probe(&device) == 0);
	assert(regmap.writes == 0);
	test_status_snapshots(&regmap);
	test_errors_and_read_only_interface(&regmap);
	puts("SC2720 charger host callback cases passed");
	return 0;
}
