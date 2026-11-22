/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "files-preview.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PREVIEW_QUEUE_COUNT 16
#define PREVIEW_CACHE_COUNT 24
#define PREVIEW_CACHE_BYTES (256 * 1024)
#define PREVIEW_OUTPUT_BYTES 65536
#define PREVIEW_MAX_DIMENSION 128
#define PREVIEW_TIMEOUT_MS 2000

struct preview_request {
	size_t index;
	char *program;
	char *path;
};

struct preview_result {
	size_t index;
	struct files_preview_image image;
};

struct preview_job {
	pid_t child;
	int output_fd;
	bool failed;
	bool discarded;
	size_t index;
	size_t bytes;
	uint64_t started_ms;
	unsigned char output[PREVIEW_OUTPUT_BYTES + 1];
};

struct files_preview {
	struct preview_job job;
	struct preview_request queue[PREVIEW_QUEUE_COUNT];
	struct preview_result cache[PREVIEW_CACHE_COUNT];
	size_t requests;
	size_t results;
	size_t cached_bytes;
};

static bool current_time_ms(uint64_t *time_ms)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return false;
	*time_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	return true;
}

static void free_request(struct preview_request *request)
{
	free(request->program);
	free(request->path);
}

static void evict_result(struct files_preview *preview)
{
	struct files_preview_image *oldest = &preview->cache[0].image;

	preview->cached_bytes -= oldest->bytes;
	free((void *)oldest->data);
	--preview->results;
	memmove(preview->cache, preview->cache + 1,
		preview->results * sizeof(preview->cache[0]));
}

static void close_output(struct preview_job *job)
{
	if (job->output_fd >= 0)
		close(job->output_fd);
	job->output_fd = -1;
}

static void stop_process_group(pid_t child)
{
	if (kill(-child, SIGKILL) < 0)
		kill(child, SIGKILL);
}

static void interrupt_job(struct preview_job *job)
{
	stop_process_group(job->child);
	close_output(job);
	job->failed = true;
}

static void reap_child(pid_t child, int options)
{
	while (waitpid(child, NULL, options) < 0 && errno == EINTR)
		;
}

struct files_preview *files_preview_new(void)
{
	struct files_preview *preview = calloc(1, sizeof(*preview));

	if (preview)
		preview->job.output_fd = -1;
	return preview;
}

void files_preview_reset(struct files_preview *preview)
{
	if (!preview)
		return;
	while (preview->requests)
		free_request(&preview->queue[--preview->requests]);
	while (preview->results)
		evict_result(preview);
	if (preview->job.child > 0) {
		interrupt_job(&preview->job);
		preview->job.discarded = true;
	}
}

void files_preview_free(struct files_preview *preview)
{
	if (!preview)
		return;
	files_preview_reset(preview);
	if (preview->job.child > 0)
		reap_child(preview->job.child, 0);
	free(preview);
}

bool files_preview_busy(const struct files_preview *preview)
{
	return preview && (preview->job.child > 0 || preview->requests);
}

const struct files_preview_image *
files_preview_request(struct files_preview *preview, size_t index,
		      const char *program, const char *path)
{
	struct preview_request request;

	if (!preview)
		return NULL;
	for (size_t i = 0; i < preview->results; ++i) {
		struct preview_result result = preview->cache[i];
		struct preview_result *newest =
			&preview->cache[preview->results - 1];

		if (result.index != index)
			continue;
		memmove(preview->cache + i, preview->cache + i + 1,
			(preview->results - i - 1) * sizeof(result));
		*newest = result;
		return newest->image.data ? &newest->image : NULL;
	}
	if (preview->job.child > 0 && !preview->job.discarded &&
	    preview->job.index == index)
		return NULL;
	if (!program || program[0] != '/' || !path || path[0] != '/' ||
	    strnlen(program, PATH_MAX) == PATH_MAX ||
	    strnlen(path, PATH_MAX) == PATH_MAX)
		return NULL;
	for (size_t i = 0; i < preview->requests; ++i) {
		if (preview->queue[i].index != index)
			continue;
		request = preview->queue[i];
		memmove(preview->queue + i, preview->queue + i + 1,
			(preview->requests - i - 1) * sizeof(request));
		preview->queue[preview->requests - 1] = request;
		return NULL;
	}
	request = (struct preview_request){ .index = index,
					    .program = strdup(program),
					    .path = strdup(path) };
	if (!request.program || !request.path) {
		free_request(&request);
		return NULL;
	}
	if (preview->requests == PREVIEW_QUEUE_COUNT) {
		free_request(&preview->queue[0]);
		--preview->requests;
		memmove(preview->queue, preview->queue + 1,
			preview->requests * sizeof(request));
	}
	preview->queue[preview->requests++] = request;
	return NULL;
}

static void run_helper(const struct preview_request *request, const int pipe[2])
{
	char *const argv[] = { request->program, request->path, NULL };
	int null_fd;

	close(pipe[0]);
	if (setpgid(0, 0) < 0)
		_exit(1);
	if (pipe[1] == STDOUT_FILENO) {
		if (fcntl(pipe[1], F_SETFD, 0) < 0)
			_exit(1);
	} else {
		if (dup2(pipe[1], STDOUT_FILENO) < 0)
			_exit(1);
		close(pipe[1]);
	}
	null_fd = open("/dev/null", O_RDWR);
	if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
	    dup2(null_fd, STDERR_FILENO) < 0)
		_exit(1);
	if (null_fd > STDERR_FILENO)
		close(null_fd);
	execv(request->program, argv);
	_exit(1);
}

static bool start_job(struct preview_job *job,
		      const struct preview_request *request)
{
	pid_t child;
	int pipe[2];

	job->index = request->index;
	job->bytes = 0;
	job->failed = false;
	job->discarded = false;
	if (!current_time_ms(&job->started_ms) || pipe2(pipe, O_CLOEXEC) < 0)
		return false;
	child = -1;
	if (fcntl(pipe[0], F_SETFL, O_NONBLOCK) == 0)
		child = fork();
	if (child == 0)
		run_helper(request, pipe);
	close(pipe[1]);
	if (child < 0) {
		close(pipe[0]);
		return false;
	}
	/* Either side can establish the group before exec. */
	setpgid(child, child);
	job->child = child;
	job->output_fd = pipe[0];
	return true;
}

static bool read_output(struct preview_job *job)
{
	while (job->bytes < sizeof(job->output)) {
		ssize_t count = read(job->output_fd, job->output + job->bytes,
				     sizeof(job->output) - job->bytes);

		if (count > 0) {
			job->bytes += (size_t)count;
		} else if (count == 0) {
			close_output(job);
			return true;
		} else if (errno == EAGAIN) {
			return true;
		} else if (errno != EINTR) {
			return false;
		}
	}
	return false;
}

static bool reap_finished(struct preview_job *job, bool *success)
{
	siginfo_t status = { 0 };
	int result;

	do {
		result = waitid(P_PID, job->child, &status,
				WEXITED | WNOHANG | WNOWAIT);
	} while (result < 0 && errno == EINTR);
	if (result == 0 && !status.si_pid)
		return false;
	if (result == 0) {
		/* Keep the group ID reserved until any remaining writers stop. */
		stop_process_group(job->child);
		reap_child(job->child, WNOHANG);
	}
	job->child = 0;
	*success = result == 0 && !job->failed &&
		   status.si_code == CLD_EXITED && status.si_status == 0;
	return true;
}

static uint32_t png_u32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | bytes[3];
}

static struct files_preview_image read_image(const struct preview_job *job)
{
	static const unsigned char header[] = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
		0,    0,   0,	13,  'I',  'H',	 'D',  'R',
	};
	uint32_t width, height;

	if (job->bytes < 33 || memcmp(job->output, header, sizeof(header)))
		return (struct files_preview_image){ 0 };
	width = png_u32(job->output + 16);
	height = png_u32(job->output + 20);
	if (!width || width > PREVIEW_MAX_DIMENSION || !height ||
	    height > PREVIEW_MAX_DIMENSION)
		return (struct files_preview_image){ 0 };
	return (struct files_preview_image){ .data = job->output,
					     .bytes = job->bytes,
					     .width = width,
					     .height = height };
}

static bool store_result(struct files_preview *preview, bool success,
			 size_t *completed_index)
{
	struct preview_result result = { .index = preview->job.index };

	if (preview->job.discarded)
		return false;
	if (success)
		result.image = read_image(&preview->job);
	while (preview->results == PREVIEW_CACHE_COUNT ||
	       preview->cached_bytes + result.image.bytes > PREVIEW_CACHE_BYTES)
		evict_result(preview);
	if (result.image.bytes) {
		unsigned char *data = malloc(result.image.bytes);

		if (data) {
			memcpy(data, result.image.data, result.image.bytes);
			result.image.data = data;
		} else {
			result.image = (struct files_preview_image){ 0 };
		}
	}
	preview->cache[preview->results++] = result;
	preview->cached_bytes += result.image.bytes;
	*completed_index = result.index;
	return true;
}

bool files_preview_poll(struct files_preview *preview, size_t *completed_index)
{
	struct preview_job *job;
	uint64_t now_ms;
	bool success;

	if (!preview)
		return false;
	job = &preview->job;
	if (!job->child) {
		struct preview_request *request;

		if (!preview->requests)
			return false;
		request = &preview->queue[--preview->requests];
		success = start_job(job, request);
		free_request(request);
		return !success &&
		       store_result(preview, false, completed_index);
	}
	if (job->output_fd >= 0 && !read_output(job))
		interrupt_job(job);
	if (job->output_fd < 0 && reap_finished(job, &success))
		return store_result(preview, success, completed_index);
	if (!job->failed && (!current_time_ms(&now_ms) ||
			     now_ms - job->started_ms >= PREVIEW_TIMEOUT_MS))
		interrupt_job(job);
	return false;
}
