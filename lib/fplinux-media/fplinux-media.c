/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "fplinux-media.h"
#include "fplinux-media-amr-nb.h"
#include "fplinux-utf8.h"

#include <errno.h>
#include <fcntl.h>
#include <mpg123.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEDIA_IO_BYTES (64U * 1024U)
#define MEDIA_CHUNKS_MAX 128U

struct fplinux_media {
	struct fplinux_media_info info;
	int fd;
	unsigned char *bytes;
	uint64_t size;
	uint64_t source_offset;
	uint64_t input_position;
	uint64_t data_offset;
	uint64_t total_frames;
	uint64_t seek_target;
	unsigned int bits;
	unsigned int gain;
	mpg123_handle *mp3;
	struct fplinux_media_amr_nb *amr_nb;
	size_t io_remaining;
	int io_error;
};

static uint16_t read_le16(const unsigned char *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static uint32_t read_be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static uint32_t read_synchsafe(const unsigned char *p)
{
	return (uint32_t)(p[0] & 0x7fU) << 21 | (uint32_t)(p[1] & 0x7fU) << 14 |
	       (uint32_t)(p[2] & 0x7fU) << 7 | (p[3] & 0x7fU);
}

static ssize_t read_at(struct fplinux_media *media, void *out, size_t count,
		       uint64_t offset)
{
	if (offset > media->size) {
		errno = EINVAL;
		return -1;
	}
	if (count > media->size - offset)
		count = (size_t)(media->size - offset);
	offset += media->source_offset;
	if (media->bytes) {
		memcpy(out, media->bytes + offset, count);
		return (ssize_t)count;
	}
	ssize_t result;
	do {
		result = pread(media->fd, out, count, (off_t)offset);
	} while (result < 0 && errno == EINTR);
	return result;
}

static int read_exact(void *context, void *out, size_t count, uint64_t offset)
{
	struct fplinux_media *media = context;
	size_t done = 0;
	while (done < count) {
		ssize_t size = read_at(media, (unsigned char *)out + done,
				       count - done, offset + done);
		if (size <= 0) {
			if (!size)
				errno = EIO;
			return -1;
		}
		done += (size_t)size;
	}
	return 0;
}

static void set_duration(struct fplinux_media *media)
{
	media->info.duration_ms =
		media->total_frames * 1000U / media->info.rate;
	media->info.duration_known = true;
}

static bool append_text(char *out, size_t *used, uint32_t cp)
{
	if (!cp)
		return false;
	if (cp < 32U || (cp >= 0x7fU && cp <= 0x9fU))
		cp = ' ';
	size_t count = cp < 0x80U    ? 1U :
		       cp < 0x800U   ? 2U :
		       cp < 0x10000U ? 3U :
				       4U;
	if (count >= FPLINUX_MEDIA_TEXT_BYTES - *used)
		return false;
	if (count == 1U) {
		out[(*used)++] = (char)cp;
	} else {
		out[(*used)++] = (char)((count == 2U ? 0xc0U :
					 count == 3U ? 0xe0U :
						       0xf0U) |
					(cp >> (6U * (count - 1U))));
		for (size_t i = count - 1U; i; i--)
			out[(*used)++] =
				(char)(0x80U |
				       ((cp >> (6U * (i - 1U))) & 0x3fU));
	}
	out[*used] = '\0';
	return true;
}

static void decode_text(char *out, const unsigned char *bytes, size_t size)
{
	if (!size)
		return;
	unsigned int encoding = *bytes++;
	size--;
	bool little = false;
	if (encoding == 1U) {
		if (size < 2U || !((bytes[0] == 0xffU && bytes[1] == 0xfeU) ||
				   (bytes[0] == 0xfeU && bytes[1] == 0xffU)))
			return;
		little = bytes[0] == 0xffU;
		bytes += 2;
		size -= 2;
	}
	const unsigned char *end = bytes + size;
	size_t used = 0;
	while (bytes < end) {
		uint32_t cp;
		if (encoding == 0U) {
			cp = *bytes++;
		} else if (encoding == 3U) {
			if (!fplinux_utf8_next(&bytes, end, &cp)) {
				out[0] = '\0';
				return;
			}
		} else if (encoding == 1U || encoding == 2U) {
			if (end - bytes < 2)
				break;
			cp = little ? read_le16(bytes) :
				      (uint32_t)bytes[0] << 8 | bytes[1];
			bytes += 2;
			if (cp >= 0xd800U && cp <= 0xdbffU) {
				if (end - bytes < 2)
					break;
				uint32_t next = little ? read_le16(bytes) :
							 (uint32_t)bytes[0]
									 << 8 |
								 bytes[1];
				bytes += 2;
				if (next < 0xdc00U || next > 0xdfffU)
					break;
				cp = 0x10000U + ((cp - 0xd800U) << 10) + next -
				     0xdc00U;
			} else if (cp >= 0xdc00U && cp <= 0xdfffU) {
				break;
			}
		} else {
			return;
		}
		if (!append_text(out, &used, cp))
			break;
	}
	while (used && out[used - 1U] == ' ')
		out[--used] = '\0';
}

static uint64_t read_metadata(struct fplinux_media *media)
{
	unsigned char header[10];
	if (read_exact(media, header, sizeof(header), 0) < 0 ||
	    memcmp(header, "ID3", 3))
		return 0;
	uint64_t end = 10U + read_synchsafe(header + 6);
	if (end > media->size)
		return 0;
	unsigned int version = header[3];
	/* Extended/unsynchronised tags are skipped without retaining their payload. */
	if ((version != 3U && version != 4U) || header[5] ||
	    end > MEDIA_IO_BYTES)
		return end;
	for (uint64_t at = 10, frames = 0;
	     at <= end && end - at >= 10U && frames < 128U; frames++) {
		if (read_exact(media, header, sizeof(header), at) < 0 ||
		    !header[0])
			break;
		uint32_t size = version == 4U ? read_synchsafe(header + 4) :
						read_be32(header + 4);
		at += sizeof(header);
		if (size > end - at)
			break;
		char *text = !memcmp(header, "TIT2", 4) ? media->info.title :
			     !memcmp(header, "TPE1", 4) ? media->info.artist :
			     !memcmp(header, "TALB", 4) ? media->info.album :
							  NULL;
		if (text && !header[8] && !header[9] && size && size <= 1024U) {
			unsigned char bytes[1024];
			if (!read_exact(media, bytes, size, at))
				decode_text(text, bytes, size);
		}
		at += size;
	}
	return end;
}

static bool has_frame_count(struct fplinux_media *media, uint64_t offset)
{
	unsigned char bytes[4096];
	ssize_t count = read_at(media, bytes, sizeof(bytes), offset);
	if (count < 48)
		return false;
	for (size_t i = 0; i + 48U < (size_t)count; i++) {
		if (bytes[i] != 0xffU || (bytes[i + 1U] & 0xe0U) != 0xe0U ||
		    (bytes[i + 1U] & 6U) != 2U)
			continue;
		unsigned int version = (bytes[i + 1U] >> 3) & 3U;
		if (version == 1U)
			continue;
		bool mono = (bytes[i + 3U] >> 6) == 3U;
		size_t side = version == 3U ? (mono ? 17U : 32U) :
					      (mono ? 9U : 17U);
		size_t xing = i + 4U + ((bytes[i + 1U] & 1U) ? 0U : 2U) + side;
		if (xing + 12U <= (size_t)count &&
		    (!memcmp(bytes + xing, "Xing", 4) ||
		     !memcmp(bytes + xing, "Info", 4)) &&
		    (read_be32(bytes + xing + 4U) & 1U))
			return true;
		return false;
	}
	return false;
}

static int open_mp3(struct fplinux_media *media);

static int open_wav(struct fplinux_media *media)
{
	unsigned char header[16];
	if (read_exact(media, header, 12, 0) < 0)
		return -1;
	uint64_t limit = (uint64_t)read_le32(header + 4) + 8U;
	if (limit > media->size || limit < 12U) {
		errno = EINVAL;
		return -1;
	}
	enum { WAVE_PCM = 1, WAVE_MP3 = 0x55 };
	unsigned int format = 0;
	uint64_t data_bytes = 0;
	for (uint64_t at = 12, chunks = 0;
	     at <= limit && limit - at >= 8U && chunks < MEDIA_CHUNKS_MAX;
	     chunks++) {
		if (read_exact(media, header, 8, at) < 0)
			return -1;
		uint32_t bytes = read_le32(header + 4);
		at += 8U;
		if (bytes > limit - at) {
			errno = EINVAL;
			return -1;
		}
		if (!memcmp(header, "fmt ", 4)) {
			if (format || bytes < 16U ||
			    read_exact(media, header, 16, at) < 0) {
				errno = EINVAL;
				return -1;
			}
			media->info.channels = read_le16(header + 2);
			media->info.rate = read_le32(header + 4);
			media->bits = read_le16(header + 14);
			format = read_le16(header);
			if ((format != WAVE_PCM && format != WAVE_MP3) ||
			    (media->info.channels != 1U &&
			     media->info.channels != 2U) ||
			    media->info.rate < 8000U ||
			    media->info.rate > 48000U) {
				errno = ENOTSUP;
				return -1;
			}
			if (format == WAVE_PCM) {
				if (media->bits != 8U && media->bits != 16U) {
					errno = ENOTSUP;
					return -1;
				}
				unsigned int align =
					media->info.channels * media->bits / 8U;
				if (read_le16(header + 12) != align ||
				    read_le32(header + 8) !=
					    media->info.rate * align) {
					errno = EINVAL;
					return -1;
				}
			}
		} else if (!memcmp(header, "data", 4)) {
			if (media->data_offset) {
				errno = ENOTSUP;
				return -1;
			}
			media->data_offset = at;
			data_bytes = bytes;
		}
		at += bytes;
		if (bytes & 1U)
			at++;
	}
	if (!format || !media->data_offset) {
		errno = EINVAL;
		return -1;
	}
	if (format == WAVE_MP3) {
		/* The MP3 decoder's reads and seeks are relative to the data chunk. */
		media->source_offset = media->data_offset;
		media->size = data_bytes;
		media->data_offset = 0;
		return open_mp3(media);
	}
	if (data_bytes % (media->info.channels * media->bits / 8U)) {
		errno = EINVAL;
		return -1;
	}
	media->total_frames =
		data_bytes / (media->info.channels * media->bits / 8U);
	media->info.codec = FPLINUX_MEDIA_WAV;
	media->info.eof = !media->total_frames;
	set_duration(media);
	return 0;
}

static mpg123_ssize_t mp3_read(void *context, void *buffer, size_t count)
{
	struct fplinux_media *media = context;
	if (!media->io_remaining) {
		media->io_error = EIO;
		errno = EIO;
		return -1;
	}
	if (count > 4096U)
		count = 4096U;
	if (count > media->io_remaining)
		count = media->io_remaining;
	ssize_t size = read_at(media, buffer, count, media->input_position);
	if (size < 0)
		media->io_error = errno;
	else {
		media->input_position += (size_t)size;
		media->io_remaining -= (size_t)size;
	}
	return size;
}

static off_t mp3_seek(void *context, off_t offset, int whence)
{
	struct fplinux_media *media = context;
	uint64_t base;
	if (whence == SEEK_SET)
		base = 0;
	else if (whence == SEEK_CUR)
		base = media->input_position;
	else if (whence == SEEK_END)
		base = media->size;
	else {
		errno = EINVAL;
		return -1;
	}
	if ((offset < 0 && (uint64_t)(-(offset + 1)) + 1U > base) ||
	    (offset >= 0 && (uint64_t)offset > media->size - base)) {
		errno = EINVAL;
		return -1;
	}
	media->input_position =
		offset < 0 ? base - ((uint64_t)(-(offset + 1)) + 1U) :
			     base + (uint64_t)offset;
	return (off_t)media->input_position;
}

static int mp3_format(struct fplinux_media *media)
{
	long rate;
	int channels, encoding;
	if (mpg123_getformat(media->mp3, &rate, &channels, &encoding) !=
		    MPG123_OK ||
	    rate < 8000 || rate > 48000 || (channels != 1 && channels != 2) ||
	    encoding != MPG123_ENC_SIGNED_16) {
		errno = ENOTSUP;
		return -1;
	}
	if (media->info.rate &&
	    (media->info.rate != (unsigned int)rate ||
	     media->info.channels != (unsigned int)channels)) {
		errno = ENOTSUP;
		return -1;
	}
	media->info.rate = (unsigned int)rate;
	media->info.channels = (unsigned int)channels;
	return 0;
}

static int open_mp3(struct fplinux_media *media)
{
	static const long rates[] = { 8000,  11025, 12000, 16000, 22050,
				      24000, 32000, 44100, 48000 };
	uint64_t audio_offset = read_metadata(media);
	int error;
	if (mpg123_init() != MPG123_OK ||
	    !(media->mp3 = mpg123_new(NULL, &error))) {
		errno = ENOMEM;
		return -1;
	}
	if (mpg123_param(media->mp3, MPG123_ADD_FLAGS,
			 MPG123_QUIET | MPG123_SKIP_ID3V2 |
				 MPG123_NO_FRANKENSTEIN,
			 0) != MPG123_OK ||
	    mpg123_param(media->mp3, MPG123_INDEX_SIZE, 1000, 0) != MPG123_OK ||
	    mpg123_param(media->mp3, MPG123_RESYNC_LIMIT, 1024, 0) !=
		    MPG123_OK ||
	    mpg123_format_none(media->mp3) != MPG123_OK)
		goto unsupported;
	for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
		if (mpg123_format(media->mp3, rates[i],
				  MPG123_MONO | MPG123_STEREO,
				  MPG123_ENC_SIGNED_16) != MPG123_OK)
			goto unsupported;
	media->io_remaining = MEDIA_IO_BYTES;
	if (mpg123_replace_reader_handle(media->mp3, mp3_read, mp3_seek,
					 NULL) != MPG123_OK ||
	    mpg123_open_handle(media->mp3, media) != MPG123_OK ||
	    mp3_format(media) < 0)
		goto unsupported;
	if (media->io_error) {
		errno = media->io_error;
		return -1;
	}
	media->info.codec = FPLINUX_MEDIA_MP3;
	int64_t length = mpg123_length64(media->mp3);
	if (length >= 0 && has_frame_count(media, audio_offset)) {
		media->total_frames = (uint64_t)length;
		set_duration(media);
	}
	return 0;
unsupported:
	errno = media->io_error ? media->io_error : ENOTSUP;
	return -1;
}

static int open_amr_nb(struct fplinux_media *media)
{
	media->amr_nb =
		fplinux_media_amr_nb_open(read_exact, media, media->size);
	if (!media->amr_nb)
		return -1;
	media->info.codec = FPLINUX_MEDIA_AMR_NB;
	media->info.rate = FPLINUX_MEDIA_AMR_NB_RATE;
	media->info.channels = 1;
	return 0;
}

static struct fplinux_media *open_input(struct fplinux_media *media)
{
	unsigned char prefix[12];
	int result;

	media->gain = 100;
	if (media->size < sizeof(prefix) ||
	    read_exact(media, prefix, sizeof(prefix), 0) < 0) {
		errno = ENOTSUP;
		goto fail;
	}
	if (!memcmp(prefix, FPLINUX_MEDIA_AMR_NB_MAGIC,
		    sizeof(FPLINUX_MEDIA_AMR_NB_MAGIC) - 1U))
		result = open_amr_nb(media);
	else if (!memcmp(prefix, "RIFF", 4) && !memcmp(prefix + 8, "WAVE", 4))
		result = open_wav(media);
	else
		result = open_mp3(media);
	if (result < 0)
		goto fail;
	return media;
fail:;
	int error = errno;
	fplinux_media_close(media);
	errno = error;
	return NULL;
}

struct fplinux_media *fplinux_media_open_fd(int fd)
{
	struct stat st;
	if (fstat(fd, &st) < 0)
		return NULL;
	if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uint64_t)st.st_size > FPLINUX_MEDIA_FILE_BYTES) {
		errno = EINVAL;
		return NULL;
	}
	struct fplinux_media *media = calloc(1, sizeof(*media));
	if (!media)
		return NULL;
	media->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (media->fd < 0) {
		free(media);
		return NULL;
	}
	media->size = (uint64_t)st.st_size;
	return open_input(media);
}

struct fplinux_media *fplinux_media_open_bytes(const void *bytes, size_t count)
{
	if (!bytes || !count || count > FPLINUX_MEDIA_RESOURCE_BYTES) {
		errno = EINVAL;
		return NULL;
	}
	struct fplinux_media *media = calloc(1, sizeof(*media));
	if (!media)
		return NULL;
	media->fd = -1;
	media->bytes = malloc(count);
	if (!media->bytes) {
		free(media);
		return NULL;
	}
	memcpy(media->bytes, bytes, count);
	media->size = count;
	return open_input(media);
}

void fplinux_media_close(struct fplinux_media *media)
{
	if (!media)
		return;
	if (media->mp3)
		mpg123_delete(media->mp3);
	fplinux_media_amr_nb_close(media->amr_nb);
	if (media->fd >= 0)
		close(media->fd);
	free(media->bytes);
	free(media);
}

const struct fplinux_media_info *
fplinux_media_info(const struct fplinux_media *media)
{
	return media ? &media->info : NULL;
}

static ssize_t read_pcm(struct fplinux_media *media, int16_t *samples,
			size_t capacity)
{
	if (media->info.eof)
		return 0;
	if (media->amr_nb) {
		ssize_t frames = fplinux_media_amr_nb_read(
			media->amr_nb, samples, capacity, &media->total_frames);

		if (frames < 0)
			return -1;
		media->info.position_frames += (uint64_t)frames;
		if (media->total_frames && !media->info.duration_known)
			set_duration(media);
		media->info.eof = media->info.duration_known &&
				  media->info.position_frames ==
					  media->total_frames;
		return frames;
	}
	size_t frames;
	if (media->info.codec == FPLINUX_MEDIA_WAV) {
		uint64_t remaining =
			media->total_frames - media->info.position_frames;
		frames = remaining < capacity ? (size_t)remaining : capacity;
		unsigned char bytes[FPLINUX_MEDIA_BLOCK_FRAMES * 4U];
		size_t channels = media->info.channels == 1U ? 1U : 2U;
		size_t count = frames * channels;
		size_t width = media->bits == 8U ? 1U : 2U;
		uint64_t offset =
			media->data_offset +
			media->info.position_frames * channels * width;
		size_t byte_count = count * width;
		if (read_exact(media, bytes, byte_count, offset) < 0)
			return -1;
		if (width == 1U) {
			for (size_t i = 0; i < byte_count; i++)
				samples[i] =
					(int16_t)(((int)bytes[i] - 128) * 256);
		} else {
			for (size_t i = 0; i + 1U < byte_count; i += 2U)
				samples[i / 2U] = (int16_t)read_le16(bytes + i);
		}
		media->info.position_frames += frames;
		media->info.eof = media->info.position_frames ==
				  media->total_frames;
	} else {
		media->io_remaining = MEDIA_IO_BYTES;
		media->io_error = 0;
		size_t bytes = 0;
		int code = mpg123_read(media->mp3, samples,
				       capacity * media->info.channels *
					       sizeof(*samples),
				       &bytes);
		if (media->io_error) {
			errno = media->io_error;
			return -1;
		}
		if (code == MPG123_NEW_FORMAT && mp3_format(media) < 0)
			return -1;
		if ((code != MPG123_OK && code != MPG123_NEW_FORMAT &&
		     code != MPG123_DONE) ||
		    bytes % (media->info.channels * sizeof(*samples))) {
			errno = EIO;
			return -1;
		}
		frames = bytes / (media->info.channels * sizeof(*samples));
		if (!frames && code != MPG123_DONE) {
			errno = EAGAIN;
			return -1;
		}
		media->info.position_frames += frames;
		if (code == MPG123_DONE) {
			media->total_frames = media->info.position_frames;
			media->info.eof = true;
			set_duration(media);
		}
	}
	return (ssize_t)frames;
}

ssize_t fplinux_media_read(struct fplinux_media *media, int16_t *samples,
			   size_t capacity)
{
	if (!media || !samples || !capacity ||
	    capacity > FPLINUX_MEDIA_BLOCK_FRAMES) {
		errno = EINVAL;
		return -1;
	}
	if (media->info.seeking) {
		uint64_t remaining =
			media->seek_target - media->info.position_frames;
		size_t step = remaining < FPLINUX_MEDIA_BLOCK_FRAMES ?
				      (size_t)remaining :
				      FPLINUX_MEDIA_BLOCK_FRAMES;
		int16_t scratch[FPLINUX_MEDIA_BLOCK_FRAMES * 2U];
		ssize_t count = read_pcm(media, scratch, step);
		if (count < 0)
			return -1;
		media->info.seeking = !media->info.eof &&
				      media->info.position_frames <
					      media->seek_target;
		errno = EAGAIN;
		return -1;
	}
	ssize_t count = read_pcm(media, samples, capacity);
	if (count > 0 && media->gain != 100U)
		for (size_t i = 0; i < (size_t)count * media->info.channels;
		     i++)
			samples[i] = (int16_t)((int32_t)samples[i] *
					       (int)media->gain / 100);
	return count;
}

int fplinux_media_seek(struct fplinux_media *media, uint64_t position_ms)
{
	if (!media || position_ms > UINT64_MAX / 48000U) {
		errno = EINVAL;
		return -1;
	}
	uint64_t target = position_ms * media->info.rate / 1000U;
	if (media->info.duration_known && target > media->total_frames)
		target = media->total_frames;
	if (media->info.codec == FPLINUX_MEDIA_WAV) {
		media->info.position_frames = target;
		media->info.eof = target == media->total_frames;
		return 0;
	}
	if (media->amr_nb) {
		if (target < media->info.position_frames ||
		    fplinux_media_amr_nb_failed(media->amr_nb)) {
			if (fplinux_media_amr_nb_rewind(media->amr_nb) < 0)
				return -1;
			media->info.position_frames = 0;
			media->info.eof = false;
		}
	} else if (target < media->info.position_frames || media->info.eof) {
		media->io_remaining = MEDIA_IO_BYTES;
		media->io_error = 0;
		int64_t result = mpg123_seek64(media->mp3, 0, SEEK_SET);
		if (result < 0 || media->io_error) {
			errno = media->io_error ? media->io_error : EIO;
			return -1;
		}
		media->info.position_frames = (uint64_t)result;
		media->info.eof = false;
	}
	media->seek_target = target;
	media->info.seeking = target != media->info.position_frames;
	return media->info.seeking ? 1 : 0;
}

int fplinux_media_set_gain(struct fplinux_media *media, unsigned int level)
{
	if (!media || level > 100U) {
		errno = EINVAL;
		return -1;
	}
	media->gain = level;
	return 0;
}
