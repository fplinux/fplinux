/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_UBOOT_STAGE0_HANDOFF_H
#define FPLINUX_UMS9117_UBOOT_STAGE0_HANDOFF_H

#include "ums9117-boot-contract.h"

const struct fplinux_uboot_handoff *ums9117_uboot_handoff_get(void);
const struct fplinux_stage0_ops *ums9117_stage0_ops(void);

#endif
