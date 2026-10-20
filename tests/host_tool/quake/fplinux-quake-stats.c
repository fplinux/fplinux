// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "fplinux-quake-stats.h"

#include <assert.h>
#include <errno.h>
#include <glob.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cvar.h"
#include "draw.h"
#include "screen.h"

struct acquisitions {
	unsigned int monotonic;
	unsigned int process;
	unsigned int files;
	unsigned int thermal;
	unsigned int page_size;
};

int scr_scaled_width = 320;
int scr_scaled_height = 200;
int scr_copytop;
int scr_fullupdate;

static const char *fixture_directory;
static cvar_t *stats_cvar;
static uint64_t monotonic_ns;
static uint64_t process_ns;
static bool monotonic_failed;
static bool process_failed;
static long page_bytes = 4096;
static struct acquisitions acquired;
static char drawn_lines[16][64];
static size_t drawn_count;
static bool panel_drawn;

FILE *__real_fopen(const char *path, const char *mode);
int __real_glob(const char *pattern, int flags,
		int (*error_function)(const char *, int), glob_t *paths);
FILE *__wrap_fopen(const char *path, const char *mode);
int __wrap_glob(const char *pattern, int flags,
		int (*error_function)(const char *, int), glob_t *paths);
int __wrap_clock_gettime(clockid_t clock, struct timespec *value);
long __wrap_sysconf(int name);

static void fixture_path(char *path, size_t capacity, const char *relative)
{
	int length =
		snprintf(path, capacity, "%s/%s", fixture_directory, relative);

	assert(length > 0 && (size_t)length < capacity);
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
	char fixture[1024];

	++acquired.files;
	if (!strcmp(path, "/proc/self/statm")) {
		fixture_path(fixture, sizeof(fixture), "statm");
		return __real_fopen(fixture, mode);
	}
	assert(strstr(path, fixture_directory) == path);
	return __real_fopen(path, mode);
}

int __wrap_glob(const char *pattern, int flags,
		int (*error_function)(const char *, int), glob_t *paths)
{
	char fixture[1024];

	++acquired.thermal;
	assert(!strcmp(pattern, "/sys/class/thermal/thermal_zone*/temp"));
	fixture_path(fixture, sizeof(fixture), "thermal/thermal_zone*/temp");
	return __real_glob(fixture, flags, error_function, paths);
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *value)
{
	uint64_t elapsed_ns;
	bool failed;

	if (clock == CLOCK_MONOTONIC) {
		++acquired.monotonic;
		elapsed_ns = monotonic_ns;
		failed = monotonic_failed;
	} else {
		assert(clock == CLOCK_PROCESS_CPUTIME_ID);
		++acquired.process;
		elapsed_ns = process_ns;
		failed = process_failed;
	}
	if (failed) {
		errno = EIO;
		return -1;
	}
	value->tv_sec = elapsed_ns / 1000000000;
	value->tv_nsec = elapsed_ns % 1000000000;
	return 0;
}

long __wrap_sysconf(int name)
{
	assert(name == _SC_PAGESIZE);
	++acquired.page_size;
	return page_bytes;
}

void Cvar_RegisterVariable(cvar_t *variable)
{
	assert(!stats_cvar);
	assert(!strcmp(variable->name, "fplinux_stats"));
	assert(!strcmp(variable->string, "0"));
	variable->value = 0.0f;
	stats_cvar = variable;
}

void Draw_Fill(int x, int y, int width, int height, int color)
{
	(void)color;
	assert(x >= 0 && y >= 0 && width > 0 && height > 0);
	assert(x <= scr_scaled_width && width <= scr_scaled_width - x);
	assert(y <= scr_scaled_height && height <= scr_scaled_height - y);
	panel_drawn = true;
}

void Draw_String(int x, int y, const char *text)
{
	size_t length = strlen(text);

	assert(drawn_count < 16 && length < sizeof(drawn_lines[0]));
	assert(x >= 0 && y >= 0 && x <= scr_scaled_width);
	assert(length * 8 <= (size_t)(scr_scaled_width - x));
	assert(y <= scr_scaled_height && 8 <= scr_scaled_height - y);
	memcpy(drawn_lines[drawn_count++], text, length + 1);
}

static void set_enabled(bool enabled)
{
	stats_cvar->value = enabled ? 1.0f : 0.0f;
	if (stats_cvar->callback)
		stats_cvar->callback(stats_cvar);
	assert(fplinux_quake_stats_enabled() == enabled);
}

static void draw_at(uint64_t wall_ns, uint64_t cpu_ns)
{
	monotonic_ns = wall_ns;
	process_ns = cpu_ns;
	drawn_count = 0;
	panel_drawn = false;
	scr_copytop = 0;
	fplinux_quake_stats_draw();
}

static void expect_line(size_t row, const char *expected)
{
	assert(row < drawn_count);
	if (strcmp(drawn_lines[row], expected)) {
		fprintf(stderr, "line %zu: expected [%s], got [%s]\n", row,
			expected, drawn_lines[row]);
		abort();
	}
}

static void expect_unavailable(void)
{
	expect_line(0, "FPS --  FRAME --ms");
	expect_line(1, "CPU --ms");
	expect_line(2, "GPU SCENE --ms");
	expect_line(3, "RSS --MiB  TEMP --C");
}

static void expect_no_acquisition(struct acquisitions before)
{
	assert(before.monotonic == acquired.monotonic);
	assert(before.process == acquired.process);
	assert(before.files == acquired.files);
	assert(before.thermal == acquired.thermal);
	assert(before.page_size == acquired.page_size);
}

static void write_fixture(const char *relative, const char *content)
{
	char path[1024];
	FILE *file;

	fixture_path(path, sizeof(path), relative);
	file = __real_fopen(path, "w");
	assert(file);
	assert(fputs(content, file) >= 0);
	assert(fclose(file) == 0);
}

static void remove_fixture(const char *relative)
{
	char path[1024];

	fixture_path(path, sizeof(path), relative);
	assert(unlink(path) == 0);
}

static void check_metrics(void)
{
	unsigned int frame;

	set_enabled(true);
	draw_at(5000000000, 12300000000);
	expect_unavailable();
	for (frame = 1; frame <= 48; ++frame) {
		uint64_t wall_ns = frame == 48 ? 6200000000 :
						 5000000000 + frame * 10000000;

		fplinux_quake_stats_gpu_sample(frame % 2 ? 2000000 : 6000000);
		draw_at(wall_ns, 12300000000 + frame * 10000000);
	}
	expect_line(0, "FPS 40.0  FRAME 25.0ms");
	expect_line(1, "CPU 10.0ms");
	expect_line(2, "GPU SCENE 4.0ms");
	expect_line(3, "RSS 12.0MiB  TEMP 42.5C");
	assert(scr_copytop);

	draw_at(7400000000, 12790000000);
	expect_line(1, "CPU 10.0ms");
	expect_line(2, "GPU SCENE --ms");
}

static void check_optional_recovery(void)
{
	set_enabled(true);
	draw_at(10000000000, 3000000000);
	draw_at(11200000000, 3060000000);
	expect_line(0, "FPS 0.8  FRAME >999ms");
	expect_line(1, "CPU 60.0ms");
	expect_line(2, "GPU SCENE --ms");
	expect_line(3, "RSS --MiB  TEMP --C");

	write_fixture("statm", "4096 2048 0 0 0 0 0\n");
	write_fixture("thermal/thermal_zone0/temp", "-5500\n");
	draw_at(12400000000, 3110000000);
	expect_line(3, "RSS 8.0MiB  TEMP -5.5C");

	write_fixture("statm", "resident pages unavailable\n");
	write_fixture("thermal/thermal_zone0/temp", "unavailable\n");
	draw_at(13600000000, 3160000000);
	expect_line(3, "RSS --MiB  TEMP --C");

	write_fixture("statm", "4096 1024 0 0 0 0 0\n");
	write_fixture("thermal/thermal_zone0/temp", "28500\n");
	draw_at(14800000000, 3210000000);
	expect_line(3, "RSS 4.0MiB  TEMP 28.5C");
}

static void check_ambiguous_temperature(void)
{
	set_enabled(true);
	draw_at(1000000000, 0);
	draw_at(2000000000, 10000000);
	expect_line(3, "RSS 12.0MiB  TEMP --C");

	remove_fixture("thermal/thermal_zone1/temp");
	page_bytes = -1;
	draw_at(3000000000, 20000000);
	expect_line(3, "RSS --MiB  TEMP 42.5C");

	page_bytes = 4096;
	draw_at(4000000000, 30000000);
	expect_line(3, "RSS 12.0MiB  TEMP 42.5C");
}

static void check_disabled_and_reenabled(void)
{
	struct acquisitions before = acquired;

	assert(!fplinux_quake_stats_enabled());
	draw_at(1000000000, 1000000000);
	fplinux_quake_stats_gpu_sample(2000000);
	assert(!panel_drawn && !drawn_count);
	expect_no_acquisition(before);

	set_enabled(true);
	draw_at(2000000000, 2000000000);
	fplinux_quake_stats_gpu_sample(100000000);
	draw_at(3200000000, 2100000000);
	expect_line(1, "CPU 100.0ms");
	expect_line(2, "GPU SCENE 100.0ms");

	before = acquired;
	scr_fullupdate = 7;
	set_enabled(false);
	assert(scr_fullupdate == 0);
	draw_at(90000000000, 40000000000);
	fplinux_quake_stats_gpu_sample(200000000);
	assert(!panel_drawn && !drawn_count);
	expect_no_acquisition(before);

	set_enabled(true);
	fplinux_quake_stats_gpu_sample(300000000);
	draw_at(100000000000, 50000000000);
	expect_unavailable();
	fplinux_quake_stats_gpu_sample(2000000);
	draw_at(100250000000, 50200000000);
	expect_unavailable();
	fplinux_quake_stats_gpu_sample(6000000);
	draw_at(101000000000, 50400000000);
	expect_line(0, "FPS 2.0  FRAME 500.0ms");
	expect_line(1, "CPU 200.0ms");
	expect_line(2, "GPU SCENE 4.0ms");
}

static void check_clock_recovery(void)
{
	set_enabled(true);
	process_failed = true;
	draw_at(1000000000, 1000000000);
	process_failed = false;
	draw_at(2000000000, 1100000000);
	expect_line(1, "CPU --ms");
	draw_at(3000000000, 1120000000);
	expect_line(1, "CPU 20.0ms");

	process_failed = true;
	draw_at(4000000000, 1140000000);
	expect_line(1, "CPU --ms");
	process_failed = false;
	draw_at(5000000000, 1160000000);
	expect_line(1, "CPU --ms");
	draw_at(6000000000, 1180000000);
	expect_line(1, "CPU 20.0ms");

	monotonic_failed = true;
	draw_at(7000000000, 1200000000);
	expect_unavailable();
	monotonic_failed = false;
	draw_at(20000000000, 5000000000);
	expect_unavailable();
	draw_at(21000000000, 5030000000);
	expect_line(0, "FPS 1.0  FRAME >999ms");
	expect_line(1, "CPU 30.0ms");
}

static void check_geometry(int width, int height)
{
	scr_scaled_width = width;
	scr_scaled_height = height;
	set_enabled(true);
	draw_at(1000000000, 0);
	fplinux_quake_stats_gpu_sample(1500000000);
	draw_at(2200000000, 2000000000);
	if (width >= 240 && height >= 200) {
		expect_line(0, "FPS 0.8  FRAME >999ms");
		expect_line(1, "CPU >999ms");
		expect_line(2, "GPU SCENE >999ms");
		expect_line(3, "RSS >999MiB  TEMP <-99C");
	} else {
		assert(panel_drawn && drawn_count);
	}
}

int main(int argc, char **argv)
{
	assert(argc == 3 || argc == 5);
	fixture_directory = argv[2];
	fplinux_quake_stats_init();
	expect_no_acquisition((struct acquisitions){ 0 });
	if (!strcmp(argv[1], "metrics"))
		check_metrics();
	else if (!strcmp(argv[1], "optional-recovery"))
		check_optional_recovery();
	else if (!strcmp(argv[1], "ambiguous-temperature"))
		check_ambiguous_temperature();
	else if (!strcmp(argv[1], "disabled"))
		check_disabled_and_reenabled();
	else if (!strcmp(argv[1], "clock-recovery"))
		check_clock_recovery();
	else if (!strcmp(argv[1], "geometry")) {
		assert(argc == 5);
		check_geometry(atoi(argv[3]), atoi(argv[4]));
	} else {
		abort();
	}
	return 0;
}
