/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Sketch: the flash command layer of a UMS9117 download stage
 * (the "flash stage" the persistent-boot design runs once nandboot
 * is installed: it is the only stage that ever writes NAND).
 *
 * The shape follows the t117_fdl reference exactly: handlers built
 * on common.h/packet.h/cmd_def.h, a dl_main() packet loop whose
 * handlers return a BSL_REP_* reply code (0 = reply already sent),
 * replies assembled in dl_send_buf() and posted with dl_send_packet().
 * Everything below compiles against the t117_fdl headers as they
 * are; only the NAND primitives are extern, mirroring sequences
 * ums9117-nandc-ro.c already drives for reads (the erase/program
 * side needs the same instruction sequences plus READ_STATUS).
 *
 * Channel init, the sync-burst handling and the BSL_REP_VER version
 * reply are the same as in t117_fdl main.c and are not repeated here.
 * The reply codes are pinned one for one by test_flash_reply_codes in
 * selftest.py, against the executable model in nandsim.py.
 */

#include "common.h"
#include "cmd_def.h"
#include "packet.h"

/* The trimmed t117_fdl cmd_def.h has no ENABLE_WRITE_FLASH; it is
 * the SPRD dloader write-unlock command value (spd_dump's 0x1B). */
#ifndef BSL_CMD_ENABLE_WRITE_FLASH
#define BSL_CMD_ENABLE_WRITE_FLASH 0x1B
#endif

/* Region IDs the 4G feature-phone downloaders use. Production code
 * must read the phone's own VBM_BOOT table instead of hard-coding;
 * these are the values the installer defines, and the discovery
 * pass byte-compares what it reads against them. */
#define REGION_BOOT0   0x80000001u
#define REGION_BOOT1   0x80000002u
#define REGION_KERNEL  0x80000003u
#define REGION_USER    0x80000004u

/* The biggest image this stage ever stages: boot1, four blocks. */
#define NAND_BLOCK_BYTES  (2048u * 64)	/* page size x pages per block */
#define STAGED_IMAGE_MAX  (4 * NAND_BLOCK_BYTES)

struct download_ctx {
	uint32_t addr;        /* region id being staged */
	uint32_t size;        /* declared image size from START_DATA */
	uint32_t received;    /* bytes staged by MIDST_DATA so far */
	uint8_t  active;      /* a START_DATA was seen since stage start */
	uint8_t  write_armed; /* ENABLE_WRITE_FLASH: consumed by the commit */
};

static struct download_ctx dl;

/* NAND layer (see the block comment above). */
extern int nand_block_factory_bad(uint32_t block);
extern int nand_erase_block_swept(uint32_t block);
extern int nand_program(uint32_t first_block, const uint8_t *data, uint32_t size);
extern int nand_read_bytes(uint32_t addr, uint8_t *dst, uint32_t size);
extern void nand_stage_payload(uint32_t at, const uint8_t *src, uint16_t len);
extern const uint8_t *nand_staged_image(void);
extern uint8_t nand_verify_buf[STAGED_IMAGE_MAX];
extern int mem_equals(const void *a, const void *b, uint32_t size);

static uint32_t region_blocks(uint32_t region)
{
	/* Production: parsed from the phone's VBM_BOOT table instead. */
	switch (region) {
	case REGION_BOOT0:  return 1;   /* block 0 only: the loader slot  */
	case REGION_BOOT1:  return 4;   /* U-Boot and its headroom       */
	case REGION_KERNEL: return 32;  /* stock app slot, never written  */
	case REGION_USER:   return 256 - 37;
	}
	return 0;
}

static int boot_region(uint32_t region)
{
	uint32_t blocks = region_blocks(region);

	return blocks != 0 && region != REGION_KERNEL && region != REGION_USER;
}

static uint32_t region_first_block(uint32_t region)
{
	return (region == REGION_BOOT0) ? 0 : 1;
}

/* One write transaction per region: erase only the blocks the image
 * needs, program with ECC, then read the whole image back once. */
static int program_region(uint32_t region, const uint8_t *data, uint32_t size)
{
	uint32_t first, need, b;

	if (!boot_region(region))
		return -1;	/* only the two boot slots are programmable */
	if (size > region_blocks(region) * NAND_BLOCK_BYTES)
		return -1;	/* nothing to program if it cannot fit */
	first = region_first_block(region);
	need = (size + NAND_BLOCK_BYTES - 1) / NAND_BLOCK_BYTES;
	for (b = first; b < first + need; b++) {
		if (nand_block_factory_bad(b))
			return -1;
		if (nand_erase_block_swept(b))
			return -1;
	}
	if (nand_program(first, data, size))
		return -1;
	if (nand_read_bytes(first * NAND_BLOCK_BYTES, nand_verify_buf, size))
		return -1;
	return mem_equals(nand_verify_buf, data, size) ? 0 : -1;
}

static int write_enable(void)
{
	dl.write_armed = 1;
	return BSL_REP_ACK;
}

static int data_start(uint8_t *pkt)
{
	unsigned len = READ16_BE(pkt + 2);
	uint32_t addr = READ32_BE(pkt + 4);
	uint32_t size = READ32_BE(pkt + 8);

	if (len != 8)
		return BSL_REP_INVALID_CMD;
	if (!dl.write_armed)
		/* the design's gate: in front of every program too, so a
		 * stray download can never be committed by accident */
		return BSL_REP_VERIFY_ERROR;
	if (!boot_region(addr))
		/* kernel and user are read-only by construction */
		return BSL_REP_DOWN_DEST_ERROR;
	if (size > STAGED_IMAGE_MAX)
		/* the stage buffer is boot-region sized */
		return BSL_REP_DOWN_SIZE_ERROR;
	dl.addr = addr;
	dl.size = size;
	dl.received = 0;
	dl.active = 1;
	return BSL_REP_ACK;
}

static int data_midst(uint8_t *pkt)
{
	unsigned len = READ16_BE(pkt + 2);
	unsigned remain = dl.size - dl.received;

	if (!dl.active || len > remain)
		/* stricter than the reference, which would copy to the
		 * still-uninitialised download address instead */
		return BSL_REP_DOWN_SIZE_ERROR;
	nand_stage_payload(dl.received, pkt + 4, len);
	dl.received += len;
	return BSL_REP_ACK;
}

static int data_end(void)
{
	if (!dl.active || dl.received != dl.size)
		return BSL_REP_DOWN_EARLY_END;
	if (!dl.write_armed)
		return BSL_REP_VERIFY_ERROR;
	dl.write_armed = 0;	/* consumed by the commit, success or not */
	if (program_region(dl.addr, nand_staged_image(), dl.size))
		return BSL_REP_OPERATION_FAILED;
	return BSL_REP_ACK;
}

static int flash_erase(uint8_t *pkt)
{
	unsigned len = READ16_BE(pkt + 2);
	uint32_t addr = READ32_BE(pkt + 4);
	uint32_t size = READ32_BE(pkt + 8);
	uint32_t blocks, b, first;

	if (len != 8)
		return BSL_REP_INVALID_CMD;
	if (!dl.write_armed)
		return BSL_REP_VERIFY_ERROR;
	if (!boot_region(addr))
		/* refuse erasing outside the boot slots: the stock chain
		 * next door stays, armed or not */
		return BSL_REP_OPERATION_FAILED;
	blocks = (size + NAND_BLOCK_BYTES - 1) / NAND_BLOCK_BYTES;
	if (blocks > region_blocks(addr))
		return BSL_REP_OPERATION_FAILED;
	first = region_first_block(addr);
	for (b = first; b < first + blocks; b++) {
		if (nand_erase_block_swept(b))
			return BSL_REP_OPERATION_FAILED;
	}
	/* the arm is kept: only the program commit consumes it */
	return BSL_REP_ACK;
}

static int flash_read(uint8_t *pkt)
{
	unsigned len = READ16_BE(pkt + 2);
	uint32_t addr = READ32_BE(pkt + 4);
	uint32_t size = READ32_BE(pkt + 8);
	uint32_t off, bytes;
	uint8_t *out;

	if (len != 8 && len != 12)
		/* t117_fdl read_flash accepts both forms: with and
		 * without the explicit offset word */
		return BSL_REP_INVALID_CMD;
	off = (len == 12) ? READ32_BE(pkt + 12) : 0;
	if (size >= MAX_PKT_PAYLOAD)
		/* a chunk that cannot fit one packet is refused up front,
		 * exactly like the reference read_flash */
		return BSL_REP_INVALID_CMD;
	if (region_blocks(addr) == 0)
		return BSL_REP_OPERATION_FAILED;
	bytes = region_blocks(addr) * NAND_BLOCK_BYTES;
	if (off >= bytes || size > bytes - off)
		return BSL_REP_OPERATION_FAILED;
	out = dl_send_buf();
	WRITE16_BE(out, BSL_REP_READ_FLASH);
	WRITE16_BE(out + 2, size);
	if (nand_read_bytes(region_first_block(addr) * NAND_BLOCK_BYTES + off,
			    out + 4, size))
		return BSL_REP_OPERATION_FAILED;
	dl_send_packet(out);
	return 0;	/* reply already sent */
}

void flash_stage_main(void)
{
	uint8_t *pkt;

	dl.addr = REGION_BOOT0;
	dl.size = 0;
	dl.received = 0;
	dl.active = 0;
	dl.write_armed = 0;

	for (;;) {
		int ret;

		pkt = dl_get_packet();
		switch (READ16_BE(pkt)) {
		case BSL_CMD_CONNECT:
			ret = BSL_REP_ACK;
			break;

		case BSL_CMD_ENABLE_WRITE_FLASH:
			ret = write_enable();
			break;

		case BSL_CMD_START_DATA:
			ret = data_start(pkt);
			break;

		case BSL_CMD_MIDST_DATA:
			ret = data_midst(pkt);
			break;

		case BSL_CMD_END_DATA:
			ret = data_end();
			break;

		case BSL_CMD_READ_FLASH:
			ret = flash_read(pkt);
			break;

		case BSL_CMD_ERASE_FLASH:
			ret = flash_erase(pkt);
			break;

		default:
			ret = BSL_REP_UNKNOWN_CMD;
			break;
		}
		if (ret > 0)
			dl_send_ack(ret);
	}
}

