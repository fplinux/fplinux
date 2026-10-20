// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/aw87339.h>

#include "qin-hp.h"

#define SC2721_PMU2 0x708
#define SC2721_PMU3 0x70c
#define SC2721_CLOCK0 0x718
#define SC2721_CDC1 0x720
#define SC2721_CDC2 0x724
#define SC2721_CDC3 0x728
#define SC2721_CDC4 0x72c
#define SC2721_DCL0 0x73c
#define SC2721_DCL4 0x74c
#define SC2721_DCL5 0x750
#define SC2721_DCL6 0x754
#define SC2721_DCL7 0x758
#define SC2721_STS0 0x75c
#define SC2721_STS2 0x764
#define SC2721_STS3 0x768
#define SC2721_STS7 0x778
#define SC2721_CFGA_CLEAR 0x800
#define SC2721_PROTECTION 0x820
#define SC2721_IRQ_STATUS 0x828
#define SC2721_IRQ_ENABLE 0x82c
#define SC2721_INTERFACE 0x830

const struct aw87339_profile qin_hp_speaker_profile = {
	.initial_control = 0x06,
	.registers_2_to_10 = { 0xa3, 0x06, 0x05, 0x10, 0x09, 0x52, 0x06, 0x08,
			       0x96 },
	.active_control = 0x0e,
};

static int check_protection(struct qin_hp *hp, const struct qin_hp_io *io)
{
	u32 value;
	int ret;

	ret = io->read(io->context, SC2721_STS7, &value);
	if (ret)
		return ret;
	if (value & 0x3f) {
		hp->fault = true;
		hp->ready = false;
		return -EIO;
	}
	ret = io->read(io->context, SC2721_IRQ_STATUS, &value);
	if (ret)
		return ret;
	if (value & 0x1b) {
		hp->fault = true;
		hp->ready = false;
		return -EIO;
	}
	return 0;
}

int qin_hp_mute(struct qin_hp *hp, const struct qin_hp_io *io)
{
	return hp->active ? io->update(io->context, SC2721_CDC4, 0xff, 0xff) :
			    0;
}

int qin_hp_set_volume(struct qin_hp *hp, const struct qin_hp_io *io,
		      unsigned int volume)
{
	static const u8 gain[] = { 0xf, 0xa, 0x9, 0x8, 0x7, 0x6, 0x5, 0x4 };
	u32 value;
	int ret;

	if (volume > QIN_HP_VOLUME_MAX)
		return -EINVAL;
	if (!hp->active || !hp->ready || hp->fault)
		return -EBADFD;
	ret = check_protection(hp, io);
	if (ret)
		return ret;
	value = gain[volume];
	return io->update(io->context, SC2721_CDC4, 0xff, (value << 4) | value);
}

struct qin_hp_field {
	unsigned int reg;
	u32 mask;
};

/* Status bits, the W1C register and the retained calibration word are absent. */
static const struct qin_hp_field hp_fields[] = {
	{ SC2721_PROTECTION, 0x0008 }, { SC2721_IRQ_ENABLE, 0x001b },
	{ SC2721_PMU3, 0x0c5c },       { SC2721_CLOCK0, 0x0300 },
	{ SC2721_DCL0, 0x0063 },       { SC2721_STS3, 0x0300 },
	{ SC2721_CDC2, 0x1ffe },       { SC2721_STS2, 0xebce },
	{ SC2721_DCL5, 0xfff8 },       { SC2721_DCL6, 0x7ffe },
	{ SC2721_DCL7, 0x7fff },       { SC2721_STS0, 0xffff },
	{ SC2721_INTERFACE, 0x0014 },  { SC2721_CDC4, 0x00ff },
	{ SC2721_CDC1, 0x0038 },       { SC2721_PMU2, 0x0018 },
	{ SC2721_CDC3, 0x1300 },       { SC2721_PMU2, 0x3806 },
};

int qin_hp_merge_controls(unsigned int reg, u32 previous, u32 mask, u32 value,
			  u32 *word)
{
	u32 controls;

	if (reg == SC2721_STS2)
		controls = 0xebce;
	else if (reg == SC2721_DCL5)
		controls = 0xfff8;
	else
		return -EINVAL;
	if (mask & ~controls)
		return -EINVAL;
	*word = ((previous & controls) & ~mask) | (value & mask);
	return 0;
}

static int wait_bits(const struct qin_hp_io *io, unsigned int reg, u32 mask,
		     u32 expected, unsigned int attempts, unsigned int delay_ms)
{
	u32 value;
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < attempts; attempt++) {
		ret = io->read(io->context, reg, &value);
		if (ret)
			return ret;
		if ((value & mask) == expected)
			return 0;
		io->wait_ms(io->context, delay_ms);
	}
	return -ETIMEDOUT;
}

static int wait_calibration(const struct qin_hp_io *io)
{
	u32 value;
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < 20; attempt++) {
		ret = io->read(io->context, SC2721_STS2, &value);
		if (ret)
			return ret;
		if ((value & 0x1400) == 0x1400) {
			io->wait_ms(io->context, 5);
			ret = io->read(io->context, SC2721_STS2, &value);
			if (ret)
				return ret;
			if ((value & 0x1400) == 0x1400)
				return 0;
		}
		io->wait_ms(io->context, 15);
	}
	return -ETIMEDOUT;
}

static int calibrate(const struct qin_hp_io *io)
{
	static const struct {
		unsigned int reg;
		u32 value;
	} calibration[] = {
		{ SC2721_STS2, 0x6000 },
		/* This calibration parameter remains programmed after shutdown. */
		{ SC2721_DCL4, 0xffff },
		{ SC2721_DCL5, 0xfa10 },
		{ SC2721_DCL6, 0x4cd8 },
		{ SC2721_DCL7, 0x2e6c },
		{ SC2721_STS0, 0xa820 },
	};
	unsigned int index;
	int ret;

	ret = io->update(io->context, SC2721_STS3, 0x0300, 0x0100);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_CDC2, 0x0004, 0x0004);
	if (ret)
		return ret;
	for (index = 0; index < ARRAY_SIZE(calibration); index++) {
		ret = io->write(io->context, calibration[index].reg,
				calibration[index].value);
		if (ret)
			return ret;
	}
	io->wait_ms(io->context, 2);
	ret = io->command(io->context, SC2721_STS2, 0x8000, 0x8000);
	if (ret)
		return ret;
	ret = wait_calibration(io);
	if (ret)
		return ret;
	return io->update(io->context, SC2721_CDC2, 0x1ffe, 0);
}

static int prepare_drivers(struct qin_hp *hp, const struct qin_hp_io *io)
{
	int ret;

	hp->drivers_active = true;
	ret = io->update(io->context, SC2721_CDC2, 0x0018, 0x0018);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_PMU2, 0x3806, 0x1006);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_CDC2, 0x0600, 0x0400);
	if (ret)
		return ret;
	io->wait_ms(io->context, 1);
	ret = io->update(io->context, SC2721_CDC2, 0x0600, 0x0200);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_CDC2, 0x0180, 0x0100);
	if (ret)
		return ret;
	io->wait_ms(io->context, 1);
	return io->update(io->context, SC2721_CDC2, 0x0180, 0x0080);
}

int qin_hp_prepare(struct qin_hp *hp, const struct qin_hp_io *io)
{
	u32 value;
	unsigned int index;
	int ret;

	if (hp->active)
		return -EBUSY;
	ret = io->read(io->context, SC2721_CDC2, &value);
	if (ret)
		return ret;
	if (value & 0x3fff)
		return -EBUSY;
	ret = io->read(io->context, SC2721_INTERFACE, &value);
	if (ret)
		return ret;
	if (value & 0x14)
		return -EBUSY;
	ret = io->read(io->context, SC2721_STS2, &value);
	if (ret)
		return ret;
	if (value & 0x8200)
		return -EBUSY;
	for (index = 0; index < ARRAY_SIZE(hp_fields); index++) {
		ret = io->save(io->context, hp_fields[index].reg,
			       hp_fields[index].mask);
		if (ret)
			return ret;
	}
	*hp = (struct qin_hp){ .active = true };
	ret = io->update(io->context, SC2721_CDC4, 0xff, 0xff);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_IRQ_ENABLE, 0x1b, 0);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_PROTECTION, 0x8, 0x8);
	if (ret)
		return ret;
	ret = io->write(io->context, SC2721_CFGA_CLEAR, 0x1b);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_PMU3, 0xc5c, 0x408);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_CLOCK0, 0x200, 0x200);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_CLOCK0, 0x100, 0x100);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_DCL0, 0x63, 0x42);
	if (ret)
		return ret;
	ret = io->pulse(io->context, SC2721_DCL0, 0x20, 10);
	if (ret)
		return ret;
	hp->irq_enabled = true;
	io->irq(io->context, true);
	ret = io->update(io->context, SC2721_IRQ_ENABLE, 0x1b, 0x1b);
	if (ret)
		return ret;
	ret = calibrate(io);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_INTERFACE, 0x14, 0x14);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_CDC1, 0x38, 0x10);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_CDC3, 0x1000, 0x1000);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_PMU2, 0x18, 0);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_CDC2, 0x1000, 0x1000);
	if (ret)
		return ret;
	io->wait_ms(io->context, 1);
	ret = io->update(io->context, SC2721_CDC2, 0x0800, 0x0800);
	if (ret)
		return ret;
	io->wait_ms(io->context, 1);
	ret = io->update(io->context, SC2721_CDC2, 0x4, 0x4);
	if (ret)
		return ret;
	ret = wait_bits(io, SC2721_STS2, 0x400, 0x400, 20, 5);
	if (ret)
		return ret;
	ret = io->update(io->context, SC2721_STS2, 0x1c0, 0x1c0);
	if (ret)
		return ret;
	io->wait_ms(io->context, 2);
	ret = io->command(io->context, SC2721_STS2, 0x200, 0x200);
	if (ret)
		return ret;
	ret = wait_bits(io, SC2721_STS2, 0x30, 0x30, 30, 20);
	if (ret)
		return ret;
	ret = prepare_drivers(hp, io);
	if (ret)
		return ret;
	hp->mixers_active = true;
	ret = io->update(io->context, SC2721_CDC3, 0x300, 0x300);
	if (ret)
		return ret;
	ret = wait_bits(io, SC2721_DCL5, 0x4, 0x4, 9, 20);
	if (ret)
		return ret;
	ret = wait_bits(io, SC2721_DCL5, 0x2, 0x2, 9, 20);
	if (ret)
		return ret;
	ret = check_protection(hp, io);
	if (ret)
		return ret;
	hp->ready = true;
	return 0;
}

static void keep_error(int *first, int ret)
{
	if (!*first && ret)
		*first = ret;
}

int qin_hp_stop(struct qin_hp *hp, const struct qin_hp_io *io)
{
	int ret = 0;

	if (!hp->active)
		return 0;
	hp->ready = false;
	if (hp->irq_enabled) {
		hp->irq_enabled = false;
		io->irq(io->context, false);
	}
	keep_error(&ret, io->update(io->context, SC2721_IRQ_ENABLE, 0x1b, 0));
	keep_error(&ret, qin_hp_mute(hp, io));
	keep_error(&ret, io->update(io->context, SC2721_CDC3, 0x300, 0));
	if (hp->mixers_active) {
		keep_error(&ret, wait_bits(io, SC2721_DCL5, 0x4, 0, 9, 20));
		keep_error(&ret, wait_bits(io, SC2721_DCL5, 0x2, 0, 9, 20));
	}
	if (hp->drivers_active) {
		keep_error(&ret,
			   io->update(io->context, SC2721_CDC2, 0x600, 0x400));
		keep_error(&ret,
			   io->update(io->context, SC2721_CDC2, 0x600, 0));
		keep_error(&ret,
			   io->update(io->context, SC2721_CDC2, 0x180, 0x100));
		keep_error(&ret,
			   io->update(io->context, SC2721_CDC2, 0x180, 0));
	}
	keep_error(&ret, io->update(io->context, SC2721_CDC2, 0x18, 0));
	keep_error(&ret, io->command(io->context, SC2721_STS2, 0x200, 0));
	keep_error(&ret, io->update(io->context, SC2721_CDC2, 0x4, 0));
	keep_error(&ret, io->update(io->context, SC2721_CDC2, 0x1000, 0));
	io->wait_ms(io->context, 1);
	keep_error(&ret, io->update(io->context, SC2721_CDC2, 0x800, 0));
	io->wait_ms(io->context, 1);
	keep_error(&ret, io->update(io->context, SC2721_CDC3, 0x1000, 0));
	keep_error(&ret, io->update(io->context, SC2721_INTERFACE, 0x14, 0));
	keep_error(&ret, io->command(io->context, SC2721_STS2, 0x8000, 0));
	keep_error(&ret, io->write(io->context, SC2721_CFGA_CLEAR, 0x1b));
	if (!ret) {
		hp->active = false;
		hp->drivers_active = false;
		hp->mixers_active = false;
	}
	return ret;
}

int qin_hp_interrupt(struct qin_hp *hp, const struct qin_hp_io *io, u32 *cause,
		     u32 *status)
{
	int ret;
	int first;

	*cause = 0;
	*status = 0;
	if (!hp->irq_enabled)
		return 0;
	first = io->read(io->context, SC2721_IRQ_STATUS, cause);
	*cause &= 0x1b;
	if (!first && !*cause)
		return 0;
	hp->fault = true;
	hp->ready = false;
	hp->irq_enabled = false;
	io->irq(io->context, false);
	ret = io->read(io->context, SC2721_STS7, status);
	keep_error(&first, ret);
	keep_error(&first, io->update(io->context, SC2721_IRQ_ENABLE, 0x1b, 0));
	keep_error(&first, qin_hp_mute(hp, io));
	keep_error(&first, io->write(io->context, SC2721_CFGA_CLEAR, *cause));
	return first ? first : 1;
}
