/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DT_BINDINGS_CLK_SPRD_UMS9117_H
#define _DT_BINDINGS_CLK_SPRD_UMS9117_H

/* AON APB gates */
#define CLK_ADI_EB 0
#define CLK_SPLK_EB 1
#define CLK_EFUSE_EB 2
#define CLK_MBOX_EB 3
#define CLK_THM1_EB 4
#define CLK_THM_RTC_EB 5
#define CLK_GPIO_EB 6
#define CLK_AON_APB_GATE_NUM 7

/* AP APB gates */
#define CLK_I2C0_EB 0
#define CLK_AP_APB_GATE_NUM 1

/* AP AHB gates */
#define CLK_DMA_EB 0
#define CLK_SDIO0_EB 1
#define CLK_AP_AHB_GATE_NUM 2

/* AP functional clocks */
#define CLK_SDIO0 0
#define CLK_AP_CLK_NUM 1

/* AP I2C0 functional clock */
#define CLK_AP_I2C0 0
#define CLK_AP_I2C0_CLK_NUM 1

/* AON sensor clock */
#define CLK_SENSOR0 0
#define CLK_AON_SENSOR_CLK_NUM 1

#endif
