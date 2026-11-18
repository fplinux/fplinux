/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-pm-app.h"
#include "fplinux-pm-vm.h"
#include <fplinux/fplinux-ui-screen.h>

#include <assert.h>

static enum fplinux_pm_vm_state state;
static bool loaded;

/* Only Java execution is controlled; the shell and its UI runtime stay real. */
bool fplinux_pm_vm_start(const struct fplinux_pm_vm_config *config)
{
	assert(state == FPLINUX_PM_VM_IDLE && config->wakeup &&
	       config->storage_dir);
	state = FPLINUX_PM_VM_RUNNING;
	return true;
}

int fplinux_pm_vm_slice(void)
{
	if (state == FPLINUX_PM_VM_DESTROYING) {
		state = FPLINUX_PM_VM_EXITED;
		return FPLINUX_PM_VM_SLICE_EXITED;
	}
	if (!loaded) {
		struct fplinux_ui *ui = fplinux_pm_app_ui();
		struct fplinux_ui_screen *screen =
			fplinux_ui_form_new(ui, "Example MIDlet");

		assert(screen && fplinux_ui_replace(ui, screen));
		loaded = true;
		fplinux_pm_app_loaded();
	}
	return FPLINUX_PM_VM_SLICE_IDLE;
}

void fplinux_pm_vm_request_destroy(void)
{
	state = FPLINUX_PM_VM_DESTROYING;
}

void fplinux_pm_vm_set_visible(bool visible)
{
	(void)visible;
}

enum fplinux_pm_vm_state fplinux_pm_vm_get_state(void)
{
	return state;
}

int fplinux_pm_vm_exit_code(void)
{
	return 0;
}

void fplinux_pm_keys_cancel(enum fplinux_ui_key_source source)
{
	(void)source;
}

void fplinux_pm_canvas_view_changed(void)
{
}
