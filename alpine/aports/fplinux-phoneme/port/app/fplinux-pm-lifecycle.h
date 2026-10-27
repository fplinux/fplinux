/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * When the app shell runs the VM. The vm_tick timer, the wakeup hook,
 * visibility and CLOSE decide here, without LVGL or the VM, so host tests
 * drive the decisions with made-up slice results and times;
 * fplinux-pm-app.c carries them out.
 *
 *   PICKING ──choose──> LOADING ──first frame──> STARTING ──start──> RUNNING
 *   RUNNING ──hide──> HIDING ──settled──> HIDDEN ──show──> RUNNING
 *   any VM phase ──slice -2──> EXITED
 */
#ifndef FPLINUX_PM_LIFECYCLE_H
#define FPLINUX_PM_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

/* After hideNotify is queued, slices run at most this long before the freeze. */
#define FPLINUX_PM_HIDE_DRAIN_MS 200u
/* destroyApp(true) gets this long after CLOSE; the manager's SIGTERM is at 500 ms. */
#define FPLINUX_PM_CLOSE_BUDGET_MS 300u

enum fplinux_pm_phase {
	/* Waiting for a document, the MIDlet chooser or its error; no VM. */
	FPLINUX_PM_PHASE_PICKING,
	/* The loading screen waits for its first frame. */
	FPLINUX_PM_PHASE_LOADING,
	/* The next tick starts the VM. */
	FPLINUX_PM_PHASE_STARTING,
	FPLINUX_PM_PHASE_RUNNING,
	/* hideNotify is queued; slices run until the VM settles. */
	FPLINUX_PM_PHASE_HIDING,
	/* Frozen: no slices until the app is visible again. */
	FPLINUX_PM_PHASE_HIDDEN,
	/* The VM has finished, or could not start. */
	FPLINUX_PM_PHASE_EXITED,
};

enum fplinux_pm_timer {
	/* Leave the timer as it is. */
	FPLINUX_PM_TIMER_KEEP,
	FPLINUX_PM_TIMER_PAUSE,
	/* Run after delay_ms; 0 is after one turn of the event loop. */
	FPLINUX_PM_TIMER_RUN,
};

struct fplinux_pm_schedule {
	enum fplinux_pm_timer timer;
	uint32_t delay_ms;
};

struct fplinux_pm_lifecycle {
	enum fplinux_pm_phase phase;
	bool visible;
	/* The wakeup hook fired during the current slice. */
	bool woken;
	uint64_t hide_deadline_ms;
	uint64_t close_deadline_ms;
};

/* What the app tells the VM and does with the timer after a visibility event. */
struct fplinux_pm_visibility {
	/* Call fplinux_pm_vm_set_visible() with the new value. */
	bool notify_vm;
	struct fplinux_pm_schedule schedule;
};

enum fplinux_pm_close_step {
	/* Run the next slice now. */
	FPLINUX_PM_CLOSE_CONTINUE,
	/* Sleep wait_ms, then run the next slice. */
	FPLINUX_PM_CLOSE_WAIT,
	/* The VM has exited. */
	FPLINUX_PM_CLOSE_DONE,
	/* The budget is spent or nothing can run: exit without the VM. */
	FPLINUX_PM_CLOSE_HARD_STOP,
};

void fplinux_pm_lifecycle_init(struct fplinux_pm_lifecycle *lifecycle,
			       bool visible);
/* A MIDlet was chosen and the loading screen is shown: create the timer. */
struct fplinux_pm_schedule
fplinux_pm_lifecycle_load(struct fplinux_pm_lifecycle *lifecycle);
/* A tick while LOADING has drawn the frame; the next one starts the VM. */
struct fplinux_pm_schedule
fplinux_pm_lifecycle_loaded(struct fplinux_pm_lifecycle *lifecycle);
/* The result of fplinux_pm_vm_start(). */
struct fplinux_pm_schedule
fplinux_pm_lifecycle_started(struct fplinux_pm_lifecycle *lifecycle,
			     bool started);
/*
 * Call just before each fplinux_pm_vm_slice(), in vm_tick and in the CLOSE
 * loop: a wakeup before the slice is handled by that slice, so only one
 * that fires during it asks for another.
 */
void fplinux_pm_lifecycle_slice_begin(struct fplinux_pm_lifecycle *lifecycle);
/* The result of one fplinux_pm_vm_slice() at now_ms. */
struct fplinux_pm_schedule
fplinux_pm_lifecycle_slice(struct fplinux_pm_lifecycle *lifecycle, int result,
			   uint64_t now_ms);
/*
 * The VM's wakeup hook fired. True when a slice should run soon: then the
 * app readies the timer, unless the hook fired inside a slice, whose result
 * already takes it into account. A frozen VM stays frozen until shown.
 */
bool fplinux_pm_lifecycle_wakeup(struct fplinux_pm_lifecycle *lifecycle);
/* VISIBILITY: hideNotify or showNotify only, never pauseApp. */
struct fplinux_pm_visibility
fplinux_pm_lifecycle_visibility(struct fplinux_pm_lifecycle *lifecycle,
				bool visible, uint64_t now_ms);
/*
 * CLOSE: true when the VM is running and must be asked for destroyApp(true)
 * with fplinux_pm_vm_request_destroy(); then slice while each step says so.
 */
bool fplinux_pm_lifecycle_close_begin(struct fplinux_pm_lifecycle *lifecycle,
				      uint64_t now_ms);
enum fplinux_pm_close_step
fplinux_pm_lifecycle_close_step(struct fplinux_pm_lifecycle *lifecycle,
				int result, uint64_t now_ms, uint32_t *wait_ms);

#endif
