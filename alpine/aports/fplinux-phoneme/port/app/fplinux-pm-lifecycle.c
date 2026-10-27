// SPDX-License-Identifier: GPL-2.0-only
/* The app shell's decisions about running the VM; see the header. */
#include "fplinux-pm-lifecycle.h"

#include "../../vm/fplinux-pm-vm.h"

static const struct fplinux_pm_schedule keep_timer = { FPLINUX_PM_TIMER_KEEP,
						       0 };
static const struct fplinux_pm_schedule pause_timer = { FPLINUX_PM_TIMER_PAUSE,
							0 };
static const struct fplinux_pm_schedule run_soon = { FPLINUX_PM_TIMER_RUN, 0 };

static bool vm_started(enum fplinux_pm_phase phase)
{
	return phase == FPLINUX_PM_PHASE_RUNNING ||
	       phase == FPLINUX_PM_PHASE_HIDING ||
	       phase == FPLINUX_PM_PHASE_HIDDEN;
}

/* The wakeup flag, cleared for the next slice. */
static bool take_woken(struct fplinux_pm_lifecycle *lifecycle)
{
	bool woken = lifecycle->woken;

	lifecycle->woken = false;
	return woken;
}

void fplinux_pm_lifecycle_init(struct fplinux_pm_lifecycle *lifecycle,
			       bool visible)
{
	*lifecycle = (struct fplinux_pm_lifecycle){
		.phase = FPLINUX_PM_PHASE_PICKING,
		.visible = visible,
	};
}

struct fplinux_pm_schedule
fplinux_pm_lifecycle_load(struct fplinux_pm_lifecycle *lifecycle)
{
	if (lifecycle->phase != FPLINUX_PM_PHASE_PICKING)
		return keep_timer;
	lifecycle->phase = FPLINUX_PM_PHASE_LOADING;
	return lifecycle->visible ? run_soon : pause_timer;
}

struct fplinux_pm_schedule
fplinux_pm_lifecycle_loaded(struct fplinux_pm_lifecycle *lifecycle)
{
	if (lifecycle->phase != FPLINUX_PM_PHASE_LOADING)
		return keep_timer;
	lifecycle->phase = FPLINUX_PM_PHASE_STARTING;
	/* A whole period, so that the main loop submits the frame first. */
	return lifecycle->visible ?
		       (struct fplinux_pm_schedule){ FPLINUX_PM_TIMER_RUN, 1 } :
		       pause_timer;
}

struct fplinux_pm_schedule
fplinux_pm_lifecycle_started(struct fplinux_pm_lifecycle *lifecycle,
			     bool started)
{
	if (lifecycle->phase != FPLINUX_PM_PHASE_STARTING)
		return keep_timer;
	lifecycle->woken = false;
	if (!started) {
		lifecycle->phase = FPLINUX_PM_PHASE_EXITED;
		return pause_timer;
	}
	lifecycle->phase = FPLINUX_PM_PHASE_RUNNING;
	return run_soon;
}

void fplinux_pm_lifecycle_slice_begin(struct fplinux_pm_lifecycle *lifecycle)
{
	lifecycle->woken = false;
}

struct fplinux_pm_schedule
fplinux_pm_lifecycle_slice(struct fplinux_pm_lifecycle *lifecycle, int result,
			   uint64_t now_ms)
{
	bool woken = take_woken(lifecycle);

	if (lifecycle->phase != FPLINUX_PM_PHASE_RUNNING &&
	    lifecycle->phase != FPLINUX_PM_PHASE_HIDING)
		return pause_timer;
	if (result == FPLINUX_PM_VM_SLICE_EXITED) {
		lifecycle->phase = FPLINUX_PM_PHASE_EXITED;
		return pause_timer;
	}
	if (lifecycle->phase == FPLINUX_PM_PHASE_HIDING) {
		/* Nothing runnable is left: hideNotify has been handled. */
		if ((result != FPLINUX_PM_VM_SLICE_AGAIN && !woken) ||
		    now_ms >= lifecycle->hide_deadline_ms) {
			lifecycle->phase = FPLINUX_PM_PHASE_HIDDEN;
			return pause_timer;
		}
		return run_soon;
	}
	if (woken || result == FPLINUX_PM_VM_SLICE_AGAIN)
		return run_soon;
	if (result == FPLINUX_PM_VM_SLICE_IDLE || result < 0)
		return pause_timer;
	return (struct fplinux_pm_schedule){ FPLINUX_PM_TIMER_RUN,
					     (uint32_t)result };
}

bool fplinux_pm_lifecycle_wakeup(struct fplinux_pm_lifecycle *lifecycle)
{
	lifecycle->woken = true;
	return lifecycle->phase == FPLINUX_PM_PHASE_RUNNING ||
	       lifecycle->phase == FPLINUX_PM_PHASE_HIDING;
}

struct fplinux_pm_visibility
fplinux_pm_lifecycle_visibility(struct fplinux_pm_lifecycle *lifecycle,
				bool visible, uint64_t now_ms)
{
	struct fplinux_pm_visibility result = { false, keep_timer };
	bool changed = lifecycle->visible != visible;

	lifecycle->visible = visible;
	if (!changed)
		return result;
	switch (lifecycle->phase) {
	case FPLINUX_PM_PHASE_LOADING:
	case FPLINUX_PM_PHASE_STARTING:
		/* The VM starts only while the app is seen. */
		result.schedule = visible ? run_soon : pause_timer;
		break;
	case FPLINUX_PM_PHASE_RUNNING:
		if (!visible) {
			lifecycle->phase = FPLINUX_PM_PHASE_HIDING;
			lifecycle->hide_deadline_ms =
				now_ms + FPLINUX_PM_HIDE_DRAIN_MS;
			result.notify_vm = true;
			result.schedule = run_soon;
		}
		break;
	case FPLINUX_PM_PHASE_HIDING:
	case FPLINUX_PM_PHASE_HIDDEN:
		if (visible) {
			lifecycle->phase = FPLINUX_PM_PHASE_RUNNING;
			result.notify_vm = true;
			result.schedule = run_soon;
		}
		break;
	case FPLINUX_PM_PHASE_PICKING:
	case FPLINUX_PM_PHASE_EXITED:
		break;
	}
	return result;
}

bool fplinux_pm_lifecycle_close_begin(struct fplinux_pm_lifecycle *lifecycle,
				      uint64_t now_ms)
{
	if (!vm_started(lifecycle->phase))
		return false;
	lifecycle->close_deadline_ms = now_ms + FPLINUX_PM_CLOSE_BUDGET_MS;
	lifecycle->woken = false;
	return true;
}

enum fplinux_pm_close_step
fplinux_pm_lifecycle_close_step(struct fplinux_pm_lifecycle *lifecycle,
				int result, uint64_t now_ms, uint32_t *wait_ms)
{
	bool woken = take_woken(lifecycle);
	uint64_t left;

	*wait_ms = 0;
	if (result == FPLINUX_PM_VM_SLICE_EXITED) {
		lifecycle->phase = FPLINUX_PM_PHASE_EXITED;
		return FPLINUX_PM_CLOSE_DONE;
	}
	if (now_ms >= lifecycle->close_deadline_ms)
		return FPLINUX_PM_CLOSE_HARD_STOP;
	if (result == FPLINUX_PM_VM_SLICE_AGAIN || woken)
		return FPLINUX_PM_CLOSE_CONTINUE;
	/* Every thread waits, and destroyApp() has not ended the MIDlet. */
	if (result < 0)
		return FPLINUX_PM_CLOSE_HARD_STOP;
	left = lifecycle->close_deadline_ms - now_ms;
	*wait_ms = (uint64_t)result < left ? (uint32_t)result : (uint32_t)left;
	return FPLINUX_PM_CLOSE_WAIT;
}
