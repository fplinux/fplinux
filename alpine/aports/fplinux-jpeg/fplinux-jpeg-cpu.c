/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CPU reference for the FPLinux JPEG comparison boundary.
 *
 * This is deliberately separate from the V4L2 hardware CLI.  It has no
 * device path and never selects a hardware fallback.
 */
#define _POSIX_C_SOURCE 200809L

#include "fplinux-cli.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>

enum {
	COMPONENTS = 3,
	DQT_BYTES = 128,
	IO_CHUNK = 8192,
	MAX_REPEAT = 4096,
	MAX_RAW_BYTES = 32 * 1024 * 1024,
	MAX_ENCODED_BYTES = 64 * 1024 * 1024,
};

enum operation { OP_ENCODE, OP_DECODE, OP_SCALE };
enum decode_mode { DECODE_REDUCED, DECODE_BOX };

enum cli_option {
	CLI_OPTION_OPERATION,
	CLI_OPTION_INPUT,
	CLI_OPTION_OUTPUT,
	CLI_OPTION_WIDTH,
	CLI_OPTION_HEIGHT,
	CLI_OPTION_SCALE,
	CLI_OPTION_QUALITY,
	CLI_OPTION_RESTART,
	CLI_OPTION_REPEAT,
	CLI_OPTION_DQT,
	CLI_OPTION_DECODE_MODE,
};

struct options {
	enum operation operation;
	enum decode_mode decode_mode;
	const char *input_path;
	const char *output_path;
	const char *dqt_path;
	unsigned width;
	unsigned height;
	unsigned scale;
	unsigned quality;
	unsigned restart;
	unsigned repeat;
	bool width_set;
	bool height_set;
	bool scale_set;
	bool quality_set;
	bool restart_set;
	bool decode_mode_set;
};

struct jpeg_error_state {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
	char message[JMSG_LENGTH_MAX];
};

struct metric {
	double wall;
	double user;
	double system;
};

struct metric_start {
	double wall;
	struct rusage usage;
};

struct raw_rows {
	JSAMPIMAGE rows;
	JSAMPROW *row_ptrs[COMPONENTS];
	uint8_t *samples[COMPONENTS];
	size_t row_width[COMPONENTS];
	unsigned row_count[COMPONENTS];
};

struct raw_image {
	uint8_t *plane[COMPONENTS];
	unsigned width[COMPONENTS];
	unsigned height[COMPONENTS];
	unsigned vsub;
};

struct nv_frame {
	uint8_t *data;
	size_t size;
	unsigned width;
	unsigned height;
	unsigned chroma_width;
	unsigned chroma_height;
	unsigned vsub;
};

/*
 * libjpeg reports fatal errors with longjmp().  Every value which owns
 * cleanup state after that boundary belongs to a heap operation object, not
 * to an automatic local that longjmp() may make indeterminate.
 */
struct decode_operation {
	struct jpeg_decompress_struct decoder;
	struct jpeg_error_state error;
	struct raw_rows rows;
	struct raw_image image;
	unsigned produced[COMPONENTS];
	unsigned vsub;
	bool created;
	bool ok;
};

struct encode_operation {
	struct jpeg_compress_struct encoder;
	struct jpeg_error_state error;
	struct raw_rows rows;
	struct metric_start start;
	FILE *file;
	size_t result_size;
	bool created;
	bool ok;
};

static void set_error(char *error, size_t error_size, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(error, error_size, format, arguments);
	va_end(arguments);
}

static double monotonic_seconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1.0;
	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static double timeval_seconds(const struct timeval *value)
{
	return (double)value->tv_sec + (double)value->tv_usec / 1000000.0;
}

static bool metric_start(struct metric_start *start)
{
	start->wall = monotonic_seconds();
	return start->wall >= 0.0 && getrusage(RUSAGE_SELF, &start->usage) == 0;
}

static bool metric_stop(struct metric *total, const struct metric_start *start)
{
	struct rusage end;
	double wall = monotonic_seconds();

	if (wall < 0.0 || getrusage(RUSAGE_SELF, &end) != 0)
		return false;
	total->wall += wall - start->wall;
	total->user += timeval_seconds(&end.ru_utime) -
		       timeval_seconds(&start->usage.ru_utime);
	total->system += timeval_seconds(&end.ru_stime) -
			 timeval_seconds(&start->usage.ru_stime);
	return true;
}

static void print_metric(const char *name, const struct metric *value)
{
	printf("%s_wall_ms=%.3f %s_user_ms=%.3f %s_system_ms=%.3f %s_cpu_ms=%.3f\n",
	       name, 1000.0 * value->wall, name, 1000.0 * value->user, name,
	       1000.0 * value->system, name,
	       1000.0 * (value->user + value->system));
}

static struct metric add_metrics(const struct metric *left,
				 const struct metric *right)
{
	return (struct metric){
		.wall = left->wall + right->wall,
		.user = left->user + right->user,
		.system = left->system + right->system,
	};
}

static const char *parse_cli_option(size_t option, const char *value,
				    void *data)
{
	struct options *options = data;

	switch (option) {
	case CLI_OPTION_OPERATION:
		if (!strcmp(value, "encode"))
			options->operation = OP_ENCODE;
		else if (!strcmp(value, "decode"))
			options->operation = OP_DECODE;
		else if (!strcmp(value, "scale"))
			options->operation = OP_SCALE;
		else
			return "--operation must be encode, decode or scale";
		break;
	case CLI_OPTION_WIDTH:
		if (!fplinux_cli_unsigned(value, 1, UINT_MAX, &options->width))
			return "--width and --height must be positive unsigned integers";
		break;
	case CLI_OPTION_HEIGHT:
		if (!fplinux_cli_unsigned(value, 1, UINT_MAX, &options->height))
			return "--width and --height must be positive unsigned integers";
		break;
	case CLI_OPTION_SCALE:
		if (!fplinux_cli_unsigned(value, 0, 4, &options->scale) ||
		    (options->scale != 1 && options->scale != 2 &&
		     options->scale != 4))
			return "--scale must be 1, 2 or 4";
		break;
	case CLI_OPTION_QUALITY:
		if (!fplinux_cli_unsigned(value, 1, 100, &options->quality))
			return "--quality must be in 1..100";
		break;
	case CLI_OPTION_RESTART:
		if (!fplinux_cli_unsigned(value, 0, 65535, &options->restart))
			return "--restart must be in 0..65535";
		break;
	case CLI_OPTION_REPEAT:
		if (!fplinux_cli_unsigned(value, 1, MAX_REPEAT,
					  &options->repeat))
			return "--repeat must be in 1..4096";
		break;
	case CLI_OPTION_DECODE_MODE:
		if (!strcmp(value, "reduced"))
			options->decode_mode = DECODE_REDUCED;
		else if (!strcmp(value, "box"))
			options->decode_mode = DECODE_BOX;
		else
			return "--decode-mode must be reduced or box";
		break;
	case CLI_OPTION_INPUT:
	case CLI_OPTION_OUTPUT:
	case CLI_OPTION_DQT:
		break;
	}
	return NULL;
}

static enum fplinux_cli_result parse_options(int argc, char **argv,
					     struct options *options)
{
	struct fplinux_cli_option cli_options[] = {
		[CLI_OPTION_OPERATION] = {
			.name = "operation",
			.metavar = "encode|decode|scale",
			.help = "select the operation",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_INPUT] = {
			.name = "input",
			.metavar = "FILE",
			.help = "read the input image",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_OUTPUT] = {
			.name = "output",
			.metavar = "FILE",
			.help = "write the output image",
			.flags = FPLINUX_CLI_REQUIRED | FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_WIDTH] = {
			.name = "width",
			.metavar = "N",
			.help = "set positive raw input width for encode or scale",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_HEIGHT] = {
			.name = "height",
			.metavar = "N",
			.help = "set positive raw input height for encode or scale",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_SCALE] = {
			.name = "scale",
			.metavar = "1|2|4",
			.help = "set the divisor (default: decode 1, scale 2)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_QUALITY] = {
			.name = "quality",
			.metavar = "N",
			.help = "set encode quality in 1..100 (default: 75)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_RESTART] = {
			.name = "restart",
			.metavar = "N",
			.help = "set encode restart interval in 0..65535 (default: 0)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_REPEAT] = {
			.name = "repeat",
			.metavar = "N",
			.help = "repeat the operation 1..4096 times (default: 1)",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_DQT] = {
			.name = "dqt",
			.metavar = "FILE",
			.help = "read 128 encode quantization bytes in natural DCT order",
			.flags = FPLINUX_CLI_REPEAT,
		},
		[CLI_OPTION_DECODE_MODE] = {
			.name = "decode-mode",
			.metavar = "reduced|box",
			.help = "select decode scaling (default: reduced)",
			.flags = FPLINUX_CLI_REPEAT,
		},
	};
	struct fplinux_cli cli = {
		.program = argv[0],
		.description =
			"Encode NV16 to JPEG, decode JPEG to NV12/NV16 or scale NV16 on the CPU.\n"
			"Encode requires even raw width and a height. Scale accepts "
			"640x480 or 320x240 input with divisor 2.\n"
			"Quality, restart and DQT apply only to encode; decode-mode only to decode.",
		.options = cli_options,
		.option_count = sizeof(cli_options) / sizeof(cli_options[0]),
		.parse_option = parse_cli_option,
		.data = options,
	};
	enum fplinux_cli_result result;
	bool valid;

	memset(options, 0, sizeof(*options));
	options->decode_mode = DECODE_REDUCED;
	options->scale = 1;
	options->quality = 75;
	options->repeat = 1;
	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	options->input_path = cli_options[CLI_OPTION_INPUT].value;
	options->output_path = cli_options[CLI_OPTION_OUTPUT].value;
	options->dqt_path = cli_options[CLI_OPTION_DQT].value;
	options->width_set = cli_options[CLI_OPTION_WIDTH].count != 0;
	options->height_set = cli_options[CLI_OPTION_HEIGHT].count != 0;
	options->scale_set = cli_options[CLI_OPTION_SCALE].count != 0;
	options->quality_set = cli_options[CLI_OPTION_QUALITY].count != 0;
	options->restart_set = cli_options[CLI_OPTION_RESTART].count != 0;
	options->decode_mode_set = cli_options[CLI_OPTION_DECODE_MODE].count !=
				   0;
	if (options->operation == OP_ENCODE)
		valid = options->width_set && options->height_set &&
			!(options->width & 1U) && !options->scale_set &&
			!options->decode_mode_set;
	else if (options->operation == OP_SCALE) {
		if (!options->scale_set)
			options->scale = 2;
		valid = ((options->width == 640 && options->height == 480) ||
			 (options->width == 320 && options->height == 240)) &&
			options->scale == 2 && !options->decode_mode_set &&
			!options->dqt_path && !options->quality_set &&
			!options->restart_set;
	} else {
		valid = !options->width_set && !options->height_set &&
			!options->dqt_path && !options->quality_set &&
			!options->restart_set;
	}
	if (!valid)
		return fplinux_cli_error(
			&cli, "invalid options for the selected operation");
	return FPLINUX_CLI_READY;
}

static bool multiply_size(size_t left, size_t right, size_t *result)
{
	if (left && right > SIZE_MAX / left)
		return false;
	*result = left * right;
	return true;
}

static bool add_size(size_t left, size_t right, size_t *result)
{
	if (right > SIZE_MAX - left)
		return false;
	*result = left + right;
	return true;
}

static unsigned ceil_div(unsigned value, unsigned divisor)
{
	return value / divisor + (value % divisor != 0);
}

static bool read_file_bounded(const char *path, size_t maximum, uint8_t **data,
			      size_t *size, char *error, size_t error_size)
{
	struct stat status;
	int fd = -1;
	uint8_t *buffer = NULL;
	size_t offset = 0;

	*data = NULL;
	*size = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
	    status.st_size < 1 || (uintmax_t)status.st_size > maximum) {
		set_error(error, error_size,
			  "cannot read bounded regular input: %s", path);
		goto fail;
	}
	buffer = malloc((size_t)status.st_size);
	if (!buffer) {
		set_error(error, error_size, "out of memory reading: %s", path);
		goto fail;
	}
	while (offset < (size_t)status.st_size) {
		ssize_t count = read(fd, buffer + offset,
				     (size_t)status.st_size - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			set_error(error, error_size, "short read: %s", path);
			goto fail;
		}
		offset += (size_t)count;
	}
	if (close(fd) != 0) {
		fd = -1;
		set_error(error, error_size, "cannot close input: %s", path);
		goto fail;
	}
	*data = buffer;
	*size = offset;
	return true;
fail:
	if (fd >= 0)
		close(fd);
	free(buffer);
	return false;
}

static bool expected_nv16_size(unsigned width, unsigned height, size_t *size)
{
	size_t pixels;

	return multiply_size(width, height, &pixels) &&
	       multiply_size(pixels, 2, size) && *size <= MAX_RAW_BYTES;
}

static bool valid_dqt(const uint8_t *dqt)
{
	unsigned index;

	for (index = 0; index < DQT_BYTES; index++)
		if (!dqt[index])
			return false;
	return true;
}

static void jpeg_fatal(j_common_ptr cinfo)
{
	struct jpeg_error_state *error = (struct jpeg_error_state *)cinfo->err;

	(*cinfo->err->format_message)(cinfo, error->message);
	longjmp(error->jump, 1);
}

static void jpeg_warning(j_common_ptr cinfo, int level)
{
	if (level < 0)
		jpeg_fatal(cinfo);
}

static void free_raw_rows(struct raw_rows *rows)
{
	unsigned component;

	for (component = 0; component < COMPONENTS; component++) {
		free(rows->row_ptrs[component]);
		free(rows->samples[component]);
	}
	free(rows->rows);
	memset(rows, 0, sizeof(*rows));
}

static bool allocate_decode_rows(const struct jpeg_decompress_struct *decoder,
				 struct raw_rows *rows)
{
	unsigned component;

	rows->rows = calloc(COMPONENTS, sizeof(*rows->rows));
	if (!rows->rows)
		return false;
	for (component = 0; component < COMPONENTS; component++) {
		const jpeg_component_info *info =
			&decoder->comp_info[component];
		size_t samples;
		unsigned row;

		if (!info->DCT_h_scaled_size || !info->DCT_v_scaled_size ||
		    !info->v_samp_factor ||
		    !multiply_size(info->width_in_blocks,
				   (size_t)info->DCT_h_scaled_size,
				   &rows->row_width[component]))
			return false;
		rows->row_count[component] = (unsigned)info->v_samp_factor *
					     (unsigned)info->DCT_v_scaled_size;
		if (!rows->row_width[component] ||
		    !rows->row_count[component] ||
		    !multiply_size(rows->row_width[component],
				   rows->row_count[component], &samples))
			return false;
		rows->row_ptrs[component] =
			calloc(rows->row_count[component],
			       sizeof(*rows->row_ptrs[component]));
		rows->samples[component] = malloc(samples);
		if (!rows->row_ptrs[component] || !rows->samples[component])
			return false;
		rows->rows[component] = rows->row_ptrs[component];
		for (row = 0; row < rows->row_count[component]; row++)
			rows->row_ptrs[component][row] =
				rows->samples[component] +
				row * rows->row_width[component];
	}
	return true;
}

static bool allocate_encode_rows(const struct jpeg_compress_struct *encoder,
				 struct raw_rows *rows)
{
	unsigned component;

	rows->rows = calloc(COMPONENTS, sizeof(*rows->rows));
	if (!rows->rows)
		return false;
	for (component = 0; component < COMPONENTS; component++) {
		const jpeg_component_info *info =
			&encoder->comp_info[component];
		size_t samples;
		unsigned row;

		if (!info->v_samp_factor ||
		    !multiply_size(info->width_in_blocks, DCTSIZE,
				   &rows->row_width[component]))
			return false;
		rows->row_count[component] =
			(unsigned)info->v_samp_factor * DCTSIZE;
		if (!rows->row_width[component] ||
		    !rows->row_count[component] ||
		    !multiply_size(rows->row_width[component],
				   rows->row_count[component], &samples))
			return false;
		rows->row_ptrs[component] =
			calloc(rows->row_count[component],
			       sizeof(*rows->row_ptrs[component]));
		rows->samples[component] = malloc(samples);
		if (!rows->row_ptrs[component] || !rows->samples[component])
			return false;
		rows->rows[component] = rows->row_ptrs[component];
		for (row = 0; row < rows->row_count[component]; row++)
			rows->row_ptrs[component][row] =
				rows->samples[component] +
				row * rows->row_width[component];
	}
	return true;
}

static void free_raw_image(struct raw_image *image)
{
	unsigned component;

	for (component = 0; component < COMPONENTS; component++)
		free(image->plane[component]);
	memset(image, 0, sizeof(*image));
}

static bool allocate_raw_image(const struct jpeg_decompress_struct *decoder,
			       unsigned vsub, struct raw_image *image)
{
	size_t total = 0;
	unsigned component;

	memset(image, 0, sizeof(*image));
	image->vsub = vsub;
	for (component = 0; component < COMPONENTS; component++) {
		size_t bytes;

		image->width[component] =
			decoder->comp_info[component].downsampled_width;
		image->height[component] =
			decoder->comp_info[component].downsampled_height;
		if (!image->width[component] || !image->height[component] ||
		    !multiply_size(image->width[component],
				   image->height[component], &bytes) ||
		    !add_size(total, bytes, &total) || total > MAX_RAW_BYTES)
			goto fail;
		image->plane[component] = malloc(bytes);
		if (!image->plane[component])
			goto fail;
	}
	return true;
fail:
	free_raw_image(image);
	return false;
}

static void free_nv_frame(struct nv_frame *frame)
{
	free(frame->data);
	memset(frame, 0, sizeof(*frame));
}

static bool allocate_nv_frame(unsigned width, unsigned height, unsigned vsub,
			      struct nv_frame *frame)
{
	size_t y_size, chroma_size, uv_size, total;

	memset(frame, 0, sizeof(*frame));
	if (!width || !height || (vsub != 1 && vsub != 2))
		return false;
	frame->width = width;
	frame->height = height;
	frame->vsub = vsub;
	frame->chroma_width = ceil_div(width, 2);
	frame->chroma_height = ceil_div(height, vsub);
	if (!multiply_size(width, height, &y_size) ||
	    !multiply_size(frame->chroma_width, frame->chroma_height,
			   &chroma_size) ||
	    !multiply_size(chroma_size, 2, &uv_size) ||
	    !add_size(y_size, uv_size, &total) || total > MAX_RAW_BYTES)
		return false;
	frame->data = malloc(total);
	if (!frame->data)
		return false;
	frame->size = total;
	return true;
}

static bool supported_header(const struct jpeg_decompress_struct *decoder,
			     unsigned *vsub)
{
	const jpeg_component_info *y = &decoder->comp_info[0];
	const jpeg_component_info *cb = &decoder->comp_info[1];
	const jpeg_component_info *cr = &decoder->comp_info[2];

	if (decoder->jpeg_color_space != JCS_YCbCr ||
	    decoder->num_components != COMPONENTS ||
	    decoder->progressive_mode || decoder->data_precision != 8 ||
	    y->h_samp_factor != 2 || cb->h_samp_factor != 1 ||
	    cr->h_samp_factor != 1 || cb->v_samp_factor != 1 ||
	    cr->v_samp_factor != 1 ||
	    (y->v_samp_factor != 1 && y->v_samp_factor != 2))
		return false;
	*vsub = (unsigned)y->v_samp_factor;
	return true;
}

static bool decode_once(const uint8_t *input, size_t input_size, unsigned scale,
			struct raw_image *image, char *error, size_t error_size)
{
	struct decode_operation *operation = calloc(1, sizeof(*operation));
	bool ok;

	memset(image, 0, sizeof(*image));
	if (!operation) {
		set_error(error, error_size,
			  "cannot allocate JPEG decode operation");
		return false;
	}
	operation->decoder.err = jpeg_std_error(&operation->error.pub);
	operation->error.pub.error_exit = jpeg_fatal;
	operation->error.pub.emit_message = jpeg_warning;
	if (setjmp(operation->error.jump)) {
		set_error(error, error_size, "libjpeg decode: %s",
			  operation->error.message);
		goto done;
	}
	jpeg_create_decompress(&operation->decoder);
	operation->created = true;
	jpeg_mem_src(&operation->decoder, input, (unsigned long)input_size);
	if (jpeg_read_header(&operation->decoder, TRUE) != JPEG_HEADER_OK ||
	    !supported_header(&operation->decoder, &operation->vsub)) {
		set_error(
			error, error_size,
			"input is not baseline 8-bit YCbCr 4:2:0 or 4:2:2 JPEG");
		goto done;
	}
	operation->decoder.raw_data_out = TRUE;
	operation->decoder.out_color_space = JCS_YCbCr;
	operation->decoder.dct_method = JDCT_ISLOW;
	operation->decoder.do_fancy_upsampling = FALSE;
	operation->decoder.do_block_smoothing = FALSE;
	operation->decoder.scale_num = 1;
	operation->decoder.scale_denom = scale;
	if (!jpeg_start_decompress(&operation->decoder) ||
	    operation->decoder.comp_info[0].downsampled_width !=
		    operation->decoder.output_width ||
	    operation->decoder.comp_info[0].downsampled_height !=
		    operation->decoder.output_height ||
	    operation->decoder.comp_info[1].downsampled_width !=
		    operation->decoder.comp_info[2].downsampled_width ||
	    operation->decoder.comp_info[1].downsampled_height !=
		    operation->decoder.comp_info[2].downsampled_height ||
	    !allocate_raw_image(&operation->decoder, operation->vsub,
				&operation->image) ||
	    !allocate_decode_rows(&operation->decoder, &operation->rows)) {
		set_error(error, error_size,
			  "cannot allocate bounded raw decode buffers");
		goto done;
	}
	while (operation->decoder.output_scanline <
	       operation->decoder.output_height) {
		unsigned component;
		JDIMENSION lines =
			(JDIMENSION)(operation->decoder.max_v_samp_factor *
				     operation->decoder.min_DCT_v_scaled_size);

		if (!lines ||
		    !jpeg_read_raw_data(&operation->decoder,
					operation->rows.rows, lines)) {
			set_error(error, error_size,
				  "incomplete raw JPEG decode");
			goto done;
		}
		for (component = 0; component < COMPONENTS; component++) {
			unsigned row_count =
				operation->rows.row_count[component];
			unsigned valid;
			unsigned row;

			if (operation->produced[component] >
			    operation->image.height[component]) {
				set_error(error, error_size,
					  "raw component row overrun");
				goto done;
			}
			valid = operation->image.height[component] -
				operation->produced[component];
			if (row_count > valid)
				row_count = valid;
			for (row = 0; row < row_count; row++)
				memcpy(operation->image.plane[component] +
					       (size_t)(operation->produced
								[component] +
							row) *
						       operation->image
							       .width[component],
				       operation->rows.rows[component][row],
				       operation->image.width[component]);
			operation->produced[component] += row_count;
		}
	}
	if (operation->produced[0] != operation->image.height[0] ||
	    operation->produced[1] != operation->image.height[1] ||
	    operation->produced[2] != operation->image.height[2] ||
	    !jpeg_finish_decompress(&operation->decoder)) {
		set_error(error, error_size,
			  "incomplete decoded component plane");
		goto done;
	}
	operation->ok = true;
done:
	free_raw_rows(&operation->rows);
	if (operation->created)
		jpeg_destroy_decompress(&operation->decoder);
	if (operation->ok) {
		*image = operation->image;
		memset(&operation->image, 0, sizeof(operation->image));
	} else {
		free_raw_image(&operation->image);
	}
	ok = operation->ok;
	free(operation);
	return ok;
}

static void nearest_plane(const uint8_t *source, unsigned source_width,
			  unsigned source_height, uint8_t *destination,
			  unsigned destination_width,
			  unsigned destination_height)
{
	unsigned y;

	for (y = 0; y < destination_height; y++) {
		unsigned source_y = (unsigned)(((uintmax_t)y * source_height) /
					       destination_height);
		unsigned x;

		for (x = 0; x < destination_width; x++) {
			unsigned source_x =
				(unsigned)(((uintmax_t)x * source_width) /
					   destination_width);

			destination[(size_t)y * destination_width + x] =
				source[(size_t)source_y * source_width +
				       source_x];
		}
	}
}

static void copy_or_nearest_plane(const uint8_t *source, unsigned source_width,
				  unsigned source_height, uint8_t *destination,
				  unsigned destination_width,
				  unsigned destination_height)
{
	size_t size;

	if (source_width == destination_width &&
	    source_height == destination_height &&
	    multiply_size(source_width, source_height, &size)) {
		memcpy(destination, source, size);
		return;
	}
	nearest_plane(source, source_width, source_height, destination,
		      destination_width, destination_height);
}

static void box_plane(const uint8_t *source, unsigned source_width,
		      unsigned source_height, uint8_t *destination,
		      unsigned destination_width, unsigned destination_height,
		      unsigned scale)
{
	unsigned y;

	for (y = 0; y < destination_height; y++) {
		unsigned top = y * scale;
		unsigned bottom = top + scale;
		unsigned x;

		if (bottom > source_height)
			bottom = source_height;
		for (x = 0; x < destination_width; x++) {
			unsigned left = x * scale;
			unsigned right = left + scale;
			unsigned sum = 0;
			unsigned count = 0;
			unsigned source_y;

			if (right > source_width)
				right = source_width;
			for (source_y = top; source_y < bottom; source_y++) {
				unsigned source_x;

				for (source_x = left; source_x < right;
				     source_x++) {
					sum += source[(size_t)source_y *
							      source_width +
						      source_x];
					count++;
				}
			}
			destination[(size_t)y * destination_width + x] =
				(uint8_t)((sum + count / 2) / count);
		}
	}
}

static void interleave_uv(const uint8_t *cb, const uint8_t *cr, unsigned width,
			  unsigned height, uint8_t *destination)
{
	unsigned y;

	for (y = 0; y < height; y++) {
		unsigned x;

		for (x = 0; x < width; x++) {
			destination[2 * ((size_t)y * width + x)] =
				cb[(size_t)y * width + x];
			destination[2 * ((size_t)y * width + x) + 1] =
				cr[(size_t)y * width + x];
		}
	}
}

static bool make_reduced_nv(const struct raw_image *image,
			    struct nv_frame *frame)
{
	uint8_t *uv;
	uint8_t *cb = NULL;
	uint8_t *cr = NULL;
	size_t chroma_size;
	bool ok = false;

	if (!allocate_nv_frame(image->width[0], image->height[0], image->vsub,
			       frame) ||
	    !multiply_size(frame->chroma_width, frame->chroma_height,
			   &chroma_size) ||
	    !chroma_size)
		return false;
	cb = malloc(chroma_size);
	cr = malloc(chroma_size);
	if (!cb || !cr)
		goto done;
	copy_or_nearest_plane(image->plane[0], image->width[0],
			      image->height[0], frame->data, frame->width,
			      frame->height);
	copy_or_nearest_plane(image->plane[1], image->width[1],
			      image->height[1], cb, frame->chroma_width,
			      frame->chroma_height);
	copy_or_nearest_plane(image->plane[2], image->width[2],
			      image->height[2], cr, frame->chroma_width,
			      frame->chroma_height);
	uv = frame->data + (size_t)frame->width * frame->height;
	interleave_uv(cb, cr, frame->chroma_width, frame->chroma_height, uv);
	ok = true;
done:
	free(cb);
	free(cr);
	if (!ok)
		free_nv_frame(frame);
	return ok;
}

static bool make_box_nv(const struct raw_image *image, unsigned scale,
			struct nv_frame *frame)
{
	uint8_t *uv;
	uint8_t *cb = NULL;
	uint8_t *cr = NULL;
	size_t chroma_size;
	unsigned width = ceil_div(image->width[0], scale);
	unsigned height = ceil_div(image->height[0], scale);
	bool ok = false;

	if (!allocate_nv_frame(width, height, image->vsub, frame) ||
	    frame->chroma_width != ceil_div(image->width[1], scale) ||
	    frame->chroma_height != ceil_div(image->height[1], scale) ||
	    !multiply_size(frame->chroma_width, frame->chroma_height,
			   &chroma_size) ||
	    !chroma_size)
		return false;
	cb = malloc(chroma_size);
	cr = malloc(chroma_size);
	if (!cb || !cr)
		goto done;
	box_plane(image->plane[0], image->width[0], image->height[0],
		  frame->data, frame->width, frame->height, scale);
	box_plane(image->plane[1], image->width[1], image->height[1], cb,
		  frame->chroma_width, frame->chroma_height, scale);
	box_plane(image->plane[2], image->width[2], image->height[2], cr,
		  frame->chroma_width, frame->chroma_height, scale);
	uv = frame->data + (size_t)frame->width * frame->height;
	interleave_uv(cb, cr, frame->chroma_width, frame->chroma_height, uv);
	ok = true;
done:
	free(cb);
	free(cr);
	if (!ok)
		free_nv_frame(frame);
	return ok;
}

static bool same_nv_frame(const struct nv_frame *left,
			  const struct nv_frame *right)
{
	return left->width == right->width && left->height == right->height &&
	       left->chroma_width == right->chroma_width &&
	       left->chroma_height == right->chroma_height &&
	       left->vsub == right->vsub && left->size == right->size &&
	       memcmp(left->data, right->data, left->size) == 0;
}

static bool write_file(const char *path, const uint8_t *data, size_t size,
		       char *error, size_t error_size)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	size_t offset = 0;

	if (fd < 0) {
		set_error(error, error_size, "cannot open output: %s", path);
		return false;
	}
	while (offset < size) {
		ssize_t count = write(fd, data + offset, size - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			set_error(error, error_size, "short write: %s", path);
			close(fd);
			return false;
		}
		offset += (size_t)count;
	}
	if (close(fd) != 0) {
		set_error(error, error_size, "cannot close output: %s", path);
		return false;
	}
	return true;
}

static bool compare_temp_files(FILE *left, size_t left_size, FILE *right,
			       size_t right_size)
{
	uint8_t left_buffer[IO_CHUNK];
	uint8_t right_buffer[IO_CHUNK];
	size_t remaining = left_size;

	if (left_size != right_size || fseek(left, 0, SEEK_SET) != 0 ||
	    fseek(right, 0, SEEK_SET) != 0)
		return false;
	while (remaining) {
		size_t chunk = remaining > sizeof(left_buffer) ?
				       sizeof(left_buffer) :
				       remaining;

		if (fread(left_buffer, 1, chunk, left) != chunk ||
		    fread(right_buffer, 1, chunk, right) != chunk ||
		    memcmp(left_buffer, right_buffer, chunk) != 0)
			return false;
		remaining -= chunk;
	}
	return true;
}

static bool write_temp_file(FILE *source, size_t size, const char *path,
			    char *error, size_t error_size)
{
	uint8_t buffer[IO_CHUNK];
	int fd;
	size_t remaining = size;

	if (fseek(source, 0, SEEK_SET) != 0 ||
	    (fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666)) <
		    0) {
		set_error(error, error_size, "cannot prepare output: %s", path);
		return false;
	}
	while (remaining) {
		size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) :
							    remaining;
		size_t read_count = fread(buffer, 1, chunk, source);
		size_t offset = 0;

		if (read_count != chunk) {
			set_error(error, error_size,
				  "short read from encoded temporary output");
			close(fd);
			return false;
		}
		while (offset < chunk) {
			ssize_t written =
				write(fd, buffer + offset, chunk - offset);

			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0) {
				set_error(error, error_size, "short write: %s",
					  path);
				close(fd);
				return false;
			}
			offset += (size_t)written;
		}
		remaining -= chunk;
	}
	if (close(fd) != 0) {
		set_error(error, error_size, "cannot close output: %s", path);
		return false;
	}
	return true;
}

static void set_exact_dqt(struct jpeg_compress_struct *encoder,
			  const uint8_t *dqt)
{
	unsigned index;

	for (index = 0; index < DCTSIZE2; index++) {
		encoder->quant_tbl_ptrs[0]->quantval[index] = dqt[index];
		encoder->quant_tbl_ptrs[1]->quantval[index] =
			dqt[DCTSIZE2 + index];
	}
	encoder->quant_tbl_ptrs[0]->sent_table = FALSE;
	encoder->quant_tbl_ptrs[1]->sent_table = FALSE;
}

static bool fill_encode_rows(const struct raw_rows *rows, const uint8_t *input,
			     unsigned width, unsigned height, unsigned top)
{
	size_t y_size = (size_t)width * height;
	unsigned chroma_width = width / 2;
	unsigned component;

	if (rows->row_count[0] != DCTSIZE || rows->row_count[1] != DCTSIZE ||
	    rows->row_count[2] != DCTSIZE || rows->row_width[0] < width ||
	    rows->row_width[1] < chroma_width ||
	    rows->row_width[2] < chroma_width)
		return false;
	for (component = 0; component < COMPONENTS; component++) {
		unsigned row;

		for (row = 0; row < DCTSIZE; row++) {
			unsigned source_y = top + row;
			uint8_t *destination = rows->rows[component][row];

			if (source_y >= height)
				source_y = height - 1;
			if (!component) {
				memcpy(destination,
				       input + (size_t)source_y * width, width);
				memset(destination + width,
				       destination[width - 1],
				       rows->row_width[0] - width);
			} else {
				unsigned column;
				const uint8_t *uv = input + y_size +
						    (size_t)source_y * width;

				for (column = 0; column < chroma_width;
				     column++)
					destination[column] =
						uv[2 * column + component - 1];
				memset(destination + chroma_width,
				       destination[chroma_width - 1],
				       rows->row_width[component] -
					       chroma_width);
			}
		}
	}
	return true;
}

static bool encode_once(const struct options *options, const uint8_t *input,
			const uint8_t *dqt, FILE **result, size_t *result_size,
			struct metric *copy_metric, struct metric *codec_metric,
			char *error, size_t error_size)
{
	struct encode_operation *operation = calloc(1, sizeof(*operation));
	bool ok;

	*result = NULL;
	*result_size = 0;
	if (!operation) {
		set_error(error, error_size,
			  "cannot allocate JPEG encode operation");
		return false;
	}
	operation->encoder.err = jpeg_std_error(&operation->error.pub);
	operation->error.pub.error_exit = jpeg_fatal;
	operation->error.pub.emit_message = jpeg_warning;
	if (setjmp(operation->error.jump)) {
		set_error(error, error_size, "libjpeg encode: %s",
			  operation->error.message);
		goto done;
	}
	operation->file = tmpfile();
	if (!operation->file) {
		set_error(error, error_size,
			  "cannot create bounded temporary encoded output");
		goto done;
	}
	if (!metric_start(&operation->start)) {
		set_error(error, error_size, "cannot start CPU timing");
		goto done;
	}
	jpeg_create_compress(&operation->encoder);
	operation->created = true;
	jpeg_stdio_dest(&operation->encoder, operation->file);
	operation->encoder.image_width = options->width;
	operation->encoder.image_height = options->height;
	operation->encoder.input_components = COMPONENTS;
	operation->encoder.in_color_space = JCS_YCbCr;
	jpeg_set_defaults(&operation->encoder);
	jpeg_set_colorspace(&operation->encoder, JCS_YCbCr);
	operation->encoder.comp_info[0].h_samp_factor = 2;
	operation->encoder.comp_info[0].v_samp_factor = 1;
	operation->encoder.comp_info[1].h_samp_factor = 1;
	operation->encoder.comp_info[1].v_samp_factor = 1;
	operation->encoder.comp_info[2].h_samp_factor = 1;
	operation->encoder.comp_info[2].v_samp_factor = 1;
	jpeg_set_quality(&operation->encoder, (int)options->quality, TRUE);
	if (dqt)
		set_exact_dqt(&operation->encoder, dqt);
	operation->encoder.raw_data_in = TRUE;
	operation->encoder.dct_method = JDCT_ISLOW;
	operation->encoder.optimize_coding = FALSE;
	operation->encoder.arith_code = FALSE;
	operation->encoder.restart_interval = options->restart;
	operation->encoder.restart_in_rows = 0;
	jpeg_start_compress(&operation->encoder, TRUE);
	if (!allocate_encode_rows(&operation->encoder, &operation->rows) ||
	    !metric_stop(codec_metric, &operation->start)) {
		set_error(error, error_size,
			  "cannot allocate or time raw encode rows");
		goto done;
	}
	for (unsigned top = 0; top < options->height; top += DCTSIZE) {
		JDIMENSION lines =
			(JDIMENSION)(operation->encoder.max_v_samp_factor *
				     DCTSIZE);

		if (!metric_start(&operation->start) ||
		    !fill_encode_rows(&operation->rows, input, options->width,
				      options->height, top) ||
		    !metric_stop(copy_metric, &operation->start)) {
			set_error(error, error_size,
				  "cannot prepare NV16 raw encode rows");
			goto done;
		}
		if (!metric_start(&operation->start) ||
		    jpeg_write_raw_data(&operation->encoder,
					operation->rows.rows, lines) != lines ||
		    !metric_stop(codec_metric, &operation->start)) {
			set_error(error, error_size,
				  "incomplete raw JPEG encode");
			goto done;
		}
	}
	if (!metric_start(&operation->start)) {
		set_error(error, error_size, "cannot finish JPEG encode");
		goto done;
	}
	jpeg_finish_compress(&operation->encoder);
	if (!metric_stop(codec_metric, &operation->start)) {
		set_error(error, error_size, "cannot time JPEG encode finish");
		goto done;
	}
	if (fflush(operation->file) != 0 ||
	    fseek(operation->file, 0, SEEK_END) != 0) {
		set_error(error, error_size,
			  "cannot size temporary encoded output");
		goto done;
	}
	{
		long length = ftell(operation->file);

		if (length < 1 || (uintmax_t)length > MAX_ENCODED_BYTES) {
			set_error(error, error_size,
				  "encoded output exceeds %u-byte bound",
				  (unsigned)MAX_ENCODED_BYTES);
			goto done;
		}
		operation->result_size = (size_t)length;
	}
	operation->ok = true;
done:
	free_raw_rows(&operation->rows);
	if (operation->created)
		jpeg_destroy_compress(&operation->encoder);
	if (operation->ok) {
		*result = operation->file;
		*result_size = operation->result_size;
		operation->file = NULL;
	}
	if (operation->file)
		fclose(operation->file);
	ok = operation->ok;
	free(operation);
	return ok;
}

static bool run_encode(const struct options *options, const uint8_t *input,
		       size_t input_size, const uint8_t *dqt,
		       const struct metric *input_loaded, char *error,
		       size_t error_size)
{
	struct metric input_metric = { 0 }, copy_metric = { 0 },
		      codec_metric = { 0 };
	struct metric batch_metric = { 0 }, output_metric = { 0 },
		      measured_stages_metric = { 0 };
	struct metric_start start;
	FILE *reference = NULL;
	size_t reference_size = 0;
	unsigned iteration = 0;
	bool ok = false;

	input_metric = *input_loaded;
	if (!metric_start(&start)) {
		set_error(error, error_size, "cannot start batch timing");
		return false;
	}
	do {
		FILE *current = NULL;
		size_t current_size = 0;

		if (!encode_once(options, input, dqt, &current, &current_size,
				 &copy_metric, &codec_metric, error,
				 error_size))
			goto done;
		if (!reference) {
			reference = current;
			reference_size = current_size;
		} else {
			bool same = compare_temp_files(reference,
						       reference_size, current,
						       current_size);

			fclose(current);
			if (!same) {
				set_error(
					error, error_size,
					"non-deterministic JPEG output across repeats");
				goto done;
			}
		}
	} while (++iteration < options->repeat);
	if (!metric_stop(&batch_metric, &start) || !metric_start(&start) ||
	    !write_temp_file(reference, reference_size, options->output_path,
			     error, error_size) ||
	    !metric_stop(&output_metric, &start))
		goto done;
	measured_stages_metric = add_metrics(&input_metric, &batch_metric);
	measured_stages_metric =
		add_metrics(&measured_stages_metric, &output_metric);
	printf("operation=encode input_format=NV16 output_format=JPEG sampling=4:2:2 "
	       "quality=%u restart_interval=%u dqt=%s huffman=default dct=ISLOW\n",
	       options->quality, options->restart,
	       dqt ? "exact-user-128-byte" : "quality-default");
	printf("iterations=%u input_bytes=%zu output_bytes=%zu width=%u height=%u "
	       "deterministic_repeats=exact\n",
	       options->repeat, input_size, reference_size, options->width,
	       options->height);
	print_metric("input_load", &input_metric);
	print_metric("encode_row_copy", &copy_metric);
	print_metric("encode_codec", &codec_metric);
	print_metric("encode_batch", &batch_metric);
	print_metric("output_write", &output_metric);
	print_metric("measured_stages", &measured_stages_metric);
	ok = true;
done:
	if (reference)
		fclose(reference);
	return ok;
}

static bool run_decode(const struct options *options, const uint8_t *input,
		       size_t input_size, const struct metric *input_loaded,
		       char *error, size_t error_size)
{
	struct metric input_metric = { 0 }, codec_metric = { 0 },
		      convert_metric = { 0 };
	struct metric batch_metric = { 0 }, output_metric = { 0 },
		      measured_stages_metric = { 0 };
	struct metric_start start, batch_start;
	struct nv_frame reference = { 0 };
	bool ok = false;

	input_metric = *input_loaded;
	if (!metric_start(&batch_start)) {
		set_error(error, error_size,
			  "cannot start decode batch timing");
		return false;
	}
	for (unsigned iteration = 0; iteration < options->repeat; iteration++) {
		struct raw_image raw = { 0 };
		struct nv_frame current = { 0 };

		if (!metric_start(&start) ||
		    !decode_once(input, input_size,
				 options->decode_mode == DECODE_BOX ?
					 1 :
					 options->scale,
				 &raw, error, error_size) ||
		    !metric_stop(&codec_metric, &start)) {
			free_raw_image(&raw);
			goto done;
		}
		if (!metric_start(&start) ||
		    !(options->decode_mode == DECODE_BOX ?
			      make_box_nv(&raw, options->scale, &current) :
			      make_reduced_nv(&raw, &current)) ||
		    !metric_stop(&convert_metric, &start)) {
			free_raw_image(&raw);
			free_nv_frame(&current);
			set_error(error, error_size,
				  "cannot allocate bounded NV output");
			goto done;
		}
		free_raw_image(&raw);
		if (!reference.data) {
			reference = current;
			memset(&current, 0, sizeof(current));
		} else if (!same_nv_frame(&reference, &current)) {
			free_nv_frame(&current);
			set_error(
				error, error_size,
				"non-deterministic raw output across repeats");
			goto done;
		}
		free_nv_frame(&current);
	}
	if (!metric_stop(&batch_metric, &batch_start) ||
	    !metric_start(&start) ||
	    !write_file(options->output_path, reference.data, reference.size,
			error, error_size) ||
	    !metric_stop(&output_metric, &start))
		goto done;
	measured_stages_metric = add_metrics(&input_metric, &batch_metric);
	measured_stages_metric =
		add_metrics(&measured_stages_metric, &output_metric);
	printf("operation=decode input_format=JPEG output_format=%s decode_mode=%s scale=%u "
	       "dct=ISLOW fancy_upsampling=off block_smoothing=off\n",
	       reference.vsub == 2 ? "NV12" : "NV16",
	       options->decode_mode == DECODE_BOX ? "full-idct-plus-box" :
						    "reduced-idct-plus-nv-pack",
	       options->scale);
	printf("iterations=%u input_bytes=%zu output_bytes=%zu width=%u height=%u chroma_width=%u "
	       "chroma_height=%u deterministic_repeats=exact\n",
	       options->repeat, input_size, reference.size, reference.width,
	       reference.height, reference.chroma_width,
	       reference.chroma_height);
	print_metric("input_load", &input_metric);
	print_metric("decode_codec", &codec_metric);
	print_metric("decode_layout_convert", &convert_metric);
	print_metric("decode_batch", &batch_metric);
	print_metric("output_write", &output_metric);
	print_metric("measured_stages", &measured_stages_metric);
	ok = true;
done:
	free_nv_frame(&reference);
	return ok;
}

static void scale_nv16_box2(const uint8_t *source, uint8_t *destination,
			    unsigned source_width, unsigned source_height)
{
	unsigned destination_width = source_width / 2;
	unsigned destination_height = source_height / 2;
	unsigned plane;

	for (plane = 0; plane < 2; plane++) {
		unsigned step = plane ? 2 : 1;
		const uint8_t *input =
			source + (size_t)plane * source_width * source_height;
		uint8_t *output = destination + (size_t)plane *
							destination_width *
							destination_height;
		unsigned y;

		for (y = 0; y < destination_height; y++) {
			const uint8_t *row =
				input + (size_t)y * 2 * source_width;
			unsigned x;

			for (x = 0; x < destination_width; x++) {
				unsigned column =
					(x / step) * (2 * step) + x % step;
				unsigned sum =
					row[column] + row[column + step] +
					row[source_width + column] +
					row[source_width + column + step];

				output[(size_t)y * destination_width + x] =
					(uint8_t)((sum + 2) >> 2);
			}
		}
	}
}

static bool run_scale(const struct options *options, const uint8_t *input,
		      size_t input_size, const struct metric *input_metric,
		      char *error, size_t error_size)
{
	unsigned destination_width = options->width / 2;
	unsigned destination_height = options->height / 2;
	struct metric filter_metric = { 0 }, batch_metric = { 0 };
	struct metric output_metric = { 0 }, measured_stages_metric;
	struct metric_start start, batch_start;
	struct nv_frame reference = { 0 };
	bool ok = false;

	if (!metric_start(&batch_start))
		return false;
	for (unsigned iteration = 0; iteration < options->repeat; iteration++) {
		struct nv_frame current = { 0 };

		if (!allocate_nv_frame(destination_width, destination_height, 1,
				       &current) ||
		    !metric_start(&start)) {
			free_nv_frame(&current);
			goto done;
		}
		scale_nv16_box2(input, current.data, options->width,
				options->height);
		if (!metric_stop(&filter_metric, &start)) {
			free_nv_frame(&current);
			goto done;
		}
		if (!reference.data) {
			reference = current;
			memset(&current, 0, sizeof(current));
		} else if (!same_nv_frame(&reference, &current)) {
			free_nv_frame(&current);
			set_error(error, error_size,
				  "non-deterministic scale output");
			goto done;
		}
		free_nv_frame(&current);
	}
	if (!metric_stop(&batch_metric, &batch_start) ||
	    !metric_start(&start) ||
	    !write_file(options->output_path, reference.data, reference.size,
			error, error_size) ||
	    !metric_stop(&output_metric, &start))
		goto done;
	measured_stages_metric = add_metrics(input_metric, &batch_metric);
	measured_stages_metric =
		add_metrics(&measured_stages_metric, &output_metric);
	printf("operation=scale input_format=NV16 output_format=NV16 filter=box2 "
	       "rounding=nearest-up input_width=%u input_height=%u width=%u height=%u\n",
	       options->width, options->height, destination_width,
	       destination_height);
	printf("iterations=%u input_bytes=%zu output_bytes=%zu deterministic_repeats=exact\n",
	       options->repeat, input_size, reference.size);
	print_metric("input_load", input_metric);
	print_metric("scale_filter", &filter_metric);
	print_metric("scale_batch", &batch_metric);
	print_metric("output_write", &output_metric);
	print_metric("measured_stages", &measured_stages_metric);
	ok = true;
done:
	free_nv_frame(&reference);
	return ok;
}

int main(int argc, char **argv)
{
	struct options options;
	struct metric input_metric = { 0 };
	struct metric_start start;
	uint8_t *input = NULL;
	uint8_t *dqt = NULL;
	size_t input_size = 0;
	size_t dqt_size = 0;
	size_t expected_size = 0;
	char error[256] = { 0 };
	bool ok;
	enum fplinux_cli_result parse_result;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != FPLINUX_CLI_READY)
		return parse_result;
	if (!metric_start(&start) ||
	    !read_file_bounded(options.input_path, MAX_RAW_BYTES, &input,
			       &input_size, error, sizeof(error)) ||
	    !metric_stop(&input_metric, &start)) {
		fprintf(stderr, "%s\n", error[0] ? error : "cannot read input");
		free(input);
		return EXIT_FAILURE;
	}
	if (options.operation != OP_DECODE &&
	    (!expected_nv16_size(options.width, options.height,
				 &expected_size) ||
	     input_size != expected_size)) {
		fprintf(stderr,
			"NV16 input must be exactly %zu tight Y+UV bytes\n",
			expected_size);
		free(input);
		return EXIT_FAILURE;
	}
	if (options.dqt_path &&
	    (!read_file_bounded(options.dqt_path, DQT_BYTES, &dqt, &dqt_size,
				error, sizeof(error)) ||
	     dqt_size != DQT_BYTES || !valid_dqt(dqt))) {
		fprintf(stderr, "%s\n",
			error[0] ?
				error :
				"DQT must contain exactly 128 nonzero bytes");
		free(input);
		free(dqt);
		return EXIT_FAILURE;
	}
	if (options.operation == OP_ENCODE)
		ok = run_encode(&options, input, input_size, dqt, &input_metric,
				error, sizeof(error));
	else if (options.operation == OP_SCALE)
		ok = run_scale(&options, input, input_size, &input_metric,
			       error, sizeof(error));
	else
		ok = run_decode(&options, input, input_size, &input_metric,
				error, sizeof(error));
	if (!ok)
		fprintf(stderr, "%s\n",
			error[0] ? error : "JPEG operation failed");
	free(input);
	free(dqt);
	if (ok) {
		struct rusage usage;

		if (getrusage(RUSAGE_SELF, &usage) != 0)
			return EXIT_FAILURE;
		printf("memory_peak_rss_kib=%ld minor_faults=%ld major_faults=%ld\n",
		       usage.ru_maxrss, usage.ru_minflt, usage.ru_majflt);
	}
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
