// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "boot-session.h"
#include "bootstrap.h"

#define FPLINUX_SESSION_BYTES 512U
#define FPLINUX_SESSION_MAGIC_OFFSET 0x000U
#define FPLINUX_SESSION_HEADER_RESERVED_OFFSET 0x008U
#define FPLINUX_SESSION_HEADER_RESERVED_BYTES 4U
#define FPLINUX_SESSION_SIZE_OFFSET 0x00cU
#define FPLINUX_SESSION_ID_OFFSET 0x010U
#define FPLINUX_SESSION_ID_BYTES 32U
#define FPLINUX_SESSION_SEED_OFFSET 0x030U
#define FPLINUX_SESSION_SEED_BYTES 64U
#define FPLINUX_SESSION_CLIENT_KEY_OFFSET 0x070U
#define FPLINUX_SESSION_CLIENT_KEY_BYTES 68U
#define FPLINUX_SESSION_USB_CONFIG_OFFSET 0x0b4U
#define FPLINUX_SESSION_USB_CONFIG_BYTES 256U
#define FPLINUX_SESSION_RESERVED_OFFSET 0x1b4U
#define FPLINUX_SESSION_RESERVED_BYTES 72U
#define FPLINUX_SESSION_CRC_OFFSET 0x1fcU

#define FPLINUX_DTB_RNG_SEED_MARKER 0xa1U
#define FPLINUX_DTB_CLIENT_KEY_MARKER 0xb2U
#define FPLINUX_DTB_SESSION_ID_MARKER 0xc3U
#define FPLINUX_DTB_USB_CONFIG_MARKER 0xd4U

extern unsigned char fplinux_session_start[];
extern unsigned char fplinux_session_end[];

static const unsigned char fplinux_session_magic[8] = {
	'F', 'P', 'L', 'S', 'E', 'S', 'S', '\0',
};

static uint32_t read_le32(const unsigned char *source)
{
	return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
	       ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

static int bytes_are_zero(const unsigned char *bytes, size_t count)
{
	size_t index;

	for (index = 0; index < count; ++index) {
		if (bytes[index] != 0)
			return 0;
	}
	return 1;
}

static uint32_t crc32_ieee(const unsigned char *bytes, size_t count)
{
	uint32_t crc = 0xffffffffU;
	size_t index;
	unsigned bit;

	for (index = 0; index < count; ++index) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (crc & 1U ? 0xedb88320U : 0U);
	}
	return crc ^ 0xffffffffU;
}

static int base64_value(unsigned char character)
{
	if (character >= 'A' && character <= 'Z')
		return character - 'A';
	if (character >= 'a' && character <= 'z')
		return character - 'a' + 26;
	if (character >= '0' && character <= '9')
		return character - '0' + 52;
	if (character == '+')
		return 62;
	if (character == '/')
		return 63;
	return -1;
}

static int valid_ssh_client_key(const unsigned char *encoded)
{
	static const unsigned char prefix[] = {
		0x00, 0x00, 0x00, 0x0b, 's', 's',  'h',	 '-',  'e',  'd',
		'2',  '5',  '5',  '1',	'9', 0x00, 0x00, 0x00, 0x20,
	};
	unsigned char decoded[51];
	size_t input;
	size_t output = 0;
	int first;
	int second;
	int third;
	int fourth;

	for (input = 0; input < FPLINUX_SESSION_CLIENT_KEY_BYTES; input += 4) {
		first = base64_value(encoded[input]);
		second = base64_value(encoded[input + 1]);
		third = base64_value(encoded[input + 2]);
		fourth = base64_value(encoded[input + 3]);
		if (first < 0 || second < 0 || third < 0 || fourth < 0)
			return 0;
		decoded[output++] =
			(unsigned char)((first << 2) | (second >> 4));
		decoded[output++] =
			(unsigned char)((second << 4) | (third >> 2));
		decoded[output++] = (unsigned char)((third << 6) | fourth);
	}
	return output == sizeof(decoded) &&
	       memcmp(decoded, prefix, sizeof(prefix)) == 0 &&
	       !bytes_are_zero(decoded + sizeof(prefix),
			       sizeof(decoded) - sizeof(prefix));
}

static int valid_usb_config(const unsigned char *config)
{
	size_t end = 0;
	size_t index;

	while (end < FPLINUX_SESSION_USB_CONFIG_BYTES && config[end] != 0)
		++end;
	if (end == 0 || end == FPLINUX_SESSION_USB_CONFIG_BYTES ||
	    config[end - 1] != '\n')
		return 0;
	for (index = 0; index < end; ++index) {
		if (config[index] != '\n' &&
		    (config[index] < 0x20U || config[index] > 0x7eU))
			return 0;
	}
	return bytes_are_zero(config + end,
			      FPLINUX_SESSION_USB_CONFIG_BYTES - end);
}

static int find_run_once(unsigned char *bytes, size_t count,
			 unsigned char value, size_t run, unsigned char **match)
{
	size_t found = 0;
	size_t offset;
	size_t index;

	if (run == 0 || count < run)
		return 0;
	for (offset = 0; offset <= count - run; ++offset) {
		for (index = 0; index < run; ++index) {
			if (bytes[offset + index] != value)
				break;
		}
		if (index != run)
			continue;
		*match = bytes + offset;
		if (++found > 1)
			return 0;
	}
	return found == 1;
}

static int bytes_appear_once(unsigned char *bytes, size_t count,
			     const unsigned char *needle, size_t needle_bytes)
{
	size_t found = 0;
	size_t offset;

	if (needle_bytes == 0 || count < needle_bytes)
		return 0;
	for (offset = 0; offset <= count - needle_bytes; ++offset) {
		if (memcmp(bytes + offset, needle, needle_bytes) != 0)
			continue;
		if (++found > 1)
			return 0;
	}
	return found == 1;
}

static void clear_session_record(unsigned char *record)
{
	volatile unsigned char *bytes = record;
	size_t index;

	for (index = 0; index < FPLINUX_SESSION_BYTES; ++index)
		bytes[index] = 0;
	ums9117_bootstrap_flush_dcache_range(record, FPLINUX_SESSION_BYTES);
}

enum ums9117_bootstrap_session_status ums9117_boot_session_copy_to_dtb(
	const unsigned char *record, size_t record_bytes, unsigned char *tree,
	size_t tree_bytes, uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	static const unsigned char rng_seed_name[] = "rng-seed";
	static const unsigned char client_key_name[] = "fplinux,ssh-client-key";
	static const unsigned char session_id_name[] = "fplinux,session-id";
	static const unsigned char usb_config_name[] = "fplinux,usb-session";
	unsigned char *seed_marker;
	unsigned char *client_key_marker;
	unsigned char *session_id_marker;
	unsigned char *usb_config_marker;

	if (session_id == NULL)
		return UMS9117_BOOTSTRAP_SESSION_OUTPUT;
	if (((uintptr_t)record & 63U) != 0 ||
	    record_bytes != FPLINUX_SESSION_BYTES)
		return UMS9117_BOOTSTRAP_SESSION_LAYOUT;
	if (memcmp(record + FPLINUX_SESSION_MAGIC_OFFSET, fplinux_session_magic,
		   sizeof(fplinux_session_magic)) != 0)
		return UMS9117_BOOTSTRAP_SESSION_MAGIC;
	if (read_le32(record + FPLINUX_SESSION_SIZE_OFFSET) !=
	    FPLINUX_SESSION_BYTES)
		return UMS9117_BOOTSTRAP_SESSION_SIZE;
	if (read_le32(record + FPLINUX_SESSION_CRC_OFFSET) !=
	    crc32_ieee(record, FPLINUX_SESSION_CRC_OFFSET))
		return UMS9117_BOOTSTRAP_SESSION_CRC;
	if (!bytes_are_zero(record + FPLINUX_SESSION_HEADER_RESERVED_OFFSET,
			    FPLINUX_SESSION_HEADER_RESERVED_BYTES) ||
	    !bytes_are_zero(record + FPLINUX_SESSION_RESERVED_OFFSET,
			    FPLINUX_SESSION_RESERVED_BYTES))
		return UMS9117_BOOTSTRAP_SESSION_RESERVED;
	if (bytes_are_zero(record + FPLINUX_SESSION_ID_OFFSET,
			   FPLINUX_SESSION_ID_BYTES))
		return UMS9117_BOOTSTRAP_SESSION_ID;
	if (bytes_are_zero(record + FPLINUX_SESSION_SEED_OFFSET,
			   FPLINUX_SESSION_SEED_BYTES))
		return UMS9117_BOOTSTRAP_SESSION_SEED;
	if (!valid_ssh_client_key(record + FPLINUX_SESSION_CLIENT_KEY_OFFSET))
		return UMS9117_BOOTSTRAP_SESSION_CLIENT_KEY;
	if (!valid_usb_config(record + FPLINUX_SESSION_USB_CONFIG_OFFSET))
		return UMS9117_BOOTSTRAP_SESSION_USB_CONFIG;

	if (!find_run_once(tree, tree_bytes, FPLINUX_DTB_RNG_SEED_MARKER,
			   FPLINUX_SESSION_SEED_BYTES, &seed_marker) ||
	    !find_run_once(tree, tree_bytes, FPLINUX_DTB_CLIENT_KEY_MARKER,
			   FPLINUX_SESSION_CLIENT_KEY_BYTES,
			   &client_key_marker) ||
	    !find_run_once(tree, tree_bytes, FPLINUX_DTB_SESSION_ID_MARKER,
			   FPLINUX_SESSION_ID_BYTES, &session_id_marker) ||
	    !find_run_once(tree, tree_bytes, FPLINUX_DTB_USB_CONFIG_MARKER,
			   FPLINUX_SESSION_USB_CONFIG_BYTES,
			   &usb_config_marker) ||
	    !bytes_appear_once(tree, tree_bytes, rng_seed_name,
			       sizeof(rng_seed_name)) ||
	    !bytes_appear_once(tree, tree_bytes, client_key_name,
			       sizeof(client_key_name)) ||
	    !bytes_appear_once(tree, tree_bytes, session_id_name,
			       sizeof(session_id_name)) ||
	    !bytes_appear_once(tree, tree_bytes, usb_config_name,
			       sizeof(usb_config_name)))
		return UMS9117_BOOTSTRAP_SESSION_DTB;

	memcpy(seed_marker, record + FPLINUX_SESSION_SEED_OFFSET,
	       FPLINUX_SESSION_SEED_BYTES);
	memcpy(client_key_marker, record + FPLINUX_SESSION_CLIENT_KEY_OFFSET,
	       FPLINUX_SESSION_CLIENT_KEY_BYTES);
	memcpy(session_id_marker, record + FPLINUX_SESSION_ID_OFFSET,
	       FPLINUX_SESSION_ID_BYTES);
	memcpy(usb_config_marker, record + FPLINUX_SESSION_USB_CONFIG_OFFSET,
	       FPLINUX_SESSION_USB_CONFIG_BYTES);
	memcpy(session_id, record + FPLINUX_SESSION_ID_OFFSET,
	       FPLINUX_HANDOFF_SESSION_ID_BYTES);
	return UMS9117_BOOTSTRAP_SESSION_OK;
}

enum ums9117_bootstrap_session_status ums9117_bootstrap_personalize_dtb(
	uint32_t destination, size_t bytes,
	uint8_t session_id[FPLINUX_HANDOFF_SESSION_ID_BYTES])
{
	unsigned char *record = fplinux_session_start;
	unsigned char *tree = (unsigned char *)(uintptr_t)destination;
	uintptr_t start = (uintptr_t)fplinux_session_start;
	uintptr_t end = (uintptr_t)fplinux_session_end;
	size_t record_bytes = end < start ? 0 : (size_t)(end - start);
	enum ums9117_bootstrap_session_status status;

	status = ums9117_boot_session_copy_to_dtb(record, record_bytes, tree,
						  bytes, session_id);
	if (status != UMS9117_BOOTSTRAP_SESSION_OK)
		return status;
	ums9117_bootstrap_flush_dcache_range(tree, bytes);
	clear_session_record(record);
	return UMS9117_BOOTSTRAP_SESSION_OK;
}

const char *
ums9117_bootstrap_session_error(enum ums9117_bootstrap_session_status status)
{
	switch (status) {
	case UMS9117_BOOTSTRAP_SESSION_LAYOUT:
		return "SESSION LAYOUT";
	case UMS9117_BOOTSTRAP_SESSION_MAGIC:
		return "SESSION MAGIC";
	case UMS9117_BOOTSTRAP_SESSION_SIZE:
		return "SESSION SIZE";
	case UMS9117_BOOTSTRAP_SESSION_CRC:
		return "SESSION CRC";
	case UMS9117_BOOTSTRAP_SESSION_RESERVED:
		return "SESSION RESERVED";
	case UMS9117_BOOTSTRAP_SESSION_ID:
		return "SESSION ID";
	case UMS9117_BOOTSTRAP_SESSION_SEED:
		return "SESSION RNG SEED";
	case UMS9117_BOOTSTRAP_SESSION_CLIENT_KEY:
		return "SESSION CLIENT KEY";
	case UMS9117_BOOTSTRAP_SESSION_USB_CONFIG:
		return "SESSION USB CONFIG";
	case UMS9117_BOOTSTRAP_SESSION_DTB:
		return "SESSION DTB MARKERS";
	case UMS9117_BOOTSTRAP_SESSION_OUTPUT:
		return "SESSION OUTPUT";
	case UMS9117_BOOTSTRAP_SESSION_OK:
	default:
		return "SESSION UNKNOWN";
	}
}
