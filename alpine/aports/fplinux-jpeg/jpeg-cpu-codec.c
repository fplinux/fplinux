/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include "jpeg-cpu-codec.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <jpeglib.h>

/* The v6b ABI exposes one size for both axes of each scaled DCT. */
#if JPEG_LIB_VERSION < 70
#define DCT_h_scaled_size DCT_scaled_size
#define DCT_v_scaled_size DCT_scaled_size
#define min_DCT_v_scaled_size min_DCT_scaled_size
#endif

enum { COMPONENTS = 3, MAX_ENCODED_BYTES = 64 * 1024 * 1024 };

struct jpeg_error_state {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
	char message[JMSG_LENGTH_MAX];
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
	struct fplinux_jpeg_cpu_metric_start start;
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

bool fplinux_jpeg_cpu_metric_start(struct fplinux_jpeg_cpu_metric_start *start)
{
	start->wall = monotonic_seconds();
	return start->wall >= 0.0 && getrusage(RUSAGE_SELF, &start->usage) == 0;
}

bool fplinux_jpeg_cpu_metric_stop(
	struct fplinux_jpeg_cpu_metric *total,
	const struct fplinux_jpeg_cpu_metric_start *start)
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
		    !add_size(total, bytes, &total) ||
		    total > FPLINUX_JPEG_CPU_MAX_RAW_BYTES)
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

void fplinux_jpeg_cpu_frame_cleanup(struct fplinux_jpeg_cpu_frame *frame)
{
	free(frame->data);
	memset(frame, 0, sizeof(*frame));
}

static bool allocate_nv_frame(unsigned width, unsigned height, unsigned vsub,
			      struct fplinux_jpeg_cpu_frame *frame)
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
	    !add_size(y_size, uv_size, &total) ||
	    total > FPLINUX_JPEG_CPU_MAX_RAW_BYTES)
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
			    struct fplinux_jpeg_cpu_frame *frame)
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
		fplinux_jpeg_cpu_frame_cleanup(frame);
	return ok;
}

static bool make_box_nv(const struct raw_image *image, unsigned scale,
			struct fplinux_jpeg_cpu_frame *frame)
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
		fplinux_jpeg_cpu_frame_cleanup(frame);
	return ok;
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

bool fplinux_jpeg_cpu_encode(
	const struct fplinux_jpeg_cpu_encode_request *request,
	const uint8_t *input, const uint8_t *dqt, FILE **result,
	size_t *result_size, struct fplinux_jpeg_cpu_metric *copy_metric,
	struct fplinux_jpeg_cpu_metric *codec_metric, char *error,
	size_t error_size)
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
	if (!fplinux_jpeg_cpu_metric_start(&operation->start)) {
		set_error(error, error_size, "cannot start CPU timing");
		goto done;
	}
	jpeg_create_compress(&operation->encoder);
	operation->created = true;
	jpeg_stdio_dest(&operation->encoder, operation->file);
	operation->encoder.image_width = request->width;
	operation->encoder.image_height = request->height;
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
	jpeg_set_quality(&operation->encoder, (int)request->quality, TRUE);
	if (dqt)
		set_exact_dqt(&operation->encoder, dqt);
	operation->encoder.raw_data_in = TRUE;
	operation->encoder.dct_method = JDCT_ISLOW;
	operation->encoder.optimize_coding = FALSE;
	operation->encoder.arith_code = FALSE;
	operation->encoder.restart_interval = request->restart;
	operation->encoder.restart_in_rows = 0;
	jpeg_start_compress(&operation->encoder, TRUE);
	if (!allocate_encode_rows(&operation->encoder, &operation->rows) ||
	    !fplinux_jpeg_cpu_metric_stop(codec_metric, &operation->start)) {
		set_error(error, error_size,
			  "cannot allocate or time raw encode rows");
		goto done;
	}
	for (unsigned top = 0; top < request->height; top += DCTSIZE) {
		JDIMENSION lines =
			(JDIMENSION)(operation->encoder.max_v_samp_factor *
				     DCTSIZE);

		if (!fplinux_jpeg_cpu_metric_start(&operation->start) ||
		    !fill_encode_rows(&operation->rows, input, request->width,
				      request->height, top) ||
		    !fplinux_jpeg_cpu_metric_stop(copy_metric,
						  &operation->start)) {
			set_error(error, error_size,
				  "cannot prepare NV16 raw encode rows");
			goto done;
		}
		if (!fplinux_jpeg_cpu_metric_start(&operation->start) ||
		    jpeg_write_raw_data(&operation->encoder,
					operation->rows.rows, lines) != lines ||
		    !fplinux_jpeg_cpu_metric_stop(codec_metric,
						  &operation->start)) {
			set_error(error, error_size,
				  "incomplete raw JPEG encode");
			goto done;
		}
	}
	if (!fplinux_jpeg_cpu_metric_start(&operation->start)) {
		set_error(error, error_size, "cannot finish JPEG encode");
		goto done;
	}
	jpeg_finish_compress(&operation->encoder);
	if (!fplinux_jpeg_cpu_metric_stop(codec_metric, &operation->start)) {
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

bool fplinux_jpeg_cpu_decode(const uint8_t *input, size_t input_size,
			     unsigned scale,
			     enum fplinux_jpeg_cpu_decode_mode mode,
			     struct fplinux_jpeg_cpu_frame *frame,
			     struct fplinux_jpeg_cpu_metric *codec_metric,
			     struct fplinux_jpeg_cpu_metric *convert_metric,
			     char *error, size_t error_size)
{
	struct raw_image raw = { 0 };
	struct fplinux_jpeg_cpu_metric_start start;

	if (!fplinux_jpeg_cpu_metric_start(&start) ||
	    !decode_once(input, input_size,
			 mode == FPLINUX_JPEG_CPU_DECODE_BOX ? 1 : scale, &raw,
			 error, error_size) ||
	    !fplinux_jpeg_cpu_metric_stop(codec_metric, &start)) {
		free_raw_image(&raw);
		return false;
	}
	if (!fplinux_jpeg_cpu_metric_start(&start) ||
	    !(mode == FPLINUX_JPEG_CPU_DECODE_BOX ?
		      make_box_nv(&raw, scale, frame) :
		      make_reduced_nv(&raw, frame)) ||
	    !fplinux_jpeg_cpu_metric_stop(convert_metric, &start)) {
		free_raw_image(&raw);
		fplinux_jpeg_cpu_frame_cleanup(frame);
		set_error(error, error_size,
			  "cannot allocate bounded NV output");
		return false;
	}
	free_raw_image(&raw);
	return true;
}

bool fplinux_jpeg_cpu_scale(const uint8_t *input, unsigned width,
			    unsigned height,
			    struct fplinux_jpeg_cpu_frame *frame,
			    struct fplinux_jpeg_cpu_metric *filter_metric)
{
	struct fplinux_jpeg_cpu_metric_start start;

	if (!allocate_nv_frame(width / 2, height / 2, 1, frame) ||
	    !fplinux_jpeg_cpu_metric_start(&start)) {
		fplinux_jpeg_cpu_frame_cleanup(frame);
		return false;
	}
	scale_nv16_box2(input, frame->data, width, height);
	if (!fplinux_jpeg_cpu_metric_stop(filter_metric, &start)) {
		fplinux_jpeg_cpu_frame_cleanup(frame);
		return false;
	}
	return true;
}
