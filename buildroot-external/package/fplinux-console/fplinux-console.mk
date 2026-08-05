################################################################################
#
# fplinux-console
#
################################################################################

FPLINUX_CONSOLE_VERSION = 1
FPLINUX_CONSOLE_SITE = $(BR2_EXTERNAL_FPLINUX_PATH)/package/fplinux-console
FPLINUX_CONSOLE_SITE_METHOD = local
FPLINUX_CONSOLE_LICENSE = GPL-2.0-only

FPLINUX_CONSOLE_CFLAGS = $(TARGET_CFLAGS) -Os -std=c11 -Wall -Wextra -Werror
FPLINUX_CONSOLE_LDFLAGS = $(TARGET_LDFLAGS) -static -Wl,--build-id=none

define FPLINUX_CONSOLE_BUILD_CMDS
	$(TARGET_CC) $(FPLINUX_CONSOLE_CFLAGS) \
		-o $(@D)/fplinux-console $(@D)/fplinux-console.c \
		$(FPLINUX_CONSOLE_LDFLAGS)
	$(TARGET_STRIP) --strip-all $(@D)/fplinux-console
endef

define FPLINUX_CONSOLE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/fplinux-console $(TARGET_DIR)/bin/fplinux-console
endef

$(eval $(generic-package))
