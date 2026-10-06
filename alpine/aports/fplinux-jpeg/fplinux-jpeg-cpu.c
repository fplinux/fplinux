/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CPU reference for the FPLinux JPEG comparison boundary.
 *
 * This is deliberately separate from the V4L2 hardware CLI.  It has no
 * device path and never selects a hardware fallback.
 */
#define _POSIX_C_SOURCE 200809L

#include "fplinux-cli.h"
#include "jpeg-cpu-codec.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	IO_CHUNK = 8192,
	MAX_REPEAT = 4096,
};

enum operation { OP_ENCODE, OP_DECODE, OP_SCALE };

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
	enum fplinux_jpeg_cpu_decode_mode decode_mode;
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

static void set_error(char *error, size_t error_size, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(error, error_size, format, arguments);
	va_end(arguments);
}

static void print_metric(const char *name,
			 const struct fplinux_jpeg_cpu_metric *value)
{
	printf("%s_wall_ms=%.3f %s_user_ms=%.3f %s_system_ms=%.3f %s_cpu_ms=%.3f\n",
	       name, 1000.0 * value->wall, name, 1000.0 * value->user, name,
	       1000.0 * value->system, name,
	       1000.0 * (value->user + value->system));
}

static struct fplinux_jpeg_cpu_metric
add_metrics(const struct fplinux_jpeg_cpu_metric *left,
	    const struct fplinux_jpeg_cpu_metric *right)
{
	return (struct fplinux_jpeg_cpu_metric){
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
			options->decode_mode = FPLINUX_JPEG_CPU_DECODE_REDUCED;
		else if (!strcmp(value, "box"))
			options->decode_mode = FPLINUX_JPEG_CPU_DECODE_BOX;
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
	options->decode_mode = FPLINUX_JPEG_CPU_DECODE_REDUCED;
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
	       multiply_size(pixels, 2, size) &&
	       *size <= FPLINUX_JPEG_CPU_MAX_RAW_BYTES;
}

static bool valid_dqt(const uint8_t *dqt)
{
	unsigned index;

	for (index = 0; index < FPLINUX_JPEG_CPU_DQT_BYTES; index++)
		if (!dqt[index])
			return false;
	return true;
}

static bool same_nv_frame(const struct fplinux_jpeg_cpu_frame *left,
			  const struct fplinux_jpeg_cpu_frame *right)
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

static bool run_encode(const struct options *options, const uint8_t *input,
		       size_t input_size, const uint8_t *dqt,
		       const struct fplinux_jpeg_cpu_metric *input_loaded,
		       char *error, size_t error_size)
{
	struct fplinux_jpeg_cpu_metric input_metric = { 0 },
				       copy_metric = { 0 },
				       codec_metric = { 0 };
	struct fplinux_jpeg_cpu_metric batch_metric = { 0 },
				       output_metric = { 0 },
				       measured_stages_metric = { 0 };
	struct fplinux_jpeg_cpu_metric_start start;
	const struct fplinux_jpeg_cpu_encode_request request = {
		.width = options->width,
		.height = options->height,
		.quality = options->quality,
		.restart = options->restart,
	};
	FILE *reference = NULL;
	size_t reference_size = 0;
	unsigned iteration = 0;
	bool ok = false;

	input_metric = *input_loaded;
	if (!fplinux_jpeg_cpu_metric_start(&start)) {
		set_error(error, error_size, "cannot start batch timing");
		return false;
	}
	do {
		FILE *current = NULL;
		size_t current_size = 0;

		if (!fplinux_jpeg_cpu_encode(&request, input, dqt, &current,
					     &current_size, &copy_metric,
					     &codec_metric, error, error_size))
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
	if (!fplinux_jpeg_cpu_metric_stop(&batch_metric, &start) ||
	    !fplinux_jpeg_cpu_metric_start(&start) ||
	    !write_temp_file(reference, reference_size, options->output_path,
			     error, error_size) ||
	    !fplinux_jpeg_cpu_metric_stop(&output_metric, &start))
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
		       size_t input_size,
		       const struct fplinux_jpeg_cpu_metric *input_loaded,
		       char *error, size_t error_size)
{
	struct fplinux_jpeg_cpu_metric input_metric = { 0 },
				       codec_metric = { 0 },
				       convert_metric = { 0 };
	struct fplinux_jpeg_cpu_metric batch_metric = { 0 },
				       output_metric = { 0 },
				       measured_stages_metric = { 0 };
	struct fplinux_jpeg_cpu_metric_start start, batch_start;
	struct fplinux_jpeg_cpu_frame reference = { 0 };
	bool ok = false;

	input_metric = *input_loaded;
	if (!fplinux_jpeg_cpu_metric_start(&batch_start)) {
		set_error(error, error_size,
			  "cannot start decode batch timing");
		return false;
	}
	for (unsigned iteration = 0; iteration < options->repeat; iteration++) {
		struct fplinux_jpeg_cpu_frame current = { 0 };

		if (!fplinux_jpeg_cpu_decode(input, input_size, options->scale,
					     options->decode_mode, &current,
					     &codec_metric, &convert_metric,
					     error, error_size))
			goto done;
		if (!reference.data) {
			reference = current;
			memset(&current, 0, sizeof(current));
		} else if (!same_nv_frame(&reference, &current)) {
			fplinux_jpeg_cpu_frame_cleanup(&current);
			set_error(
				error, error_size,
				"non-deterministic raw output across repeats");
			goto done;
		}
		fplinux_jpeg_cpu_frame_cleanup(&current);
	}
	if (!fplinux_jpeg_cpu_metric_stop(&batch_metric, &batch_start) ||
	    !fplinux_jpeg_cpu_metric_start(&start) ||
	    !write_file(options->output_path, reference.data, reference.size,
			error, error_size) ||
	    !fplinux_jpeg_cpu_metric_stop(&output_metric, &start))
		goto done;
	measured_stages_metric = add_metrics(&input_metric, &batch_metric);
	measured_stages_metric =
		add_metrics(&measured_stages_metric, &output_metric);
	printf("operation=decode input_format=JPEG output_format=%s decode_mode=%s scale=%u "
	       "dct=ISLOW fancy_upsampling=off block_smoothing=off\n",
	       reference.vsub == 2 ? "NV12" : "NV16",
	       options->decode_mode == FPLINUX_JPEG_CPU_DECODE_BOX ?
		       "full-idct-plus-box" :
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
	fplinux_jpeg_cpu_frame_cleanup(&reference);
	return ok;
}

static bool run_scale(const struct options *options, const uint8_t *input,
		      size_t input_size,
		      const struct fplinux_jpeg_cpu_metric *input_metric,
		      char *error, size_t error_size)
{
	unsigned destination_width = options->width / 2;
	unsigned destination_height = options->height / 2;
	struct fplinux_jpeg_cpu_metric filter_metric = { 0 },
				       batch_metric = { 0 };
	struct fplinux_jpeg_cpu_metric output_metric = { 0 },
				       measured_stages_metric;
	struct fplinux_jpeg_cpu_metric_start start, batch_start;
	struct fplinux_jpeg_cpu_frame reference = { 0 };
	bool ok = false;

	if (!fplinux_jpeg_cpu_metric_start(&batch_start))
		return false;
	for (unsigned iteration = 0; iteration < options->repeat; iteration++) {
		struct fplinux_jpeg_cpu_frame current = { 0 };

		if (!fplinux_jpeg_cpu_scale(input, options->width,
					    options->height, &current,
					    &filter_metric))
			goto done;
		if (!reference.data) {
			reference = current;
			memset(&current, 0, sizeof(current));
		} else if (!same_nv_frame(&reference, &current)) {
			fplinux_jpeg_cpu_frame_cleanup(&current);
			set_error(error, error_size,
				  "non-deterministic scale output");
			goto done;
		}
		fplinux_jpeg_cpu_frame_cleanup(&current);
	}
	if (!fplinux_jpeg_cpu_metric_stop(&batch_metric, &batch_start) ||
	    !fplinux_jpeg_cpu_metric_start(&start) ||
	    !write_file(options->output_path, reference.data, reference.size,
			error, error_size) ||
	    !fplinux_jpeg_cpu_metric_stop(&output_metric, &start))
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
	fplinux_jpeg_cpu_frame_cleanup(&reference);
	return ok;
}

int main(int argc, char **argv)
{
	struct options options;
	struct fplinux_jpeg_cpu_metric input_metric = { 0 };
	struct fplinux_jpeg_cpu_metric_start start;
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
	if (!fplinux_jpeg_cpu_metric_start(&start) ||
	    !read_file_bounded(options.input_path,
			       FPLINUX_JPEG_CPU_MAX_RAW_BYTES, &input,
			       &input_size, error, sizeof(error)) ||
	    !fplinux_jpeg_cpu_metric_stop(&input_metric, &start)) {
		fprintf(stderr, "fplinux-jpeg-cpu: %s\n",
			error[0] ? error : "cannot read input");
		free(input);
		return EXIT_FAILURE;
	}
	if (options.operation != OP_DECODE &&
	    (!expected_nv16_size(options.width, options.height,
				 &expected_size) ||
	     input_size != expected_size)) {
		fprintf(stderr,
			"fplinux-jpeg-cpu: NV16 input must be exactly %zu tight Y+UV bytes\n",
			expected_size);
		free(input);
		return EXIT_FAILURE;
	}
	if (options.dqt_path &&
	    (!read_file_bounded(options.dqt_path, FPLINUX_JPEG_CPU_DQT_BYTES,
				&dqt, &dqt_size, error, sizeof(error)) ||
	     dqt_size != FPLINUX_JPEG_CPU_DQT_BYTES || !valid_dqt(dqt))) {
		fprintf(stderr, "fplinux-jpeg-cpu: %s\n",
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
		fprintf(stderr, "fplinux-jpeg-cpu: %s\n",
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
