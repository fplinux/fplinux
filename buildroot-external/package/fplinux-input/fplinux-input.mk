################################################################################
#
# fplinux-input
#
################################################################################

FPLINUX_INPUT_VERSION = 1
FPLINUX_INPUT_SITE = $(BR2_EXTERNAL_FPLINUX_PATH)/package/fplinux-input
FPLINUX_INPUT_SITE_METHOD = local
FPLINUX_INPUT_LICENSE = GPL-2.0-only

FPLINUX_INPUT_CFLAGS = $(TARGET_CFLAGS) -Os -std=c11 -Wall -Wextra -Werror
FPLINUX_INPUT_LDFLAGS = $(TARGET_LDFLAGS) -static -Wl,--build-id=none

define FPLINUX_INPUT_BUILD_CMDS
	$(TARGET_CC) $(FPLINUX_INPUT_CFLAGS) \
		-o $(@D)/fplinux-input $(@D)/fplinux-input.c \
		$(FPLINUX_INPUT_LDFLAGS)
	$(TARGET_STRIP) --strip-all $(@D)/fplinux-input
endef

define FPLINUX_INPUT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/fplinux-input $(TARGET_DIR)/bin/fplinux-input
endef

$(eval $(generic-package))
