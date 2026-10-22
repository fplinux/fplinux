/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-terminal.h"
#include "terminal-pty.h"
#include "terminal-session.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define SCREEN_COLUMNS 128U
#define SCREEN_ROWS 32U

struct job_set {
	pid_t shell;
	pid_t supervisor;
	pid_t pids[3];
};

static struct {
	int fd;
	char bytes[32768];
	size_t used;
} capture = { .fd = -1 };

static char transcript[32768];
static size_t transcript_used;

ssize_t __real_read(int fd, void *bytes, size_t size);
ssize_t __wrap_read(int fd, void *bytes, size_t size);

/* Observe the session's actual reads without changing its I/O or feed path. */
ssize_t __wrap_read(int fd, void *bytes, size_t size)
{
	ssize_t count = __real_read(fd, bytes, size);

	if (fd == capture.fd && count > 0) {
		assert((size_t)count < sizeof(capture.bytes) - capture.used);
		memcpy(capture.bytes + capture.used, bytes, (size_t)count);
		capture.used += (size_t)count;
		capture.bytes[capture.used] = '\0';
		assert((size_t)count < sizeof(transcript) - transcript_used);
		memcpy(transcript + transcript_used, bytes, (size_t)count);
		transcript_used += (size_t)count;
		transcript[transcript_used] = '\0';
	}
	return count;
}

static uint64_t now_ms(void)
{
	struct timespec now;

	assert(!clock_gettime(CLOCK_MONOTONIC, &now));
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static bool screen_has(struct fplinux_terminal *terminal, const char *text)
{
	const struct tsm_screen_cell *cells =
		tsm_screen_draw2(terminal->screen);
	size_t length = strlen(text);
	unsigned int row;
	unsigned int column;

	for (row = 0; row < SCREEN_ROWS; ++row) {
		for (column = 0; column + length <= SCREEN_COLUMNS; ++column) {
			size_t index;

			for (index = 0; index < length; ++index)
				if (cells[row * SCREEN_COLUMNS + column + index]
					    .ch != (unsigned char)text[index])
					break;
			if (index == length)
				return true;
		}
	}
	return false;
}

static void dispatch(struct fplinux_terminal_session *session)
{
	struct pollfd descriptor = {
		.fd = fplinux_terminal_session_fd(session),
		.events = fplinux_terminal_session_events(session),
	};
	int result;

	do {
		result = poll(&descriptor, 1, 20);
	} while (result < 0 && errno == EINTR);
	assert(result >= 0);
	assert(fplinux_terminal_session_dispatch(session, descriptor.revents));
}

static void wait_for_text(struct fplinux_terminal_session *session,
			  const char *text)
{
	uint64_t deadline = now_ms() + 5000;

	while (!screen_has(session->terminal, text)) {
		assert(!session->finished);
		assert(now_ms() < deadline);
		dispatch(session);
	}
}

static void finish(struct fplinux_terminal_session *session)
{
	uint64_t deadline = now_ms() + 5000;

	while (!session->finished) {
		assert(now_ms() < deadline);
		dispatch(session);
	}
	assert(!fplinux_terminal_session_can_input(session));
	assert(fplinux_terminal_session_fd(session) == -1);
}

static void save_pids(const struct fplinux_terminal_pty *pty)
{
	FILE *file = fopen("pty-pids", "w");

	assert(file);
	assert(fprintf(file, "%ld %ld\n", (long)pty->shell,
		       (long)pty->supervisor) > 0);
	assert(!fclose(file));
}

static void backpressure(struct fplinux_terminal_session *session)
{
	struct fplinux_terminal *terminal = session->terminal;
	size_t queued = 0;
	size_t index;
	unsigned int batch;
	FILE *gate;

	wait_for_text(session, "INPUT-READY");
	for (batch = 0; batch < 64; ++batch) {
		assert(!terminal->output_size);
		for (index = 0; index < 12000; ++index) {
			unsigned char character = "q7$Z"[queued % 4];

			fplinux_terminal_key(terminal, character, character, 0,
					     character);
			++queued;
		}
		assert(!terminal->failed);
		assert(terminal->output_size == 12000);
		assert(!fplinux_terminal_session_can_input(session));
		assert(fplinux_terminal_session_dispatch(session, POLLOUT));
		if (terminal->output_size)
			break;
	}
	assert(batch < 64);
	/* The child has not read yet: these unsent literal bytes must survive. */
	for (index = 0; index < terminal->output_size; ++index)
		assert(terminal->output[index] ==
		       "q7$Z"[(queued - terminal->output_size + index) % 4]);
	gate = fopen("input-gate-pending", "w");
	assert(gate);
	assert(fprintf(gate, "%zu\n", queued) > 0);
	assert(!fclose(gate));
	assert(!rename("input-gate-pending", "input-gate"));
	finish(session);
	assert(fplinux_terminal_session_result(session) == 0);
	assert(screen_has(terminal, "INPUT-EXACT"));
}

static pid_t read_pid(const char *path)
{
	FILE *file = fopen(path, "r");
	long value;

	if (!file)
		return -1;
	if (fscanf(file, "%ld", &value) != 1)
		value = -1;
	assert(!fclose(file));
	return (pid_t)value;
}

static void start_jobs(struct fplinux_terminal_session *session);

static struct job_set wait_for_jobs(struct fplinux_terminal_session *session)
{
	const char *const paths[] = {
		"foreground.pid",
		"background.pid",
		"disowned.pid",
	};
	struct fplinux_terminal_pty *pty = session->pty;
	struct job_set jobs = {
		.shell = pty->shell,
		.supervisor = pty->supervisor,
		.pids = { -1, -1, -1 },
	};
	uint64_t deadline;
	size_t index;
	size_t other;

	start_jobs(session);
	deadline = now_ms() + 5000;
	for (;;) {
		for (index = 0; index < 3; ++index) {
			jobs.pids[index] = read_pid(paths[index]);
			if (jobs.pids[index] <= 0)
				break;
		}
		if (index == 3 && tcgetpgrp(pty->fd) == getpgid(jobs.pids[0]))
			break;
		assert(now_ms() < deadline);
		dispatch(session);
	}
	assert(tcgetpgrp(pty->fd) != pty->shell);
	for (index = 0; index < 3; ++index) {
		assert(!kill(jobs.pids[index], 0));
		assert(getsid(jobs.pids[index]) == jobs.shell);
		assert(getpgid(jobs.pids[index]) != jobs.shell);
		for (other = 0; other < index; ++other)
			assert(getpgid(jobs.pids[index]) !=
			       getpgid(jobs.pids[other]));
	}
	capture.fd = -1;
	return jobs;
}

static void assert_jobs_absent(const struct job_set *jobs)
{
	size_t index;

	for (index = 0; index < 3; ++index) {
		errno = 0;
		assert(kill(jobs->pids[index], 0) < 0 && errno == ESRCH);
	}
	errno = 0;
	assert(kill(jobs->shell, 0) < 0 && errno == ESRCH);
}

static void jobs(struct fplinux_terminal_session *session)
{
	struct job_set children = wait_for_jobs(session);
	int status;

	fplinux_terminal_pty_close(session->pty);
	assert_jobs_absent(&children);
	errno = 0;
	assert(waitpid(children.supervisor, &status, WNOHANG) < 0 &&
	       errno == ECHILD);
}

static void run_owner(int report_fd, const char *startup, pid_t observer)
{
	struct fplinux_terminal terminal;
	struct fplinux_terminal_pty pty;
	struct fplinux_terminal_session session;
	struct job_set children;

	/* A failed observer must not leave its separate GUI group running. */
	assert(!prctl(PR_SET_PDEATHSIG, SIGKILL));
	assert(getppid() == observer);
	assert(!setpgid(0, 0));
	assert(fplinux_terminal_init(&terminal, SCREEN_COLUMNS, SCREEN_ROWS));
	assert(fplinux_terminal_pty_open(&pty, SCREEN_COLUMNS, SCREEN_ROWS,
					 "/bin/bash", startup));
	save_pids(&pty);
	fplinux_terminal_session_init(&session, &terminal, &pty);
	children = wait_for_jobs(&session);
	assert(write(report_fd, &children, sizeof(children)) ==
	       sizeof(children));
	assert(!close(report_fd));
	for (;;)
		pause();
}

static void owner_death(const char *startup)
{
	const struct timespec retry = { .tv_nsec = 10000000L };
	struct job_set children;
	struct pollfd ready;
	uint64_t deadline;
	pid_t observer = getpid();
	pid_t owner;
	pid_t result;
	int report[2];
	int status;

	/* Reap the actual guardian after its GUI parent is killed. */
	assert(!prctl(PR_SET_CHILD_SUBREAPER, 1));
	assert(!pipe(report));
	assert(!fcntl(report[0], F_SETFD, FD_CLOEXEC));
	assert(!fcntl(report[1], F_SETFD, FD_CLOEXEC));
	owner = fork();
	assert(owner >= 0);
	if (!owner) {
		close(report[0]);
		run_owner(report[1], startup, observer);
		_exit(99);
	}
	assert(!close(report[1]));
	ready = (struct pollfd){ .fd = report[0], .events = POLLIN };
	assert(poll(&ready, 1, 7000) == 1);
	assert(read(report[0], &children, sizeof(children)) ==
	       sizeof(children));
	assert(!close(report[0]));
	assert(getpgid(owner) == owner);
	assert(owner != getpgrp());
	assert(getsid(children.supervisor) == children.supervisor);
	assert(getpgid(children.supervisor) != owner);
	assert(getpgid(children.shell) != owner);
	assert(!kill(-owner, SIGKILL));
	assert(waitpid(owner, &status, 0) == owner);
	assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
	deadline = now_ms() + 5000;
	do {
		result = waitpid(children.supervisor, &status, WNOHANG);
		if (result < 0 && errno == EINTR)
			continue;
		assert(result >= 0);
		if (result)
			break;
		assert(now_ms() < deadline);
		nanosleep(&retry, NULL);
	} while (true);
	assert(result == children.supervisor);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert_jobs_absent(&children);
	errno = 0;
	assert(kill(children.supervisor, 0) < 0 && errno == ESRCH);
}

static void wait_for_raw(struct fplinux_terminal_session *session,
			 const char *expected)
{
	uint64_t deadline = now_ms() + 5000;

	for (;;) {
		char *match = strstr(capture.bytes, expected);

		if (match) {
			size_t consumed = (size_t)(match - capture.bytes) +
					  strlen(expected);

			capture.used -= consumed;
			memmove(capture.bytes, capture.bytes + consumed,
				capture.used + 1);
			return;
		}
		if (now_ms() >= deadline) {
			fprintf(stderr, "missing PTY bytes %s; received: %s\n",
				expected, capture.bytes);
			assert(!"missing Bash output");
		}
		assert(!session->finished);
		dispatch(session);
	}
}

static void wait_for_prompt(struct fplinux_terminal_session *session)
{
	wait_for_raw(session, geteuid() == 0 ? "# " : "$ ");
	assert(tcgetpgrp(session->pty->fd) == session->pty->shell);
}

static void type_text(struct fplinux_terminal *terminal, const char *text)
{
	for (; *text; ++text) {
		unsigned char character = (unsigned char)*text;

		fplinux_terminal_key(terminal, character, character, 0,
				     character);
	}
}

static void enter(struct fplinux_terminal *terminal)
{
	fplinux_terminal_run(terminal, FPLINUX_TERMINAL_COMMAND_ENTER);
}

static void start_jobs(struct fplinux_terminal_session *session)
{
	capture.fd = session->pty->fd;
	capture.used = 0;
	capture.bytes[0] = '\0';
	/* Startup-file commands run before Bash's interactive job control. */
	wait_for_prompt(session);
	type_text(session->terminal,
		  "\"$TERMINAL_TEST_CHILD\" job disowned.pid & disown; "
		  "\"$TERMINAL_TEST_CHILD\" job background.pid & "
		  "\"$TERMINAL_TEST_CHILD\" job foreground.pid");
	enter(session->terminal);
}

static void press(struct fplinux_terminal *terminal, unsigned int code)
{
	fplinux_terminal_phone(terminal, code, true, 0);
	fplinux_terminal_phone(terminal, code, false, 1);
}

static void bash_readline(struct fplinux_terminal_session *session)
{
	struct fplinux_terminal *terminal = session->terminal;
	FILE *completion = fopen("completion-target", "wx");

	assert(completion);
	assert(!fclose(completion));
	capture.fd = session->pty->fd;
	wait_for_prompt(session);
	type_text(
		terminal,
		"bind -x '\"\\e[98~\":printf \"<LINE:%s>\\n\" \"$READLINE_LINE\"'");
	enter(terminal);
	wait_for_prompt(session);
	type_text(terminal, "printf 'BAD-LONG-WRAPPED-LINE\\n'");
	fplinux_terminal_key(terminal, XKB_KEY_u, 'u', TSM_CONTROL_MASK, 'u');
	type_text(terminal, "\033[98~");
	wait_for_raw(session, "<LINE:>\r\n");
	wait_for_prompt(session);
	type_text(terminal, "printf 'GOOD\\n'");
	enter(terminal);
	wait_for_raw(session, "GOOD\r\n");
	wait_for_prompt(session);
	type_text(terminal, "printf '%s\\n' ./completion");
	press(terminal, KEY_PICKUP_PHONE);
	enter(terminal);
	wait_for_raw(session, "./completion-target\r\n");
	wait_for_prompt(session);
	type_text(terminal, "printf 'HISTORY-ONE\\n'");
	enter(terminal);
	wait_for_raw(session, "HISTORY-ONE\r\n");
	wait_for_prompt(session);
	press(terminal, KEY_UP);
	enter(terminal);
	wait_for_raw(session, "HISTORY-ONE\r\n");
	wait_for_prompt(session);
	fplinux_terminal_run(terminal, FPLINUX_TERMINAL_COMMAND_SEARCH);
	type_text(terminal, "HISTORY");
	enter(terminal);
	wait_for_raw(session, "HISTORY-ONE\r\n");
	wait_for_prompt(session);
	type_text(terminal, "printf 'READ-READY\\n'; read -rs -n 1 value; "
			    "printf 'GOT:%s\\n' \"$value\"");
	enter(terminal);
	wait_for_raw(session, "READ-READY\r\n");
	assert(tcgetpgrp(session->pty->fd) == session->pty->shell);
	press(terminal, KEY_NUMERIC_POUND);
	assert(!terminal->output_size);
	type_text(terminal, "q");
	wait_for_raw(session, "GOT:q\r\n");
	wait_for_prompt(session);
	type_text(terminal, "exit 0");
	enter(terminal);
	finish(session);
	assert(fplinux_terminal_session_result(session) == 0);
	capture.fd = -1;
}

static void bash_history_failure(struct fplinux_terminal_session *session)
{
	static const char warning[] =
		"History cannot be saved. It remains available in this shell.";
	struct fplinux_terminal *terminal = session->terminal;
	const char *first;

	capture.fd = session->pty->fd;
	wait_for_prompt(session);
	/* /dev/full accepts an open and rejects the real history write. */
	type_text(terminal, "HISTFILE=/dev/full");
	enter(terminal);
	wait_for_prompt(session);
	type_text(terminal, "printf 'HISTORY-KEPT\\n'");
	enter(terminal);
	wait_for_raw(session, "HISTORY-KEPT\r\n");
	wait_for_prompt(session);
	press(terminal, KEY_UP);
	enter(terminal);
	wait_for_raw(session, "HISTORY-KEPT\r\n");
	wait_for_prompt(session);
	type_text(terminal, "history | grep '[H]ISTORY-KEPT' | wc -l");
	enter(terminal);
	wait_for_raw(session, "2\r\n");
	wait_for_prompt(session);
	type_text(terminal, "printf 'INPUT-STILL-WORKS\\n'");
	enter(terminal);
	wait_for_raw(session, "INPUT-STILL-WORKS\r\n");
	wait_for_prompt(session);
	first = strstr(transcript, warning);
	assert(first && !strstr(first + strlen(warning), warning));
	assert(!strstr(transcript, "history: /dev/full"));
	type_text(terminal, "exit 0");
	enter(terminal);
	finish(session);
	assert(fplinux_terminal_session_result(session) == 0);
	first = strstr(transcript, warning);
	assert(first && !strstr(first + strlen(warning), warning));
	assert(!strstr(transcript, "history: /dev/full"));
	capture.fd = -1;
}

int main(int argc, char **argv)
{
	struct fplinux_terminal terminal;
	struct fplinux_terminal_pty pty;
	struct fplinux_terminal_session session;
	char startup[4096];
	const char *shell;
	bool is_jobs;
	bool is_bash;
	int result;

	assert(argc == 5);
	is_jobs = !strcmp(argv[1], "jobs") || !strcmp(argv[1], "owner-death");
	is_bash = !strcmp(argv[1], "bash-readline") ||
		  !strcmp(argv[1], "bash-history-failure");
	shell = is_jobs || is_bash ? "/bin/bash" : argv[2];
	if (is_jobs) {
		result = snprintf(startup, sizeof(startup),
				  "%s/terminal-jobs.bashrc", argv[3]);
		assert(result > 0 && (size_t)result < sizeof(startup));
	} else if (is_bash) {
		assert(strlen(argv[4]) < sizeof(startup));
		strcpy(startup, argv[4]);
	} else {
		assert(strlen(argv[1]) < sizeof(startup));
		strcpy(startup, argv[1]);
	}
	if (!strcmp(argv[1], "owner-death")) {
		owner_death(startup);
		return 0;
	}
	assert(fplinux_terminal_init(&terminal, is_bash ? 21 : SCREEN_COLUMNS,
				     is_bash ? 12 : SCREEN_ROWS));
	assert(fplinux_terminal_pty_open(&pty, is_bash ? 21 : SCREEN_COLUMNS,
					 is_bash ? 12 : SCREEN_ROWS, shell,
					 startup));
	save_pids(&pty);
	fplinux_terminal_session_init(&session, &terminal, &pty);
	if (!strcmp(argv[1], "bash-history-failure")) {
		bash_history_failure(&session);
		result = 0;
	} else if (is_bash) {
		bash_readline(&session);
		result = 0;
	} else if (is_jobs) {
		jobs(&session);
		result = 0;
	} else if (!strcmp(argv[1], "backpressure")) {
		backpressure(&session);
		result = 0;
	} else {
		if (!strcmp(argv[1], "closed-stream")) {
			uint64_t deadline = now_ms() + 3000;
			FILE *gate;

			while (!session.output_closed) {
				assert(now_ms() < deadline);
				dispatch(&session);
			}
			assert(!session.finished);
			assert(fplinux_terminal_session_result(&session) == 1);
			assert(fplinux_terminal_session_fd(&session) >= 0);
			gate = fopen("exit-gate", "w");
			assert(gate);
			assert(!fclose(gate));
		}
		finish(&session);
		assert(screen_has(&terminal, "LAST-LITERAL-OUTPUT"));
		result = fplinux_terminal_session_result(&session);
		if (!strcmp(argv[1], "closed-stream")) {
			assert(result == 37);
			result = 0;
		}
	}
	fplinux_terminal_pty_close(&pty);
	fplinux_terminal_destroy(&terminal);
	return result;
}
