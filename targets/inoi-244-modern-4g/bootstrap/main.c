// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "syscode.h"
#include "ums9117-bootstrap/bootstrap.h"

#define RAM_REQUIRED 0x03000000u
#define ZIMAGE_STAGE 0x82000000u
#define ZIMAGE_LIMIT 0x00d00000u
#define DTB_STAGE 0x82e00000u
#define DTB_LIMIT 0x00010000u

static void record_stage(uint32_t stage, const char *message)
{
	fprintf(stderr, "INOI244_LINUX_BOOTSTRAP stage=%lu message=%s\n",
		(unsigned long)stage, message);
}

static __attribute__((noreturn)) void fail(uint32_t code, const char *message)
{
	fprintf(stderr,
		"INOI244_LINUX_BOOTSTRAP stage=238 error=%lu message=%s\n",
		(unsigned long)code, message);
	for (;;)
		;
}

static int enable_and_probe_sprd_timer(void)
{
	struct ums9117_bootstrap_timer_result timer;
	int timer_ok;

	ums9117_bootstrap_enable_timer_gates(NULL);
	timer_ok = ums9117_bootstrap_probe_timer(&timer);

	fprintf(stderr,
		"INOI244_SPRD_TIMER syscnt=%lu->%lu polls=%lu "
		"value=%lu shadow=%lu int=0x%08lx\n",
		(unsigned long)timer.syscnt_before,
		(unsigned long)timer.syscnt_after, timer.polls,
		(unsigned long)timer.value, (unsigned long)timer.shadow,
		(unsigned long)timer.int_status);

	return timer_ok;
}

static void quiesce_usb(void)
{
	if ((ums9117_bootstrap_quiesce_usb_dma_channel(5) &
	     UMS9117_BOOTSTRAP_DMA_OK) != UMS9117_BOOTSTRAP_DMA_OK)
		fail(6, "USB DMA5 QUIESCE FAIL");
	if ((ums9117_bootstrap_quiesce_usb_dma_channel(21) &
	     UMS9117_BOOTSTRAP_DMA_OK) != UMS9117_BOOTSTRAP_DMA_OK)
		fail(7, "USB DMA21 QUIESCE FAIL");

	ums9117_bootstrap_cleanup_usb_dma_and_disconnect();
}

int main(int argc, char **argv)
{
	uint32_t ram_bytes = *(volatile uint32_t *)(uintptr_t)0x80100000u;
	size_t zimage_bytes = ums9117_bootstrap_zimage_size();
	size_t dtb_bytes = ums9117_bootstrap_dtb_size();

	(void)argc;
	(void)argv;

	fprintf(stderr,
		"INOI244_LINUX_BOOTSTRAP stage=0 message=ENTRY "
		"ram=0x%08lx zimage=%lu dtb=%lu\n",
		(unsigned long)ram_bytes, (unsigned long)zimage_bytes,
		(unsigned long)dtb_bytes);

	if (ram_bytes < RAM_REQUIRED)
		fail(1, "48MB RAM REQUIRED");
	if (!enable_and_probe_sprd_timer())
		fail(2, "SPRD TIMER FAIL");
	record_stage(1, "SPRD TIMER OK");

	if (!zimage_bytes || zimage_bytes > ZIMAGE_LIMIT)
		fail(3, "BAD KERNEL SIZE");
	record_stage(2, "COPY KERNEL");
	ums9117_bootstrap_copy_zimage(ZIMAGE_STAGE, zimage_bytes);

	if (!dtb_bytes || dtb_bytes > DTB_LIMIT)
		fail(4, "BAD DTB SIZE");
	record_stage(3, "COPY DTB");
	ums9117_bootstrap_copy_dtb(DTB_STAGE, dtb_bytes);
	record_stage(4, "PAYLOAD READY");

	/* The host stops libc_server after observing this final USB record. */
	record_stage(5, "PREPARE LINUX");
	quiesce_usb();
	clean_invalidate_dcache();
	invalidate_icache();

	ums9117_linux_handoff(ZIMAGE_STAGE, DTB_STAGE);
}
