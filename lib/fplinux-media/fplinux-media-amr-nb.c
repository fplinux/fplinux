/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-media-amr-nb.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#define AMR_NB_HEADER_BYTES (sizeof(FPLINUX_MEDIA_AMR_NB_MAGIC) - 1U)
#define AMR_NB_PACKET_BYTES 32U
#define AMR_NB_PACKET_SAMPLES 160U

static const unsigned int packet_sizes[] = { 13, 14, 16, 18, 20, 21, 27, 32 };

struct fplinux_media_amr_nb {
	int (*read_exact)(void *ctx, void *out, size_t count, uint64_t offset);
	void *ctx;
	AVCodecContext *codec;
	AVPacket *packet;
	AVFrame *frame;
	uint64_t size;
	uint64_t offset;
	uint64_t frames_decoded;
	int16_t pcm[AMR_NB_PACKET_SAMPLES];
	size_t buffered;
	int error;
};

static AVCodecContext *open_codec(void)
{
	const AVCodec *codec = avcodec_find_decoder_by_name("amrnb");
	AVCodecContext *context;
	int result;

	if (!codec) {
		errno = ENOTSUP;
		return NULL;
	}
	context = avcodec_alloc_context3(codec);
	if (!context) {
		errno = ENOMEM;
		return NULL;
	}
	context->thread_count = 1;
	result = avcodec_open2(context, codec, NULL);
	if (result < 0) {
		avcodec_free_context(&context);
		errno = result == AVERROR(ENOMEM) ? ENOMEM : EIO;
		return NULL;
	}
	return context;
}

static int packet_size(struct fplinux_media_amr_nb *decoder,
		       unsigned char *header, uint64_t offset)
{
	unsigned int type;
	unsigned int total;

	if (decoder->read_exact(decoder->ctx, header, 1U, offset) < 0)
		return -1;
	type = (*header >> 3) & 0x0fU;
	if (type >= sizeof(packet_sizes) / sizeof(packet_sizes[0]) ||
	    !(*header & 0x04U)) {
		errno = ENOTSUP;
		return -1;
	}
	total = packet_sizes[type];
	if (total > decoder->size - offset) {
		errno = EINVAL;
		return -1;
	}
	return (int)total;
}

struct fplinux_media_amr_nb *
fplinux_media_amr_nb_open(int (*read_exact)(void *context, void *out,
					    size_t count, uint64_t offset),
			  void *context, uint64_t size)
{
	unsigned char first_packet;
	struct fplinux_media_amr_nb *decoder;
	int error;

	if (size <= AMR_NB_HEADER_BYTES) {
		errno = ENOTSUP;
		return NULL;
	}
	decoder = calloc(1, sizeof(*decoder));
	if (!decoder)
		return NULL;
	decoder->read_exact = read_exact;
	decoder->ctx = context;
	decoder->size = size;
	decoder->offset = AMR_NB_HEADER_BYTES;
	if (packet_size(decoder, &first_packet, decoder->offset) < 0)
		goto fail;
	decoder->codec = open_codec();
	if (!decoder->codec)
		goto fail;
	decoder->packet = av_packet_alloc();
	decoder->frame = av_frame_alloc();
	if (!decoder->packet || !decoder->frame) {
		errno = ENOMEM;
		goto fail;
	}
	return decoder;

fail:
	error = errno;
	fplinux_media_amr_nb_close(decoder);
	errno = error;
	return NULL;
}

void fplinux_media_amr_nb_close(struct fplinux_media_amr_nb *decoder)
{
	if (!decoder)
		return;
	av_frame_free(&decoder->frame);
	av_packet_free(&decoder->packet);
	avcodec_free_context(&decoder->codec);
	free(decoder);
}

static int16_t pcm16(float sample)
{
	if (sample >= 1.0F)
		return INT16_MAX;
	if (sample <= -1.0F)
		return INT16_MIN;
	long scaled = lrintf(sample * 32768.0F);
	return scaled > INT16_MAX ? INT16_MAX : (int16_t)scaled;
}

static int decode_packet(struct fplinux_media_amr_nb *decoder)
{
	unsigned char data[AMR_NB_PACKET_BYTES +
			   AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
	int size = packet_size(decoder, data, decoder->offset);
	AVPacket *packet = decoder->packet;
	AVFrame *frame = decoder->frame;
	int result;

	if (size < 0 ||
	    decoder->read_exact(decoder->ctx, data + 1U, (size_t)size - 1U,
				decoder->offset + 1U) < 0)
		return -1;

	/* send_packet copies data when the packet owns no reference. */
	packet->data = data;
	packet->size = size;
	result = avcodec_send_packet(decoder->codec, packet);
	packet->data = NULL;
	packet->size = 0;
	if (result >= 0)
		result = avcodec_receive_frame(decoder->codec, frame);
	if (result < 0) {
		errno = result == AVERROR(ENOMEM) ? ENOMEM : EIO;
		return -1;
	}

	if (frame->format != AV_SAMPLE_FMT_FLTP ||
	    frame->ch_layout.nb_channels != 1 ||
	    frame->sample_rate != FPLINUX_MEDIA_AMR_NB_RATE ||
	    frame->nb_samples != AMR_NB_PACKET_SAMPLES)
		goto invalid_frame;
	const float *pcm = (const float *)frame->extended_data[0];
	for (size_t i = 0; i < AMR_NB_PACKET_SAMPLES; i++) {
		if (!isfinite(pcm[i]))
			goto invalid_frame;
		decoder->pcm[i] = pcm16(pcm[i]);
	}
	av_frame_unref(frame);
	decoder->offset += (unsigned int)size;
	decoder->buffered = AMR_NB_PACKET_SAMPLES;
	decoder->frames_decoded += AMR_NB_PACKET_SAMPLES;
	return 0;

invalid_frame:
	av_frame_unref(frame);
	errno = EIO;
	return -1;
}

ssize_t fplinux_media_amr_nb_read(struct fplinux_media_amr_nb *decoder,
				  int16_t *samples, size_t capacity,
				  uint64_t *total_frames)
{
	size_t done = 0;

	if (decoder->error) {
		errno = decoder->error;
		return -1;
	}
	while (done < capacity) {
		if (!decoder->buffered) {
			if (decoder->offset == decoder->size)
				break;
			if (decode_packet(decoder) < 0) {
				decoder->error = errno;
				return done ? (ssize_t)done : -1;
			}
			if (decoder->offset == decoder->size)
				*total_frames = decoder->frames_decoded;
		}
		size_t remaining = capacity - done;
		size_t take = decoder->buffered < remaining ?
				      decoder->buffered :
				      remaining;
		size_t start = AMR_NB_PACKET_SAMPLES - decoder->buffered;

		memcpy(samples + done, decoder->pcm + start,
		       take * sizeof(*samples));
		decoder->buffered -= take;
		done += take;
	}
	return (ssize_t)done;
}

bool fplinux_media_amr_nb_failed(const struct fplinux_media_amr_nb *decoder)
{
	return decoder->error != 0;
}

int fplinux_media_amr_nb_rewind(struct fplinux_media_amr_nb *decoder)
{
	/* Reopen the codec to clear the speech decoder's history. */
	AVCodecContext *reset = open_codec();

	if (!reset)
		return -1;
	avcodec_free_context(&decoder->codec);
	decoder->codec = reset;
	decoder->offset = AMR_NB_HEADER_BYTES;
	decoder->buffered = 0;
	decoder->frames_decoded = 0;
	decoder->error = 0;
	return 0;
}
