################################################################################
#
# fplinux-tyrquake
#
################################################################################

FPLINUX_TYRQUAKE_VERSION = 0.71
FPLINUX_TYRQUAKE_SOURCE = tyrquake-$(FPLINUX_TYRQUAKE_VERSION).tar.gz
FPLINUX_TYRQUAKE_SITE = https://disenchant.net/files/engine
FPLINUX_TYRQUAKE_LICENSE = GPL-2.0-or-later, MIT-0, CC0-1.0, MIT
FPLINUX_TYRQUAKE_LICENSE_FILES = gnu.txt external/dr_flac.h external/dr_mp3.h \
	external/dr_wav.h external/stb_vorbis.c

FPLINUX_TYRQUAKE_ENGINE_CFLAGS = $(TARGET_CFLAGS) -std=gnu99 -Wall \
	-Wno-trigraphs -Wwrite-strings -DFPLINUX_DEFAULT_HEAP_MB=32 \
	-DFPLINUX_NO_STDIN=1

FPLINUX_TYRQUAKE_ENGINE_LDFLAGS = $(TARGET_LDFLAGS) -Wl,--build-id=none

FPLINUX_TYRQUAKE_PACKAGE = \
	$(BR2_EXTERNAL_FPLINUX_PATH)/package/fplinux-tyrquake
FPLINUX_TYRQUAKE_LAUNCHER = $(FPLINUX_TYRQUAKE_PACKAGE)/fplinux-quake.c
FPLINUX_TYRQUAKE_DEVICE = $(FPLINUX_TYRQUAKE_PACKAGE)/fplinux-device.h
FPLINUX_TYRQUAKE_INPUT = $(FPLINUX_TYRQUAKE_PACKAGE)/in_fplinux.c
FPLINUX_TYRQUAKE_VIDEO = $(FPLINUX_TYRQUAKE_PACKAGE)/vid_fplinux.c

FPLINUX_TYRQUAKE_LAUNCHER_CFLAGS = $(TARGET_CFLAGS) -std=gnu11 -Wall \
	-Wextra -Werror

define FPLINUX_TYRQUAKE_BUILD_CMDS
	$(INSTALL) -D -m 0644 $(FPLINUX_TYRQUAKE_DEVICE) \
		$(@D)/common/fplinux-device.h
	$(INSTALL) -D -m 0644 $(FPLINUX_TYRQUAKE_INPUT) \
		$(@D)/common/in_fplinux.c
	$(INSTALL) -D -m 0644 $(FPLINUX_TYRQUAKE_VIDEO) \
		$(@D)/common/vid_fplinux.c
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		STRIP="$(TARGET_STRIP)" \
		CFLAGS="$(FPLINUX_TYRQUAKE_ENGINE_CFLAGS)" \
		COMMON_LFLAGS="$(FPLINUX_TYRQUAKE_ENGINE_LDFLAGS)" \
		TARGET_OS=UNIX \
		TARGET_UNIX=linux \
		LOCALBASE= \
		USE_X86_ASM=N \
		VID_TARGET=fplinux \
		IN_TARGET=fplinux \
		SND_TARGET=null \
		CD_TARGET=null \
		QBASEDIR=/tmp/fplinux-quake \
		TYR_VERSION=v$(FPLINUX_TYRQUAKE_VERSION) \
		TYR_VERSION_TIME="$(SOURCE_DATE_EPOCH)" \
		bin/tyr-quake
	$(TARGET_CC) $(FPLINUX_TYRQUAKE_LAUNCHER_CFLAGS) \
		$(FPLINUX_TYRQUAKE_LAUNCHER) $(TARGET_LDFLAGS) \
		-Wl,--build-id=none -o $(@D)/bin/fplinux-quake
endef

define FPLINUX_TYRQUAKE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bin/tyr-quake \
		$(TARGET_DIR)/usr/bin/tyr-quake
	$(INSTALL) -D -m 0755 $(@D)/bin/fplinux-quake \
		$(TARGET_DIR)/usr/bin/quake
endef

$(eval $(generic-package))
