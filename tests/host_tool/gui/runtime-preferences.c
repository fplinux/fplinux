/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-ui-prefs.h"
#include "fplinux-ui-recent.h"
#include "fplinux-ui-view-settings.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool fail_directory_sync;

int __real_fsync(int fd);

int __wrap_fsync(int fd)
{
	struct stat status;

	assert(fstat(fd, &status) == 0);
	if (fail_directory_sync && S_ISDIR(status.st_mode)) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static void check_recent(struct fplinux_ui_prefs *prefs)
{
	struct fplinux_ui_recent recent = { 0 }, loaded = { 0 };
	char path[32], title[32];

	assert(fplinux_ui_recent_load(&recent, prefs));
	assert(recent.count == 0);
	for (int i = 0; i < 11; ++i) {
		snprintf(path, sizeof(path), "/missing/%d.jar", i);
		snprintf(title, sizeof(title), "Program %d", i);
		assert(fplinux_ui_recent_record(&recent, path, title));
	}
	assert(recent.count == 10);
	assert(!strcmp(recent.items[0].path, "/missing/10.jar"));
	assert(!strcmp(recent.items[9].path, "/missing/1.jar"));
	assert(fplinux_ui_recent_record(&recent, "/missing/5.jar", "Renamed"));
	assert(recent.count == 10);
	assert(!strcmp(recent.items[0].title, "Renamed"));
	assert(!strcmp(recent.items[1].path, "/missing/10.jar"));
	assert(fplinux_ui_recent_remove(&recent, 1));
	assert(fplinux_ui_recent_store(&recent, prefs));
	assert(fplinux_ui_recent_load(&loaded, prefs));
	assert(loaded.count == 9);
	assert(!strcmp(loaded.items[0].path, "/missing/5.jar"));
	assert(!strcmp(loaded.items[0].title, "Renamed"));
	assert(!strcmp(loaded.items[1].path, "/missing/9.jar"));
	assert(!strcmp(loaded.items[8].path, "/missing/1.jar"));
	fplinux_ui_recent_cleanup(&recent);
	fplinux_ui_recent_cleanup(&loaded);
}

static void check_view(struct fplinux_ui_prefs *prefs)
{
	struct fplinux_ui_view_settings defaults =
		fplinux_ui_view_settings_defaults();
	struct fplinux_ui_view_settings loaded, global;

	assert(defaults.scale == FPLINUX_RGB565_FIT);
	assert(defaults.rotation == FPLINUX_RGB565_ROTATE_0);
	global = defaults;
	global.scale = FPLINUX_RGB565_INTEGER;
	global.rotation = FPLINUX_RGB565_ROTATE_90;
	assert(fplinux_ui_view_settings_store(&global, prefs));
	assert(fplinux_ui_view_settings_load(&loaded, prefs, &defaults));
	assert(loaded.scale == FPLINUX_RGB565_INTEGER);
	assert(loaded.rotation == FPLINUX_RGB565_ROTATE_90);
	assert(fplinux_ui_prefs_set(prefs, "view-settings",
				    "{\"rotation\":\"270\"}"));
	assert(fplinux_ui_view_settings_load(&loaded, prefs, &global));
	assert(loaded.scale == FPLINUX_RGB565_INTEGER);
	assert(loaded.rotation == FPLINUX_RGB565_ROTATE_270);
	assert(fplinux_ui_prefs_set(prefs, "view-settings",
				    "{\"scale\":\"unknown\"}"));
	assert(!fplinux_ui_view_settings_load(&loaded, prefs, &global));
	assert(loaded.rotation == FPLINUX_RGB565_ROTATE_270);
	assert(fplinux_ui_view_settings_reset(prefs));
	assert(fplinux_ui_view_settings_load(&loaded, prefs, &global));
	assert(loaded.scale == global.scale &&
	       loaded.rotation == global.rotation);
}

int main(int argc, char **argv)
{
	struct fplinux_ui_prefs *prefs, *reopened;
	struct fplinux_storage_result result;
	struct fplinux_ui_recent recent = { 0 };
	struct fplinux_ui_view_settings defaults =
		fplinux_ui_view_settings_defaults();
	struct fplinux_ui_view_settings global = { FPLINUX_RGB565_INTEGER,
						   FPLINUX_RGB565_ROTATE_90 };
	struct fplinux_ui_view_settings loaded;
	char blocked[4096], child[4096];
	FILE *file;

	assert(argc == 2);
	prefs = fplinux_ui_prefs_open_directory(argv[1]);
	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "unrelated", "kept"));
	check_recent(prefs);
	check_view(prefs);
	assert(fplinux_ui_view_settings_store(&global, prefs));
	fail_directory_sync = true;
	result = fplinux_ui_prefs_save(prefs);
	assert(result.state == FPLINUX_STORAGE_PUBLISHED_UNSYNCED);
	assert(result.error == EIO);
	fail_directory_sync = false;
	result = fplinux_ui_prefs_save(prefs);
	assert(result.state == FPLINUX_STORAGE_DURABLE && !result.error);
	fplinux_ui_prefs_close(prefs);
	reopened = fplinux_ui_prefs_open_directory(argv[1]);
	assert(reopened);
	assert(!strcmp(fplinux_ui_prefs_get(reopened, "unrelated", ""),
		       "kept"));
	assert(fplinux_ui_recent_load(&recent, reopened));
	assert(recent.count == 9);
	assert(!strcmp(recent.items[0].path, "/missing/5.jar"));
	assert(fplinux_ui_view_settings_load(&loaded, reopened, &defaults));
	assert(loaded.scale == FPLINUX_RGB565_INTEGER);
	assert(loaded.rotation == FPLINUX_RGB565_ROTATE_90);
	fplinux_ui_recent_cleanup(&recent);
	fplinux_ui_prefs_close(reopened);
	assert(snprintf(blocked, sizeof(blocked), "%s-blocked", argv[1]) <
	       (int)sizeof(blocked));
	assert(snprintf(child, sizeof(child), "%s/child", blocked) <
	       (int)sizeof(child));
	prefs = fplinux_ui_prefs_open_directory(child);
	assert(prefs);
	assert(fplinux_ui_prefs_set(prefs, "unrelated", "still in memory"));
	file = fopen(blocked, "w");
	assert(file && fclose(file) == 0);
	result = fplinux_ui_prefs_save(prefs);
	assert(result.state == FPLINUX_STORAGE_UNPUBLISHED &&
	       result.error == ENOTDIR);
	assert(!strcmp(fplinux_ui_prefs_get(prefs, "unrelated", ""),
		       "still in memory"));
	assert(unlink(blocked) == 0);
	result = fplinux_ui_prefs_save(prefs);
	assert(result.state == FPLINUX_STORAGE_DURABLE && !result.error);
	fplinux_ui_prefs_close(prefs);
	puts("runtime-preferences: ok");
	return 0;
}
