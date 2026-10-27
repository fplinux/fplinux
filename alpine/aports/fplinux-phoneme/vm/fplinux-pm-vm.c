/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * Slave-mode glue between the app shell and the CLDC-HI VM: the VM runs
 * org.fplinux.phoneme.Runner, which starts the MIDlet suite, and executes
 * Java only inside fplinux_pm_vm_slice(). App requests (destroy,
 * visibility) reach Runner through one blocking native; MIDP natives that
 * unblock a Java thread call the timeslice hook, which forwards to the app's
 * wakeup hook. Compiled ARM-mode: the interpreter calls these natives.
 */
#include "fplinux-pm-vm.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jvm.h>
#include <jvmspi.h>
#include <kni.h>
#include <midp_thread.h>
#include <midpServices.h>
#include <sni.h>

#include "fplinux-pm-midp.h"
#include "fplinux-pm-network.h"

#define RUNNER_CLASS "org.fplinux.phoneme.Runner"
#define EXIT_START_FAILED 1
/* Runner's request thread waits on this AMS_SIGNAL descriptor ("PM"). */
#define REQUEST_DESCRIPTOR 0x504d
#define NETWORK_POLL_MS 20

/* Requests for Runner.waitRequest0(), values shared with Runner.java. */
enum runner_request {
	REQUEST_NONE = 0,
	REQUEST_DESTROY = 1,
	REQUEST_SHOW = 2,
	REQUEST_HIDE = 3,
};

static enum fplinux_pm_vm_state vm_state = FPLINUX_PM_VM_IDLE;
static int vm_exit_code;
static fplinux_pm_vm_wakeup_fn vm_wakeup;
static void *vm_wakeup_context;

/* JVM_Start keeps using the classpath and arguments while the VM runs. */
static char *vm_classpath;
static char *vm_main_class;
static char *vm_arguments[1];

static bool destroy_pending;
static enum runner_request visibility_pending = REQUEST_NONE;
static bool vm_visible = true;

static void wake_app(void)
{
	if (vm_wakeup != NULL)
		vm_wakeup(vm_wakeup_context);
}

/* midp_thread_unblock() calls this after it has unblocked a Java thread. */
static void timeslice_requested(void)
{
	wake_app();
}

static void release_start_strings(void)
{
	free(vm_classpath);
	free(vm_main_class);
	free(vm_arguments[0]);
	vm_classpath = NULL;
	vm_main_class = NULL;
	vm_arguments[0] = NULL;
}

static void finish(int exit_code)
{
	vm_exit_code = exit_code;
	vm_state = FPLINUX_PM_VM_EXITED;
	fplinux_pm_network_cleanup();
	fplinux_pm_midp_cleanup();
	release_start_strings();
}

static bool config_is_valid(const struct fplinux_pm_vm_config *config)
{
	return config != NULL && config->jar_path != NULL &&
	       config->jar_path[0] != '\0' && config->midlet_class != NULL &&
	       config->midlet_class[0] != '\0' && config->wakeup != NULL &&
	       strchr(config->jar_path, ':') == NULL &&
	       (config->system_classes == NULL ||
		strchr(config->system_classes, ':') == NULL);
}

bool fplinux_pm_vm_start(const struct fplinux_pm_vm_config *config)
{
	if (vm_state != FPLINUX_PM_VM_IDLE) {
		fputs("fplinux-phoneme: the VM was already started\n", stderr);
		return false;
	}
	if (!config_is_valid(config)) {
		fputs("fplinux-phoneme: invalid VM configuration\n", stderr);
		return false;
	}

	const char *system_classes = config->system_classes != NULL ?
					     config->system_classes :
					     FPLINUX_PM_VM_SYSTEM_CLASSES;
	unsigned int heap_kib = config->heap_kib != 0 ?
					config->heap_kib :
					FPLINUX_PM_VM_HEAP_KIB_DEFAULT;
	if (heap_kib > (unsigned int)INT_MAX / 1024u) {
		fputs("fplinux-phoneme: Java heap size is too large\n", stderr);
		return false;
	}
	size_t classpath_size =
		strlen(system_classes) + strlen(config->jar_path) + 2;
	vm_classpath = malloc(classpath_size);
	vm_main_class = strdup(RUNNER_CLASS);
	vm_arguments[0] = strdup(config->midlet_class);
	if (vm_classpath == NULL || vm_main_class == NULL ||
	    vm_arguments[0] == NULL) {
		release_start_strings();
		fputs("fplinux-phoneme: out of memory\n", stderr);
		return false;
	}
	snprintf(vm_classpath, classpath_size, "%s:%s", system_classes,
		 config->jar_path);

	vm_wakeup = config->wakeup;
	vm_wakeup_context = config->wakeup_context;
	vm_state = FPLINUX_PM_VM_RUNNING;
	/* MIDP initialization sets VM system properties. */
	JVM_Initialize();
	if (!fplinux_pm_midp_init(config->storage_dir)) {
		fputs("fplinux-phoneme: MIDP initialization failed\n", stderr);
		finish(EXIT_START_FAILED);
		return false;
	}
	JVM_SetConfig(JVM_CONFIG_HEAP_CAPACITY, (int)(heap_kib * 1024u));
	JVM_SetConfig(JVM_CONFIG_SLAVE_MODE, KNI_TRUE);
	midp_thread_set_timeslice_proc(timeslice_requested);
	/* In slave mode this returns once the main class is loaded. */
	JVM_Start(vm_classpath, vm_main_class, 1, vm_arguments);
	if (!JVM_IsStarted()) {
		fputs("fplinux-phoneme: the VM did not start\n", stderr);
		finish(EXIT_START_FAILED);
		return false;
	}
	return true;
}

int fplinux_pm_vm_slice(void)
{
	JVMSPI_BlockedThreadInfo *threads;
	int count;

	if (vm_state != FPLINUX_PM_VM_RUNNING &&
	    vm_state != FPLINUX_PM_VM_DESTROYING)
		return FPLINUX_PM_VM_SLICE_EXITED;

	threads = SNI_GetBlockedThreads(&count);
	fplinux_pm_network_check_events(threads, count);
	jlong delay_ms = JVM_TimeSlice();
	if (delay_ms <= -2) {
		int exit_code = JVM_CleanUp();
		finish(exit_code);
		return FPLINUX_PM_VM_SLICE_EXITED;
	}
	/* Slave mode does not call CheckEvents; keep the GUI timer polling only
	 * while native network operations wait, without sleeping in a native. */
	threads = SNI_GetBlockedThreads(&count);
	if (fplinux_pm_network_has_waiters(threads, count) &&
	    (delay_ms < 0 || delay_ms > NETWORK_POLL_MS))
		delay_ms = NETWORK_POLL_MS;
	if (delay_ms > INT_MAX)
		return INT_MAX;
	return (int)delay_ms;
}

/* Unblocks Runner's request thread if it waits; it reads the request then. */
static void deliver_requests(void)
{
	midp_thread_signal(AMS_SIGNAL, REQUEST_DESCRIPTOR, 0);
	wake_app();
}

void fplinux_pm_vm_request_destroy(void)
{
	if (vm_state != FPLINUX_PM_VM_RUNNING)
		return;
	vm_state = FPLINUX_PM_VM_DESTROYING;
	destroy_pending = true;
	fplinux_pm_network_cancel();
	deliver_requests();
}

void fplinux_pm_vm_set_visible(bool visible)
{
	if (vm_state != FPLINUX_PM_VM_RUNNING || visible == vm_visible)
		return;
	vm_visible = visible;
	visibility_pending = visible ? REQUEST_SHOW : REQUEST_HIDE;
	deliver_requests();
}

enum fplinux_pm_vm_state fplinux_pm_vm_get_state(void)
{
	return vm_state;
}

int fplinux_pm_vm_exit_code(void)
{
	return vm_state == FPLINUX_PM_VM_EXITED ? vm_exit_code : 0;
}

/* System.out and System.err of Java code. */
void JVMSPI_PrintRaw(const char *text, int length)
{
	if (text == NULL || length <= 0)
		return;
	fwrite(text, 1, (size_t)length, stdout);
	if (memchr(text, '\n', (size_t)length) != NULL)
		fflush(stdout);
}

/* Only the master-mode launcher exits here; slave mode returns -2. */
void JVMSPI_Exit(int code)
{
	vm_exit_code = code;
}

/*
 * This definition keeps CLDC's incompatible socket-polling implementation
 * out of the link. The slave-mode pump also polls a fresh MIDP snapshot.
 */
void JVMSPI_CheckEvents(JVMSPI_BlockedThreadInfo *blocked_threads,
			int blocked_threads_count, jlong timeout)
{
	(void)timeout;
	fplinux_pm_network_check_events(blocked_threads, blocked_threads_count);
}

static enum runner_request take_request(void)
{
	if (destroy_pending) {
		destroy_pending = false;
		return REQUEST_DESTROY;
	}
	enum runner_request request = visibility_pending;

	visibility_pending = REQUEST_NONE;
	return request;
}

/* private static native int waitRequest0(); re-entered after unblocking */
KNIEXPORT KNI_RETURNTYPE_INT Java_org_fplinux_phoneme_Runner_waitRequest0(void)
{
	enum runner_request request = take_request();

	if (request == REQUEST_NONE)
		midp_thread_wait(AMS_SIGNAL, REQUEST_DESCRIPTOR, NULL);
	KNI_ReturnInt(request);
}

/* private static native void exit0(int status); */
KNIEXPORT KNI_RETURNTYPE_VOID Java_org_fplinux_phoneme_Runner_exit0(void)
{
	int status = (int)KNI_GetParameterAsInt(1);

	fflush(stdout);
	JVM_Stop(status);
	KNI_ReturnVoid();
}
