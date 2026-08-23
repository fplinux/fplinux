// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bootstrap.h"
#include "syscode.h"

#define UMS9117_AON_TIMER_BASE_PHYS 0x40050000U
#define UMS9117_AP_SYSCNT_PHYS 0x4023000cU
#define UMS9117_AON_APB_EB0_PHYS 0x402e0000U
#define UMS9117_AON_APB_RTC_EB_PHYS 0x402e0010U
#define UMS9117_AON_APB_CLK_EB0_PHYS 0x402e0134U
#define UMS9117_MUSB_BASE_PHYS 0x20200000U

#define BIT(n) (1U << (n))
#define UMS9117_TIMER_EB0_BITS (BIT(11) | BIT(10))
#define UMS9117_TIMER_RTC_BITS (BIT(4) | BIT(3))
#define UMS9117_TIMER_CLK_BITS BIT(11)

#define UMS9117_TIMER_LOAD_LO 0x00U
#define UMS9117_TIMER_LOAD_HI 0x04U
#define UMS9117_TIMER_VALUE_LO 0x08U
#define UMS9117_TIMER_CTL 0x10U
#define UMS9117_TIMER_INT 0x14U
#define UMS9117_TIMER_SHDW_LO 0x18U
#define UMS9117_TIMER_CTL_PERIOD BIT(0)
#define UMS9117_TIMER_CTL_ENABLE BIT(1)
#define UMS9117_TIMER_CTL_64BIT BIT(16)
#define UMS9117_TIMER_INT_EN BIT(0)
#define UMS9117_TIMER_INT_RAW BIT(1)
#define UMS9117_TIMER_INT_MASK BIT(2)
#define UMS9117_TIMER_INT_CLR BIT(3)

#define UMS9117_MUSB_POWER 0x001U
#define UMS9117_MUSB_DMA_CHANNEL(n) (0x1c00U + ((n) - 1U) * 0x20U)
#define UMS9117_MUSB_DMA_PAUSE 0x00U
#define UMS9117_MUSB_DMA_CFG 0x04U
#define UMS9117_MUSB_DMA_INTR 0x08U
#define UMS9117_MUSB_DMA_LLIST_PTR 0x14U
#define UMS9117_MUSB_DMA_CHN_EN BIT(0)
#define UMS9117_MUSB_DMA_CLEAR_INT_EN BIT(5)
#define UMS9117_MUSB_DMA_CHN_CLR BIT(15)
#define UMS9117_MUSB_DMA_CLR_STATUS BIT(21)
#define UMS9117_MUSB_DMA_INTR_CLEAR \
	(BIT(24) | BIT(25) | BIT(26) | BIT(27) | BIT(28))
#define UMS9117_MUSB_POWER_SOFTCONN BIT(6)
#define UMS9117_MUSB_RXCSR_DMA_BITS (BIT(15) | BIT(13) | BIT(11))

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

extern const unsigned char linux_zimage_start[];
extern const unsigned char linux_zimage_end[];
extern const unsigned char linux_dtb_start[];
extern const unsigned char linux_dtb_end[];
extern unsigned char fplinux_session_start[];
extern unsigned char fplinux_session_end[];

static const unsigned char fplinux_session_magic[8] = {
	'F', 'P', 'L', 'S', 'E', 'S', 'S', '\0',
};

static uint32_t reg_read(uint32_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}

static uint16_t reg_read16(uint32_t address)
{
	return *(volatile uint16_t *)(uintptr_t)address;
}

static uint8_t reg_read8(uint32_t address)
{
	return *(volatile uint8_t *)(uintptr_t)address;
}

static void reg_write(uint32_t address, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)address = value;
}

static void reg_write16(uint32_t address, uint16_t value)
{
	*(volatile uint16_t *)(uintptr_t)address = value;
}

static void reg_write8(uint32_t address, uint8_t value)
{
	*(volatile uint8_t *)(uintptr_t)address = value;
}

static void reg_or(uint32_t address, uint32_t bits)
{
	reg_write(address, reg_read(address) | bits);
}

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
	clean_dcache_range(record, record + FPLINUX_SESSION_BYTES);
}

void lcd_appinit(void)
{
	struct sys_display *display = &sys_data.display;

	display->w2 = display->w1;
	display->h2 = display->h1;
}

void keytrn_init(void)
{
	uint8_t map[64];
	int index;

	(void)sys_getkeymap(map);
	for (index = 0; index < 64; ++index) {
		sys_data.keytrn[0][index] = map[index];
		sys_data.keytrn[1][index] = map[index] | 0x8000;
	}
}

void ums9117_bootstrap_enable_timer_gates(
	struct ums9117_bootstrap_timer_gates *snapshot)
{
	if (snapshot != NULL) {
		snapshot->eb0_before = reg_read(UMS9117_AON_APB_EB0_PHYS);
		snapshot->rtc_before = reg_read(UMS9117_AON_APB_RTC_EB_PHYS);
		snapshot->clk_before = reg_read(UMS9117_AON_APB_CLK_EB0_PHYS);
	}
	reg_or(UMS9117_AON_APB_EB0_PHYS, UMS9117_TIMER_EB0_BITS);
	reg_or(UMS9117_AON_APB_RTC_EB_PHYS, UMS9117_TIMER_RTC_BITS);
	reg_or(UMS9117_AON_APB_CLK_EB0_PHYS, UMS9117_TIMER_CLK_BITS);
	__asm__ volatile("dsb sy\n\tisb" : : : "memory");
	if (snapshot != NULL) {
		snapshot->eb0_after = reg_read(UMS9117_AON_APB_EB0_PHYS);
		snapshot->rtc_after = reg_read(UMS9117_AON_APB_RTC_EB_PHYS);
		snapshot->clk_after = reg_read(UMS9117_AON_APB_CLK_EB0_PHYS);
	}
}

int ums9117_bootstrap_probe_timer(struct ums9117_bootstrap_timer_result *result)
{
	if (result == NULL)
		return 0;

	result->ctl_before =
		reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL,
		  result->ctl_before & ~UMS9117_TIMER_CTL_ENABLE);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT,
		  reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT) |
			  UMS9117_TIMER_INT_CLR);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_LOAD_HI, 0);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_LOAD_LO, 256);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT,
		  UMS9117_TIMER_INT_EN);
	result->ctl = (result->ctl_before &
		       ~(UMS9117_TIMER_CTL_PERIOD | UMS9117_TIMER_CTL_64BIT)) |
		      UMS9117_TIMER_CTL_ENABLE;

	result->syscnt_before = reg_read(UMS9117_AP_SYSCNT_PHYS);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL, result->ctl);
	result->polls = 0;
	do {
		result->int_status = reg_read(UMS9117_AON_TIMER_BASE_PHYS +
					      UMS9117_TIMER_INT);
		result->syscnt_after = reg_read(UMS9117_AP_SYSCNT_PHYS);
	} while (!(result->int_status &
		   (UMS9117_TIMER_INT_RAW | UMS9117_TIMER_INT_MASK)) &&
		 (uint32_t)(result->syscnt_after - result->syscnt_before) <
			 50 &&
		 ++result->polls < 5000000UL);

	result->value =
		reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_VALUE_LO);
	result->shadow =
		reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_SHDW_LO);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL,
		  reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL) &
			  ~UMS9117_TIMER_CTL_ENABLE);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT,
		  result->int_status | UMS9117_TIMER_INT_CLR);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT, 0);
	__asm__ volatile("dsb sy" : : : "memory");

	return result->syscnt_after != result->syscnt_before &&
	       (result->int_status &
		(UMS9117_TIMER_INT_RAW | UMS9117_TIMER_INT_MASK));
}

uint32_t ums9117_bootstrap_quiesce_usb_dma_channel(unsigned channel)
{
	uint32_t base =
		UMS9117_MUSB_BASE_PHYS + UMS9117_MUSB_DMA_CHANNEL(channel);
	uint32_t cfg;
	uint32_t intr;
	uint32_t pause;
	unsigned long polls;

	cfg = reg_read(base + UMS9117_MUSB_DMA_CFG);
	if (!(cfg & UMS9117_MUSB_DMA_CHN_EN)) {
		reg_write(base + UMS9117_MUSB_DMA_LLIST_PTR, 0);
		reg_write(base + UMS9117_MUSB_DMA_PAUSE, 0);
		__asm__ volatile("dsb sy" : : : "memory");
		return UMS9117_BOOTSTRAP_DMA_OK;
	}

	pause = reg_read(base + UMS9117_MUSB_DMA_PAUSE);
	intr = reg_read(base + UMS9117_MUSB_DMA_INTR);
	reg_write(base + UMS9117_MUSB_DMA_INTR,
		  intr | UMS9117_MUSB_DMA_CLEAR_INT_EN);
	reg_write(base + UMS9117_MUSB_DMA_PAUSE,
		  pause | UMS9117_MUSB_DMA_CHN_CLR);
	__asm__ volatile("dsb sy" : : : "memory");

	for (polls = 0; polls < 1000000UL; ++polls) {
		intr = reg_read(base + UMS9117_MUSB_DMA_INTR);
		if (intr & UMS9117_MUSB_DMA_CLR_STATUS)
			break;
	}

	reg_write(base + UMS9117_MUSB_DMA_INTR,
		  intr | UMS9117_MUSB_DMA_INTR_CLEAR);
	reg_write(base + UMS9117_MUSB_DMA_CFG,
		  reg_read(base + UMS9117_MUSB_DMA_CFG) &
			  ~UMS9117_MUSB_DMA_CHN_EN);
	reg_write(base + UMS9117_MUSB_DMA_LLIST_PTR, 0);
	reg_write(base + UMS9117_MUSB_DMA_PAUSE, 0);
	__asm__ volatile("dsb sy" : : : "memory");

	return (intr & UMS9117_MUSB_DMA_CLR_STATUS ?
			UMS9117_BOOTSTRAP_DMA_CLEAR_SEEN :
			0) |
	       (reg_read(base + UMS9117_MUSB_DMA_CFG) &
				UMS9117_MUSB_DMA_CHN_EN ?
			0 :
			UMS9117_BOOTSTRAP_DMA_DISABLED);
}

void ums9117_bootstrap_cleanup_usb_dma_and_disconnect(void)
{
	uint16_t csr;

	csr = reg_read16(UMS9117_MUSB_BASE_PHYS + 0x152U);
	csr &= ~(BIT(15) | BIT(12));
	reg_write16(UMS9117_MUSB_BASE_PHYS + 0x152U, csr);
	csr &= ~BIT(10);
	reg_write16(UMS9117_MUSB_BASE_PHYS + 0x152U, csr);
	csr = reg_read16(UMS9117_MUSB_BASE_PHYS + 0x166U);
	reg_write16(UMS9117_MUSB_BASE_PHYS + 0x166U,
		    csr & ~UMS9117_MUSB_RXCSR_DMA_BITS);
	reg_write8(UMS9117_MUSB_BASE_PHYS + UMS9117_MUSB_POWER,
		   reg_read8(UMS9117_MUSB_BASE_PHYS + UMS9117_MUSB_POWER) &
			   ~UMS9117_MUSB_POWER_SOFTCONN);
	__asm__ volatile("dsb sy\n\tisb" : : : "memory");
}

size_t ums9117_bootstrap_zimage_size(void)
{
	return (size_t)(linux_zimage_end - linux_zimage_start);
}

size_t ums9117_bootstrap_dtb_size(void)
{
	return (size_t)(linux_dtb_end - linux_dtb_start);
}

void ums9117_bootstrap_copy_zimage(uint32_t destination, size_t bytes)
{
	memmove((void *)(uintptr_t)destination, linux_zimage_start, bytes);
	clean_dcache_range((void *)(uintptr_t)destination,
			   (void *)(uintptr_t)(destination + bytes));
}

void ums9117_bootstrap_copy_dtb(uint32_t destination, size_t bytes)
{
	memcpy((void *)(uintptr_t)destination, linux_dtb_start, bytes);
	clean_dcache_range((void *)(uintptr_t)destination,
			   (void *)(uintptr_t)(destination + bytes));
}

enum ums9117_bootstrap_session_status
ums9117_bootstrap_personalize_dtb(uint32_t destination, size_t bytes)
{
	static const unsigned char rng_seed_name[] = "rng-seed";
	static const unsigned char client_key_name[] = "fplinux,ssh-client-key";
	static const unsigned char session_id_name[] = "fplinux,session-id";
	static const unsigned char usb_config_name[] = "fplinux,usb-session";
	unsigned char *record = fplinux_session_start;
	unsigned char *tree = (unsigned char *)(uintptr_t)destination;
	unsigned char *seed_marker;
	unsigned char *client_key_marker;
	unsigned char *session_id_marker;
	unsigned char *usb_config_marker;
	uintptr_t start;
	uintptr_t end;

	start = (uintptr_t)fplinux_session_start;
	end = (uintptr_t)fplinux_session_end;
	if ((start & 63U) != 0 || end < start ||
	    end - start != FPLINUX_SESSION_BYTES)
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

	if (!find_run_once(tree, bytes, FPLINUX_DTB_RNG_SEED_MARKER,
			   FPLINUX_SESSION_SEED_BYTES, &seed_marker) ||
	    !find_run_once(tree, bytes, FPLINUX_DTB_CLIENT_KEY_MARKER,
			   FPLINUX_SESSION_CLIENT_KEY_BYTES,
			   &client_key_marker) ||
	    !find_run_once(tree, bytes, FPLINUX_DTB_SESSION_ID_MARKER,
			   FPLINUX_SESSION_ID_BYTES, &session_id_marker) ||
	    !find_run_once(tree, bytes, FPLINUX_DTB_USB_CONFIG_MARKER,
			   FPLINUX_SESSION_USB_CONFIG_BYTES,
			   &usb_config_marker) ||
	    !bytes_appear_once(tree, bytes, rng_seed_name,
			       sizeof(rng_seed_name)) ||
	    !bytes_appear_once(tree, bytes, client_key_name,
			       sizeof(client_key_name)) ||
	    !bytes_appear_once(tree, bytes, session_id_name,
			       sizeof(session_id_name)) ||
	    !bytes_appear_once(tree, bytes, usb_config_name,
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
	clean_dcache_range(tree, tree + bytes);
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
	case UMS9117_BOOTSTRAP_SESSION_OK:
	default:
		return "SESSION UNKNOWN";
	}
}
