/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_MEDIA_AMR_NB_H
#define FPLINUX_MEDIA_AMR_NB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FPLINUX_MEDIA_AMR_NB_RATE 8000U
#define FPLINUX_MEDIA_AMR_NB_MAGIC "#!AMR\n"

struct fplinux_media_amr_nb;

/* The caller owns and bounds the source and has checked the file signature. */
struct fplinux_media_amr_nb *
fplinux_media_amr_nb_open(int (*read_exact)(void *context, void *out,
					    size_t count, uint64_t offset),
			  void *context, uint64_t size);
void fplinux_media_amr_nb_close(struct fplinux_media_amr_nb *decoder);
ssize_t fplinux_media_amr_nb_read(struct fplinux_media_amr_nb *decoder,
				  int16_t *samples, size_t capacity,
				  uint64_t *total_frames);
bool fplinux_media_amr_nb_failed(const struct fplinux_media_amr_nb *decoder);
int fplinux_media_amr_nb_rewind(struct fplinux_media_amr_nb *decoder);

#endif
