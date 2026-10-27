/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_FILES_JOB_H
#define FPLINUX_FILES_JOB_H

#include "files-core.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

struct fplinux_files_job_status {
	off_t bytes;
	off_t total;
	bool syncing;
	bool done;
	int error;
};

/* One worker and a coalesced notification pipe; only the UI collects results. */
struct fplinux_files_job {
	pthread_t thread;
	pthread_mutex_t lock;
	atomic_bool cancelled;
	bool active;
	bool reading;
	bool create_directory;
	bool retry;
	bool has_operation;
	int pipe[2];
	int directory_fd;
	uint64_t last_progress_ms;
	uint32_t mount_id;
	struct fplinux_files_job_status status;
	struct fplinux_files_directory directory;
	struct fplinux_files_operation operation;
	enum fplinux_files_action action;
	char source[FPLINUX_FILES_PATH_MAX];
	char destination[FPLINUX_FILES_PATH_MAX];
};

int fplinux_files_job_init(struct fplinux_files_job *job);
int fplinux_files_job_read(struct fplinux_files_job *job, const char *path,
			   bool create_directory, uint32_t mount_id);
int fplinux_files_job_mutate(struct fplinux_files_job *job,
			     enum fplinux_files_action action,
			     const char *source, const char *destination);
int fplinux_files_job_retry(struct fplinux_files_job *job);
void fplinux_files_job_cancel(struct fplinux_files_job *job);
/* Drains notices and joins only an already finished worker. */
struct fplinux_files_job_status
fplinux_files_job_collect(struct fplinux_files_job *job);
/* Call only while inactive; releases retained operation and directory data. */
void fplinux_files_job_reset(struct fplinux_files_job *job);
/* Final process cleanup may wait; ordinary close uses cancel + collect. */
void fplinux_files_job_close(struct fplinux_files_job *job);

#endif
