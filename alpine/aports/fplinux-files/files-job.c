/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "files-job.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void notify(struct fplinux_files_job *job)
{
	char notice = 1;

	while (write(job->pipe[1], &notice, 1) < 0 && errno == EINTR)
		;
}

static bool cancelled(void *context)
{
	struct fplinux_files_job *job = context;

	return atomic_load(&job->cancelled);
}

static void progress(void *context, off_t bytes, off_t total, bool syncing)
{
	struct fplinux_files_job *job = context;
	struct timespec now;
	uint64_t milliseconds;

	clock_gettime(CLOCK_MONOTONIC, &now);
	milliseconds = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	if (!syncing && milliseconds - job->last_progress_ms < 100)
		return;
	job->last_progress_ms = milliseconds;
	pthread_mutex_lock(&job->lock);
	job->status.bytes = bytes;
	job->status.total = total;
	job->status.syncing = syncing;
	pthread_mutex_unlock(&job->lock);
	notify(job);
}

static void *run(void *context)
{
	struct fplinux_files_job *job = context;
	int result;
	int error;

	if (job->reading) {
		if (job->create_directory) {
			struct fplinux_files_operation create;

			result = fplinux_files_operation_open(
				&create, FPLINUX_FILES_MKDIR, NULL,
				job->source);
			if (!result)
				result = fplinux_files_operation_run(
					&create, cancelled, NULL, job);
			error = errno;
			fplinux_files_operation_close(&create);
			if (result < 0 && error != EEXIST) {
				errno = error;
				goto finished;
			}
		}
		job->directory_fd = fplinux_files_directory_open(job->source,
								 job->mount_id);
		result = job->directory_fd < 0 ?
				 -1 :
				 fplinux_files_directory_read(job->source,
							      &job->directory,
							      cancelled, job);
	} else {
		result = job->retry ? 0 :
				      fplinux_files_operation_open(
					      &job->operation, job->action,
					      job->source,
					      job->destination[0] ?
						      job->destination :
						      NULL);
		job->has_operation = true;
		if (!result)
			result = fplinux_files_operation_run(
				&job->operation, cancelled, progress, job);
	}
finished:
	error = result < 0 ? errno : 0;
	pthread_mutex_lock(&job->lock);
	job->status.error = error;
	job->status.done = true;
	pthread_mutex_unlock(&job->lock);
	notify(job);
	return NULL;
}

int fplinux_files_job_init(struct fplinux_files_job *job)
{
	int error;

	*job = (struct fplinux_files_job){ .directory_fd = -1,
					   .pipe = { -1, -1 } };
	atomic_init(&job->cancelled, false);
	error = pthread_mutex_init(&job->lock, NULL);
	if (error) {
		errno = error;
		return -1;
	}
	if (pipe2(job->pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		error = errno;
		pthread_mutex_destroy(&job->lock);
		errno = error;
		return -1;
	}
	return 0;
}

void fplinux_files_job_reset(struct fplinux_files_job *job)
{
	if (job->active)
		return;
	if (job->has_operation)
		fplinux_files_operation_close(&job->operation);
	job->has_operation = false;
	fplinux_files_directory_free(&job->directory);
	if (job->directory_fd >= 0)
		close(job->directory_fd);
	job->directory_fd = -1;
}

static int start(struct fplinux_files_job *job)
{
	int error;

	if (job->active) {
		errno = EBUSY;
		return -1;
	}
	job->status = (struct fplinux_files_job_status){ 0 };
	job->last_progress_ms = 0;
	atomic_store(&job->cancelled, false);
	error = pthread_create(&job->thread, NULL, run, job);
	if (error) {
		errno = error;
		return -1;
	}
	job->active = true;
	return 0;
}

static int copy_path(char *out, const char *path)
{
	if (path && strlen(path) >= FPLINUX_FILES_PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	snprintf(out, FPLINUX_FILES_PATH_MAX, "%s", path ? path : "");
	return 0;
}

int fplinux_files_job_read(struct fplinux_files_job *job, const char *path,
			   bool create_directory, uint32_t mount_id)
{
	if (job->active) {
		errno = EBUSY;
		return -1;
	}
	fplinux_files_job_reset(job);
	if (copy_path(job->source, path) < 0)
		return -1;
	job->reading = true;
	job->create_directory = create_directory;
	job->mount_id = mount_id;
	job->retry = false;
	return start(job);
}

int fplinux_files_job_mutate(struct fplinux_files_job *job,
			     enum fplinux_files_action action,
			     const char *source, const char *destination)
{
	if (job->active) {
		errno = EBUSY;
		return -1;
	}
	fplinux_files_job_reset(job);
	if (copy_path(job->source, source) < 0 ||
	    copy_path(job->destination, destination) < 0)
		return -1;
	job->action = action;
	job->reading = false;
	job->retry = false;
	return start(job);
}

int fplinux_files_job_retry(struct fplinux_files_job *job)
{
	if (!job->has_operation ||
	    job->operation.step == FPLINUX_FILES_EXECUTE) {
		errno = EINVAL;
		return -1;
	}
	job->reading = false;
	job->retry = true;
	return start(job);
}

void fplinux_files_job_cancel(struct fplinux_files_job *job)
{
	atomic_store(&job->cancelled, true);
}

struct fplinux_files_job_status
fplinux_files_job_collect(struct fplinux_files_job *job)
{
	struct fplinux_files_job_status status;
	char notices[128];

	while (read(job->pipe[0], notices, sizeof(notices)) > 0)
		;
	if (!job->active)
		return (struct fplinux_files_job_status){ 0 };
	pthread_mutex_lock(&job->lock);
	status = job->status;
	pthread_mutex_unlock(&job->lock);
	if (status.done && job->active) {
		pthread_join(job->thread, NULL);
		job->active = false;
	}
	return status;
}

void fplinux_files_job_close(struct fplinux_files_job *job)
{
	if (job->active) {
		fplinux_files_job_cancel(job);
		pthread_join(job->thread, NULL);
		job->active = false;
	}
	fplinux_files_job_reset(job);
	close(job->pipe[0]);
	close(job->pipe[1]);
	pthread_mutex_destroy(&job->lock);
}
