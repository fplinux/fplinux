// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "marlin2-packet.h"

#define MARLIN2_SDIO_PACKET_PORT 0x20
#define MARLIN2_GET_INFO_TIMEOUT_MS 3000U

int marlin2_packet_get_info_locked(struct sdio_func *func,
				   struct gpio_desc *rx_request, u32 token,
				   unsigned long deadline,
				   struct marlin2_packet_result *result)
{
	struct mmc_host *host = func->card->host;
	unsigned long command_deadline =
		jiffies + msecs_to_jiffies(MARLIN2_GET_INFO_TIMEOUT_MS);
	size_t read_bytes = MARLIN2_PACKET_RX_INITIAL_BYTES;
	u32 pending_payload_bytes = 0;
	u8 *buffer;
	int pending;
	int ret;

	memset(result, 0, sizeof(*result));
	result->token = token;
	result->multi_block = func->card->cccr.multi_block;
	if (func->num != 1 || !rx_request || func->cur_blksize != 512 ||
	    !func->card->cccr.multi_block ||
	    host->max_req_size < MARLIN2_PACKET_RX_MAX_BYTES ||
	    host->max_seg_size < MARLIN2_PACKET_RX_MAX_BYTES ||
	    host->max_blk_count < MARLIN2_PACKET_RX_MAX_BYTES / 512)
		return -EPROTONOSUPPORT;
	buffer = kmalloc(MARLIN2_PACKET_RX_MAX_BYTES, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	marlin2_packet_build_get_info(buffer, token);
	if (time_after_eq(jiffies, deadline) ||
	    time_after_eq(jiffies, command_deadline)) {
		ret = -ETIMEDOUT;
		goto out_free;
	}
	/* Packet mode is a FIFO: every CMD53 byte uses the same function address. */
	ret = sdio_writesb(func, MARLIN2_SDIO_PACKET_PORT, buffer,
			   MARLIN2_PACKET_TX_BYTES);
	if (ret)
		goto out_free;
	result->request_sent = true;
	for (;;) {
		if (time_after_eq(jiffies, deadline) ||
		    time_after_eq(jiffies, command_deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		if (!pending_payload_bytes) {
			pending = gpiod_get_value_cansleep(rx_request);
			if (pending < 0) {
				ret = pending;
				break;
			}
			if (!pending) {
				usleep_range(1000, 2000);
				continue;
			}
			result->rx_request_seen = true;
		}
		ret = sdio_readsb(func, buffer, MARLIN2_SDIO_PACKET_PORT,
				  read_bytes);
		if (ret)
			break;
		ret = marlin2_packet_decode(buffer, read_bytes, token, result);
		if (ret) {
			if (ret == 1)
				ret = 0;
			break;
		}
		pending_payload_bytes = result->next_payload_bytes;
		read_bytes = marlin2_packet_next_read(pending_payload_bytes);
	}
out_free:
	kfree(buffer);
	return ret;
}
