/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_SDIO_WRITE_HOST_BOUNDARY_H
#define FPLINUX_MARLIN2_SDIO_WRITE_HOST_BOUNDARY_H

/* Only kernel environment and the external SDIO API are represented here. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;

#define BIT(bit) (1UL << (bit))
#define min_t(type, left, right) \
	((type)(left) < (type)(right) ? (type)(left) : (type)(right))
#define GFP_KERNEL 0
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define IS_ENABLED(option) option
#define SHA256_DIGEST_SIZE 32
#define MMC_QUIRK_LENIENT_FN0 BIT(0)
#define SDIO_ANY_ID 0xffff
#define SDIO_DEVICE(vendor_id, device_id) \
	.vendor = (vendor_id), .device = (device_id)
#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_LICENSE(license)
#define dev_err(...) \
	do {         \
	} while (0)
#define msecs_to_jiffies(milliseconds) (milliseconds)
#define time_after_eq(left, right) ((long)((left) - (right)) >= 0)
#define jiffies_to_msecs(ticks) (ticks)

struct device {
	void *of_node;
	void *driver_data;
};
struct dev_pm_ops {
	int (*suspend)(struct device *);
	int (*resume)(struct device *);
};
struct device_driver {
	const struct dev_pm_ops *pm;
};
struct mmc_host {
	unsigned int max_req_size;
	unsigned int max_seg_size;
	unsigned int max_blk_count;
	unsigned int max_blk_size;
	unsigned int pm_caps;
	unsigned int pm_flags;
};
struct mmc_card {
	struct mmc_host *host;
	unsigned int quirks;
};
struct sdio_func {
	struct mmc_card *card;
	unsigned int num;
	unsigned int max_blksize;
	unsigned int cur_blksize;
	struct device dev;
};
struct firmware {
	size_t size;
	const u8 *data;
};
struct sdio_device_id {
	unsigned int vendor;
	unsigned int device;
};
struct sdio_driver {
	const char *name;
	const struct sdio_device_id *id_table;
	int (*probe)(struct sdio_func *, const struct sdio_device_id *);
	void (*remove)(struct sdio_func *);
	struct device_driver drv;
};

#define MMC_PM_KEEP_POWER (1U << 0)
#define DEFINE_SIMPLE_DEV_PM_OPS(name, suspend_fn, resume_fn) \
	const struct dev_pm_ops name = {                      \
		.suspend = (suspend_fn),                      \
		.resume = (resume_fn),                        \
	}
#define pm_sleep_ptr(pointer) (pointer)

static inline struct sdio_func *dev_to_sdio_func(struct device *dev)
{
	return (struct sdio_func *)((char *)dev -
				    offsetof(struct sdio_func, dev));
}

extern unsigned long jiffies;
void msleep(unsigned int);
void *kmalloc(size_t, int);
void kfree(void *);
void *devm_kzalloc(struct device *, size_t, int);
int device_property_read_string(struct device *, const char *, const char **);
int request_firmware_direct(const struct firmware **, const char *,
			    struct device *);
void release_firmware(const struct firmware *);
void sha256(const u8 *, size_t, u8 *);
bool of_device_is_compatible(void *, const char *);
int dev_err_probe(struct device *, int, const char *, ...);
void sdio_set_drvdata(struct sdio_func *, void *);
void *sdio_get_drvdata(struct sdio_func *);
int sdio_set_host_pm_flags(struct sdio_func *, unsigned int);
void sdio_claim_host(struct sdio_func *);
void sdio_release_host(struct sdio_func *);
int sdio_enable_func(struct sdio_func *);
int sdio_disable_func(struct sdio_func *);
int sdio_set_block_size(struct sdio_func *, unsigned int);
int sdio_memcpy_toio(struct sdio_func *, unsigned int, void *, int);
int sdio_memcpy_toio_fixed_port(struct sdio_func *, unsigned int, void *, int);
int sdio_memcpy_fromio(struct sdio_func *, void *, unsigned int, int);
u32 sdio_readl(struct sdio_func *, unsigned int, int *);
void sdio_writel(struct sdio_func *, u32, unsigned int, int *);
u8 sdio_f0_readb(struct sdio_func *, unsigned int, int *);
void sdio_f0_writeb(struct sdio_func *, u8, unsigned int, int *);

#endif
