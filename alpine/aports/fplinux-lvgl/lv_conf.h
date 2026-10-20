/* SPDX-License-Identifier: MIT */
#ifndef FPLINUX_LV_CONF_H
#define FPLINUX_LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
/* Wayland integration is outside this library's display driver. */
#define LV_USE_WAYLAND 0
/* lv_display_create applies the default theme; the others are never used. */
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_THORVG 0
#define LV_USE_LODEPNG 1
/* Keep RGB565 snapshot support available. */
#define LV_USE_SNAPSHOT 1
/* Include PNG decoding without encoding. */
#define LODEPNG_NO_COMPILE_ENCODER
#define LV_BUILD_DEMOS 0
#define LV_BUILD_EXAMPLES 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
/* This variant is declared in no header; its source enables it by default. */
#define LV_FONT_MONTSERRAT_14_ALIGNED 0

#endif
