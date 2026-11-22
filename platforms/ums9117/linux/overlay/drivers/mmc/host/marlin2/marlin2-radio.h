/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MARLIN2_RADIO_H
#define MARLIN2_RADIO_H

#include <crypto/sha2.h>
#include <linux/mmc/sdio_func.h>
#include <linux/types.h>

#include "marlin2-packet.h"

struct gpio_desc;

struct marlin2_radio_pin {
	u32 offset;
	u32 value;
	const char *value_property;
};

struct marlin2_radio_board {
	const struct marlin2_radio_pin *pins;
	size_t num_pins;
	const char *pins_phandle;
	int (*enable_controls)(struct gpio_desc *enable,
			       struct gpio_desc *reset);
	u8 image_sha256[SHA256_DIGEST_SIZE];
	unsigned int timeout_ms;
	int wcn_voltage_uv;
	bool wifipa_first;
	bool require_get_info;
	bool fm;
};

extern struct sdio_driver marlin2_sdio_driver;

enum marlin2_boot_phase {
	MARLIN2_BOOT_IDLE,
	MARLIN2_BOOT_FIRMWARE_REQUEST,
	MARLIN2_BOOT_CHIP_ID,
	MARLIN2_BOOT_IMAGE_DOWNLOAD,
	MARLIN2_BOOT_CPU_START,
	MARLIN2_BOOT_CALIBRATION_DOWNLOAD,
	MARLIN2_BOOT_CALIBRATION_WAIT,
	MARLIN2_BOOT_CALIBRATION_ACK,
	MARLIN2_BOOT_READY_WAIT,
	MARLIN2_BOOT_READY_ACK,
	MARLIN2_BOOT_GET_INFO,
	MARLIN2_BOOT_COMPLETE,
};

struct marlin2_boot_result {
	enum marlin2_boot_phase phase;
	u32 chip_id[2];
	u32 image_bytes;
	u32 calibration_flag;
	u32 ready_flag;
	u8 interrupt_status;
	int cleanup_error;
	int packet_error;
	struct marlin2_packet_result packet;
	bool image_verified;
	bool calibration_verified;
	bool ready;
	bool packet_attempted;
};

const struct marlin2_radio_board *
marlin2_radio_get_board(struct sdio_func *func);
void marlin2_radio_report(struct sdio_func *func, int error,
			  const struct marlin2_boot_result *result);
bool marlin2_radio_runtime_requested(struct sdio_func *func);
void marlin2_radio_runtime_cleanup_error(struct sdio_func *func, int error);
unsigned long marlin2_radio_deadline(struct sdio_func *func);
struct gpio_desc *marlin2_radio_rx_request(struct sdio_func *func);
int marlin2_sdio_bt_write_locked(struct sdio_func *func, const u8 *data,
				 size_t bytes);

#endif
