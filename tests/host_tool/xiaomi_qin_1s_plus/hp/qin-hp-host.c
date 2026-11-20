// SPDX-License-Identifier: GPL-2.0-only
#include "qin-hp.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum event_type { SAVE, UPDATE, COMMAND, WRITE, PULSE, WAIT, IRQ };

struct event {
	enum event_type type;
	unsigned int reg;
	u32 mask;
	u32 value;
};

struct fixture {
	u32 regs[0x900 / 4];
	struct event events[512];
	unsigned int event_count;
	unsigned int mutations;
	unsigned int failed_mutation;
	unsigned int saved_count;
	unsigned int calibration_reads;
	unsigned int status_reads;
	unsigned int elapsed_ms;
	unsigned int irq_enables;
	unsigned int irq_disables;
	u32 rdac_block;
	bool calibration_stall;
	bool calibration_glitch;
	bool depop_valid_lost;
	bool charge_stall;
	bool off_rdac_stall;
	bool irq_read_failure;
	bool irq_enabled;
	bool volume_changes;
};

static void event(struct fixture *fixture, enum event_type type,
		  unsigned int reg, u32 mask, u32 value)
{
	assert(fixture->event_count <
	       sizeof(fixture->events) / sizeof(fixture->events[0]));
	fixture->events[fixture->event_count++] =
		(struct event){ type, reg, mask, value };
}

static int mutation(struct fixture *fixture)
{
	return ++fixture->mutations == fixture->failed_mutation ? -EREMOTEIO :
								  0;
}

static int read_register(void *context, unsigned int reg, u32 *value)
{
	struct fixture *fixture = context;
	u32 controls;

	assert(reg >= 0x700 && reg <= 0x83c && !(reg & 3));
	*value = fixture->regs[reg / 4];
	if (reg == 0x764) {
		fixture->status_reads++;
		controls = *value;
		if (controls & 0x8000) {
			fixture->calibration_reads++;
			if (!fixture->calibration_stall &&
			    !(fixture->calibration_glitch &&
			      fixture->calibration_reads == 2))
				*value |= 0x1400;
			if (fixture->depop_valid_lost &&
			    fixture->calibration_reads > 2)
				*value &= ~0x400;
		}
		if ((controls & 0x200) && !fixture->charge_stall)
			*value |= 0x30;
	} else if (reg == 0x750) {
		if (fixture->regs[0x728 / 4] & 0x200)
			*value |= 0x4 & ~fixture->rdac_block;
		if (fixture->regs[0x728 / 4] & 0x100)
			*value |= 0x2 & ~fixture->rdac_block;
		if (fixture->off_rdac_stall &&
		    !(fixture->regs[0x728 / 4] & 0x300))
			*value |= 0x6;
	} else if (reg == 0x828 && fixture->irq_read_failure) {
		return -EREMOTEIO;
	}
	return 0;
}

static int save_field(void *context, unsigned int reg, u32 mask)
{
	struct fixture *fixture = context;

	if (fixture->saved_count == 64)
		return -EOVERFLOW;
	fixture->saved_count++;
	event(fixture, SAVE, reg, mask, 0);
	return 0;
}

static int update_register(void *context, unsigned int reg, u32 mask, u32 value)
{
	struct fixture *fixture = context;

	assert(reg >= 0x700 && reg <= 0x83c && !(reg & 3));
	if (reg == 0x72c)
		assert((mask & 0xff) == 0xff &&
		       (fixture->volume_changes || (value & 0xff) == 0xff));
	fixture->regs[reg / 4] = (fixture->regs[reg / 4] & ~mask) |
				 (value & mask);
	event(fixture, UPDATE, reg, mask, value);
	return mutation(fixture);
}

static int command_register(void *context, unsigned int reg, u32 mask,
			    u32 value)
{
	struct fixture *fixture = context;

	assert(reg == 0x764);
	fixture->regs[reg / 4] = (fixture->regs[reg / 4] & ~mask) |
				 (value & mask);
	event(fixture, COMMAND, reg, mask, value);
	return mutation(fixture);
}

static int write_register(void *context, unsigned int reg, u32 value)
{
	struct fixture *fixture = context;

	assert(reg == 0x764 || reg == 0x74c || reg == 0x750 || reg == 0x754 ||
	       reg == 0x758 || reg == 0x75c || reg == 0x800);
	if (reg == 0x800)
		fixture->regs[0x828 / 4] &= ~value;
	else
		fixture->regs[reg / 4] = value;
	event(fixture, WRITE, reg, 0, value);
	return mutation(fixture);
}

static int pulse_register(void *context, unsigned int reg, u32 mask,
			  unsigned int delay_us)
{
	struct fixture *fixture = context;
	int ret;

	assert(reg == 0x73c && mask == 0x20 && delay_us == 10);
	fixture->regs[reg / 4] |= mask;
	event(fixture, PULSE, reg, mask, delay_us);
	ret = mutation(fixture);
	if (!ret)
		fixture->regs[reg / 4] &= ~mask;
	return ret;
}

static void wait_ms(void *context, unsigned int delay_ms)
{
	struct fixture *fixture = context;

	fixture->elapsed_ms += delay_ms;
	event(fixture, WAIT, 0, 0, delay_ms);
}

static void set_irq(void *context, bool enabled)
{
	struct fixture *fixture = context;

	assert(enabled != fixture->irq_enabled);
	fixture->irq_enabled = enabled;
	if (enabled)
		fixture->irq_enables++;
	else
		fixture->irq_disables++;
	event(fixture, IRQ, 0, 0, enabled);
}

static struct qin_hp_io fixture_io(struct fixture *fixture)
{
	return (struct qin_hp_io){
		.context = fixture,
		.read = read_register,
		.save = save_field,
		.update = update_register,
		.command = command_register,
		.write = write_register,
		.pulse = pulse_register,
		.wait_ms = wait_ms,
		.irq = set_irq,
	};
}

static void prepare_fixture(struct fixture *fixture)
{
	memset(fixture, 0, sizeof(*fixture));
	/* Existing digital/base-power fields already occupy 32 slots. */
	fixture->saved_count = 32;
	fixture->regs[0x70c / 4] = 0x8003;
	fixture->regs[0x72c / 4] = 0xab00;
}

static void assert_off(const struct fixture *fixture, const struct qin_hp *hp)
{
	assert(!hp->active && !hp->ready && !hp->irq_enabled);
	assert(!fixture->irq_enabled);
	assert(!(fixture->regs[0x724 / 4] & 0x1ffe));
	assert(!(fixture->regs[0x728 / 4] & 0x1300));
	assert(!(fixture->regs[0x830 / 4] & 0x14));
	assert(!(fixture->regs[0x764 / 4] & 0x8200));
	assert((fixture->regs[0x72c / 4] & 0xffff) == 0xabff);
}

static unsigned int find_event(const struct fixture *fixture,
			       enum event_type type, unsigned int reg, u32 mask,
			       u32 value)
{
	unsigned int index;

	for (index = 0; index < fixture->event_count; index++) {
		const struct event *candidate = &fixture->events[index];

		if (candidate->type == type && candidate->reg == reg &&
		    candidate->mask == mask && candidate->value == value)
			return index;
	}
	assert(false);
	return 0;
}

static void assert_source_sequence(const struct fixture *fixture)
{
	const unsigned int registers[] = { 0x764, 0x74c, 0x750,
					   0x754, 0x758, 0x75c };
	const u32 values[] = { 0x6000, 0xffff, 0xfa10, 0x4cd8, 0x2e6c, 0xa820 };
	unsigned int first = find_event(fixture, WRITE, 0x764, 0, 0x6000);
	unsigned int index;
	unsigned int depop;
	unsigned int drivers;
	unsigned int mixer;

	for (index = 0; index < 6; index++) {
		assert(fixture->events[first + index].type == WRITE);
		assert(fixture->events[first + index].reg == registers[index]);
		assert(fixture->events[first + index].value == values[index]);
	}
	depop = find_event(fixture, COMMAND, 0x764, 0x200, 0x200);
	drivers = find_event(fixture, UPDATE, 0x724, 0x18, 0x18);
	mixer = find_event(fixture, UPDATE, 0x728, 0x300, 0x300);
	assert(first < depop && depop < drivers && drivers < mixer);
	assert(find_event(fixture, PULSE, 0x73c, 0x20, 10) < first);
	assert(find_event(fixture, IRQ, 0, 0, 1) <
	       find_event(fixture, UPDATE, 0x82c, 0x1b, 0x1b));
}

static void test_merge(void)
{
	u32 word = 0xdeadbeef;

	assert(qin_hp_merge_controls(0x764, 0xffff, 0x200, 0, &word) == 0);
	assert(word == 0xe9ce);
	assert(qin_hp_merge_controls(0x750, 0xffff, 0x8000, 0, &word) == 0);
	assert(word == 0x7ff8);
	word = 0xdeadbeef;
	assert(qin_hp_merge_controls(0x764, 0, 0x1400, 0, &word) == -EINVAL);
	assert(word == 0xdeadbeef);
	assert(qin_hp_merge_controls(0x74c, 0, 0xffff, 0, &word) == -EINVAL);
	assert(word == 0xdeadbeef);
}

int main(int argc, char **argv)
{
	struct fixture fixture;
	struct qin_hp hp = {};
	struct qin_hp_io io;
	u32 cause;
	u32 status;
	unsigned int successful_mutations;
	unsigned int index;
	int expected = 0;
	int ret;

	assert(argc == 2);
	if (strcmp(argv[1], "mixed-controls") == 0) {
		test_merge();
		return 0;
	}
	prepare_fixture(&fixture);
	io = fixture_io(&fixture);
	if (strcmp(argv[1], "volume-before-ready") == 0) {
		assert(qin_hp_set_volume(&hp, &io, 1) == -EBADFD);
		assert(qin_hp_mute(&hp, &io) == 0);
		assert(fixture.mutations == 0);
		return 0;
	}
	if (strcmp(argv[1], "foreign-output") == 0) {
		fixture.regs[0x724 / 4] = 1;
		expected = -EBUSY;
	} else if (strcmp(argv[1], "ledger-full") == 0) {
		fixture.saved_count = 64;
		expected = -EOVERFLOW;
	} else if (strcmp(argv[1], "calibration-timeout") == 0) {
		fixture.calibration_stall = true;
		expected = -ETIMEDOUT;
	} else if (strcmp(argv[1], "calibration-glitch") == 0) {
		fixture.calibration_glitch = true;
	} else if (strcmp(argv[1], "depop-valid-timeout") == 0) {
		fixture.depop_valid_lost = true;
		expected = -ETIMEDOUT;
	} else if (strcmp(argv[1], "charge-timeout") == 0) {
		fixture.charge_stall = true;
		expected = -ETIMEDOUT;
	} else if (strcmp(argv[1], "left-rdac-timeout") == 0) {
		fixture.rdac_block = 4;
		expected = -ETIMEDOUT;
	} else if (strcmp(argv[1], "right-rdac-timeout") == 0) {
		fixture.rdac_block = 2;
		expected = -ETIMEDOUT;
	} else if (strcmp(argv[1], "protection-at-prepare") == 0) {
		fixture.regs[0x778 / 4] = 1;
		expected = -EIO;
	}
	ret = qin_hp_prepare(&hp, &io);
	assert(ret == expected);
	assert(fixture.saved_count <= 64);
	if (!ret) {
		assert(hp.ready && hp.active && fixture.irq_enabled);
		assert((fixture.regs[0x70c / 4] & 0xffff) == 0x840b);
		assert(fixture.regs[0x74c / 4] == 0xffff);
		assert_source_sequence(&fixture);
	}
	if (strcmp(argv[1], "foreign-output") == 0 ||
	    strcmp(argv[1], "ledger-full") == 0) {
		assert(fixture.mutations == 0 && !hp.active);
		assert(qin_hp_stop(&hp, &io) == 0);
		return 0;
	}
	if (strcmp(argv[1], "calibration-glitch") == 0)
		assert(fixture.calibration_reads >= 4 &&
		       fixture.elapsed_ms >= 25);
	if (strcmp(argv[1], "volume-mapping") == 0) {
		const u32 expected[] = { 0xabff, 0xabaa, 0xab99, 0xab88,
					 0xab77, 0xab66, 0xab55, 0xab44 };

		fixture.volume_changes = true;
		for (index = 0; index < sizeof(expected) / sizeof(expected[0]);
		     index++) {
			assert(qin_hp_set_volume(&hp, &io, index) == 0);
			assert(fixture.regs[0x72c / 4] == expected[index]);
		}
	} else if (strcmp(argv[1], "volume-range") == 0) {
		unsigned int before = fixture.mutations;

		assert(qin_hp_set_volume(&hp, &io, 8) == -EINVAL);
		assert(qin_hp_set_volume(&hp, &io, (unsigned int)-1) ==
		       -EINVAL);
		assert(fixture.mutations == before);
	} else if (strcmp(argv[1], "volume-stale-protection") == 0) {
		unsigned int before = fixture.mutations;

		fixture.regs[0x778 / 4] = 0x20;
		assert(qin_hp_set_volume(&hp, &io, 1) == -EIO);
		assert(hp.fault && !hp.ready);
		assert(fixture.mutations == before);
	} else if (strcmp(argv[1], "volume-write-failure") == 0) {
		fixture.volume_changes = true;
		fixture.failed_mutation = fixture.mutations + 1;
		assert(qin_hp_set_volume(&hp, &io, 2) == -EREMOTEIO);
		assert(fixture.regs[0x72c / 4] == 0xab99);
		fixture.failed_mutation = 0;
		assert(qin_hp_mute(&hp, &io) == 0);
		assert(fixture.regs[0x72c / 4] == 0xabff);
	}
	if (strcmp(argv[1], "protection-irq") == 0 ||
	    strcmp(argv[1], "irq-read-failure") == 0) {
		fixture.regs[0x828 / 4] = 0x10;
		fixture.regs[0x778 / 4] = 0x20;
		fixture.irq_read_failure =
			strcmp(argv[1], "irq-read-failure") == 0;
		ret = qin_hp_interrupt(&hp, &io, &cause, &status);
		assert(ret == (fixture.irq_read_failure ? -EREMOTEIO : 1));
		assert(hp.fault && !hp.ready && !hp.irq_enabled);
		assert(!fixture.irq_enabled && status == 0x20);
		fixture.irq_read_failure = false;
	} else if (strcmp(argv[1], "spurious-irq") == 0) {
		fixture.regs[0x828 / 4] = 0x20;
		assert(qin_hp_interrupt(&hp, &io, &cause, &status) == 0);
		assert(hp.ready && hp.irq_enabled && !hp.fault);
	}
	if (strcmp(argv[1], "off-rdac-timeout") == 0) {
		fixture.off_rdac_stall = true;
		assert(qin_hp_stop(&hp, &io) == -ETIMEDOUT);
		assert(hp.active && !hp.ready && !fixture.irq_enabled);
		assert(!(fixture.regs[0x724 / 4] & 0x1ffe));
		assert(!(fixture.regs[0x764 / 4] & 0x8200));
		fixture.off_rdac_stall = false;
	}
	if (strcmp(argv[1], "cleanup-partial-failure") == 0) {
		fixture.failed_mutation = fixture.mutations + 1;
		assert(qin_hp_stop(&hp, &io) == -EREMOTEIO);
		assert(hp.active && !hp.ready && !fixture.irq_enabled);
		assert(!(fixture.regs[0x724 / 4] & 0x1ffe));
		assert(!(fixture.regs[0x764 / 4] & 0x8200));
		fixture.failed_mutation = 0;
	}
	successful_mutations = fixture.mutations;
	assert(qin_hp_stop(&hp, &io) == 0);
	assert_off(&fixture, &hp);
	if (strcmp(argv[1], "muted-ready-stop") == 0) {
		assert(qin_hp_stop(&hp, &io) == 0);
		assert(fixture.irq_enables == 1 && fixture.irq_disables == 1);
		assert(fixture.regs[0x74c / 4] == 0xffff);
	}
	if (strcmp(argv[1], "partial-prepare-failures") == 0) {
		for (index = 1; index <= successful_mutations; index++) {
			prepare_fixture(&fixture);
			hp = (struct qin_hp){};
			fixture.failed_mutation = index;
			assert(qin_hp_prepare(&hp, &io) == -EREMOTEIO);
			fixture.failed_mutation = 0;
			assert(qin_hp_stop(&hp, &io) == 0);
			assert_off(&fixture, &hp);
		}
	}
	puts("PASS muted HP readiness component");
	return 0;
}
