/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-media.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const unsigned char wav[] = {
	'R',  'I',  'F', 'F',  52,   0,	   0, 0, 'W',  'A',  'V', 'E',
	'f',  'm',  't', ' ',  16,   0,	   0, 0, 1,    0,    1,	  0,
	0x40, 0x1f, 0,	 0,    0x80, 0x3e, 0, 0, 2,    0,    16,  0,
	'd',  'a',  't', 'a',  16,   0,	   0, 0, 0,    0x80, 0,	  0xc0,
	0,    0,    0,	 0x40, 0xff, 0x7f, 1, 0, 0xff, 0xff, 0,	  0x20,
};

static void test_wav(void)
{
	const int16_t expected[] = { -32768, -16384, 0,	 16384,
				     32767,  1,	     -1, 8192 };
	struct fplinux_media *media =
		fplinux_media_open_bytes(wav, sizeof(wav));
	assert(media);
	const struct fplinux_media_info *info = fplinux_media_info(media);
	assert(info->codec == FPLINUX_MEDIA_WAV && info->rate == 8000 &&
	       info->channels == 1 && info->wave_container);
	assert(info->duration_known && info->duration_ms == 1 && !info->eof);
	int16_t samples[1024];
	assert(fplinux_media_read(media, samples, 8) == 8);
	assert(!memcmp(samples, expected, sizeof(expected)));
	assert(info->eof && info->position_frames == 8);
	assert(fplinux_media_read(media, samples, 8) == 0);
	assert(fplinux_media_seek(media, 0) == 0 && !info->eof);
	assert(fplinux_media_set_gain(media, 50) == 0);
	assert(fplinux_media_read(media, samples, 4) == 4);
	assert(samples[0] == -16384 && samples[1] == -8192 && samples[2] == 0 &&
	       samples[3] == 8192);
	assert(fplinux_media_seek(media, 1000) == 0 && info->eof);
	assert(fplinux_media_set_gain(media, 101) == -1 && errno == EINVAL);
	fplinux_media_close(media);

	unsigned char pcm8[52];
	memcpy(pcm8, wav, 44);
	pcm8[4] = 44;
	pcm8[28] = 0x40;
	pcm8[29] = 0x1f;
	pcm8[32] = 1;
	pcm8[34] = 8;
	pcm8[40] = 8;
	const unsigned char values[] = { 0, 64, 128, 192, 255, 127, 129, 160 };
	memcpy(pcm8 + 44, values, sizeof(values));
	media = fplinux_media_open_bytes(pcm8, sizeof(pcm8));
	assert(media && fplinux_media_read(media, samples, 8) == 8);
	const int16_t expected8[] = { -32768, -16384, 0,   16384,
				      32512,  -256,   256, 8192 };
	assert(!memcmp(samples, expected8, sizeof(expected8)));
	fplinux_media_close(media);

	pcm8[20] = 3;
	assert(!fplinux_media_open_bytes(pcm8, sizeof(pcm8)) &&
	       errno == ENOTSUP);
	assert(!fplinux_media_open_bytes(wav,
					 FPLINUX_MEDIA_RESOURCE_BYTES + 1U) &&
	       errno == EINVAL);
}

static void test_descriptor(void)
{
	int fd = memfd_create("media-input", MFD_CLOEXEC);
	assert(fd >= 0 && write(fd, wav, sizeof(wav)) == (ssize_t)sizeof(wav));
	assert(lseek(fd, 7, SEEK_SET) == 7);
	struct fplinux_media *media = fplinux_media_open_fd(fd);
	assert(media && lseek(fd, 0, SEEK_CUR) == 7);
	int16_t samples[8];
	assert(fplinux_media_read(media, samples, 8) == 8 &&
	       samples[0] == -32768);
	assert(fplinux_media_seek(media, 0) == 0);
	assert(ftruncate(fd, 44) == 0);
	assert(fplinux_media_read(media, samples, 8) == -1 && errno == EIO);
	close(fd);
	fplinux_media_close(media);
}

static void test_mp3(struct fplinux_media *media, bool wave_container)
{
	assert(media);
	const struct fplinux_media_info *info = fplinux_media_info(media);
	assert(info->codec == FPLINUX_MEDIA_MP3 && info->rate == 44100 &&
	       info->channels == 1 && info->wave_container == wave_container);
	assert(info->duration_known && info->duration_ms == 2000);
	assert(!strcmp(info->title, "Finite tone") &&
	       !strcmp(info->artist, "FPLinux tests"));
	assert(fplinux_media_seek(media, 1000) == 1);
	int16_t samples[1024];
	unsigned int steps = 0;
	while (info->seeking) {
		assert(fplinux_media_read(media, samples, 64) == -1 &&
		       errno == EAGAIN);
		assert(++steps < 100);
	}
	assert(info->position_frames == 44100 && !info->eof);
	assert(fplinux_media_read(media, samples, 64) == 64);
	const int16_t reference[] = {
		6,     748,   1487,  2221,  2946,  3659,  4358,	 5039,
		5701,  6341,  6956,  7543,  8101,  8627,  9119,	 9575,
		9994,  10374, 10712, 11009, 11263, 11472, 11636, 11755,
		11827, 11853, 11832, 11765, 11652, 11492, 11288, 11040,
		10747, 10413, 10038, 9623,  9171,  8682,  8160,	 7605,
		7021,  6408,  5771,  5111,  4431,  3734,  3022,	 2298,
		1565,  826,   84,    -659,  -1399, -2134, -2860, -3575,
		-4276, -4960, -5625, -6267, -6886, -7476, -8038, -8569,
	};
	for (size_t i = 0; i < sizeof(reference) / sizeof(reference[0]); i++)
		assert(abs(samples[i] - reference[i]) <= 32);
	while (!info->eof) {
		ssize_t count = fplinux_media_read(media, samples, 1024);
		assert(count > 0 || (count == -1 && errno == EAGAIN) ||
		       info->eof);
	}
	assert(info->position_frames == 88200 && info->duration_ms == 2000);
	assert(fplinux_media_read(media, samples, 1024) == 0);
	assert(fplinux_media_seek(media, 250) == 1);
	assert(fplinux_media_seek(media, 0) == 0 && !info->seeking &&
	       !info->eof);
	fplinux_media_close(media);
}

static void test_amr_nb(const char *encoded_path, const char *reference_path)
{
	unsigned char encoded[294];
	int16_t expected[1440];
	FILE *input = fopen(encoded_path, "rb");

	assert(input &&
	       fread(encoded, 1, sizeof(encoded), input) == sizeof(encoded));
	assert(fgetc(input) == EOF && feof(input));
	fclose(input);
	input = fopen(reference_path, "rb");
	assert(input &&
	       fread(expected, sizeof(*expected), 1440, input) == 1440);
	assert(fgetc(input) == EOF && feof(input));
	fclose(input);

	struct fplinux_media *media =
		fplinux_media_open_bytes(encoded, sizeof(encoded));
	const struct fplinux_media_info *info = fplinux_media_info(media);
	int16_t samples[1024];

	assert(info && info->codec == FPLINUX_MEDIA_AMR_NB &&
	       info->rate == 8000 && info->channels == 1 &&
	       !info->wave_container && !info->duration_known && !info->eof);
	assert(fplinux_media_read(media, samples, 1024) == 1024);
	assert(!memcmp(samples, expected, 1024 * sizeof(*samples)));
	assert(fplinux_media_read(media, samples, 401) == 401);
	assert(!memcmp(samples, expected + 1024, 401 * sizeof(*samples)));
	assert(info->duration_known && info->duration_ms == 180 && !info->eof);
	assert(fplinux_media_read(media, samples, 1024) == 15 && info->eof);
	assert(!memcmp(samples, expected + 1425, 15 * sizeof(*samples)));
	assert(info->position_frames == 1440 &&
	       fplinux_media_read(media, samples, 1024) == 0);

	assert(fplinux_media_seek(media, 140) == 1 && !info->eof);
	assert(fplinux_media_read(media, samples, 37) == -1 && errno == EAGAIN);
	assert(info->seeking && info->position_frames == 1024);
	assert(fplinux_media_seek(media, 26) == 1);
	assert(fplinux_media_read(media, samples, 37) == -1 && errno == EAGAIN);
	assert(!info->seeking && info->position_frames == 208);
	assert(fplinux_media_read(media, samples, 37) == 37);
	assert(!memcmp(samples, expected + 208, 37 * sizeof(*samples)));
	assert(fplinux_media_seek(media, 0) == 0 && !info->eof);
	assert(fplinux_media_read(media, samples, 257) == 257);
	assert(!memcmp(samples, expected, 257 * sizeof(*samples)));
	fplinux_media_close(media);

	media = fplinux_media_open_bytes(encoded, sizeof(encoded) - 1);
	assert(media && fplinux_media_read(media, samples, 1024) == 1024);
	assert(fplinux_media_read(media, samples, 1024) == 256);
	assert(fplinux_media_read(media, samples, 1024) == -1 &&
	       errno == EINVAL);
	fplinux_media_close(media);
}

static void test_ima_adpcm(const char *path, const char *reference,
			   unsigned int channels)
{
	const size_t frames = 43;
	int16_t expected[43 * 2];
	FILE *input = fopen(reference, "rb");
	assert(input);
	assert(fread(expected, sizeof(*expected), frames * channels, input) ==
	       frames * channels);
	assert(fgetc(input) == EOF && feof(input));
	fclose(input);

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	assert(fd >= 0);
	struct fplinux_media *media = fplinux_media_open_fd(fd);
	close(fd);
	const struct fplinux_media_info *info = fplinux_media_info(media);
	assert(info && info->codec == FPLINUX_MEDIA_WAV &&
	       info->wave_container && info->rate == 8000 &&
	       info->channels == channels);
	assert(info->duration_known && info->duration_ms == 5 && !info->eof);

	int16_t pcm[9 * 2];
	for (unsigned int play = 0; play < 3; play++) {
		assert(fplinux_media_seek(media, 0) == 0 && !info->eof);
		for (size_t offset = 0; offset < frames; offset += 9U) {
			size_t take = frames - offset < 9U ? frames - offset :
							     9U;
			assert(fplinux_media_read(media, pcm, 9) ==
			       (ssize_t)take);
			assert(!memcmp(pcm, expected + offset * channels,
				       take * channels * sizeof(*pcm)));
			assert(info->position_frames == offset + take);
			assert(info->eof == (offset + take == frames));
		}
		assert(fplinux_media_read(media, pcm, 9) == 0);
	}
	assert(fplinux_media_seek(media, 3) == 0 && !info->eof &&
	       !info->seeking);
	assert(info->position_frames == 24 &&
	       fplinux_media_read(media, pcm, 9) == 9);
	assert(!memcmp(pcm, expected + 24 * channels,
		       9 * channels * sizeof(*pcm)));
	assert(fplinux_media_seek(media, 99) == 0 && info->eof);
	assert(info->position_frames == frames &&
	       fplinux_media_read(media, pcm, 9) == 0);
	fplinux_media_close(media);
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "amr-nb")) {
		test_amr_nb(argv[2], argv[3]);
		puts("AMR-NB decode: ok");
		return 0;
	}
	if (argc == 5 && !strcmp(argv[1], "ima-adpcm")) {
		test_ima_adpcm(argv[2], argv[3],
			       !strcmp(argv[4], "1") ? 1U : 2U);
		puts("IMA ADPCM decode: ok");
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "reject-wav")) {
		int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
		assert(fd >= 0);
		assert(!fplinux_media_open_fd(fd) && errno == EINVAL);
		close(fd);
		return 0;
	}
	assert(argc == 3);
	test_wav();
	test_descriptor();
	int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	assert(fd >= 0);
	struct fplinux_media *file = fplinux_media_open_fd(fd);
	close(fd);
	test_mp3(file, false);

	unsigned char buffer[64 * 1024];
	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	assert(fd >= 0);
	ssize_t size = read(fd, buffer, sizeof(buffer));
	assert(size > 0 && (size_t)size < sizeof(buffer));
	close(fd);
	test_mp3(fplinux_media_open_bytes(buffer, (size_t)size), true);
	puts("media decode: ok");
	return 0;
}
