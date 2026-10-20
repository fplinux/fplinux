// SPDX-License-Identifier: GPL-2.0-or-later
/* fplinux-check: package-embedded */

#include "fplinux-quake-stats.h"

#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cvar.h"
#include "draw.h"
#include "screen.h"

#define FPLINUX_QUAKE_STATS_ROWS 4
#define FPLINUX_QUAKE_STATS_COLUMNS 25
#define FPLINUX_QUAKE_STATS_LINE_BYTES 32
#define FPLINUX_QUAKE_STATS_VALUE_BYTES 6
#define FPLINUX_QUAKE_STATS_MARGIN 4
#define FPLINUX_QUAKE_STATS_PADDING 4
#define FPLINUX_QUAKE_STATS_GLYPH_SIZE 8

struct stats_window {
	struct timespec wall_start;
	struct timespec cpu_start;
	uint64_t frames;
	double gpu_total_ns;
	uint64_t gpu_samples;
	bool active;
	bool cpu_valid;
};

static struct stats_window window;
static char lines[FPLINUX_QUAKE_STATS_ROWS][FPLINUX_QUAKE_STATS_LINE_BYTES];

static void reset_window(cvar_t *variable)
{
	(void)variable;
	memset(&window, 0, sizeof(window));
	/* Software borders may retain the panel outside the current view. */
	scr_fullupdate = 0;
	snprintf(lines[0], sizeof(lines[0]), "FPS --  FRAME --ms");
	snprintf(lines[1], sizeof(lines[1]), "CPU --ms");
	snprintf(lines[2], sizeof(lines[2]), "GPU SCENE --ms");
	snprintf(lines[3], sizeof(lines[3]), "RSS --MiB  TEMP --C");
}

static cvar_t stats_cvar = {
	.name = "fplinux_stats",
	.string = "0",
	.callback = reset_window,
};

static double elapsed_seconds(const struct timespec *end,
			      const struct timespec *start)
{
	return (double)end->tv_sec - (double)start->tv_sec +
	       ((double)end->tv_nsec - (double)start->tv_nsec) / 1000000000.0;
}

static bool read_rss_mib(double *rss_mib)
{
	unsigned long long resident_pages;
	long page_bytes = sysconf(_SC_PAGESIZE);
	FILE *file;
	int fields;

	if (page_bytes <= 0)
		return false;
	file = fopen("/proc/self/statm", "re");
	if (!file)
		return false;
	fields = fscanf(file, "%*s %llu", &resident_pages);
	fclose(file);
	if (fields != 1)
		return false;
	*rss_mib = (double)resident_pages * (double)page_bytes / 1048576.0;
	return true;
}

static bool read_temperature_c(double *temperature_c)
{
	glob_t paths = { 0 };
	size_t readable = 0;
	size_t i;

	if (glob("/sys/class/thermal/thermal_zone*/temp", 0, NULL, &paths) !=
	    0) {
		globfree(&paths);
		return false;
	}
	for (i = 0; i < paths.gl_pathc; ++i) {
		FILE *file = fopen(paths.gl_pathv[i], "re");
		long temperature_mc;
		int fields;

		if (!file)
			continue;
		fields = fscanf(file, "%ld", &temperature_mc);
		fclose(file);
		if (fields != 1)
			continue;
		*temperature_c = (double)temperature_mc / 1000.0;
		if (++readable > 1)
			break;
	}
	globfree(&paths);
	return readable == 1;
}

static void format_value(char *text, bool valid, double value)
{
	if (!valid)
		snprintf(text, FPLINUX_QUAKE_STATS_VALUE_BYTES, "--");
	else if (value > 999.9)
		snprintf(text, FPLINUX_QUAKE_STATS_VALUE_BYTES, ">999");
	else if (value < -99.9)
		snprintf(text, FPLINUX_QUAKE_STATS_VALUE_BYTES, "<-99");
	else
		snprintf(text, FPLINUX_QUAKE_STATS_VALUE_BYTES, "%.1f", value);
}

static void publish_window(const struct timespec *now, double wall_seconds)
{
	char fps[FPLINUX_QUAKE_STATS_VALUE_BYTES];
	char frame[FPLINUX_QUAKE_STATS_VALUE_BYTES];
	char cpu[FPLINUX_QUAKE_STATS_VALUE_BYTES];
	char gpu[FPLINUX_QUAKE_STATS_VALUE_BYTES];
	char rss[FPLINUX_QUAKE_STATS_VALUE_BYTES];
	char temperature[FPLINUX_QUAKE_STATS_VALUE_BYTES];
	struct timespec cpu_now;
	bool cpu_valid = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_now) == 0;
	double cpu_seconds =
		cpu_valid && window.cpu_valid ?
			elapsed_seconds(&cpu_now, &window.cpu_start) :
			-1.0;
	double rss_mib = 0.0;
	double temperature_c = 0.0;
	bool rss_valid = read_rss_mib(&rss_mib);
	bool temperature_valid = read_temperature_c(&temperature_c);

	format_value(fps, true, (double)window.frames / wall_seconds);
	format_value(frame, true, wall_seconds * 1000.0 / window.frames);
	format_value(cpu, cpu_seconds >= 0.0,
		     cpu_seconds * 1000.0 / window.frames);
	format_value(gpu, window.gpu_samples != 0,
		     window.gpu_samples ?
			     window.gpu_total_ns / window.gpu_samples /
				     1000000.0 :
			     0.0);
	format_value(rss, rss_valid, rss_mib);
	format_value(temperature, temperature_valid, temperature_c);
	snprintf(lines[0], sizeof(lines[0]), "FPS %s  FRAME %sms", fps, frame);
	snprintf(lines[1], sizeof(lines[1]), "CPU %sms", cpu);
	snprintf(lines[2], sizeof(lines[2]), "GPU SCENE %sms", gpu);
	snprintf(lines[3], sizeof(lines[3]), "RSS %sMiB  TEMP %sC", rss,
		 temperature);

	window.wall_start = *now;
	if (cpu_valid)
		window.cpu_start = cpu_now;
	window.cpu_valid = cpu_valid;
	window.frames = 0;
	window.gpu_total_ns = 0.0;
	window.gpu_samples = 0;
}

static void draw_panel(void)
{
	int available_width = scr_scaled_width - FPLINUX_QUAKE_STATS_MARGIN * 2;
	int available_height =
		scr_scaled_height - FPLINUX_QUAKE_STATS_MARGIN * 2;
	int columns = (available_width - FPLINUX_QUAKE_STATS_PADDING * 2) /
		      FPLINUX_QUAKE_STATS_GLYPH_SIZE;
	int rows = (available_height - FPLINUX_QUAKE_STATS_PADDING * 2) /
		   FPLINUX_QUAKE_STATS_GLYPH_SIZE;
	int i;

	if (columns <= 0 || rows <= 0)
		return;
	if (columns > FPLINUX_QUAKE_STATS_COLUMNS)
		columns = FPLINUX_QUAKE_STATS_COLUMNS;
	if (rows > FPLINUX_QUAKE_STATS_ROWS)
		rows = FPLINUX_QUAKE_STATS_ROWS;
	Draw_Fill(FPLINUX_QUAKE_STATS_MARGIN, FPLINUX_QUAKE_STATS_MARGIN,
		  columns * FPLINUX_QUAKE_STATS_GLYPH_SIZE +
			  FPLINUX_QUAKE_STATS_PADDING * 2,
		  rows * FPLINUX_QUAKE_STATS_GLYPH_SIZE +
			  FPLINUX_QUAKE_STATS_PADDING * 2,
		  0);
	for (i = 0; i < rows; ++i) {
		char text[FPLINUX_QUAKE_STATS_LINE_BYTES];

		snprintf(text, sizeof(text), "%.*s", columns, lines[i]);
		Draw_String(FPLINUX_QUAKE_STATS_MARGIN +
				    FPLINUX_QUAKE_STATS_PADDING,
			    FPLINUX_QUAKE_STATS_MARGIN +
				    FPLINUX_QUAKE_STATS_PADDING +
				    i * FPLINUX_QUAKE_STATS_GLYPH_SIZE,
			    text);
	}
	scr_copytop = 1;
}

void fplinux_quake_stats_init(void)
{
	reset_window(NULL);
	Cvar_RegisterVariable(&stats_cvar);
}

bool fplinux_quake_stats_enabled(void)
{
	return stats_cvar.value != 0.0f;
}

void fplinux_quake_stats_draw(void)
{
	struct timespec now;

	if (!fplinux_quake_stats_enabled())
		return;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		reset_window(NULL);
	} else if (!window.active) {
		window.wall_start = now;
		window.cpu_valid = clock_gettime(CLOCK_PROCESS_CPUTIME_ID,
						 &window.cpu_start) == 0;
		window.active = true;
	} else {
		double wall_seconds = elapsed_seconds(&now, &window.wall_start);

		++window.frames;
		if (wall_seconds >= 1.0)
			publish_window(&now, wall_seconds);
	}
	draw_panel();
}

void fplinux_quake_stats_gpu_sample(uint64_t elapsed_ns)
{
	if (!fplinux_quake_stats_enabled() || !window.active)
		return;
	window.gpu_total_ns += (double)elapsed_ns;
	++window.gpu_samples;
}
