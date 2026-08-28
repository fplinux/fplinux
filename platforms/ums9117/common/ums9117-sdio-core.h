/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_SDIO_CORE_H
#define FPLINUX_UMS9117_SDIO_CORE_H

#include "ums9117-sdio-regs.h"

enum ums9117_sdio_clock_profile {
	UMS9117_SDIO_CLOCK_LEGACY,
	UMS9117_SDIO_CLOCK_HIGH_SPEED,
};

enum ums9117_sdio_response_type {
	UMS9117_SDIO_RESPONSE_NONE,
	UMS9117_SDIO_RESPONSE_LONG,
	UMS9117_SDIO_RESPONSE_SHORT,
	UMS9117_SDIO_RESPONSE_SHORT_BUSY,
	UMS9117_SDIO_RESPONSE_OCR,
};

enum ums9117_sdio_completion_order {
	UMS9117_SDIO_RESPONSE_BEFORE_ACK,
	UMS9117_SDIO_ACK_BEFORE_RESPONSE,
};

struct ums9117_sdio_io {
	void *context;
	u32 (*read)(void *context, enum ums9117_sdio_reg reg);
	void (*write)(void *context, enum ums9117_sdio_reg reg, u32 value);
	u64 (*time_us)(void *context);
	void (*delay_us)(void *context, u32 usec);
	void (*sleep_us)(void *context, u32 min, u32 max);
	void (*data_barrier)(void *context);
};

struct ums9117_sdio_state {
	u32 clock_snapshot;
	u32 host_control1_snapshot;
	u32 status_enable_snapshot;
	bool snapshot_valid;
	bool card_clock_on;
	bool physical_width4;
	u32 actual_clock_hz;
};

struct ums9117_sdio_data_setup {
	u32 blocks;
	u32 block_size;
	u32 adma_address;
};

struct ums9117_sdio_completion {
	u32 status;
	u32 owned_status;
	u32 status_readback;
	u32 auto_command_status;
	u32 response[4];
};

struct ums9117_sdio_transition_record {
	u32 clock_before;
	u32 clock_stopped;
	u32 clock_candidate;
	u32 clock_readback;
	u32 clock_after;
	u32 control_before;
	u32 control_after;
};

int ums9117_sdio_snapshot_controller(const struct ums9117_sdio_io *io,
				     struct ums9117_sdio_state *state);
void ums9117_sdio_mask_and_ack(const struct ums9117_sdio_io *io);
int ums9117_sdio_reset_controller(const struct ums9117_sdio_io *io);
int ums9117_sdio_set_1bit(const struct ums9117_sdio_io *io,
			  struct ums9117_sdio_state *state);
int ums9117_sdio_configure_1bit(const struct ums9117_sdio_io *io,
				struct ums9117_sdio_state *state);
void ums9117_sdio_restore_controller(const struct ums9117_sdio_io *io,
				     const struct ums9117_sdio_state *state);
int ums9117_sdio_enable_ident_clock(const struct ums9117_sdio_io *io,
				    struct ums9117_sdio_state *state);
int ums9117_sdio_disable_card_clock(const struct ums9117_sdio_io *io,
				    struct ums9117_sdio_state *state);
int ums9117_sdio_set_operational_clock(
	const struct ums9117_sdio_io *io, struct ums9117_sdio_state *state,
	enum ums9117_sdio_clock_profile profile,
	struct ums9117_sdio_transition_record *record);
int ums9117_sdio_validate_active(const struct ums9117_sdio_io *io,
				 const struct ums9117_sdio_state *state);
int ums9117_sdio_wait_inhibit(const struct ums9117_sdio_io *io, bool data);
int ums9117_sdio_wait_quiescent(const struct ums9117_sdio_io *io,
				u32 timeout_us, u32 *last_present);
int ums9117_sdio_abort(const struct ums9117_sdio_io *io,
		       struct ums9117_sdio_state *state, u32 timeout_us);

int ums9117_sdio_response_flags(enum ums9117_sdio_response_type type,
				u16 *flags);
int ums9117_sdio_status_error(u32 status);
int ums9117_sdio_r1_error(u32 response);
bool ums9117_sdio_status_terminal(u32 status, u32 terminal_bit);
int ums9117_sdio_prepare_request(const struct ums9117_sdio_io *io,
				 const struct ums9117_sdio_data_setup *data);
void ums9117_sdio_issue_request(const struct ums9117_sdio_io *io, u32 argument,
				u16 command, u16 transfer, u32 signal);
void ums9117_sdio_capture_completion(
	const struct ums9117_sdio_io *io, u32 status, bool response_136,
	bool mask_signal, enum ums9117_sdio_completion_order order,
	struct ums9117_sdio_completion *completion);
int ums9117_sdio_validate_completion(
	const struct ums9117_sdio_completion *completion, u32 required_status);

#endif
