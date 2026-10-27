/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fplinux-pm-lifecycle.h"
#include "fplinux-pm-vm.h"

static void start(struct fplinux_pm_lifecycle *lifecycle)
{
	struct fplinux_pm_schedule schedule;

	fplinux_pm_lifecycle_init(lifecycle, true);
	schedule = fplinux_pm_lifecycle_load(lifecycle);
	assert(schedule.timer == FPLINUX_PM_TIMER_RUN && !schedule.delay_ms);
	schedule = fplinux_pm_lifecycle_loaded(lifecycle);
	assert(schedule.timer == FPLINUX_PM_TIMER_RUN &&
	       schedule.delay_ms == 1);
	schedule = fplinux_pm_lifecycle_started(lifecycle, true);
	assert(schedule.timer == FPLINUX_PM_TIMER_RUN && !schedule.delay_ms);
}

int main(int argc, char **argv)
{
	struct fplinux_pm_lifecycle lifecycle;
	struct fplinux_pm_schedule schedule;
	struct fplinux_pm_visibility visibility;
	uint32_t wait;

	assert(argc == 2);
	start(&lifecycle);
	if (!strcmp(argv[1], "hidden-wakeup")) {
		visibility =
			fplinux_pm_lifecycle_visibility(&lifecycle, false, 100);
		assert(visibility.notify_vm &&
		       visibility.schedule.timer == FPLINUX_PM_TIMER_RUN);
		fplinux_pm_lifecycle_slice_begin(&lifecycle);
		schedule = fplinux_pm_lifecycle_slice(
			&lifecycle, FPLINUX_PM_VM_SLICE_IDLE, 101);
		assert(schedule.timer == FPLINUX_PM_TIMER_PAUSE);
		assert(!fplinux_pm_lifecycle_wakeup(&lifecycle));
		visibility =
			fplinux_pm_lifecycle_visibility(&lifecycle, true, 500);
		assert(visibility.notify_vm &&
		       visibility.schedule.timer == FPLINUX_PM_TIMER_RUN);
	} else if (!strcmp(argv[1], "hide-deadline")) {
		fplinux_pm_lifecycle_visibility(&lifecycle, false, 100);
		fplinux_pm_lifecycle_slice_begin(&lifecycle);
		schedule = fplinux_pm_lifecycle_slice(&lifecycle, 0, 299);
		assert(schedule.timer == FPLINUX_PM_TIMER_RUN);
		fplinux_pm_lifecycle_slice_begin(&lifecycle);
		schedule = fplinux_pm_lifecycle_slice(&lifecycle, 0, 300);
		assert(schedule.timer == FPLINUX_PM_TIMER_PAUSE);
	} else if (!strcmp(argv[1], "slice-wakeup")) {
		fplinux_pm_lifecycle_slice_begin(&lifecycle);
		assert(fplinux_pm_lifecycle_wakeup(&lifecycle));
		schedule = fplinux_pm_lifecycle_slice(&lifecycle, 50, 100);
		assert(schedule.timer == FPLINUX_PM_TIMER_RUN &&
		       !schedule.delay_ms);
		fplinux_pm_lifecycle_slice_begin(&lifecycle);
		schedule = fplinux_pm_lifecycle_slice(&lifecycle, 50, 101);
		assert(schedule.timer == FPLINUX_PM_TIMER_RUN &&
		       schedule.delay_ms == 50);
	} else if (!strcmp(argv[1], "close-budget")) {
		assert(fplinux_pm_lifecycle_close_begin(&lifecycle, 1000));
		fplinux_pm_lifecycle_slice_begin(&lifecycle);
		assert(fplinux_pm_lifecycle_close_step(&lifecycle, 500, 1001,
						       &wait) ==
		       FPLINUX_PM_CLOSE_WAIT);
		assert(wait == 299);
		fplinux_pm_lifecycle_slice_begin(&lifecycle);
		assert(fplinux_pm_lifecycle_close_step(&lifecycle, 0, 1300,
						       &wait) ==
		       FPLINUX_PM_CLOSE_HARD_STOP);
		assert(!wait);
	} else if (!strcmp(argv[1], "close-completion")) {
		assert(fplinux_pm_lifecycle_close_begin(&lifecycle, 1000));
		fplinux_pm_lifecycle_slice_begin(&lifecycle);
		assert(fplinux_pm_lifecycle_close_step(&lifecycle, -2, 1001,
						       &wait) ==
		       FPLINUX_PM_CLOSE_DONE);
		assert(!fplinux_pm_lifecycle_close_begin(&lifecycle, 1002));
	} else {
		return 2;
	}
	puts("lifecycle: passed");
	return 0;
}
