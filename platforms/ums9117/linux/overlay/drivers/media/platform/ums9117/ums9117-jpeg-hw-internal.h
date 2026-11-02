/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UMS9117_JPEG_HW_INTERNAL_H
#define UMS9117_JPEG_HW_INTERNAL_H

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "ums9117-jpeg-hw.h"

#define JPEG_MMIO_BYTES 0x11a00U
#define JPEG_GATE BIT(10)
#define JPEG_DCAM_RESET BIT(0)
#define JPEG_RESET BIT(8)
#define JPEG_CCIR_RESET BIT(13)
#define JPEG_CLOCK_MASK (GENMASK(9, 8) | GENMASK(1, 0))
#define JPEG_CLOCK_SELECTOR_MASK GENMASK(1, 0)
#define JPEG_CODEC_CLOCK_SELECTOR 1U
#define DCAM_CCIR_ROUTE 0x5d0U
#define DCAM_CCIR_ROUTE_PARALLEL BIT(0)

#define JPEG_CFG 0x00000U
#define DCAM_AUTO_COPY 0x00004U
#define JPEG_SC_FORMAT 0x00008U
#define JPEG_SOURCE_SIZE 0x0000cU
#define JPEG_DEST_SIZE 0x00010U
#define JPEG_TIMER 0x00014U
#define JPEG_INT_STATUS 0x00020U
#define JPEG_INT_MASK 0x00024U
#define JPEG_INT_CLEAR 0x00028U
#define JPEG_INT_RAW 0x0002cU
#define JPEG_FRAME0_Y 0x00040U
#define JPEG_FRAME0_UV 0x00044U
#define JPEG_FRAME1_Y 0x00048U
#define JPEG_FRAME1_UV 0x0004cU
#define JPEG_STREAM0 0x00050U
#define JPEG_STREAM1 0x00054U
#define JPEG_FRAME6 0x00058U
#define JPEG_BURST_GAP 0x00060U
#define JPEG_ENDIAN 0x00064U
#define JPEG_AHB_STATUS 0x00068U
#define DCAM_CAP_CTRL 0x00100U
#define DCAM_CAP_FRM_CNT 0x00104U
#define DCAM_CAP_START 0x00108U
#define DCAM_CAP_END 0x0010cU
#define DCAM_CAP_DECIMATION 0x00110U
#define DCAM_SPI_WIDTH 0x00124U
#define JPEG_QUANT_TABLE 0x00300U
#define JPEG_SCALER_H_TABLE 0x00200U
#define JPEG_SCALER_V_TABLE 0x00300U
#define JPEG_SCALING_CONFIG 0x00130U
#define JPEG_TRIM_START 0x00134U
#define JPEG_TRIM_SIZE 0x00138U
#define JPEG_SLICE_SIZE 0x0013cU
#define JPEG_HUFF_VALUE_TABLE 0x02000U
#define JPEG_GLOBAL_CFG0 0x10000U
#define JPEG_GLOBAL_CFG1 0x10004U
#define JPEG_BSM_CFG0 0x10400U
#define JPEG_BSM_CFG1 0x10404U
#define JPEG_BSM_CONTROL 0x10408U
#define JPEG_BSM_DATA 0x1040cU
#define JPEG_BSM_BITS 0x10414U
#define JPEG_BSM_DEBUG 0x10418U
#define JPEG_BSM_WRITE_READY 0x10420U
#define JPEG_VLD_CONTROL 0x10800U
#define JPEG_RESTART_COUNT 0x10870U
#define JPEG_DC_Y 0x10874U
#define JPEG_DC_UV 0x10878U
#define JPEG_VLD_MCU_COUNT 0x1087cU
#define JPEG_RESTART_INTERVAL 0x10880U
#define JPEG_HUFF_META_TABLE 0x10884U
#define JPEG_VLC_MCU_COUNT 0x10c00U
#define JPEG_VLC_CONTROL 0x10c04U
#define JPEG_DCT_CONFIG 0x11000U
#define JPEG_DCT_FINISH 0x11040U
#define JPEG_MBIO_CONFIG 0x11800U
#define JPEG_MBIO_PROGRESS 0x11808U
#define JPEG_MBIO_MCU_COUNT 0x1180cU
#define JPEG_MBIO_STATUS 0x11810U
#define JPEG_MBIO_ARM 0x11818U

#define JPEG_CFG_VSP (BIT(0) | BIT(3))
#define JPEG_CFG_CPU_TABLES BIT(4)
#define JPEG_CFG_CPU_ACK BIT(7)
#define JPEG_AHB_BUSY BIT(0)
#define DCAM_CAP_ENABLE BIT(5)
#define DCAM_QOS_READ_MASK GENMASK(15, 12)
#define DCAM_QOS_WRITE_MASK GENMASK(11, 8)
#define DCAM_QOS_MASK (DCAM_QOS_READ_MASK | DCAM_QOS_WRITE_MASK)
#define CPU_QOS_READ_MASK GENMASK(7, 4)
#define CPU_QOS_WRITE_MASK GENMASK(3, 0)
#define DCAM_CAP_FRAME_CLEAR BIT(22)
#define DCAM_CAP_TRANSPORT_MASK (GENMASK(10, 7) | GENMASK(5, 0))
#define DCAM_CAPTURE_DONE BIT(4)
#define DCAM_CAPTURE_ERRORS (BIT(5) | BIT(6) | BIT(10))
#define DCAM_CAPTURE_IRQS (DCAM_CAPTURE_DONE | DCAM_CAPTURE_ERRORS)
#define JPEG_BSM_BUFFER0 BIT(31)
#define JPEG_BSM_BUFFERS GENMASK(31, 30)
#define JPEG_BSM_READY BIT(31)
#define JPEG_MBIO_FRAME_DONE BIT(2)

#define JPEG_IRQ_BSM BIT(7)
#define JPEG_IRQ_VLC BIT(8)
#define JPEG_IRQ_DONE BIT(9)
#define JPEG_IRQ_TIMEOUT BIT(12)
#define JPEG_IRQ_VLD_ERROR BIT(13)
#define JPEG_DECODE_IRQ_ERRORS (JPEG_IRQ_TIMEOUT | JPEG_IRQ_VLD_ERROR)
#define JPEG_DECODE_IRQ_ENABLED \
	(JPEG_IRQ_BSM | JPEG_IRQ_DONE | JPEG_DECODE_IRQ_ERRORS)
#define JPEG_ENCODE_IRQ_ERRORS (BIT(11) | BIT(12) | BIT(13))
#define JPEG_ENCODE_IRQ_ENABLED \
	(JPEG_IRQ_BSM | JPEG_IRQ_VLC | JPEG_IRQ_DONE | JPEG_ENCODE_IRQ_ERRORS)
#define JPEG_SCALE_IRQ_DONE BIT(4)
#define JPEG_SCALE_IRQ_ERRORS (BIT(5) | BIT(6) | BIT(10))
#define JPEG_SCALE_IRQ_ENABLED (JPEG_SCALE_IRQ_DONE | JPEG_SCALE_IRQ_ERRORS)
#define JPEG_IRQ_KNOWN GENMASK(13, 0)

#define JPEG_WAIT_MS 3000U
#define JPEG_POLL_US 100000U
#define JPEG_GUARD_BYTES 256U
#define JPEG_GUARD_VALUE 0xa5
#define JPEG_STREAM_TAIL_BYTES 64U
/* 2048 pixels * 64 rows plus guards fits a 256 KiB DMA allocation. */
#define JPEG_SLICE_ROWS 64U
#define JPEG_ENCODE_MAX_OUTPUT_SIZE UMS9117_JPEG_MAX_INPUT_SIZE
#define JPEG_ENCODE_FILL 0xcc

#define JPEG_SCALE_H_WORDS 48U
#define JPEG_SCALE_V_WORDS 68U

enum ums9117_jpeg_hw_operation {
	JPEG_OPERATION_NONE,
	JPEG_OPERATION_DECODE,
	JPEG_OPERATION_ENCODE,
	JPEG_OPERATION_SCALE,
	JPEG_OPERATION_COUNT,
};

struct ums9117_jpeg_dma_buffer {
	void *allocation;
	dma_addr_t dma;
	size_t allocation_size;
	size_t size;
};

struct ums9117_jpeg_hw_operation_stats {
	u64 requests;
	u64 completed;
	u64 units;
	u64 irqs;
	u64 completion_irqs;
	u64 error_irqs;
	u64 timeouts;
	u64 cancellations;
	u64 resets;
	u64 guard_failures;
	u64 input_failures;
	u64 output_failures;
	u64 idle_failures;
	u64 last_duration_ns;
	u64 last_bytes;
	u32 last_events;
	u32 last_ahb;
	u32 last_command;
	u32 recovery_ahb;
	int last_result;
};

struct ums9117_jpeg_encode_timeout_stats {
	bool valid;
	bool poll_valid;
	u32 strip;
	u32 poll_offset;
	u32 poll_mask;
	u32 poll_expected;
	u32 poll_value;
	u32 irq_events;
	u32 irq_raw;
	u32 bsm_debug;
	u32 bsm_write_ready;
	u32 bsm_bits;
	u32 vlc_control;
	u32 ahb;
};

struct ums9117_jpeg_hw_stats {
	u64 slices;
	u64 bsm_irqs;
	u32 last_status;
	u32 last_raw;
	u32 last_mbio;
	u32 last_progress;
	u32 last_ahb;
	u32 last_bsm_cfg0;
	u32 last_bsm_cfg1;
	u32 last_bsm_debug;
	u32 last_vld;
	u32 recovery_ahb;
	int last_result;
	struct ums9117_jpeg_hw_operation_stats operation[JPEG_OPERATION_COUNT];
	struct ums9117_jpeg_encode_timeout_stats encode_timeout;
};

struct ums9117_jpeg_hw {
	struct device *dev;
	void __iomem *base;
	void __iomem *gate_state;
	void __iomem *gate_set;
	void __iomem *gate_clear;
	void __iomem *reset_set;
	void __iomem *reset_clear;
	void __iomem *clock;
	void __iomem *cpu_qos;
	void __iomem *dcam_qos;
	struct regmap *aon_apb;
	u32 saved_gate;
	u32 saved_clock;
	u32 saved_qos;
	int irq;
	spinlock_t lock;
	struct mutex owner_lock;
	enum ums9117_dcam_owner owner;
	unsigned int codec_users;
	bool owner_active;
	bool codec_waiting;
	bool capture_pending;
	bool codec_turn;
	wait_queue_head_t owner_wait;
	u32 codec_saved_clock;
	u32 capture_saved_clock;
	u32 capture_saved_route;
	void (*capture_irq)(void *data, u32 status);
	void (*capture_ready)(void *data);
	void *capture_irq_data;
	struct completion completion;
	atomic_t cancelled;
	enum ums9117_jpeg_hw_operation operation;
	u32 active_encode_strip;
	u32 active_irq_mask;
	u32 active_irq_count;
	u32 operation_events;
	u32 events;
	u32 irq_raw;
	bool failed;
	struct ums9117_jpeg_dma_buffer buffers[4];
	struct ums9117_jpeg_hw_stats stats;
	struct dentry *debugfs;
};

u32 ums9117_jpeg_read(struct ums9117_jpeg_hw *hw, u32 reg);
void ums9117_jpeg_write(struct ums9117_jpeg_hw *hw, u32 reg, u32 value);
void *ums9117_jpeg_buffer_data(struct ums9117_jpeg_dma_buffer *buffer);
dma_addr_t ums9117_jpeg_buffer_dma(struct ums9117_jpeg_dma_buffer *buffer);
void ums9117_jpeg_free_buffers(struct ums9117_jpeg_hw *hw);
int ums9117_jpeg_prepare_buffers(struct ums9117_jpeg_hw *hw,
				 const size_t sizes[4]);
bool ums9117_jpeg_guards_ok(struct ums9117_jpeg_hw *hw);
bool ums9117_jpeg_buffers_disjoint(struct ums9117_jpeg_hw *hw);
u32 ums9117_jpeg_events(struct ums9117_jpeg_hw *hw);
void ums9117_jpeg_encode_snapshot_timeout(struct ums9117_jpeg_hw *hw,
					  bool poll_valid, u32 poll_offset,
					  u32 poll_mask, u32 poll_expected);
void ums9117_jpeg_set_operation(struct ums9117_jpeg_hw *hw,
				enum ums9117_jpeg_hw_operation operation,
				u32 irq_mask);
void ums9117_jpeg_clear_operation(struct ums9117_jpeg_hw *hw);
void ums9117_jpeg_reset_submission(struct ums9117_jpeg_hw *hw);
unsigned long ums9117_jpeg_subdeadline(unsigned long deadline);
int ums9117_jpeg_operation_check(struct ums9117_jpeg_hw *hw, u32 error_mask,
				 u32 full_mask, unsigned long deadline);
int ums9117_jpeg_operation_poll(struct ums9117_jpeg_hw *hw, u32 reg, u32 mask,
				u32 expected, u32 error_mask, u32 full_mask,
				unsigned long deadline);
void ums9117_jpeg_operation_request(struct ums9117_jpeg_hw *hw,
				    enum ums9117_jpeg_hw_operation operation);
void ums9117_jpeg_operation_result(struct ums9117_jpeg_hw *hw,
				   enum ums9117_jpeg_hw_operation operation,
				   int result, u64 units, u64 bytes,
				   u64 started_ns);
void ums9117_jpeg_reset(struct ums9117_jpeg_hw *hw);
int ums9117_jpeg_stop(struct ums9117_jpeg_hw *hw);
void ums9117_jpeg_stats_create(struct ums9117_jpeg_hw *hw);

#endif
