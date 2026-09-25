// SPDX-License-Identifier: GPL-2.0-only
/*
 * UMS9117 fixed-command read-only NANDC driver.
 *
 * The driver deliberately exposes no MTD, SPI, arbitrary-command, program or
 * erase path.  The first open identifies the fitted chip from its READ_ID
 * bytes through a fixed table and keeps the result for the device lifetime.
 * Each explicit read checks the NAND identity and feature state before
 * returning bytes, then restores the controller's owned platform state.
 * Registering the device does not activate or identify the NAND.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>

#define UMS9117_NANDC_MMIO_BYTES 0x1000U
#define UMS9117_NANDC_PIN_WINDOW_BYTES 0x0044U

#define UMS9117_NANDC_START 0x000U
#define UMS9117_NANDC_CFG0 0x004U
#define UMS9117_NANDC_CFG1 0x008U
#define UMS9117_NANDC_CFG2 0x00cU
#define UMS9117_NANDC_INT 0x010U
#define UMS9117_NANDC_TIMING0 0x014U
#define UMS9117_NANDC_TIMING1 0x018U
#define UMS9117_NANDC_TIMEOUT 0x034U
#define UMS9117_NANDC_CFG3 0x038U
#define UMS9117_NANDC_AXIM_STS 0x03cU
#define UMS9117_NANDC_POLY0 0x0b0U
#define UMS9117_NANDC_POLY1 0x0b4U
#define UMS9117_NANDC_POLY2 0x0b8U
#define UMS9117_NANDC_POLY3 0x0bcU
#define UMS9117_NANDC_PHY_CFG 0x0dcU
#define UMS9117_NANDC_DLL0 0x0e0U
#define UMS9117_NANDC_DLL1 0x0e4U
#define UMS9117_NANDC_DLL2 0x0e8U
#define UMS9117_NANDC_DLL 0x0ecU
#define UMS9117_NANDC_CFG4 0x0f8U
#define UMS9117_NANDC_SPI_CLK_CFG 0x180U
#define UMS9117_NANDC_SPI_WP_HOLD 0x188U
#define UMS9117_NANDC_SPI_STATUS 0x194U
#define UMS9117_NANDC_SPI_FEATURE 0x19cU
#define UMS9117_NANDC_MAIN_ADDR_HIGH 0x200U
#define UMS9117_NANDC_MAIN_ADDR_LOW 0x204U
#define UMS9117_NANDC_SPARE_ADDR_HIGH 0x208U
#define UMS9117_NANDC_SPARE_ADDR_LOW 0x20cU
#define UMS9117_NANDC_STATUS_ADDR_HIGH 0x210U
#define UMS9117_NANDC_STATUS_ADDR_LOW 0x214U
#define UMS9117_NANDC_INSTRUCTION_RAM 0x220U

#define UMS9117_NANDC_AP_AHB_GATE BIT(9)
#define UMS9117_NANDC_AP_AHB_RESET BIT(7)
#define UMS9117_NANDC_AON_APB_EB0 0x000U
#define UMS9117_NANDC_AON_APB_EB2 0x0b0U
#define UMS9117_NANDC_AON_APB_SHARED_PIN_GATE0 BIT(20)
#define UMS9117_NANDC_AON_APB_SHARED_PIN_GATE2 BIT(12)
#define UMS9117_NANDC_AON_APB_ARM_BOOT_MODE 0x02cU
#define UMS9117_NANDC_ARM_BOOT_MODE_MASK GENMASK(1, 0)
#define UMS9117_NANDC_ARM_BOOT_MODE_EXPECTED 2U

#define UMS9117_NANDC_CLOCK_ECC_MASK GENMASK(1, 0)
#define UMS9117_NANDC_CLOCK_ECC_VALUE 2U
#define UMS9117_NANDC_CLOCK_2X_VALUE 3U
#define UMS9117_NANDC_PIN_CONTROL_MASK BIT(12)
#define UMS9117_NANDC_PIN_CONTROL_VALUE BIT(12)

#define UMS9117_NANDC_CFG0_SPI_ONLY 2U
#define UMS9117_NANDC_CFG1_SPI 0x00003000U
#define UMS9117_NANDC_CFG4_SPI_ENABLE BIT(1)

#define UMS9117_NANDC_INT_DONE_RAW BIT(24)
#define UMS9117_NANDC_INT_FATAL_RAW GENMASK(31, 25)
/* The SoC specification defines AUTO_POLL_TIMEOUT at raw 31 / clear 15. */
#define UMS9117_NANDC_INT_CLEAR_W1C 0x0000ff00U
#define UMS9117_NANDC_SPI_BUSY BIT(1)
#define UMS9117_NANDC_WAIT_TIMEOUT_MS 100U
#define UMS9117_NANDC_PAGE_READY_TIMEOUT_MS 10U
#define UMS9117_NANDC_INSTRUCTION_WORDS 8U
#define UMS9117_NANDC_PINMUX_COUNT 17U
#define UMS9117_NANDC_PINCONF_COUNT 16U
/*
 * Supported geometry.  The data job transfers the main area as two 1 KiB
 * sectors; larger OOB areas, other block sizes, larger chips and multiple
 * planes are refused.
 */
#define UMS9117_NANDC_PAGE_MAIN_BYTES 2048U
#define UMS9117_NANDC_PAGES_PER_BLOCK 64U
#define UMS9117_NANDC_OOB_MAX_BYTES 128U
#define UMS9117_NANDC_PAGE_COUNT_MAX 65536U
#define UMS9117_NANDC_BATCH_BYTES (32U * UMS9117_NANDC_PAGE_MAIN_BYTES)
#define UMS9117_NANDC_FEATURE_B0_ECC_EN BIT(4)
#define UMS9117_NANDC_FEATURE_B0_OTP_EN BIT(6)
#define UMS9117_NANDC_FEATURE_STATUS_OIP BIT(0)
/* One 2 KiB page is two apart 1 KiB sectors in the NANDC ping-pong RAM. */
#define UMS9117_NANDC_DATA_CFG0 0x01000058U
#define UMS9117_NANDC_DATA_CFG1 0x000033ffU
#define UMS9117_NANDC_DATA_CFG4 0x0000003aU
#define UMS9117_NANDC_OOB_CFG0 0x00000058U
#define UMS9117_NANDC_INVALID_DMA_ADDRESS 0xffffffffU

enum ums9117_nandc_resource {
	UMS9117_NANDC_RES_NANDC,
	UMS9117_NANDC_RES_GATE_STATE,
	UMS9117_NANDC_RES_GATE_SET,
	UMS9117_NANDC_RES_GATE_CLEAR,
	UMS9117_NANDC_RES_RESET_STATE,
	UMS9117_NANDC_RES_RESET_SET,
	UMS9117_NANDC_RES_RESET_CLEAR,
	UMS9117_NANDC_RES_ECC_CLOCK,
	UMS9117_NANDC_RES_2X_CLOCK,
	UMS9117_NANDC_RES_PIN_CONTROL,
	UMS9117_NANDC_RES_PINMUX,
	UMS9117_NANDC_RES_PINCONF,
	UMS9117_NANDC_RES_COUNT,
};

enum ums9117_nandc_fixed_operation {
	UMS9117_NANDC_FIXED_RESET,
	UMS9117_NANDC_FIXED_READ_ID,
	UMS9117_NANDC_FIXED_GET_FEATURE,
	UMS9117_NANDC_FIXED_DISABLE_ECC,
	UMS9117_NANDC_FIXED_RESTORE_ECC,
	UMS9117_NANDC_FIXED_PAGE_TO_CACHE,
	UMS9117_NANDC_FIXED_READ_CACHE_X1,
	UMS9117_NANDC_FIXED_READ_CACHE_OOB,
};

static const char *const ums9117_nandc_resource_names[] = {
	[UMS9117_NANDC_RES_NANDC] = "nandc",
	[UMS9117_NANDC_RES_GATE_STATE] = "ap-ahb-gate-state",
	[UMS9117_NANDC_RES_GATE_SET] = "ap-ahb-gate-set",
	[UMS9117_NANDC_RES_GATE_CLEAR] = "ap-ahb-gate-clear",
	[UMS9117_NANDC_RES_RESET_STATE] = "ap-ahb-reset-state",
	[UMS9117_NANDC_RES_RESET_SET] = "ap-ahb-reset-set",
	[UMS9117_NANDC_RES_RESET_CLEAR] = "ap-ahb-reset-clear",
	[UMS9117_NANDC_RES_ECC_CLOCK] = "nandc-ecc-clock",
	[UMS9117_NANDC_RES_2X_CLOCK] = "nandc-2x-clock",
	[UMS9117_NANDC_RES_PIN_CONTROL] = "pin-control",
	[UMS9117_NANDC_RES_PINMUX] = "pinmux",
	[UMS9117_NANDC_RES_PINCONF] = "pinconf",
};

struct ums9117_nandc_pin {
	u32 offset;
	u32 value;
	u32 snapshot;
};

/* Physical geometry of one chip, keyed by its two READ_ID bytes. */
struct ums9117_nandc_chip {
	const char *name;
	u8 manufacturer_id;
	u8 device_id;
	u16 page_main_bytes;
	u16 oob_bytes;
	u16 pages_per_block;
	u16 block_count;
	u8 plane_count;
};

/*
 * Add a chip only when a source states its physical OOB size.  The phone
 * firmware's NAND ID table does not state it, so IDs known from that table
 * alone are absent.
 */
static const struct ums9117_nandc_chip ums9117_nandc_chips[] = {
	/* Source: hardware, complete raw dumps of the fitted Nokia TA-1618 chip. */
	{
		.name = "FM25LG01B",
		.manufacturer_id = 0xa1,
		.device_id = 0xb1,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
	},
	/* Source: hardware, complete raw dumps of the fitted INOI 240/244 chips. */
	{
		.name = "DS35M1GA",
		.manufacturer_id = 0xe5,
		.device_id = 0x21,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
	},
	/*
	 * Source: the phone firmware's NAND ID table;
	 * Linux 6.18.42 drivers/mtd/nand/spi/gigadevice.c:427.
	 */
	{
		.name = "GD5F1GQ4RExxG",
		.manufacturer_id = 0xc8,
		.device_id = 0xc1,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
	},
	/*
	 * Source: the phone firmware's NAND ID table;
	 * Linux 6.18.42 drivers/mtd/nand/spi/gigadevice.c:477.
	 */
	{
		.name = "GD5F1GQ5RExxG",
		.manufacturer_id = 0xc8,
		.device_id = 0x41,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
	},
	/*
	 * Source: the phone firmware's NAND ID table;
	 * Linux 6.18.42 drivers/mtd/nand/spi/gigadevice.c:537.
	 */
	{
		.name = "GD5F1GM7RExxG",
		.manufacturer_id = 0xc8,
		.device_id = 0x81,
		.page_main_bytes = 2048,
		.oob_bytes = 128,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
	},
	/*
	 * Source: the phone firmware's NAND ID table;
	 * Linux 6.18.42 drivers/mtd/nand/spi/gigadevice.c:597.
	 */
	{
		.name = "GD5F1GQ5RExxH",
		.manufacturer_id = 0xc8,
		.device_id = 0x21,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
	},
	/*
	 * Source: the phone firmware's NAND ID table;
	 * Linux 6.18.42 drivers/mtd/nand/spi/esmt.c:200.
	 */
	{
		.name = "F50D1G41LB",
		.manufacturer_id = 0xc8,
		.device_id = 0x11,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
	},
	/*
	 * Source: the phone firmware's NAND ID table;
	 * Linux 6.18.42 drivers/mtd/nand/spi/macronix.c:395.
	 */
	{
		.name = "MX35UF1G14AC",
		.manufacturer_id = 0xc2,
		.device_id = 0x90,
		.page_main_bytes = 2048,
		.oob_bytes = 64,
		.pages_per_block = 64,
		.block_count = 1024,
		.plane_count = 1,
	},
};

struct ums9117_nandc {
	struct device *dev;
	struct miscdevice raw_misc;
	struct mutex io_mutex;
	void __iomem *regs[UMS9117_NANDC_RES_COUNT];
	struct regmap *aon_apb;
	struct ums9117_nandc_pin *pinmux;
	struct ums9117_nandc_pin *pinconf;
	u32 pin_control_snapshot;
	u32 ecc_clock_snapshot;
	u32 clock_2x_snapshot;
	u32 gate_snapshot;
	u32 reset_snapshot;
	u32 arm_boot_mode;
	u32 last_int_raw;
	u32 last_spi_status;
	u32 last_axim_status;
	u32 id_raw;
	u32 declared_id;
	/* Identification result, written once under io_mutex by the first open. */
	const struct ums9117_nandc_chip *chip;
	u32 identity_id_raw;
	u32 identity_features[3];
	u32 physical_page_bytes;
	u32 page_count;
	u64 raw_bytes;
	u32 feature_raw[3];
	u32 raw_feature_b0;
	u32 restored_feature_b0;
	u32 completed_operations;
	u32 runtime_sessions_completed;
	u64 runtime_pages_completed;
	u32 page_status;
	u32 status_poll_count;
	u32 last_page;
	u8 *staging_buffer;
	void *page_buffer;
	dma_addr_t page_dma;
	int last_error;
	int page_read_error;
	int lifecycle_restore_error;
	int feature_restore_error;
	enum ums9117_nandc_fixed_operation last_operation;
	u8 last_feature_address;
	bool pin_control_snapshot_valid;
	bool clock_snapshots_valid;
	bool pin_snapshots_valid;
	bool gate_touched;
	bool gate_enabled;
	bool failed;
	bool attributes_created;
	bool has_declared_id;
	bool identity_valid;
	bool lifecycle_restored;
	bool raw_registered;
	bool io_faulted;
	bool shutting_down;
	bool features_valid;
	bool read_ecc_disabled;
	bool feature_restore_required;
	bool nand_feature_restored;
};

static const u32 ums9117_nandc_pinmux_offsets[] = {
	/* FDL write order in the 0x402a0154..0x402a0197 window. */
	0x40U, 0x20U, 0x0cU, 0x08U, 0x30U, 0x10U, 0x04U, 0x00U, 0x2cU,
	0x38U, 0x3cU, 0x34U, 0x28U, 0x14U, 0x18U, 0x24U, 0x1cU,
};

static const u32 ums9117_nandc_pinconf_offsets[] = {
	/* FDL write order in the 0x402a0554..0x402a0597 window. */
	0x40U, 0x0cU, 0x08U, 0x30U, 0x10U, 0x00U, 0x2cU, 0x38U,
	0x3cU, 0x34U, 0x28U, 0x14U, 0x18U, 0x1cU, 0x20U, 0x04U,
};

static bool ums9117_nandc_chip_supported(const struct ums9117_nandc_chip *chip)
{
	return chip->page_main_bytes == UMS9117_NANDC_PAGE_MAIN_BYTES &&
	       chip->oob_bytes <= UMS9117_NANDC_OOB_MAX_BYTES &&
	       chip->pages_per_block == UMS9117_NANDC_PAGES_PER_BLOCK &&
	       chip->plane_count == 1 &&
	       (u32)chip->block_count * chip->pages_per_block <=
		       UMS9117_NANDC_PAGE_COUNT_MAX;
}

/*
 * READ_ID stores its first byte, the manufacturer, in SNFC_FEATURE[7:0] and
 * the device byte in [15:8].  Any other register value is unknown.
 */
static const struct ums9117_nandc_chip *ums9117_nandc_find_chip(u32 id_raw)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ums9117_nandc_chips); i++) {
		const struct ums9117_nandc_chip *chip = &ums9117_nandc_chips[i];
		u32 chip_id = (u32)chip->device_id << 8 | chip->manufacturer_id;

		if (id_raw == chip_id)
			return ums9117_nandc_chip_supported(chip) ? chip : NULL;
	}
	return NULL;
}

static bool ums9117_nandc_b0_has_owned_ecc(u32 b0)
{
	return b0 <= U8_MAX && !(b0 & UMS9117_NANDC_FEATURE_B0_OTP_EN) &&
	       (b0 & UMS9117_NANDC_FEATURE_B0_ECC_EN);
}

static void __iomem *ums9117_nandc_map_shared(struct platform_device *pdev,
					      const char *name,
					      resource_size_t minimum_size)
{
	struct resource *resource;
	void __iomem *base;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource_size(resource) < minimum_size)
		return IOMEM_ERR_PTR(-EINVAL);

	base = devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

static int ums9117_nandc_map_resources(struct ums9117_nandc *nandc,
				       struct platform_device *pdev)
{
	struct resource *nandc_resource;
	unsigned int i;

	nandc_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "nandc");
	if (!nandc_resource ||
	    resource_size(nandc_resource) < UMS9117_NANDC_MMIO_BYTES)
		return -EINVAL;
	nandc->regs[UMS9117_NANDC_RES_NANDC] =
		devm_platform_ioremap_resource_byname(pdev, "nandc");
	if (IS_ERR(nandc->regs[UMS9117_NANDC_RES_NANDC]))
		return PTR_ERR(nandc->regs[UMS9117_NANDC_RES_NANDC]);

	for (i = UMS9117_NANDC_RES_GATE_STATE; i < UMS9117_NANDC_RES_COUNT;
	     i++) {
		resource_size_t minimum_size = sizeof(u32);

		if (i == UMS9117_NANDC_RES_PINMUX ||
		    i == UMS9117_NANDC_RES_PINCONF)
			minimum_size = UMS9117_NANDC_PIN_WINDOW_BYTES;
		nandc->regs[i] = ums9117_nandc_map_shared(
			pdev, ums9117_nandc_resource_names[i], minimum_size);
		if (IS_ERR(nandc->regs[i]))
			return PTR_ERR(nandc->regs[i]);
	}

	return 0;
}

static int ums9117_nandc_parse_pins(struct ums9117_nandc *nandc,
				    struct device_node *np,
				    const char *values_name, const u32 *offsets,
				    struct ums9117_nandc_pin **pins,
				    unsigned int expected_count,
				    void __iomem *window)
{
	struct ums9117_nandc_pin *parsed;
	unsigned int i;
	int count;

	count = of_property_count_u32_elems(np, values_name);
	if (count != expected_count)
		return -EINVAL;

	parsed = devm_kcalloc(nandc->dev, expected_count, sizeof(*parsed),
			      GFP_KERNEL);
	if (!parsed)
		return -ENOMEM;
	for (i = 0; i < expected_count; i++) {
		parsed[i].offset = offsets[i];
		if (of_property_read_u32_index(np, values_name, i,
					       &parsed[i].value))
			return -EINVAL;
		if (!IS_ALIGNED(parsed[i].offset, sizeof(u32)) ||
		    parsed[i].offset >
			    UMS9117_NANDC_PIN_WINDOW_BYTES - sizeof(u32))
			return -EINVAL;
		for (unsigned int j = 0; j < i; j++)
			if (parsed[j].offset == parsed[i].offset)
				return -EINVAL;
	}

	/* Keep the resource explicit in this verifier; no derived MMIO endpoint. */
	if (!window)
		return -EINVAL;
	*pins = parsed;
	return 0;
}

static int ums9117_nandc_parse_resources(struct ums9117_nandc *nandc,
					 struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;

	nandc->aon_apb = syscon_regmap_lookup_by_phandle(np, "sprd,aon-apb");
	if (IS_ERR(nandc->aon_apb))
		return PTR_ERR(nandc->aon_apb);

	ret = ums9117_nandc_parse_pins(nandc, np, "sprd,pinmux-values",
				       ums9117_nandc_pinmux_offsets,
				       &nandc->pinmux,
				       UMS9117_NANDC_PINMUX_COUNT,
				       nandc->regs[UMS9117_NANDC_RES_PINMUX]);
	if (ret)
		return ret;
	return ums9117_nandc_parse_pins(nandc, np, "sprd,pinconf-values",
					ums9117_nandc_pinconf_offsets,
					&nandc->pinconf,
					UMS9117_NANDC_PINCONF_COUNT,
					nandc->regs[UMS9117_NANDC_RES_PINCONF]);
}

static int ums9117_nandc_update_shared_pin_gates(struct ums9117_nandc *nandc)
{
	int ret;

	ret = regmap_update_bits(nandc->aon_apb, UMS9117_NANDC_AON_APB_EB0,
				 UMS9117_NANDC_AON_APB_SHARED_PIN_GATE0,
				 UMS9117_NANDC_AON_APB_SHARED_PIN_GATE0);
	if (ret)
		return ret;
	return regmap_update_bits(nandc->aon_apb, UMS9117_NANDC_AON_APB_EB2,
				  UMS9117_NANDC_AON_APB_SHARED_PIN_GATE2,
				  UMS9117_NANDC_AON_APB_SHARED_PIN_GATE2);
}

static int ums9117_nandc_check_arm_boot_mode(struct ums9117_nandc *nandc)
{
	int ret;

	ret = regmap_read(nandc->aon_apb, UMS9117_NANDC_AON_APB_ARM_BOOT_MODE,
			  &nandc->arm_boot_mode);
	if (ret)
		return ret;
	if ((nandc->arm_boot_mode & UMS9117_NANDC_ARM_BOOT_MODE_MASK) !=
	    UMS9117_NANDC_ARM_BOOT_MODE_EXPECTED)
		return -ENODEV;
	return 0;
}

static int ums9117_nandc_apply_pin_control(struct ums9117_nandc *nandc)
{
	void __iomem *pin_control = nandc->regs[UMS9117_NANDC_RES_PIN_CONTROL];
	u32 value;

	nandc->pin_control_snapshot = readl(pin_control);
	nandc->pin_control_snapshot_valid = true;
	value = nandc->pin_control_snapshot | UMS9117_NANDC_PIN_CONTROL_VALUE;
	writel(value, pin_control);
	return (readl(pin_control) & UMS9117_NANDC_PIN_CONTROL_MASK) ==
			       UMS9117_NANDC_PIN_CONTROL_VALUE ?
		       0 :
		       -EIO;
}

static int ums9117_nandc_apply_clocks(struct ums9117_nandc *nandc)
{
	void __iomem *ecc_clock = nandc->regs[UMS9117_NANDC_RES_ECC_CLOCK];
	void __iomem *clock_2x = nandc->regs[UMS9117_NANDC_RES_2X_CLOCK];
	u32 value;

	nandc->ecc_clock_snapshot = readl(ecc_clock);
	nandc->clock_2x_snapshot = readl(clock_2x);
	nandc->clock_snapshots_valid = true;
	writel(UMS9117_NANDC_CLOCK_2X_VALUE, clock_2x);
	value = (nandc->ecc_clock_snapshot & ~UMS9117_NANDC_CLOCK_ECC_MASK) |
		UMS9117_NANDC_CLOCK_ECC_VALUE;
	writel(value, ecc_clock);
	if ((readl(ecc_clock) & UMS9117_NANDC_CLOCK_ECC_MASK) !=
		    UMS9117_NANDC_CLOCK_ECC_VALUE ||
	    readl(clock_2x) != UMS9117_NANDC_CLOCK_2X_VALUE)
		return -EIO;
	return 0;
}

static int ums9117_nandc_apply_pins(struct ums9117_nandc *nandc)
{
	void __iomem *pinmux = nandc->regs[UMS9117_NANDC_RES_PINMUX];
	void __iomem *pinconf = nandc->regs[UMS9117_NANDC_RES_PINCONF];
	unsigned int i;

	for (i = 0; i < UMS9117_NANDC_PINMUX_COUNT; i++)
		nandc->pinmux[i].snapshot =
			readl(pinmux + nandc->pinmux[i].offset);
	for (i = 0; i < UMS9117_NANDC_PINCONF_COUNT; i++)
		nandc->pinconf[i].snapshot =
			readl(pinconf + nandc->pinconf[i].offset);
	nandc->pin_snapshots_valid = true;

	for (i = 0; i < UMS9117_NANDC_PINMUX_COUNT; i++)
		writel(nandc->pinmux[i].value,
		       pinmux + nandc->pinmux[i].offset);
	for (i = 0; i < UMS9117_NANDC_PINCONF_COUNT; i++)
		writel(nandc->pinconf[i].value,
		       pinconf + nandc->pinconf[i].offset);
	for (i = 0; i < UMS9117_NANDC_PINMUX_COUNT; i++)
		if (readl(pinmux + nandc->pinmux[i].offset) !=
		    nandc->pinmux[i].value)
			return -EIO;
	for (i = 0; i < UMS9117_NANDC_PINCONF_COUNT; i++)
		if (readl(pinconf + nandc->pinconf[i].offset) !=
		    nandc->pinconf[i].value)
			return -EIO;
	return 0;
}

static int ums9117_nandc_assert_reset(struct ums9117_nandc *nandc)
{
	void __iomem *reset_set = nandc->regs[UMS9117_NANDC_RES_RESET_SET];
	void __iomem *reset_state = nandc->regs[UMS9117_NANDC_RES_RESET_STATE];

	writel(UMS9117_NANDC_AP_AHB_RESET, reset_set);
	usleep_range(1000, 2000);
	return readl(reset_state) & UMS9117_NANDC_AP_AHB_RESET ? 0 : -EIO;
}

static int ums9117_nandc_controller_reset(struct ums9117_nandc *nandc)
{
	void __iomem *reset_clear = nandc->regs[UMS9117_NANDC_RES_RESET_CLEAR];
	void __iomem *reset_state = nandc->regs[UMS9117_NANDC_RES_RESET_STATE];
	int ret;

	ret = ums9117_nandc_assert_reset(nandc);
	if (ret)
		return ret;
	writel(UMS9117_NANDC_AP_AHB_RESET, reset_clear);
	return readl(reset_state) & UMS9117_NANDC_AP_AHB_RESET ? -EIO : 0;
}

static int ums9117_nandc_enable_controller(struct ums9117_nandc *nandc)
{
	void __iomem *gate_state = nandc->regs[UMS9117_NANDC_RES_GATE_STATE];
	void __iomem *gate_set = nandc->regs[UMS9117_NANDC_RES_GATE_SET];
	void __iomem *reset_state = nandc->regs[UMS9117_NANDC_RES_RESET_STATE];
	int ret;

	nandc->gate_snapshot = readl(gate_state);
	nandc->reset_snapshot = readl(reset_state);
	writel(UMS9117_NANDC_AP_AHB_GATE, gate_set);
	nandc->gate_touched = true;
	if (!(readl(gate_state) & UMS9117_NANDC_AP_AHB_GATE))
		return -EIO;
	nandc->gate_enabled = true;
	ret = ums9117_nandc_controller_reset(nandc);
	if (ret)
		return ret;
	return 0;
}

static void ums9117_nandc_write_instructions(struct ums9117_nandc *nandc,
					     const u16 *instructions,
					     unsigned int instruction_count)
{
	void __iomem *instruction_ram = nandc->regs[UMS9117_NANDC_RES_NANDC] +
					UMS9117_NANDC_INSTRUCTION_RAM;
	unsigned int i;

	for (i = 0; i < UMS9117_NANDC_INSTRUCTION_WORDS / 2; i++)
		writel(0, instruction_ram + i * sizeof(u32));
	for (i = 0; i < instruction_count; i += 2) {
		u32 value = instructions[i];

		if (i + 1 < instruction_count)
			value |= (u32)instructions[i + 1] << 16;
		writel(value, instruction_ram + (i / 2) * sizeof(u32));
	}
	readl(instruction_ram + ((instruction_count - 1) / 2) * sizeof(u32));
}

static void ums9117_nandc_abort_command(struct ums9117_nandc *nandc)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];

	writel(BIT(1), regs + UMS9117_NANDC_START);
	readl(regs + UMS9117_NANDC_START);
	writel(UMS9117_NANDC_INT_CLEAR_W1C, regs + UMS9117_NANDC_INT);
	readl(regs + UMS9117_NANDC_INT);
}

static int ums9117_nandc_wait(struct ums9117_nandc *nandc)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	unsigned long deadline =
		jiffies + msecs_to_jiffies(UMS9117_NANDC_WAIT_TIMEOUT_MS);
	u32 raw;
	u32 spi_status;

	for (;;) {
		raw = readl(regs + UMS9117_NANDC_INT);
		spi_status = readl(regs + UMS9117_NANDC_SPI_STATUS);
		nandc->last_int_raw = raw;
		nandc->last_spi_status = spi_status;
		nandc->last_axim_status = readl(regs + UMS9117_NANDC_AXIM_STS);
		if (raw & UMS9117_NANDC_INT_FATAL_RAW) {
			ums9117_nandc_abort_command(nandc);
			return -EIO;
		}
		if ((raw & UMS9117_NANDC_INT_DONE_RAW) &&
		    !(spi_status & UMS9117_NANDC_SPI_BUSY)) {
			writel(UMS9117_NANDC_INT_CLEAR_W1C,
			       regs + UMS9117_NANDC_INT);
			readl(regs + UMS9117_NANDC_INT);
			return 0;
		}
		if (time_after_eq(jiffies, deadline))
			break;
		usleep_range(50, 100);
	}
	ums9117_nandc_abort_command(nandc);
	return -ETIMEDOUT;
}

static bool ums9117_nandc_feature_address_allowed(u8 address)
{
	return address == 0xa0U || address == 0xb0U || address == 0xc0U;
}

static int ums9117_nandc_run_job(struct ums9117_nandc *nandc,
				 const u16 *instructions,
				 unsigned int instruction_count)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	u32 raw;
	int ret;

	if (nandc->failed)
		return -EINVAL;
	writel(UMS9117_NANDC_INT_CLEAR_W1C, regs + UMS9117_NANDC_INT);
	raw = readl(regs + UMS9117_NANDC_INT);
	if (raw & (UMS9117_NANDC_INT_DONE_RAW | UMS9117_NANDC_INT_FATAL_RAW)) {
		nandc->last_int_raw = raw;
		nandc->last_spi_status = readl(regs + UMS9117_NANDC_SPI_STATUS);
		nandc->last_axim_status = readl(regs + UMS9117_NANDC_AXIM_STS);
		ums9117_nandc_abort_command(nandc);
		return -EIO;
	}

	ums9117_nandc_write_instructions(nandc, instructions,
					 instruction_count);
	/* Publish the instruction RAM before starting the controller. */
	wmb();
	writel(1, regs + UMS9117_NANDC_START);
	ret = ums9117_nandc_wait(nandc);
	if (!ret)
		nandc->completed_operations++;
	return ret;
}

static int
ums9117_nandc_execute_fixed(struct ums9117_nandc *nandc,
			    enum ums9117_nandc_fixed_operation operation,
			    u8 feature_address)
{
	static const u16 reset[] = {
		0x00ff, 0xe000, 0x000f, 0x00c0, 0x1000, 0xe001, 0xf000,
	};
	static const u16 read_id[] = {
		0x009f, 0x0000, 0x1000, 0x1000, 0xe000, 0xf000,
	};
	u16 get_feature[] = { 0x000f, 0x0000, 0x1000, 0xe000, 0xf000 };
	u16 set_feature_b0[] = { 0x001f, 0x00b0, 0x0000, 0xe000, 0xf000 };
	const u16 *instructions;
	unsigned int instruction_count;
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	u32 original_b0 = nandc->feature_raw[1];

	if (nandc->failed)
		return -EINVAL;
	switch (operation) {
	case UMS9117_NANDC_FIXED_RESET:
		instructions = reset;
		instruction_count = ARRAY_SIZE(reset);
		break;
	case UMS9117_NANDC_FIXED_READ_ID:
		instructions = read_id;
		instruction_count = ARRAY_SIZE(read_id);
		break;
	case UMS9117_NANDC_FIXED_GET_FEATURE:
		if (!ums9117_nandc_feature_address_allowed(feature_address))
			return -EOPNOTSUPP;
		get_feature[1] = feature_address;
		instructions = get_feature;
		instruction_count = ARRAY_SIZE(get_feature);
		break;
	case UMS9117_NANDC_FIXED_DISABLE_ECC:
	case UMS9117_NANDC_FIXED_RESTORE_ECC:
		/*
		 * Only the volatile ECC_EN bit of this session's B0 value is
		 * owned: clear it alone, then write back the exact value read.
		 */
		if (!nandc->chip || !nandc->features_valid ||
		    !ums9117_nandc_b0_has_owned_ecc(original_b0))
			return -EOPNOTSUPP;
		set_feature_b0[2] = original_b0;
		if (operation == UMS9117_NANDC_FIXED_DISABLE_ECC)
			set_feature_b0[2] = original_b0 &
					    ~UMS9117_NANDC_FEATURE_B0_ECC_EN;
		instructions = set_feature_b0;
		instruction_count = ARRAY_SIZE(set_feature_b0);
		break;
	default:
		return -EOPNOTSUPP;
	}
	nandc->last_operation = operation;
	nandc->last_feature_address = feature_address;

	writel(UMS9117_NANDC_CFG0_SPI_ONLY, regs + UMS9117_NANDC_CFG0);
	writel(UMS9117_NANDC_CFG1_SPI, regs + UMS9117_NANDC_CFG1);
	writel(readl(regs + UMS9117_NANDC_CFG4) | UMS9117_NANDC_CFG4_SPI_ENABLE,
	       regs + UMS9117_NANDC_CFG4);
	return ums9117_nandc_run_job(nandc, instructions, instruction_count);
}

static int ums9117_nandc_prepare_raw_mode(struct ums9117_nandc *nandc)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	u32 original_b0 = nandc->feature_raw[1];
	int ret;

	if (original_b0 & UMS9117_NANDC_FEATURE_B0_ECC_EN) {
		/* A short command can still change B0, so restore on every exit. */
		nandc->feature_restore_required = true;
		nandc->nand_feature_restored = false;
		ret = ums9117_nandc_execute_fixed(
			nandc, UMS9117_NANDC_FIXED_DISABLE_ECC, 0xb0);
		if (ret)
			return ret;
		ret = ums9117_nandc_execute_fixed(
			nandc, UMS9117_NANDC_FIXED_GET_FEATURE, 0xb0);
		if (ret)
			return ret;
		nandc->raw_feature_b0 = readl(regs + UMS9117_NANDC_SPI_FEATURE);
		if (nandc->raw_feature_b0 !=
		    (original_b0 & ~UMS9117_NANDC_FEATURE_B0_ECC_EN))
			return -EUCLEAN;
	}
	nandc->read_ecc_disabled = true;
	return 0;
}

static int ums9117_nandc_restore_raw_mode(struct ums9117_nandc *nandc)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	int ret;

	if (!nandc->feature_restore_required)
		return 0;
	/* NAND RESET does not restore B0. Do this before resetting NANDC. */
	ret = ums9117_nandc_execute_fixed(
		nandc, UMS9117_NANDC_FIXED_RESTORE_ECC, 0xb0);
	if (!ret)
		ret = ums9117_nandc_execute_fixed(
			nandc, UMS9117_NANDC_FIXED_GET_FEATURE, 0xb0);
	if (!ret) {
		nandc->restored_feature_b0 =
			readl(regs + UMS9117_NANDC_SPI_FEATURE);
		if (nandc->restored_feature_b0 != nandc->feature_raw[1])
			ret = -EUCLEAN;
	}
	if (!ret) {
		nandc->feature_restore_required = false;
		nandc->nand_feature_restored = true;
	}
	nandc->feature_restore_error = ret;
	return ret;
}

static int ums9117_nandc_page_to_cache(struct ums9117_nandc *nandc, u32 page)
{
	u16 instructions[] = {
		0x0013, 0x0000, 0x0000, 0x0000, 0xe000, 0xf000,
	};
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];

	if (page >= nandc->page_count)
		return -ERANGE;
	instructions[1] = (page >> 16) & 0xffU;
	instructions[2] = (page >> 8) & 0xffU;
	instructions[3] = page & 0xffU;
	nandc->last_operation = UMS9117_NANDC_FIXED_PAGE_TO_CACHE;
	nandc->last_feature_address = 0;
	writel(UMS9117_NANDC_CFG0_SPI_ONLY, regs + UMS9117_NANDC_CFG0);
	writel(UMS9117_NANDC_CFG1_SPI, regs + UMS9117_NANDC_CFG1);
	writel(readl(regs + UMS9117_NANDC_CFG4) | UMS9117_NANDC_CFG4_SPI_ENABLE,
	       regs + UMS9117_NANDC_CFG4);
	return ums9117_nandc_run_job(nandc, instructions,
				     ARRAY_SIZE(instructions));
}

static int ums9117_nandc_wait_page_ready(struct ums9117_nandc *nandc)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	ktime_t deadline =
		ktime_add_ms(ktime_get(), UMS9117_NANDC_PAGE_READY_TIMEOUT_MS);
	u32 status;
	bool final_poll = false;
	int ret;

	for (;;) {
		ret = ums9117_nandc_execute_fixed(
			nandc, UMS9117_NANDC_FIXED_GET_FEATURE, 0xc0);
		if (ret)
			return ret;
		status = readl(regs + UMS9117_NANDC_SPI_FEATURE);
		nandc->page_status = status;
		nandc->status_poll_count++;
		if (!(status & UMS9117_NANDC_FEATURE_STATUS_OIP)) {
			if (status & (BIT(2) | BIT(3)))
				return -EIO;
			return 0;
		}
		if (final_poll)
			return -ETIMEDOUT;
		if (ktime_compare(ktime_get(), deadline) >= 0)
			final_poll = true;
		else
			usleep_range(50, 100);
	}
}

static int ums9117_nandc_read_page_x1(struct ums9117_nandc *nandc)
{
	static const u16 instructions[] = {
		0x000b, 0x0000, 0x0000, 0x6000, 0x3000, 0xe000, 0xf000,
	};
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	int ret;

	if (upper_32_bits(nandc->page_dma) || !IS_ALIGNED(nandc->page_dma, 64U))
		return -ERANGE;
	memset(nandc->page_buffer, 0xa5, nandc->chip->page_main_bytes);
	writel(0, regs + UMS9117_NANDC_MAIN_ADDR_HIGH);
	writel(lower_32_bits(nandc->page_dma),
	       regs + UMS9117_NANDC_MAIN_ADDR_LOW);
	writel(UMS9117_NANDC_INVALID_DMA_ADDRESS,
	       regs + UMS9117_NANDC_SPARE_ADDR_HIGH);
	writel(UMS9117_NANDC_INVALID_DMA_ADDRESS,
	       regs + UMS9117_NANDC_SPARE_ADDR_LOW);
	writel(UMS9117_NANDC_INVALID_DMA_ADDRESS,
	       regs + UMS9117_NANDC_STATUS_ADDR_HIGH);
	writel(UMS9117_NANDC_INVALID_DMA_ADDRESS,
	       regs + UMS9117_NANDC_STATUS_ADDR_LOW);
	writel(UMS9117_NANDC_DATA_CFG0, regs + UMS9117_NANDC_CFG0);
	writel(0, regs + UMS9117_NANDC_CFG2);
	writel(UMS9117_NANDC_DATA_CFG1, regs + UMS9117_NANDC_CFG1);
	writel(UMS9117_NANDC_DATA_CFG4, regs + UMS9117_NANDC_CFG4);
	writel(0, regs + UMS9117_NANDC_START);
	nandc->last_operation = UMS9117_NANDC_FIXED_READ_CACHE_X1;
	nandc->last_feature_address = 0;
	dma_wmb();
	ret = ums9117_nandc_run_job(nandc, instructions,
				    ARRAY_SIZE(instructions));
	if (ret)
		return ret;
	dma_rmb();
	if (nandc->last_axim_status)
		return -EIO;
	return 0;
}

static int ums9117_nandc_read_oob_cache(struct ums9117_nandc *nandc)
{
	u16 instructions[] = {
		0x000b, 0x0000, 0x0000, 0x6000, 0x3000, 0xe000, 0xf000,
	};
	const struct ums9117_nandc_chip *chip = nandc->chip;
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	int ret;

	if (upper_32_bits(nandc->page_dma) || !IS_ALIGNED(nandc->page_dma, 64U))
		return -ERANGE;
	/* The OOB area starts at the column after the main area. */
	instructions[1] = (chip->page_main_bytes >> 8) & 0xffU;
	instructions[2] = chip->page_main_bytes & 0xffU;
	memset(nandc->page_buffer, 0xa5, chip->oob_bytes);
	writel(0, regs + UMS9117_NANDC_MAIN_ADDR_HIGH);
	writel(lower_32_bits(nandc->page_dma),
	       regs + UMS9117_NANDC_MAIN_ADDR_LOW);
	writel(UMS9117_NANDC_INVALID_DMA_ADDRESS,
	       regs + UMS9117_NANDC_SPARE_ADDR_HIGH);
	writel(UMS9117_NANDC_INVALID_DMA_ADDRESS,
	       regs + UMS9117_NANDC_SPARE_ADDR_LOW);
	writel(UMS9117_NANDC_INVALID_DMA_ADDRESS,
	       regs + UMS9117_NANDC_STATUS_ADDR_HIGH);
	writel(UMS9117_NANDC_INVALID_DMA_ADDRESS,
	       regs + UMS9117_NANDC_STATUS_ADDR_LOW);
	writel(UMS9117_NANDC_OOB_CFG0, regs + UMS9117_NANDC_CFG0);
	writel(0, regs + UMS9117_NANDC_CFG2);
	/* CFG1 MAIN_SIZE is the transfer length minus one. */
	writel(UMS9117_NANDC_CFG1_SPI | (chip->oob_bytes - 1),
	       regs + UMS9117_NANDC_CFG1);
	writel(UMS9117_NANDC_DATA_CFG4, regs + UMS9117_NANDC_CFG4);
	writel(0, regs + UMS9117_NANDC_START);
	nandc->last_operation = UMS9117_NANDC_FIXED_READ_CACHE_OOB;
	nandc->last_feature_address = 0;
	dma_wmb();
	ret = ums9117_nandc_run_job(nandc, instructions,
				    ARRAY_SIZE(instructions));
	if (ret)
		return ret;
	dma_rmb();
	return nandc->last_axim_status ? -EIO : 0;
}

static int ums9117_nandc_read_raw_slice(struct ums9117_nandc *nandc, u32 page,
					size_t offset, size_t length,
					u8 *destination)
{
	size_t page_main_bytes = nandc->chip->page_main_bytes;
	size_t end;
	size_t main_end;
	size_t oob_start;
	int ret;

	if (page >= nandc->page_count || offset >= nandc->physical_page_bytes ||
	    length > nandc->physical_page_bytes - offset)
		return -ERANGE;
	end = offset + length;
	nandc->last_page = page;
	ret = ums9117_nandc_page_to_cache(nandc, page);
	if (ret)
		return ret;
	ret = ums9117_nandc_wait_page_ready(nandc);
	if (ret)
		return ret;
	ret = ums9117_nandc_read_page_x1(nandc);
	if (ret)
		return ret;

	main_end = min(end, page_main_bytes);
	if (offset < main_end)
		memcpy(destination, nandc->page_buffer + offset,
		       main_end - offset);

	ret = ums9117_nandc_read_oob_cache(nandc);
	if (ret)
		return ret;
	oob_start = max(offset, page_main_bytes);
	if (oob_start < end)
		memcpy(destination + oob_start - offset,
		       nandc->page_buffer + oob_start - page_main_bytes,
		       end - oob_start);
	return 0;
}

static int ums9117_nandc_apply_fdl_recipe(struct ums9117_nandc *nandc)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];

	writel(0x3a493146U, regs + UMS9117_NANDC_TIMING0);
	writel(0x192a0000U, regs + UMS9117_NANDC_TIMING1);
	writel(0x81000000U, regs + UMS9117_NANDC_TIMEOUT);
	writel(0x0003000cU, regs + UMS9117_NANDC_CFG3);
	writel(0x00001004U, regs + UMS9117_NANDC_POLY0);
	writel(0x00001004U, regs + UMS9117_NANDC_POLY1);
	writel(0x00004013U, regs + UMS9117_NANDC_POLY2);
	writel(0x00400010U, regs + UMS9117_NANDC_POLY3);
	writel(6, regs + UMS9117_NANDC_PHY_CFG);
	writel(0x00000100U, regs + UMS9117_NANDC_DLL0);
	writel(0x00000100U, regs + UMS9117_NANDC_DLL1);
	writel(0x000000bfU, regs + UMS9117_NANDC_DLL2);
	writel(0x00000c80U, regs + UMS9117_NANDC_DLL);
	writel(0x00000680U, regs + UMS9117_NANDC_SPI_CLK_CFG);
	writel(3, regs + UMS9117_NANDC_SPI_WP_HOLD);
	writel(0x0000548aU, regs + 0x12cU);
	writel(UMS9117_NANDC_CFG0_SPI_ONLY, regs + UMS9117_NANDC_CFG0);
	writel(UMS9117_NANDC_CFG1_SPI, regs + UMS9117_NANDC_CFG1);
	writel(0x00040002U, regs + UMS9117_NANDC_DLL);
	writel(readl(regs + UMS9117_NANDC_CFG4) | UMS9117_NANDC_CFG4_SPI_ENABLE,
	       regs + UMS9117_NANDC_CFG4);
	readl(regs + UMS9117_NANDC_CFG4);
	return 0;
}

static int ums9117_nandc_fail(struct ums9117_nandc *nandc, int error,
			      const char *stage)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	u32 int_raw;
	u32 axim_sts;
	u32 spi_status;
	u32 feature;
	u32 start;
	u32 cfg0;
	u32 cfg1;
	u32 cfg4;
	int reset_error;

	nandc->failed = true;
	nandc->last_error = error;
	int_raw = readl(regs + UMS9117_NANDC_INT);
	axim_sts = readl(regs + UMS9117_NANDC_AXIM_STS);
	spi_status = readl(regs + UMS9117_NANDC_SPI_STATUS);
	feature = readl(regs + UMS9117_NANDC_SPI_FEATURE);
	start = readl(regs + UMS9117_NANDC_START);
	cfg0 = readl(regs + UMS9117_NANDC_CFG0);
	cfg1 = readl(regs + UMS9117_NANDC_CFG1);
	cfg4 = readl(regs + UMS9117_NANDC_CFG4);
	ums9117_nandc_abort_command(nandc);
	reset_error = ums9117_nandc_assert_reset(nandc);
	dev_err(nandc->dev,
		"%s failed: %pe page=%u operation=%u feature_address=0x%02x completed=%u\n",
		stage, ERR_PTR(error), nandc->last_page, nandc->last_operation,
		nandc->last_feature_address, nandc->completed_operations);
	dev_dbg(nandc->dev,
		"observed_int=0x%08x observed_axim=0x%08x observed_spi=0x%08x\n",
		nandc->last_int_raw, nandc->last_axim_status,
		nandc->last_spi_status);
	dev_dbg(nandc->dev, "post_abort_int=0x%08x axim_sts=0x%08x\n", int_raw,
		axim_sts);
	dev_dbg(nandc->dev, "spi_status=0x%08x feature=0x%08x start=0x%08x\n",
		spi_status, feature, start);
	dev_dbg(nandc->dev,
		"cfg0=0x%08x cfg1=0x%08x cfg4=0x%08x reset_error=%d\n", cfg0,
		cfg1, cfg4, reset_error);
	if (reset_error)
		dev_err(nandc->dev,
			"NANDC reset did not assert after failure\n");
	return error;
}

static int ums9117_nandc_read_id(struct ums9117_nandc *nandc)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	int ret;

	ret = ums9117_nandc_execute_fixed(nandc, UMS9117_NANDC_FIXED_RESET, 0);
	if (ret)
		return ums9117_nandc_fail(nandc, ret, "fixed NAND reset");

	ret = ums9117_nandc_execute_fixed(nandc, UMS9117_NANDC_FIXED_READ_ID,
					  0);
	if (ret)
		return ums9117_nandc_fail(nandc, ret, "fixed NAND read-id");
	nandc->id_raw = readl(regs + UMS9117_NANDC_SPI_FEATURE);
	return 0;
}

static int ums9117_nandc_read_features(struct ums9117_nandc *nandc)
{
	static const u8 feature_addresses[] = { 0xa0, 0xb0, 0xc0 };
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(feature_addresses); i++) {
		ret = ums9117_nandc_execute_fixed(
			nandc, UMS9117_NANDC_FIXED_GET_FEATURE,
			feature_addresses[i]);
		if (ret)
			return ums9117_nandc_fail(nandc, ret,
						  "fixed NAND get-feature");
		nandc->feature_raw[i] = readl(regs + UMS9117_NANDC_SPI_FEATURE);
	}
	nandc->features_valid = true;
	return 0;
}

static int ums9117_nandc_run_diagnostics(struct ums9117_nandc *nandc)
{
	u32 b0;
	int ret;

	ret = ums9117_nandc_read_id(nandc);
	if (ret)
		return ret;
	if (nandc->id_raw != nandc->identity_id_raw)
		return ums9117_nandc_fail(nandc, -ENODEV, "unexpected NAND id");

	ret = ums9117_nandc_read_features(nandc);
	if (ret)
		return ret;
	b0 = nandc->feature_raw[1];
	if (b0 > U8_MAX || (b0 & UMS9117_NANDC_FEATURE_B0_OTP_EN) ||
	    nandc->feature_raw[2] != 0)
		return ums9117_nandc_fail(nandc, -EUCLEAN,
					  "unexpected NAND feature state");
	return ums9117_nandc_prepare_raw_mode(nandc);
}

static void ums9117_nandc_begin_session(struct ums9117_nandc *nandc)
{
	nandc->pin_control_snapshot_valid = false;
	nandc->clock_snapshots_valid = false;
	nandc->pin_snapshots_valid = false;
	nandc->gate_touched = false;
	nandc->gate_enabled = false;
	nandc->failed = false;
	nandc->lifecycle_restored = false;
	nandc->last_error = 0;
	nandc->page_read_error = 0;
	nandc->lifecycle_restore_error = 0;
	nandc->last_int_raw = 0;
	nandc->last_spi_status = 0;
	nandc->last_axim_status = 0;
	nandc->status_poll_count = 0;
	nandc->features_valid = false;
	memset(nandc->feature_raw, 0, sizeof(nandc->feature_raw));
	nandc->raw_feature_b0 = 0;
	nandc->restored_feature_b0 = 0;
	nandc->read_ecc_disabled = false;
	nandc->feature_restore_required = false;
	nandc->nand_feature_restored = true;
	nandc->feature_restore_error = 0;
}

static int ums9117_nandc_activate(struct ums9117_nandc *nandc)
{
	int ret;

	ums9117_nandc_begin_session(nandc);
	ret = ums9117_nandc_update_shared_pin_gates(nandc);
	if (ret)
		return ret;
	ret = ums9117_nandc_apply_pin_control(nandc);
	if (ret)
		return ret;
	ret = ums9117_nandc_apply_pins(nandc);
	if (ret)
		return ret;
	ret = ums9117_nandc_apply_clocks(nandc);
	if (ret)
		return ret;
	ret = ums9117_nandc_enable_controller(nandc);
	if (ret) {
		if (nandc->gate_enabled)
			return ums9117_nandc_fail(nandc, ret,
						  "NANDC enable/reset");
		return ret;
	}
	ret = ums9117_nandc_apply_fdl_recipe(nandc);
	if (ret)
		return ums9117_nandc_fail(nandc, ret, "NANDC recipe");
	return 0;
}

static int ums9117_nandc_restore_lifecycle(struct ums9117_nandc *nandc)
{
	void __iomem *regs = nandc->regs[UMS9117_NANDC_RES_NANDC];
	void __iomem *gate_state = nandc->regs[UMS9117_NANDC_RES_GATE_STATE];
	void __iomem *gate_set = nandc->regs[UMS9117_NANDC_RES_GATE_SET];
	void __iomem *gate_clear = nandc->regs[UMS9117_NANDC_RES_GATE_CLEAR];
	void __iomem *reset_state = nandc->regs[UMS9117_NANDC_RES_RESET_STATE];
	void __iomem *reset_set = nandc->regs[UMS9117_NANDC_RES_RESET_SET];
	void __iomem *reset_clear = nandc->regs[UMS9117_NANDC_RES_RESET_CLEAR];
	void __iomem *pin_control = nandc->regs[UMS9117_NANDC_RES_PIN_CONTROL];
	void __iomem *ecc_clock = nandc->regs[UMS9117_NANDC_RES_ECC_CLOCK];
	void __iomem *clock_2x = nandc->regs[UMS9117_NANDC_RES_2X_CLOCK];
	void __iomem *pinmux = nandc->regs[UMS9117_NANDC_RES_PINMUX];
	void __iomem *pinconf = nandc->regs[UMS9117_NANDC_RES_PINCONF];
	u32 value;
	unsigned int i;
	int ret = 0;

	if (nandc->lifecycle_restored)
		return 0;
	if (nandc->gate_touched) {
		if (nandc->gate_enabled)
			ums9117_nandc_abort_command(nandc);
		if (ums9117_nandc_assert_reset(nandc))
			ret = -EIO;
		if (nandc->gate_enabled &&
		    readl(regs + UMS9117_NANDC_SPI_STATUS) &
			    UMS9117_NANDC_SPI_BUSY &&
		    !ret)
			ret = -EBUSY;
		if (nandc->gate_enabled) {
			nandc->last_axim_status =
				readl(regs + UMS9117_NANDC_AXIM_STS);
			if (nandc->last_axim_status && !ret)
				ret = -EIO;
		}
		writel(UMS9117_NANDC_AP_AHB_GATE, gate_clear);
		value = readl(gate_state);
		if (value & UMS9117_NANDC_AP_AHB_GATE && !ret)
			ret = -EIO;
	}
	if (nandc->pin_snapshots_valid) {
		for (i = 0; i < UMS9117_NANDC_PINCONF_COUNT; i++) {
			writel(nandc->pinconf[i].snapshot,
			       pinconf + nandc->pinconf[i].offset);
			if (readl(pinconf + nandc->pinconf[i].offset) !=
				    nandc->pinconf[i].snapshot &&
			    !ret)
				ret = -EIO;
		}
		for (i = 0; i < UMS9117_NANDC_PINMUX_COUNT; i++) {
			writel(nandc->pinmux[i].snapshot,
			       pinmux + nandc->pinmux[i].offset);
			if (readl(pinmux + nandc->pinmux[i].offset) !=
				    nandc->pinmux[i].snapshot &&
			    !ret)
				ret = -EIO;
		}
	}
	if (nandc->clock_snapshots_valid) {
		writel(nandc->ecc_clock_snapshot, ecc_clock);
		writel(nandc->clock_2x_snapshot, clock_2x);
		if ((readl(ecc_clock) != nandc->ecc_clock_snapshot ||
		     readl(clock_2x) != nandc->clock_2x_snapshot) &&
		    !ret)
			ret = -EIO;
	}
	if (nandc->pin_control_snapshot_valid) {
		writel(nandc->pin_control_snapshot, pin_control);
		if (readl(pin_control) != nandc->pin_control_snapshot && !ret)
			ret = -EIO;
	}
	if (ret) {
		nandc->lifecycle_restore_error = ret;
		return ret;
	}

	if (nandc->gate_touched) {
		if (nandc->reset_snapshot & UMS9117_NANDC_AP_AHB_RESET)
			writel(UMS9117_NANDC_AP_AHB_RESET, reset_set);
		else
			writel(UMS9117_NANDC_AP_AHB_RESET, reset_clear);
		value = readl(reset_state);
		if (!!(value & UMS9117_NANDC_AP_AHB_RESET) !=
			    !!(nandc->reset_snapshot &
			       UMS9117_NANDC_AP_AHB_RESET) &&
		    !ret)
			ret = -EIO;
		if (ret) {
			nandc->lifecycle_restore_error = ret;
			return ret;
		}

		if (nandc->gate_snapshot & UMS9117_NANDC_AP_AHB_GATE)
			writel(UMS9117_NANDC_AP_AHB_GATE, gate_set);
		else
			writel(UMS9117_NANDC_AP_AHB_GATE, gate_clear);
		value = readl(gate_state);
		if (!!(value & UMS9117_NANDC_AP_AHB_GATE) !=
			    !!(nandc->gate_snapshot &
			       UMS9117_NANDC_AP_AHB_GATE) &&
		    !ret)
			ret = -EIO;
	}
	/*
	 * AON gate bits at 0x000 and 0x0b0 are shared.  They are intentionally
	 * never cleared here, including on probe failure.
	 */
	nandc->lifecycle_restore_error = ret;
	if (!ret)
		nandc->lifecycle_restored = true;
	return ret;
}

/*
 * Restore B0 and the owned platform state after an activated session.  A
 * hardware or restore error faults the device for the rest of its lifetime.
 */
static int ums9117_nandc_end_session(struct ums9117_nandc *nandc, int ret,
				     bool hardware_error)
{
	int feature_ret;
	int cleanup_ret;

	feature_ret = ums9117_nandc_restore_raw_mode(nandc);
	if (feature_ret) {
		hardware_error = true;
		ret = feature_ret;
	}
	if (hardware_error && !nandc->failed && nandc->gate_enabled)
		ret = ums9117_nandc_fail(nandc, ret, "NAND raw session");
	cleanup_ret = ums9117_nandc_restore_lifecycle(nandc);
	if (feature_ret) {
		nandc->lifecycle_restored = false;
		nandc->lifecycle_restore_error = feature_ret;
		cleanup_ret = feature_ret;
	}
	if (cleanup_ret) {
		nandc->io_faulted = true;
		ret = cleanup_ret;
	}
	if (hardware_error)
		nandc->io_faulted = true;
	return ret;
}

static const struct ums9117_nandc_chip *
ums9117_nandc_select_chip(struct ums9117_nandc *nandc, u32 id_raw)
{
	const struct ums9117_nandc_chip *chip = ums9117_nandc_find_chip(id_raw);

	if (!chip) {
		dev_err(nandc->dev, "unsupported NAND id 0x%08x\n", id_raw);
		return NULL;
	}
	if (nandc->has_declared_id && id_raw != nandc->declared_id) {
		dev_err(nandc->dev,
			"NAND id 0x%08x differs from declared 0x%08x\n", id_raw,
			nandc->declared_id);
		return NULL;
	}
	return chip;
}

/*
 * Identify the fitted chip in one session of RESET, READ_ID and the A0, B0
 * and C0 reads, without writing a feature.  The result is kept for the device
 * lifetime.  A chip outside the table, or one that differs from the declared
 * ID, gets no geometry, so every read is refused.
 */
static int ums9117_nandc_identify(struct ums9117_nandc *nandc)
{
	const struct ums9117_nandc_chip *chip;
	int ret;

	ret = ums9117_nandc_activate(nandc);
	if (!ret)
		ret = ums9117_nandc_read_id(nandc);
	if (!ret)
		ret = ums9117_nandc_read_features(nandc);
	ret = ums9117_nandc_end_session(nandc, ret, ret != 0);
	if (ret)
		return ret;

	nandc->identity_id_raw = nandc->id_raw;
	memcpy(nandc->identity_features, nandc->feature_raw,
	       sizeof(nandc->identity_features));
	nandc->identity_valid = true;
	chip = ums9117_nandc_select_chip(nandc, nandc->id_raw);
	if (chip) {
		nandc->physical_page_bytes =
			chip->page_main_bytes + chip->oob_bytes;
		nandc->page_count =
			(u32)chip->block_count * chip->pages_per_block;
		nandc->raw_bytes =
			(u64)nandc->page_count * nandc->physical_page_bytes;
		nandc->chip = chip;
	}
	return 0;
}

static void ums9117_nandc_release_buffers(struct ums9117_nandc *nandc)
{
	if (nandc->page_buffer) {
		dma_free_coherent(nandc->dev, UMS9117_NANDC_PAGE_MAIN_BYTES,
				  nandc->page_buffer, nandc->page_dma);
		nandc->page_buffer = NULL;
	}
	kvfree(nandc->staging_buffer);
	nandc->staging_buffer = NULL;
}

static ssize_t audit_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ums9117_nandc *nandc = dev_get_drvdata(dev);

	return sysfs_emit(
		buf,
		"state=%s read_only=1\n"
		"arm_boot_mode=0x%08x id_raw=0x%08x\n"
		"last_error=%d total_commands=%u\n"
		"feature_a0=0x%08x feature_b0=0x%08x feature_c0=0x%08x\n"
		"int_raw=0x%08x spi_status=0x%08x axim_status=0x%08x\n"
		"read_error=%d status_polls=%u page_status=0x%08x\n"
		"last_page=%u feature_state_valid=%u read_ecc_disabled=%u\n"
		"raw_feature_b0=0x%08x restored_feature_b0=0x%08x\n"
		"nand_feature_restored=%u feature_restore_error=%d\n"
		"physical_page_bytes=%u page_count=%u raw_bytes=%llu\n"
		"raw_registered=%u io_faulted=%u runtime_sessions=%u runtime_pages=%llu\n"
		"lifecycle_restored=%u lifecycle_restore_error=%d\n"
		"shared_aon_pin_gates_retained=1\n",
		nandc->failed			  ? "failed" :
		nandc->runtime_sessions_completed ? "ready" :
						    "idle",
		nandc->arm_boot_mode, nandc->id_raw, nandc->last_error,
		nandc->completed_operations, nandc->feature_raw[0],
		nandc->feature_raw[1], nandc->feature_raw[2],
		nandc->last_int_raw, nandc->last_spi_status,
		nandc->last_axim_status, nandc->page_read_error,
		nandc->status_poll_count, nandc->page_status, nandc->last_page,
		nandc->features_valid, nandc->read_ecc_disabled,
		nandc->raw_feature_b0, nandc->restored_feature_b0,
		nandc->nand_feature_restored, nandc->feature_restore_error,
		nandc->physical_page_bytes, nandc->page_count, nandc->raw_bytes,
		nandc->raw_registered, nandc->io_faulted,
		nandc->runtime_sessions_completed,
		nandc->runtime_pages_completed, nandc->lifecycle_restored,
		nandc->lifecycle_restore_error);
}
static DEVICE_ATTR_RO(audit);

/*
 * Host tools parse these key=value lines.  A chip outside the table, or one
 * that differs from the declared ID, keeps its ID bytes and features with zero
 * geometry.  The attribute has no value until an open has read both.
 */
static ssize_t geometry_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	static const struct ums9117_nandc_chip unknown_chip = {
		.name = "unknown",
	};
	struct ums9117_nandc *nandc = dev_get_drvdata(dev);
	const struct ums9117_nandc_chip *chip;
	const char *source;
	ssize_t ret;

	if (mutex_lock_interruptible(&nandc->io_mutex))
		return -ERESTARTSYS;
	if (!nandc->identity_valid) {
		ret = -ENODATA;
		goto out_unlock;
	}
	chip = nandc->chip ? nandc->chip : &unknown_chip;
	if (!nandc->chip)
		source = "unknown";
	else if (nandc->has_declared_id)
		source = "dt";
	else
		source = "table";
	ret = sysfs_emit(buf,
			 "id_bytes=%02x%02x\n"
			 "chip=%s\n"
			 "page_main_bytes=%u\n"
			 "oob_bytes=%u\n"
			 "pages_per_block=%u\n"
			 "block_count=%u\n"
			 "raw_bytes=%llu\n"
			 "geometry_source=%s\n"
			 "feature_a0=0x%02x\n"
			 "feature_b0=0x%02x\n"
			 "feature_c0=0x%02x\n",
			 nandc->identity_id_raw & 0xffU,
			 (nandc->identity_id_raw >> 8) & 0xffU, chip->name,
			 chip->page_main_bytes, chip->oob_bytes,
			 chip->pages_per_block, chip->block_count,
			 nandc->raw_bytes, source, nandc->identity_features[0],
			 nandc->identity_features[1],
			 nandc->identity_features[2]);
out_unlock:
	mutex_unlock(&nandc->io_mutex);
	return ret;
}
static DEVICE_ATTR_RO(geometry);

static struct attribute *ums9117_nandc_attrs[] = {
	&dev_attr_audit.attr,
	&dev_attr_geometry.attr,
	NULL,
};

static const struct attribute_group ums9117_nandc_attr_group = {
	.attrs = ums9117_nandc_attrs,
};

static int ums9117_nandc_raw_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ums9117_nandc *nandc =
		container_of(misc, struct ums9117_nandc, raw_misc);
	int ret = 0;

	if (file->f_mode & FMODE_WRITE)
		return -EROFS;
	if (mutex_lock_interruptible(&nandc->io_mutex))
		return -ERESTARTSYS;
	/* An unsupported chip still opens, so its identity can be inspected. */
	if (nandc->shutting_down)
		ret = -ENODEV;
	else if (nandc->io_faulted)
		ret = -EIO;
	else if (!nandc->identity_valid)
		ret = ums9117_nandc_identify(nandc);
	mutex_unlock(&nandc->io_mutex);
	if (ret)
		return ret;
	file->private_data = nandc;
	return 0;
}

static loff_t ums9117_nandc_raw_llseek(struct file *file, loff_t offset,
				       int whence)
{
	struct ums9117_nandc *nandc = file->private_data;

	return fixed_size_llseek(file, offset, whence, nandc->raw_bytes);
}

static ssize_t ums9117_nandc_raw_read_iter(struct kiocb *iocb,
					   struct iov_iter *to)
{
	struct ums9117_nandc *nandc = iocb->ki_filp->private_data;
	loff_t position = iocb->ki_pos;
	loff_t read_position = position;
	size_t requested;
	size_t staged = 0;
	size_t copied;
	u32 pages = 0;
	bool hardware_error = false;
	int ret = 0;

	if (!iov_iter_count(to))
		return 0;
	if (position < 0)
		return -EINVAL;
	/* Identification at open left no geometry for an unsupported chip. */
	if (!nandc->chip)
		return -ENODEV;
	if (position >= nandc->raw_bytes)
		return 0;
	requested =
		min_t(size_t, iov_iter_count(to), UMS9117_NANDC_BATCH_BYTES);
	if (requested > nandc->raw_bytes - position)
		requested = nandc->raw_bytes - position;
	if (mutex_lock_interruptible(&nandc->io_mutex))
		return -ERESTARTSYS;
	if (nandc->shutting_down) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (nandc->io_faulted) {
		ret = -EIO;
		goto out_unlock;
	}
	if (signal_pending(current)) {
		ret = -ERESTARTSYS;
		goto out_unlock;
	}

	ret = ums9117_nandc_activate(nandc);
	if (!ret)
		ret = ums9117_nandc_run_diagnostics(nandc);
	if (ret) {
		hardware_error = true;
		goto out_cleanup;
	}
	while (staged < requested) {
		u32 page_offset;
		u32 page = div_u64_rem((u64)read_position,
				       nandc->physical_page_bytes,
				       &page_offset);
		size_t chunk = min_t(size_t, requested - staged,
				     nandc->physical_page_bytes - page_offset);

		if (READ_ONCE(nandc->shutting_down)) {
			ret = -ENODEV;
			break;
		}
		if (signal_pending(current)) {
			if (!staged)
				ret = -ERESTARTSYS;
			break;
		}
		ret = ums9117_nandc_read_raw_slice(
			nandc, page, page_offset, chunk,
			nandc->staging_buffer + staged);
		if (ret) {
			nandc->page_read_error = ret;
			hardware_error = true;
			break;
		}
		staged += chunk;
		read_position += chunk;
		pages++;
	}

out_cleanup:
	ret = ums9117_nandc_end_session(nandc, ret, hardware_error);
	if (!nandc->io_faulted && pages) {
		nandc->runtime_sessions_completed++;
		nandc->runtime_pages_completed += pages;
	}
	if (ret)
		goto out_unlock;
	copied = copy_to_iter(nandc->staging_buffer, staged, to);
	if (!copied) {
		ret = -EFAULT;
		goto out_unlock;
	}
	position += copied;
	iocb->ki_pos = position;
	ret = copied;
out_unlock:
	mutex_unlock(&nandc->io_mutex);
	return ret;
}

static const struct file_operations ums9117_nandc_raw_fops = {
	.owner = THIS_MODULE,
	.open = ums9117_nandc_raw_open,
	.read_iter = ums9117_nandc_raw_read_iter,
	.llseek = ums9117_nandc_raw_llseek,
};

static int ums9117_nandc_probe(struct platform_device *pdev)
{
	struct ums9117_nandc *nandc;
	int ret;

	nandc = devm_kzalloc(&pdev->dev, sizeof(*nandc), GFP_KERNEL);
	if (!nandc)
		return -ENOMEM;
	nandc->dev = &pdev->dev;
	ret = of_property_read_u32(pdev->dev.of_node, "fplinux,nand-id",
				   &nandc->declared_id);
	if (!ret) {
		if (!ums9117_nandc_find_chip(nandc->declared_id))
			return dev_err_probe(&pdev->dev, -ENODEV,
					     "unsupported NAND identity\n");
		nandc->has_declared_id = true;
	} else if (ret != -EINVAL) {
		return dev_err_probe(&pdev->dev, ret,
				     "invalid NAND identity\n");
	}
	nandc->lifecycle_restored = true;
	mutex_init(&nandc->io_mutex);
	platform_set_drvdata(pdev, nandc);

	ret = ums9117_nandc_map_resources(nandc, pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid NANDC resources\n");
	ret = ums9117_nandc_parse_resources(nandc, pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid NANDC DT data\n");
	ret = ums9117_nandc_check_arm_boot_mode(nandc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "unexpected ARM boot mode sentinel\n");
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "NANDC requires 32-bit coherent DMA\n");
	nandc->page_buffer = dma_alloc_coherent(&pdev->dev,
						UMS9117_NANDC_PAGE_MAIN_BYTES,
						&nandc->page_dma, GFP_KERNEL);
	if (!nandc->page_buffer)
		return -ENOMEM;
	if (upper_32_bits(nandc->page_dma) ||
	    !IS_ALIGNED(nandc->page_dma, 64U)) {
		ret = -ERANGE;
		goto out_release_page;
	}
	nandc->staging_buffer = kvzalloc(UMS9117_NANDC_BATCH_BYTES, GFP_KERNEL);
	if (!nandc->staging_buffer) {
		ret = -ENOMEM;
		goto out_release_page;
	}
	ret = device_add_group(&pdev->dev, &ums9117_nandc_attr_group);
	if (ret) {
		nandc->failed = true;
		nandc->last_error = ret;
		dev_err_probe(&pdev->dev, ret,
			      "could not create NANDC sysfs attributes\n");
		goto out_release_page;
	}
	nandc->attributes_created = true;
	nandc->raw_misc.minor = MISC_DYNAMIC_MINOR;
	nandc->raw_misc.name = "ums9117-nand-raw";
	nandc->raw_misc.fops = &ums9117_nandc_raw_fops;
	nandc->raw_misc.parent = &pdev->dev;
	nandc->raw_misc.mode = 0444;
	ret = misc_register(&nandc->raw_misc);
	if (ret) {
		device_remove_group(&pdev->dev, &ums9117_nandc_attr_group);
		nandc->attributes_created = false;
		nandc->failed = true;
		nandc->last_error = ret;
		dev_err_probe(&pdev->dev, ret,
			      "could not register NAND raw device\n");
		goto out_release_page;
	}
	nandc->raw_registered = true;
	dev_dbg(&pdev->dev,
		"read-only NAND device registered; identification deferred until open\n");
	return 0;

out_release_page:
	ums9117_nandc_release_buffers(nandc);
	return ret;
}

static void ums9117_nandc_remove(struct platform_device *pdev)
{
	struct ums9117_nandc *nandc = platform_get_drvdata(pdev);
	int ret;

	WRITE_ONCE(nandc->shutting_down, true);
	if (nandc->raw_registered) {
		misc_deregister(&nandc->raw_misc);
		nandc->raw_registered = false;
	}
	if (nandc->attributes_created)
		device_remove_group(&pdev->dev, &ums9117_nandc_attr_group);
	mutex_lock(&nandc->io_mutex);
	ret = ums9117_nandc_restore_lifecycle(nandc);
	if (ret) {
		mutex_unlock(&nandc->io_mutex);
		dev_err(&pdev->dev,
			"NANDC baseline restore failed: %pe; retaining coherent buffer\n",
			ERR_PTR(ret));
		return;
	}
	ums9117_nandc_release_buffers(nandc);
	mutex_unlock(&nandc->io_mutex);
}

static void ums9117_nandc_shutdown(struct platform_device *pdev)
{
	ums9117_nandc_remove(pdev);
}

static const struct of_device_id ums9117_nandc_of_match[] = {
	{ .compatible = "fplinux,ums9117-nandc-ro" },
	{}
};
MODULE_DEVICE_TABLE(of, ums9117_nandc_of_match);

static struct platform_driver ums9117_nandc_driver = {
	.probe = ums9117_nandc_probe,
	.remove = ums9117_nandc_remove,
	.shutdown = ums9117_nandc_shutdown,
	.driver = {
		.name = "ums9117-nandc-ro",
		.of_match_table = ums9117_nandc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ums9117_nandc_driver);

MODULE_DESCRIPTION("UMS9117 fixed-command physical NAND reader");
MODULE_LICENSE("GPL");
