/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LCM_HOST_KCONFIG_H
#define LCM_HOST_KCONFIG_H

/* The host component links only the LCM transport. */
#define CONFIG_DRM_UMS9117_SPI 0
#define CONFIG_DRM_UMS9117_LCM 1
#define IS_ENABLED(option) (option)

#endif
