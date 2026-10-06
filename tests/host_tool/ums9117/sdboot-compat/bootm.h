/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SDBOOT_BOOTM_H
#define FPLINUX_TEST_SDBOOT_BOOTM_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned long ulong;

struct bootm_headers {
	const char *fit_uname_cfg, *fit_uname_os, *fit_uname_fdt;
	const void *fit_hdr_os, *fit_hdr_fdt;
	int fit_noffset_os, fit_noffset_fdt;
	struct {
		ulong start, end, load, image_len;
		uint8_t type, arch, os, comp;
	} os;
	ulong ep, rd_start, rd_end, initrd_start, initrd_end;
	void *ft_addr;
	ulong ft_len;
};

struct bootm_info {
	const char *addr_img, *cmd_name;
	struct bootm_headers *images;
};

#define BOOTM_STATE_START 1
#define BOOTM_STATE_FINDOS 2
#define BOOTM_STATE_FINDOTHER 4
#define BOOTM_STATE_LOADOS 8
#define BOOTM_STATE_OS_PREP 16
#define CONFIG_SYS_FDT_PAD 0x3000U

void bootm_init(struct bootm_info *bmi);
int bootm_run_states(struct bootm_info *bmi, int states);

#endif
