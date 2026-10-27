/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_TEXT_CODEC_H
#define FPLINUX_MRE_TEXT_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Strict BMP text, no embedded NUL; LF, CR and TAB are allowed. Decode can
 * validate/count with output == NULL. Decode capacity is uint16_t units;
 * encode capacity is bytes. Both capacities include the trailing NUL. */
bool mre_utf8_decode(const unsigned char *input, size_t bytes, uint16_t *output,
		     size_t capacity, size_t *units);
bool mre_utf8_encode(const uint16_t *input, size_t units, unsigned char *output,
		     size_t capacity, size_t *bytes);
bool mre_text_save_fits(size_t bytes, size_t units);

#endif
