/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CHARGER_HOST_KERNEL_H
#define FPLINUX_CHARGER_HOST_KERNEL_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define BIT(bit) (1UL << (bit))
#define GENMASK(high, low) ((~0UL << (low)) & (~0UL >> (63 - (high))))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define EPROBE_DEFER 517

struct regmap;
struct fwnode_handle;
struct device {
	struct device *parent;
	struct regmap *regmap;
};
struct platform_device {
	struct device dev;
};
struct of_device_id {
	const char *compatible;
};
struct platform_driver {
	int (*probe)(struct platform_device *pdev);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
};

enum power_supply_property {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};
enum power_supply_type {
	POWER_SUPPLY_TYPE_UNKNOWN,
};
enum power_supply_usb_type {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_DCP,
	POWER_SUPPLY_USB_TYPE_CDP,
};
union power_supply_propval {
	int intval;
};
struct power_supply;
struct power_supply_desc {
	const char *name;
	enum power_supply_type type;
	uint32_t usb_types;
	const enum power_supply_property *properties;
	size_t num_properties;
	int (*get_property)(struct power_supply *supply,
			    enum power_supply_property property,
			    union power_supply_propval *value);
	int (*set_property)(struct power_supply *supply,
			    enum power_supply_property property,
			    const union power_supply_propval *value);
	int (*property_is_writeable)(struct power_supply *supply,
				     enum power_supply_property property);
};
struct power_supply_config {
	struct fwnode_handle *fwnode;
	void *drv_data;
};
struct power_supply {
	const struct power_supply_desc *desc;
	void *drv_data;
};

struct regmap *dev_get_regmap(struct device *dev, const char *name);
struct fwnode_handle *dev_fwnode(struct device *dev);
void *power_supply_get_drvdata(struct power_supply *supply);
struct power_supply *
devm_power_supply_register(struct device *dev,
			   const struct power_supply_desc *desc,
			   const struct power_supply_config *config);
int regmap_read(struct regmap *regmap, unsigned int offset,
		unsigned int *value);
int regmap_write(struct regmap *regmap, unsigned int offset,
		 unsigned int value);
int regmap_update_bits(struct regmap *regmap, unsigned int offset,
		       unsigned int mask, unsigned int value);
int dev_err_probe(struct device *dev, int error, const char *format, ...);

#define PTR_ERR_OR_ZERO(ptr) ((intptr_t)(ptr) < 0 ? (int)(intptr_t)(ptr) : 0)
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_LICENSE(license)
/* Expose registration only in the linked host fixture. */
#define module_platform_driver(driver) \
	struct platform_driver *charger_host_driver = &(driver)

extern struct platform_driver *charger_host_driver;

#endif
