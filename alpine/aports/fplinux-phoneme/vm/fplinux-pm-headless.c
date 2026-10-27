/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Headless test entry point: runs one MIDlet through the fplinux-pm-vm
 * interface without the graphical session. System.out goes to stdout. It is
 * a development tool for the VM, not the phoneME app.
 *
 * usage: fplinux-phoneme-headless [--system-classes PATH] [--heap-kib N]
 *            [--timeout-ms N] [--keys SCRIPT] JAR [MIDLET-CLASS]
 * Without MIDLET-CLASS it runs MIDlet-1 of the JAR manifest. --timeout-ms
 * requests destroyApp(true) after that much time, then gives up 300 ms later.
 * --keys presses MIDP key codes on a schedule: comma-separated T:CODE[:HOLD]
 * items press CODE (e.g. -6 or 53 for '5') T ms after the start and release
 * it HOLD ms later (default 100); a hold repeats it from 500 ms on, every
 * 100 ms, as the app's Canvas peers do.
 * Exit status: the VM exit code; 124 when the MIDlet did not end in time;
 * 2 for invalid arguments, 125 for a start error.
 */
#define _GNU_SOURCE
#include "fplinux-pm-vm.h"

#include <errno.h>
#include <fplinux-cli.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define EXIT_TIMEOUT 124
#define EXIT_USAGE 125
#define DESTROY_BUDGET_MS 300
#define KEY_EVENTS_MAX 1024
#define KEY_HOLD_MS 100
#define KEY_REPEAT_DELAY_MS 500
#define KEY_REPEAT_PERIOD_MS 100

/* A scheduled key event of --keys, at_ms after the VM started. */
struct key_event {
	long long at_ms;
	int code;
	int action;
	size_t order;
};

static volatile sig_atomic_t stop_signal;
static bool wakeup_pending;
static struct key_event key_events[KEY_EVENTS_MAX];
static size_t key_count;
static size_t key_next;

static void handle_stop_signal(int signal_number)
{
	stop_signal = signal_number;
}

static void wakeup(void *context)
{
	(void)context;
	wakeup_pending = true;
}

static long long monotonic_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int remove_entry(const char *path, const struct stat *status, int type,
			struct FTW *walk)
{
	(void)status;
	(void)type;
	(void)walk;
	return remove(path);
}

static bool add_key_event(long long at_ms, int code, int action)
{
	if (key_count == KEY_EVENTS_MAX)
		return false;
	key_events[key_count] =
		(struct key_event){ at_ms, code, action, key_count };
	key_count++;
	return true;
}

static int compare_key_events(const void *a, const void *b)
{
	const struct key_event *x = a;
	const struct key_event *y = b;

	if (x->at_ms != y->at_ms)
		return x->at_ms < y->at_ms ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order;
}

/* One T:CODE[:HOLD] item: its press, repeats and release. */
static bool parse_key_item(const char *item)
{
	char *end;
	long at;
	long code;
	long hold = KEY_HOLD_MS;

	errno = 0;
	at = strtol(item, &end, 10);
	if (errno || end == item || *end != ':' || at < 0)
		return false;
	item = end + 1;
	code = strtol(item, &end, 10);
	if (errno || end == item || !code || code < -100 || code > 0xffff)
		return false;
	if (*end == ':') {
		item = end + 1;
		hold = strtol(item, &end, 10);
		if (errno || end == item || hold < 0)
			return false;
	}
	if (*end != ',' && *end != '\0')
		return false;
	if (!add_key_event(at, (int)code, FPLINUX_PM_VM_KEY_PRESSED))
		return false;
	for (long t = KEY_REPEAT_DELAY_MS; t < hold; t += KEY_REPEAT_PERIOD_MS)
		if (!add_key_event(at + t, (int)code,
				   FPLINUX_PM_VM_KEY_REPEATED))
			return false;
	return add_key_event(at + hold, (int)code, FPLINUX_PM_VM_KEY_RELEASED);
}

static bool parse_keys(const char *script)
{
	for (const char *item = script; *item;) {
		const char *comma = strchr(item, ',');

		if (!parse_key_item(item))
			return false;
		if (!comma)
			break;
		item = comma + 1;
	}
	qsort(key_events, key_count, sizeof(key_events[0]), compare_key_events);
	return key_count > 0;
}

/* Posts the scheduled key events that are due; -1 or the next one's time. */
static long long post_due_keys(long long started_ms)
{
	static const char *const names[] = { "", "press", "release", "repeat" };
	long long now = monotonic_ms() - started_ms;

	while (key_next < key_count && key_events[key_next].at_ms <= now) {
		const struct key_event *key = &key_events[key_next++];

		if (key->action != FPLINUX_PM_VM_KEY_REPEATED)
			fprintf(stderr,
				"fplinux-phoneme-headless: key %s %d at "
				"%lld ms\n",
				names[key->action], key->code, now);
		fplinux_pm_vm_post_key(key->code, key->action);
	}
	return key_next < key_count ? started_ms + key_events[key_next].at_ms :
				      -1;
}

/*
 * Waits until the deadline or a signal; -1 means no deadline, which is
 * checked again every second. Wakeups only come from inside slices here.
 * nanosleep, unlike poll() without descriptors, also sleeps under qemu-arm.
 */
static void wait_for(long long deadline_ms)
{
	long long wait_ms = 1000;

	if (wakeup_pending)
		return;
	if (deadline_ms >= 0) {
		wait_ms = deadline_ms - monotonic_ms();
		if (wait_ms <= 0)
			return;
	}
	struct timespec delay = {
		.tv_sec = (time_t)(wait_ms / 1000),
		.tv_nsec = (long)(wait_ms % 1000) * 1000000L,
	};

	nanosleep(&delay, NULL);
}

static long long earliest(long long a, long long b)
{
	if (a < 0)
		return b;
	if (b < 0)
		return a;
	return a < b ? a : b;
}

/* Runs slices until the VM exits; returns false when the deadline passed. */
static bool run_vm(unsigned int timeout_ms)
{
	long long started_ms = monotonic_ms();
	long long destroy_at = timeout_ms != 0 ? started_ms + timeout_ms : -1;
	long long give_up_at = -1;

	for (;;) {
		if (give_up_at < 0 &&
		    (stop_signal != 0 ||
		     (destroy_at >= 0 && monotonic_ms() >= destroy_at))) {
			fputs("fplinux-phoneme-headless: requesting destroy\n",
			      stderr);
			fplinux_pm_vm_request_destroy();
			give_up_at = monotonic_ms() + DESTROY_BUDGET_MS;
		}
		if (give_up_at >= 0 && monotonic_ms() >= give_up_at)
			return false;

		long long next_key =
			give_up_at < 0 ? post_due_keys(started_ms) : -1;

		wakeup_pending = false;
		int result = fplinux_pm_vm_slice();

		if (result == FPLINUX_PM_VM_SLICE_EXITED)
			return true;
		long long deadline = give_up_at >= 0 ? give_up_at : destroy_at;

		deadline = earliest(deadline, next_key);

		if (result > 0)
			deadline = earliest(deadline, monotonic_ms() + result);
		if (result != FPLINUX_PM_VM_SLICE_AGAIN)
			wait_for(deadline);
	}
}

int main(int argc, char **argv)
{
	struct fplinux_cli_option options[] = {
		{ .name = "system-classes",
		  .metavar = "PATH",
		  .help = "Preverified system classes" },
		{ .name = "heap-kib",
		  .metavar = "N",
		  .help = "Java heap size in KiB; zero uses the default" },
		{ .name = "timeout-ms",
		  .metavar = "N",
		  .help = "Request MIDlet destruction after N milliseconds" },
		{ .name = "keys",
		  .metavar = "SCRIPT",
		  .help = "Comma-separated T:CODE[:HOLD] scheduled key presses" },
		{ .metavar = "JAR",
		  .help = "MIDlet suite",
		  .flags = FPLINUX_CLI_REQUIRED },
		{ .metavar = "MIDLET-CLASS",
		  .help = "MIDlet class; default is MIDlet-1" },
	};
	struct fplinux_cli cli = {
		.program = "fplinux-phoneme-headless",
		.description =
			"Run one MIDlet without the graphical session for build verification.",
		.options = options,
		.option_count = sizeof(options) / sizeof(options[0]),
	};
	unsigned int heap_kib = 0;
	unsigned int timeout_ms = 0;
	int parsed = fplinux_cli_parse(&cli, argc, argv);

	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	if (options[1].value &&
	    !fplinux_cli_unsigned(options[1].value, 0, UINT_MAX, &heap_kib))
		return fplinux_cli_error(&cli, "invalid heap size");
	if (options[2].value &&
	    !fplinux_cli_unsigned(options[2].value, 0, UINT_MAX, &timeout_ms))
		return fplinux_cli_error(&cli, "invalid timeout");
	if (options[3].value && !parse_keys(options[3].value))
		return fplinux_cli_error(&cli, "invalid key script");
	const char *jar_path = options[4].value;
	struct fplinux_pm_suite suite = { 0 };
	const char *midlet_class = options[5].value;

	if (midlet_class == NULL) {
		enum fplinux_pm_jar_status status =
			fplinux_pm_jar_read_suite(jar_path, &suite);

		if (status != FPLINUX_PM_JAR_OK) {
			fprintf(stderr,
				"fplinux-phoneme-headless: no MIDlet in %s "
				"(status %d)\n",
				jar_path, (int)status);
			return EXIT_USAGE;
		}
		midlet_class = suite.midlets[0].class_name;
		fprintf(stderr, "fplinux-phoneme-headless: MIDlet-1 %s (%s)\n",
			midlet_class, suite.midlets[0].name);
	}

	char storage_dir[] = "/tmp/fplinux-phoneme-headless.XXXXXX";

	if (mkdtemp(storage_dir) == NULL) {
		perror("fplinux-phoneme-headless: mkdtemp");
		fplinux_pm_suite_cleanup(&suite);
		return EXIT_USAGE;
	}
	signal(SIGINT, handle_stop_signal);
	signal(SIGTERM, handle_stop_signal);

	const struct fplinux_pm_vm_config config = {
		.jar_path = jar_path,
		.midlet_class = midlet_class,
		.system_classes = options[0].value,
		.heap_kib = heap_kib,
		.storage_dir = storage_dir,
		.wakeup = wakeup,
	};
	long long started_ms = monotonic_ms();
	int exit_status = EXIT_USAGE;

	if (fplinux_pm_vm_start(&config)) {
		bool ended = run_vm(timeout_ms);

		exit_status = ended ? fplinux_pm_vm_exit_code() : EXIT_TIMEOUT;
	}

	struct rusage usage_data;

	getrusage(RUSAGE_SELF, &usage_data);
	fflush(stdout);
	fprintf(stderr,
		"fplinux-phoneme-headless: exit %d after %lld ms, "
		"max RSS %ld KiB\n",
		exit_status, monotonic_ms() - started_ms, usage_data.ru_maxrss);
	nftw(storage_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	fplinux_pm_suite_cleanup(&suite);
	return exit_status;
}
