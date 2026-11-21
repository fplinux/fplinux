/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_IPC_H
#define FPLINUX_MRE_IPC_H

#include <stdint.h>

#include "mre-audio.h"
#include "mre-http.h"
#include "mre-vfs.h"

#define MRE_SHARED_FD 3
#define MRE_ORIGINAL_FD 4
#define MRE_ORIGINAL_PATH "/proc/self/fd/4"
#define MRE_FRAME_WIDTH 240
#define MRE_FRAME_HEIGHT 320
#define MRE_EXECUTABLE_BYTES (8U * 1024U * 1024U)
#define MRE_FILE_BYTES (1024U * 1024U)
#define MRE_FILE_BUDGET (8U * 1024U * 1024U)
#define MRE_EDITOR_UNITS 32768U
#define MRE_EDITOR_BYTES (MRE_EDITOR_UNITS * 3U)
#define MRE_CAMERA_WIDTH 240U
#define MRE_CAMERA_HEIGHT 240U
#define MRE_CAMERA_BYTES (MRE_CAMERA_WIDTH * MRE_CAMERA_HEIGHT * 2U)
#define MRE_MEDIA_BYTES 32000U

/* Internal ARM32 parent/engine records. The parent owns files and networking;
 * the engine receives copied data and opaque handles. Every selected VXP is a
 * sealed snapshot. No field grants authority based on executable identity.
 */
struct mre_shared {
	char exec_path[MRE_VFS_PATH_BYTES];
	uint32_t screen_width;
	uint32_t screen_height;
	uint32_t frame_sequence;
	uint16_t pixels[MRE_FRAME_WIDTH * MRE_FRAME_HEIGHT];
	uint32_t file_bytes;
	char file_path[MRE_VFS_PATH_BYTES];
	unsigned char file_data[MRE_FILE_BYTES];
	uint32_t editor_bytes;
	int32_t editor_max_shorts;
	int32_t editor_method;
	char editor_title[256];
	unsigned char editor_data[MRE_EDITOR_BYTES + 1U];
	int32_t selector_type;
	char selector_folder[MRE_VFS_PATH_BYTES];
	struct mre_vfs_request vfs_request;
	struct mre_vfs_response vfs_response;
	uint32_t http_available;
	struct mre_http_request http_request;
	struct mre_http_response http_response;
	struct mre_audio_request audio_request;
	struct mre_audio_response audio_response;
	uint32_t camera_bytes;
	unsigned char camera_data[MRE_CAMERA_BYTES];
	uint32_t media_bytes;
	unsigned char media_data[MRE_MEDIA_BYTES];
};

#endif
