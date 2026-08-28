/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UBOOT_STAGE0_HANDOFF_H
#define FPLINUX_UBOOT_STAGE0_HANDOFF_H

#include "ums9117-boot-contract.h"

const struct fplinux_uboot_handoff *fplinux_uboot_handoff(void);
const struct fplinux_stage0_ops *fplinux_stage0_ops(void);

#endif
