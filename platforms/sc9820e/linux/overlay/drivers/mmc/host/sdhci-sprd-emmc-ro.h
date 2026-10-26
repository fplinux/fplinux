/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SC9820E_EMMC_RO_H
#define SC9820E_EMMC_RO_H

#include <linux/errno.h>
#include <linux/mmc/core.h>
#include <linux/mmc/mmc.h>

static inline bool sc9820e_emmc_ro_switch_valid(u32 arg)
{
	u32 index = (arg >> 16) & 0xff;
	u32 value = (arg >> 8) & 0xff;

	if (arg != (MMC_SWITCH_MODE_WRITE_BYTE << 24 | index << 16 |
		    value << 8 | EXT_CSD_CMD_SET_NORMAL))
		return false;

	/* Only transient settings used by legacy identification and shutdown. */
	switch (index) {
	case EXT_CSD_ERASE_GROUP_DEF:
	case EXT_CSD_HPI_MGMT:
	case EXT_CSD_CACHE_CTRL:
	case EXT_CSD_FLUSH_CACHE:
		return value == 1;
	case EXT_CSD_POWER_OFF_NOTIFICATION:
		return value == EXT_CSD_POWER_ON ||
		       value == EXT_CSD_POWER_OFF_SHORT ||
		       value == EXT_CSD_POWER_OFF_LONG;
	default:
		return false;
	}
}

static inline bool sc9820e_emmc_ro_command_valid(const struct mmc_command *cmd)
{
	switch (cmd->opcode) {
	case MMC_GO_IDLE_STATE:
		return cmd->arg == 0;
	case MMC_SET_BLOCKLEN:
		return cmd->arg == 512;
	case MMC_SWITCH:
		return sc9820e_emmc_ro_switch_valid(cmd->arg);
	case MMC_SEND_OP_COND:
	case MMC_ALL_SEND_CID:
	case MMC_SET_RELATIVE_ADDR:
	case MMC_SLEEP_AWAKE:
	case MMC_SELECT_CARD:
	case MMC_SEND_EXT_CSD:
	case MMC_SEND_CSD:
	case MMC_SEND_CID:
	case MMC_STOP_TRANSMISSION:
	case MMC_SEND_STATUS:
	case MMC_READ_SINGLE_BLOCK:
	case MMC_READ_MULTIPLE_BLOCK:
		return true;
	default:
		return false;
	}
}

static inline int sc9820e_emmc_ro_request_check(const struct mmc_request *mrq)
{
	bool read_data;

	/* USER reads use explicit STOP; CMD23 and other precommands stay off. */
	if (!mrq->cmd || mrq->sbc || !sc9820e_emmc_ro_command_valid(mrq->cmd) ||
	    (mrq->stop && (mrq->stop->opcode != MMC_STOP_TRANSMISSION ||
			   mrq->stop->arg != 0)))
		return -EROFS;

	read_data = mrq->cmd->opcode == MMC_SEND_EXT_CSD ||
		    mrq->cmd->opcode == MMC_READ_SINGLE_BLOCK ||
		    mrq->cmd->opcode == MMC_READ_MULTIPLE_BLOCK;
	if (!mrq->data)
		return read_data ? -EROFS : 0;
	if (!read_data ||
	    (mrq->data->flags & (MMC_DATA_READ | MMC_DATA_WRITE)) !=
		    MMC_DATA_READ ||
	    mrq->data->blksz != 512 || !mrq->data->blocks)
		return -EROFS;
	if (mrq->cmd->opcode != MMC_READ_MULTIPLE_BLOCK &&
	    mrq->data->blocks != 1)
		return -EROFS;

	return 0;
}

#endif
