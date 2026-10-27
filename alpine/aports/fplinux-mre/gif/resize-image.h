/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_RESIZE_IMAGE_H
#define FPLINUX_MRE_RESIZE_IMAGE_H

#include "gif-image.h"

/* Top-left nearest-neighbor sampling matches the pinned MREmu resize path.
 * Failure leaves the owned image unchanged; success replaces its buffers. */
int mre_gif_resize(struct mre_gif_image *image, int width, int height);

#endif
