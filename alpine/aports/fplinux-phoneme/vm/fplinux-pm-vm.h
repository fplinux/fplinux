/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The phoneME VM as seen by the fplinux-phoneme app shell.
 *
 * The CLDC-HI VM runs in slave mode on the caller's thread: it executes Java
 * only inside fplinux_pm_vm_slice() and never blocks or starts threads of its
 * own. Every function here must be called from that one thread (the
 * libfplinux-ui runtime thread in the app). One VM runs per process; a second
 * start after the VM has exited is not supported yet.
 */
#ifndef FPLINUX_PM_VM_H
#define FPLINUX_PM_VM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installed CLDC and MIDP system classes, preverified. */
#define FPLINUX_PM_VM_SYSTEM_CLASSES "/usr/share/fplinux-phoneme/classes.zip"
/* Java heap used when the configuration leaves heap_kib at 0. */
#define FPLINUX_PM_VM_HEAP_KIB_DEFAULT 4096u

/* Results of fplinux_pm_vm_slice() besides a positive delay. */
#define FPLINUX_PM_VM_SLICE_AGAIN 0 /* more work: call again soon */
#define FPLINUX_PM_VM_SLICE_IDLE (-1) /* every Java thread waits for an event */
#define FPLINUX_PM_VM_SLICE_EXITED (-2) /* the VM has finished; see exit code */

/* Called when a Java thread becomes runnable outside the planned schedule. */
typedef void (*fplinux_pm_vm_wakeup_fn)(void *context);

struct fplinux_pm_vm_config {
	/* MIDlet suite JAR; it becomes the classpath after the system classes. */
	const char *jar_path;
	/* MIDlet class from a MIDlet-<n> attribute, e.g. "com.example.Game". */
	const char *midlet_class;
	/* NULL selects FPLINUX_PM_VM_SYSTEM_CLASSES (tests pass a build path). */
	const char *system_classes;
	/* Java heap size; 0 selects FPLINUX_PM_VM_HEAP_KIB_DEFAULT. */
	unsigned int heap_kib;
	/*
	 * Existing writable directory owned by the caller for this run's RMS
	 * record stores and other MIDP files; the caller removes it at exit.
	 * NULL: record stores cannot be opened (RecordStoreException).
	 */
	const char *storage_dir;
	/*
	 * Wakeup hook, required. The VM calls it on the VM thread, possibly
	 * from inside fplinux_pm_vm_slice() or fplinux_pm_vm_request_destroy(),
	 * whenever a waiting Java thread becomes runnable (an event was
	 * queued, a native operation finished). The hook must only schedule
	 * a slice soon, e.g. lv_timer_ready(); it must never call
	 * fplinux_pm_vm_slice() itself.
	 */
	fplinux_pm_vm_wakeup_fn wakeup;
	void *wakeup_context;
};

enum fplinux_pm_vm_state {
	FPLINUX_PM_VM_IDLE, /* not started */
	FPLINUX_PM_VM_RUNNING, /* started; slices run Java */
	FPLINUX_PM_VM_DESTROYING, /* destroyApp(true) requested, not finished */
	FPLINUX_PM_VM_EXITED, /* finished; fplinux_pm_vm_exit_code() is final */
};

/*
 * Initialize the VM, load the launcher class and select the MIDlet. No Java
 * bytecode runs yet: the first fplinux_pm_vm_slice() starts the suite, which
 * constructs the MIDlet and calls startApp(). Returns false, with a message
 * on stderr, when the configuration is invalid, the VM is not IDLE or the VM
 * cannot start; the state is then EXITED with a non-zero exit code (or stays
 * IDLE for an invalid configuration).
 *
 * System.out and System.err of the MIDlet go to the process's stdout; VM
 * diagnostics go to stderr.
 */
bool fplinux_pm_vm_start(const struct fplinux_pm_vm_config *config);

/*
 * Run Java until every thread waits or the slice budget ends. The result has
 * the JVM_TimeSlice() meaning:
 *   > 0  milliseconds until the next Java timer or sleep is due; call again
 *        then, or earlier when the wakeup hook fires;
 *   0    (FPLINUX_PM_VM_SLICE_AGAIN) runnable work remains; call again after
 *        the event loop has run once, so input and drawing interleave;
 *   -1   (FPLINUX_PM_VM_SLICE_IDLE) every thread waits for an external event;
 *        call again only when the wakeup hook fires;
 *   -2   (FPLINUX_PM_VM_SLICE_EXITED) the VM has finished and released its
 *        Java state; further calls return -2.
 * Delays longer than INT_MAX milliseconds are returned as INT_MAX. In any
 * state other than RUNNING or DESTROYING it returns -2 without running Java.
 */
int fplinux_pm_vm_slice(void);

/*
 * Ask the running MIDlet to end: destroyApp(true), then the VM exits with
 * status 0. It is asynchronous: the request is queued and the wakeup hook
 * fires; keep running slices until -2 or until the caller's own deadline
 * (the app has about 300 ms after CLOSE). A MIDlet that blocks in
 * destroyApp() never finishes; the caller then simply exits the process.
 * Calling it again, or in any state other than RUNNING, does nothing.
 */
void fplinux_pm_vm_request_destroy(void);

/*
 * Report the app's visibility (VISIBILITY events): queues hideNotify() or
 * showNotify() for the current Displayable, never pauseApp(). Hiding also
 * releases held keys. The wakeup hook fires; the caller may stop running
 * slices once a slice after a hide returns non-zero. Repeating the current
 * value, or calling it in any state other than RUNNING, does nothing.
 */
void fplinux_pm_vm_set_visible(bool visible);

enum fplinux_pm_vm_state fplinux_pm_vm_get_state(void);

/*
 * Exit status once EXITED: 0 when the MIDlet called notifyDestroyed() or
 * was destroyed on request, non-zero when the VM or the suite failed (start
 * failure, uncaught exception, MIDlet class not found or not a MIDlet, heap
 * exhausted). Before EXITED it returns 0.
 */
int fplinux_pm_vm_exit_code(void);

/* Key event actions of fplinux_pm_vm_post_key() (MIDP EventConstants). */
#define FPLINUX_PM_VM_KEY_PRESSED 1
#define FPLINUX_PM_VM_KEY_RELEASED 2
#define FPLINUX_PM_VM_KEY_REPEATED 3

/*
 * Queue a MIDP key event (keyCode as Canvas.keyPressed() gets it, e.g. -6
 * or '5') for the current Displayable and wake the VM, outside a slice. The
 * app's Canvas peers queue keys themselves; this is for the headless runner
 * and tests. Does nothing unless the VM is RUNNING or DESTROYING.
 */
void fplinux_pm_vm_post_key(int key_code, int action);

/* --- MIDlet suite manifest (META-INF/MANIFEST.MF in the JAR) ------------ */

/* Inflated manifest size limit; larger manifests are rejected. */
#define FPLINUX_PM_JAR_MANIFEST_MAX (64u * 1024u)
/* MIDlet-<n> entries read at most; later numbers are ignored. */
#define FPLINUX_PM_JAR_MIDLETS_MAX 64u

struct fplinux_pm_midlet {
	char *name; /* first field of MIDlet-<n>, UTF-8, never empty */
	char *icon; /* second field, a resource path in the JAR; "" when none */
	char *class_name; /* third field, never empty */
};

struct fplinux_pm_suite {
	char *name; /* MIDlet-Name; "" when absent */
	char *vendor; /* MIDlet-Vendor; "" when absent */
	char *version; /* MIDlet-Version; "" when absent */
	/* MIDlet-1, MIDlet-2, ... up to the first missing number. */
	size_t midlet_count;
	struct fplinux_pm_midlet *midlets;
};

enum fplinux_pm_jar_status {
	FPLINUX_PM_JAR_OK,
	FPLINUX_PM_JAR_IO, /* cannot open or read the file; errno is set */
	FPLINUX_PM_JAR_FORMAT, /* not a ZIP archive, or a damaged one */
	FPLINUX_PM_JAR_NO_ENTRY, /* no META-INF/MANIFEST.MF or requested entry */
	FPLINUX_PM_JAR_TOO_LARGE, /* entry larger than the caller's limit */
	FPLINUX_PM_JAR_NO_MIDLETS, /* no valid MIDlet-1 attribute */
	FPLINUX_PM_JAR_NO_MEMORY,
};

/*
 * Read the suite attributes and MIDlet list of a JAR. On FPLINUX_PM_JAR_OK
 * the caller owns *suite and releases it with fplinux_pm_suite_cleanup();
 * on any other result *suite is left empty (safe to clean up). A malformed
 * MIDlet-<n> value (fewer than three fields, empty name or class) ends the
 * list as if the number were missing.
 */
enum fplinux_pm_jar_status
fplinux_pm_jar_read_suite(const char *jar_path, struct fplinux_pm_suite *suite);

void fplinux_pm_suite_cleanup(struct fplinux_pm_suite *suite);

/*
 * Read one stored or deflated JAR entry, e.g. a MIDlet icon. A leading '/'
 * in entry_path is ignored. On FPLINUX_PM_JAR_OK *data is a malloc()ed
 * buffer of *size bytes owned by the caller (a 0-byte entry gives a non-NULL
 * buffer). FPLINUX_PM_JAR_NO_ENTRY means the entry does not exist, and
 * FPLINUX_PM_JAR_TOO_LARGE that its uncompressed size in the central
 * directory exceeds max_size; that check happens before any inflation, so
 * max_size 0 is a cheap existence test (OK or TOO_LARGE: present).
 */
enum fplinux_pm_jar_status
fplinux_pm_jar_read_entry(const char *jar_path, const char *entry_path,
			  size_t max_size, unsigned char **data, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
