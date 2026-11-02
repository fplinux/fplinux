/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <fplinux/fplinux-ui-file-picker.h>
#include <fplinux/fplinux-ui-screen.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct application {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *base;
	struct fplinux_ui_file_picker *picker;
};

static void result(struct fplinux_ui_file_picker *picker,
		   enum fplinux_ui_file_picker_status status, const char *path,
		   int error, void *context)
{
	struct application *app = context;
	char filename[4096];
	FILE *output;

	assert(snprintf(filename, sizeof(filename), "%s/result",
			getenv("HOME")) < (int)sizeof(filename));
	output = fopen(filename, "w");
	assert(output);
	if (status == FPLINUX_UI_FILE_PICKER_SELECTED)
		fprintf(output, "selected:%s\n", path);
	else if (status == FPLINUX_UI_FILE_PICKER_CANCELLED)
		fputs("cancelled\n", output);
	else
		fprintf(output, "error:%d\n", error);
	assert(fclose(output) == 0);
	app->picker = NULL;
	fplinux_ui_file_picker_close(picker);
	assert(fplinux_ui_screen_set_title(app->base, "Picker closed"));
}

static void storage_result(struct fplinux_ui *ui,
			   const struct fplinux_volume_result *volume,
			   void *context)
{
	struct application *app = context;

	(void)ui;
	fplinux_ui_file_picker_storage_result(app->picker, volume);
}

static void event(struct fplinux_ui *ui, const struct fplinux_ui_event *event,
		  void *context)
{
	(void)context;
	if (event->type == FPLINUX_UI_EVENT_CLOSE)
		fplinux_ui_quit(ui);
	else if (event->type == FPLINUX_UI_EVENT_ACTIVATION)
		fplinux_ui_storage_request(ui, FPLINUX_VOLUME_REFRESH, 0);
}

int main(void)
{
	static const char *const extensions[] = { "jar", "vxp", NULL };
	struct application app = { 0 };
	struct fplinux_ui_file_picker_options options = {
		.mode = FPLINUX_UI_FILE_PICKER_FILE,
		.title = "Files",
		.extensions = extensions,
	};
	struct fplinux_ui_screen *base;
	char home[4096];
	int result_code;

	assert(snprintf(home, sizeof(home), "%s/Files", getenv("HOME")) <
	       (int)sizeof(home));
	options.home = home;
	app.ui = fplinux_ui_open("org.fplinux.files", event, &app);
	assert(app.ui);
	base = fplinux_ui_form_new(app.ui, "Done");
	assert(base && fplinux_ui_push(app.ui, base));
	app.base = base;
	fplinux_ui_set_storage_handler(app.ui, storage_result, &app);
	app.picker =
		fplinux_ui_file_picker_open(app.ui, &options, result, &app);
	assert(app.picker);
	result_code = fplinux_ui_run(app.ui);
	fplinux_ui_file_picker_close(app.picker);
	fplinux_ui_close(app.ui);
	return result_code < 0;
}
