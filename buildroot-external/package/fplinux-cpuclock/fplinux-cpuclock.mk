################################################################################
#
# fplinux-cpuclock
#
################################################################################

FPLINUX_CPUCLOCK_VERSION = 1
FPLINUX_CPUCLOCK_SITE = $(BR2_EXTERNAL_FPLINUX_PATH)/package/fplinux-cpuclock
FPLINUX_CPUCLOCK_SITE_METHOD = local
FPLINUX_CPUCLOCK_LICENSE = GPL-2.0-only

FPLINUX_CPUCLOCK_CFLAGS = $(TARGET_CFLAGS) -O2 -std=c11 -Wall -Wextra -Werror
FPLINUX_CPUCLOCK_LDFLAGS = $(TARGET_LDFLAGS) -static -Wl,--build-id=none

define FPLINUX_CPUCLOCK_BUILD_CMDS
	$(TARGET_CC) $(FPLINUX_CPUCLOCK_CFLAGS) \
		-o $(@D)/fplinux-cpuclock $(@D)/fplinux-cpuclock.c \
		$(FPLINUX_CPUCLOCK_LDFLAGS)
	$(TARGET_STRIP) --strip-all $(@D)/fplinux-cpuclock
endef

define FPLINUX_CPUCLOCK_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/fplinux-cpuclock $(TARGET_DIR)/bin/fplinux-cpuclock
endef

$(eval $(generic-package))
