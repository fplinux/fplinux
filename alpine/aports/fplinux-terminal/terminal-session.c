/* SPDX-License-Identifier: GPL-2.0-only */
#include "terminal-session.h"

#include <errno.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#define FPLINUX_TERMINAL_READ_BUDGET_BYTES 65536U

enum output_state {
	OUTPUT_PENDING,
	OUTPUT_DRAINED,
	OUTPUT_CLOSED,
	OUTPUT_ERROR,
};

void fplinux_terminal_session_init(struct fplinux_terminal_session *session,
				   struct fplinux_terminal *terminal,
				   struct fplinux_terminal_pty *pty)
{
	*session = (struct fplinux_terminal_session){
		.terminal = terminal,
		.pty = pty,
		/* A missing wait status must never become a successful exit. */
		.child_status = 1 << 8,
	};
}

int fplinux_terminal_session_fd(const struct fplinux_terminal_session *session)
{
	if (session->finished)
		return -1;
	return session->output_closed ? session->pty->status_fd :
					session->pty->fd;
}

short fplinux_terminal_session_events(
	const struct fplinux_terminal_session *session)
{
	short events = 0;

	if (session->output_closed)
		return POLLIN;
	if (session->terminal->output_size < FPLINUX_TERMINAL_OUTPUT_BYTES / 2)
		events |= POLLIN;
	if (!session->input_closed && session->terminal->output_size)
		events |= POLLOUT;
	return events;
}

bool fplinux_terminal_session_can_input(
	const struct fplinux_terminal_session *session)
{
	return !session->input_closed && !session->child_finished &&
	       !session->output_closed &&
	       session->terminal->output_size <
		       FPLINUX_TERMINAL_OUTPUT_BYTES / 2;
}

static bool flush_input(struct fplinux_terminal_session *session)
{
	struct fplinux_terminal *terminal = session->terminal;

	if (session->input_closed) {
		fplinux_terminal_consume(terminal, terminal->output_size);
		return true;
	}
	while (terminal->output_size) {
		ssize_t size = write(session->pty->fd, terminal->output,
				     terminal->output_size);

		if (size > 0) {
			fplinux_terminal_consume(terminal, (size_t)size);
			continue;
		}
		if (size < 0 && errno == EINTR)
			continue;
		if (size < 0 && errno == EIO) {
			session->input_closed = true;
			fplinux_terminal_consume(terminal,
						 terminal->output_size);
			return true;
		}
		return size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
	}
	return true;
}

static enum output_state read_output(struct fplinux_terminal_session *session)
{
	struct fplinux_terminal *terminal = session->terminal;
	char bytes[4096];
	size_t total = 0;

	while (total < FPLINUX_TERMINAL_READ_BUDGET_BYTES &&
	       terminal->output_size < FPLINUX_TERMINAL_OUTPUT_BYTES / 2) {
		ssize_t size = read(session->pty->fd, bytes, sizeof(bytes));

		if (size > 0) {
			fplinux_terminal_feed(terminal, bytes, (size_t)size);
			total += (size_t)size;
			continue;
		}
		if (size < 0 && errno == EINTR)
			continue;
		if (!size || (size < 0 && errno == EIO))
			return OUTPUT_CLOSED;
		return errno == EAGAIN || errno == EWOULDBLOCK ?
			       OUTPUT_DRAINED :
			       OUTPUT_ERROR;
	}
	return OUTPUT_PENDING;
}

bool fplinux_terminal_session_dispatch(struct fplinux_terminal_session *session,
				       short events)
{
	enum output_state output = session->output_closed ? OUTPUT_CLOSED :
							    OUTPUT_PENDING;

	if (events & POLLNVAL) {
		errno = EBADF;
		return false;
	}
	if (session->terminal->failed) {
		errno = ENOBUFS;
		return false;
	}
	if (!flush_input(session))
		return false;
	if (!session->output_closed && (events & (POLLIN | POLLHUP | POLLERR)))
		output = read_output(session);
	if (output == OUTPUT_ERROR)
		return false;
	if (!session->child_finished)
		session->child_finished = fplinux_terminal_pty_finished(
			session->pty, &session->child_status);
	if (session->child_finished) {
		session->input_closed = true;
		fplinux_terminal_consume(session->terminal,
					 session->terminal->output_size);
		if (output != OUTPUT_CLOSED) {
			output = read_output(session);
			if (output == OUTPUT_ERROR)
				return false;
		}
		session->finished = output == OUTPUT_CLOSED ||
				    output == OUTPUT_DRAINED;
	}
	if (output == OUTPUT_CLOSED) {
		session->output_closed = true;
		session->input_closed = true;
	} else if ((events & POLLERR) && output == OUTPUT_DRAINED) {
		errno = EIO;
		return false;
	}
	if (session->terminal->failed) {
		errno = ENOBUFS;
		return false;
	}
	return flush_input(session);
}

int fplinux_terminal_session_result(
	const struct fplinux_terminal_session *session)
{
	if (!session->finished)
		return 1;
	if (WIFEXITED(session->child_status))
		return WEXITSTATUS(session->child_status);
	return WIFSIGNALED(session->child_status) ?
		       128 + WTERMSIG(session->child_status) :
		       1;
}
