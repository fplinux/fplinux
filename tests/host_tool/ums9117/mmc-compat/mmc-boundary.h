/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_MMC_BOUNDARY_H
#define FPLINUX_TEST_MMC_BOUNDARY_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>

#include <linux/types.h>

#define __iomem
#define __aligned(value) __attribute__((aligned(value)))
#define IS_ALIGNED(value, alignment) (!((value) & ((alignment) - 1U)))
#define cpu_to_le16(value) ((u16)(value))
#define cpu_to_le32(value) ((u32)(value))
#define UCLASS_MMC 1
#define CMD_RET_SUCCESS 0
#define log_err(...) fprintf(stderr, __VA_ARGS__)
#define log_debug(...) ((void)0)

#define MMC_MODE_1BIT (1U << 0)
#define MMC_MODE_4BIT (1U << 1)
#define MMC_MODE_HS (1U << 2)
#define MMC_CAP_NONREMOVABLE (1U << 14)
#define MMC_VDD_29_30 (1U << 17)
#define MMC_VDD_30_31 (1U << 18)
#define MMC_DATA_READ 1U
#define MMC_RSP_PRESENT (1U << 0)
#define MMC_RSP_136 (1U << 1)
#define MMC_RSP_CRC (1U << 2)
#define MMC_RSP_BUSY (1U << 3)
#define MMC_RSP_OPCODE (1U << 4)
#define MMC_RSP_NONE 0U
#define MMC_RSP_R1 (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_RSP_R1b (MMC_RSP_R1 | MMC_RSP_BUSY)
#define MMC_RSP_R2 (MMC_RSP_PRESENT | MMC_RSP_136 | MMC_RSP_CRC)
#define MMC_RSP_R3 MMC_RSP_PRESENT
#define MMC_RSP_R6 MMC_RSP_R1
#define MMC_RSP_R7 MMC_RSP_R1
#define R1_APP_CMD (1U << 5)
#define SD_HS 1U

#define MMC_CMD_GO_IDLE_STATE 0U
#define MMC_CMD_ALL_SEND_CID 2U
#define MMC_CMD_SET_RELATIVE_ADDR 3U
#define MMC_CMD_SET_DSR 4U
#define MMC_CMD_SELECT_CARD 7U
#define MMC_CMD_SEND_CSD 9U
#define MMC_CMD_SEND_CID 10U
#define MMC_CMD_STOP_TRANSMISSION 12U
#define MMC_CMD_SEND_STATUS 13U
#define MMC_CMD_SET_BLOCKLEN 16U
#define MMC_CMD_READ_SINGLE_BLOCK 17U
#define MMC_CMD_APP_CMD 55U
#define SD_CMD_SEND_RELATIVE_ADDR 3U
#define SD_CMD_SWITCH_FUNC 6U
#define SD_CMD_SEND_IF_COND 8U
#define SD_CMD_APP_SET_BUS_WIDTH 6U
#define SD_CMD_APP_SEND_OP_COND 41U
#define SD_CMD_APP_SEND_SCR 51U

typedef unsigned long long lbaint_t;

struct mmc_config {
	const char *name;
	u32 host_caps;
	u32 voltages;
	u32 f_min;
	u32 f_max;
	u32 b_max;
};

struct mmc {
	struct mmc_config *cfg;
	void *priv;
	u32 bus_width;
	u32 selected_mode;
	u32 clock;
	bool clk_disable;
	int has_init;
	int init_in_progress;
	int op_cond_pending;
};

struct mmc_cmd {
	u32 cmdidx;
	u32 cmdarg;
	u32 resp_type;
	u32 response[4];
};

struct mmc_data {
	void *dest;
	u32 flags;
	u32 blocks;
	u32 blocksize;
};

struct mmc_uclass_priv {
	struct mmc *mmc;
};

struct udevice {
	const char *name;
	bool non_removable;
	void *plat;
	void *priv;
	struct mmc_uclass_priv uclass_priv;
};

struct udevice_id {
	const char *compatible;
};

struct dm_mmc_ops {
	int (*send_cmd)(struct udevice *, struct mmc_cmd *, struct mmc_data *);
	int (*set_ios)(struct udevice *);
	int (*get_cd)(struct udevice *);
	int (*get_wp)(struct udevice *);
	int (*host_power_cycle)(struct udevice *);
	int (*reinit)(struct udevice *);
	int (*get_b_max)(struct udevice *, void *, lbaint_t);
};

struct driver {
	const char *name;
	int id;
	const struct udevice_id *of_match;
	int (*bind)(struct udevice *);
	int (*probe)(struct udevice *);
	int (*remove)(struct udevice *);
	const struct dm_mmc_ops *ops;
	size_t plat_auto;
	size_t priv_auto;
};

struct cmd_tbl {
	int (*handler)(struct cmd_tbl *, int, int, char *const[]);
};

#define U_BOOT_DRIVER(name) const struct driver name
#define U_BOOT_CMD(name, maxargs, repeatable, function, usage, help) \
	struct cmd_tbl test_command_##name = { .handler = function }

static inline void *dev_get_plat(struct udevice *dev)
{
	return dev->plat;
}

static inline void *dev_get_priv(struct udevice *dev)
{
	return dev->priv;
}

static inline void *dev_get_uclass_priv(struct udevice *dev)
{
	return &dev->uclass_priv;
}

bool dev_read_bool(struct udevice *dev, const char *property);
int mmc_bind(struct udevice *dev, struct mmc *mmc, struct mmc_config *cfg);
u32 readl(const void *address);
void writel(u32 value, void *address);
u64 timer_get_us(void);
void udelay(u32 usec);
void mdelay(u32 msec);
void mb(void);
void hang(void) __attribute__((noreturn));

#endif
