/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CODEC_PLAYBACK_IRQFLAGS_H
#define FPLINUX_CODEC_PLAYBACK_IRQFLAGS_H

unsigned long codec_playback_irq_save(void);
void codec_playback_irq_restore(unsigned long flags);

#define local_irq_save(flags) ((flags) = codec_playback_irq_save())
#define local_irq_restore(flags) codec_playback_irq_restore(flags)

#endif
