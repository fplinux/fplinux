/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PCSL configuration for fplinux-phoneme. Upstream generates this header in
 * the PCSL make (pcsl/GNUmakefile, generate_config); the values name the
 * selected PCSL modules (see midp/natives.toml).
 */
#ifndef _PCSL_CONFIG_H_
#define _PCSL_CONFIG_H_

#define FILE_MODULE posix
#define NETWORK_MODULE bsd
#define MEMORY_MODULE malloc
#define PRINT_MODULE stdout
#define STRING_MODULE utf16

#endif /* _PCSL_CONFIG_H_ */
