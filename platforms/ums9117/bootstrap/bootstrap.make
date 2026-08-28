# SPDX-License-Identifier: GPL-2.0-only
# The bootstrap is a fixed RAM-only closure. Command-line make variables must
# not be able to add storage code or another payload.
override TWO_STAGE := 0
override T117 := 1
override LIBC_SDIO := 0

override VENDOR := ../vendor/fpdoom
override SYSDIR := $(VENDOR)/fpdoom
override SYSDIR_T117 := $(VENDOR)/ums9117
override PACK_RELOC := $(VENDOR)/pack_reloc/pack_reloc
override LDSCRIPT := $(CURDIR)/ums9117-bootstrap/ums9117-linux.ld
override OBJDIR := obj
override FPLINUX_BOOTSTRAP_IDENTITY_HEADER := generated/fplinux-bootstrap-identity.h
override FPLINUX_BOOT_LAYOUT_HEADER := generated/fplinux-boot-layout.h
override FPLINUX_BOOTSTRAP_MEMORY_LD := generated/fplinux-bootstrap-memory.ld
override FPLINUX_UBOOT_BUILD_HEADER := generated/fplinux-uboot-build.h

override SYS_EXTRA :=
override SYS_SRCS := asmcode usbio common libc syscomm syscode
override SYS_SRCS2 := start entry $(SYS_SRCS)
ifeq ($(FPLINUX_BOOTSTRAP_MODE),uboot)
override APP_SRCS := main fplinux-boot-screen/boot-screen ums9117-bootstrap/sd-stage0 ums9117-bootstrap/boot-common ums9117-bootstrap/bootstrap ums9117-bootstrap/handoff ums9117-bootstrap/uboot-handoff payload
else
override APP_SRCS := main fplinux-boot-screen/boot-screen ums9117-bootstrap/boot-main ums9117-bootstrap/boot-common ums9117-bootstrap/bootstrap ums9117-bootstrap/handoff payload
endif
override APP_OBJS1 := $(APP_SRCS:%=$(OBJDIR)/app/%.o)
override APP_OBJS2 :=
override OBJS := $(SYS_SRCS2:%=$(OBJDIR)/sys/%.o) $(APP_OBJS1)

override EXTRA_CFLAGS :=
override LD_EXTRA :=
override undefine CFLAGS
override undefine APP_CFLAGS
override undefine SYS_CFLAGS
override undefine LFLAGS

include $(VENDOR)/build_t117.make

override FPLINUX_VENDOR_CFLAGS := $(CFLAGS)
override FPLINUX_VENDOR_APP_CFLAGS := $(APP_CFLAGS)
override FPLINUX_VENDOR_SYS_CFLAGS := $(SYS_CFLAGS)
override FPLINUX_VENDOR_LFLAGS := $(LFLAGS)
override FPLINUX_STORAGE_OFF := -ULIBC_SDIO -DLIBC_SDIO=0 -UFAT_WRITE -DFAT_WRITE=0 -DFPLINUX_BOOTSTRAP_STORAGE_DISABLED=1
override FPLINUX_STORAGE_LDFLAG := -Wl,--defsym=FPLINUX_BOOTSTRAP_STORAGE_DISABLED=1
override CFLAGS := $(FPLINUX_VENDOR_CFLAGS) $(FPLINUX_STORAGE_OFF)
override APP_CFLAGS := $(FPLINUX_VENDOR_APP_CFLAGS) -std=c99 -pedantic -I$(CURDIR) -I$(SYSDIR) $(FPLINUX_STORAGE_OFF)
override SYS_CFLAGS := $(FPLINUX_VENDOR_SYS_CFLAGS) $(FPLINUX_STORAGE_OFF)
override LFLAGS := $(FPLINUX_VENDOR_LFLAGS) $(FPLINUX_STORAGE_LDFLAG)

ifneq ($(FPLINUX_BOOTSTRAP_MODE),uboot)
ifeq ($(strip $(FPLINUX_BOOTSTRAP_DTB)),)
$(error FPLinux bootstrap DTB is not configured)
endif
endif
ifeq ($(wildcard $(FPLINUX_BOOTSTRAP_IDENTITY_HEADER)),)
$(error FPLinux bootstrap identity header is not generated)
endif
ifeq ($(wildcard $(FPLINUX_BOOT_LAYOUT_HEADER)),)
$(error FPLinux bootstrap layout header is not generated)
endif
ifeq ($(wildcard $(FPLINUX_BOOTSTRAP_MEMORY_LD)),)
$(error FPLinux bootstrap linker memory is not generated)
endif
ifeq ($(FPLINUX_BOOTSTRAP_MODE),uboot)
ifeq ($(wildcard $(FPLINUX_UBOOT_BUILD_HEADER)),)
$(error FPLinux U-Boot build header is not generated)
endif
endif
ifneq ($(LIBC_SDIO),0)
$(error FPLinux bootstrap requires LIBC_SDIO=0)
endif
ifneq ($(strip $(SYS_SRCS)),asmcode usbio common libc syscomm syscode)
$(error FPLinux bootstrap source closure changed: $(SYS_SRCS))
endif
ifeq ($(FPLINUX_BOOTSTRAP_MODE),uboot)
ifneq ($(strip $(APP_SRCS)),main fplinux-boot-screen/boot-screen ums9117-bootstrap/sd-stage0 ums9117-bootstrap/boot-common ums9117-bootstrap/bootstrap ums9117-bootstrap/handoff ums9117-bootstrap/uboot-handoff payload)
$(error FPLinux U-Boot stage0 source closure changed: $(APP_SRCS))
endif
else
ifneq ($(strip $(APP_SRCS)),main fplinux-boot-screen/boot-screen ums9117-bootstrap/boot-main ums9117-bootstrap/boot-common ums9117-bootstrap/bootstrap ums9117-bootstrap/handoff payload)
$(error FPLinux bootstrap application closure changed: $(APP_SRCS))
endif
endif
override FPLINUX_FORBIDDEN_OBJS := $(OBJDIR)/sys/sdio.o $(OBJDIR)/sys/microfat.o
ifneq ($(filter $(FPLINUX_FORBIDDEN_OBJS),$(OBJS)),)
$(error FPLinux bootstrap selected forbidden storage objects: $(OBJS))
endif

.PHONY: fplinux-safety-check
fplinux-safety-check:
	@printf '%s\n' \
		'FPLINUX_BOOTSTRAP_STORAGE_DISABLED=1' \
		'LIBC_SDIO=$(LIBC_SDIO)' \
		'SYS_SRCS=$(SYS_SRCS)' \
		'OBJS=$(OBJS)'

.PHONY: $(FPLINUX_FORBIDDEN_OBJS)
$(FPLINUX_FORBIDDEN_OBJS):
	@printf '%s\n' 'FPLinux bootstrap forbids SDIO and microfat objects' >&2; exit 2

$(OBJDIR)/app/ums9117-bootstrap/boot-main.o: $(FPLINUX_BOOTSTRAP_IDENTITY_HEADER) $(FPLINUX_BOOT_LAYOUT_HEADER)
$(OBJDIR)/app/ums9117-bootstrap/boot-common.o: $(FPLINUX_BOOT_LAYOUT_HEADER)
$(OBJDIR)/app/ums9117-bootstrap/sd-stage0.o: $(FPLINUX_BOOTSTRAP_IDENTITY_HEADER) $(FPLINUX_BOOT_LAYOUT_HEADER) $(FPLINUX_UBOOT_BUILD_HEADER)

ifeq ($(FPLINUX_BOOTSTRAP_MODE),uboot)
$(OBJDIR)/app/payload.o: payload.s ../out/u-boot.bin | objdir
	$(call compile_asm,)
else
$(OBJDIR)/app/payload.o: payload.s ../out/zImage ../out/$(FPLINUX_BOOTSTRAP_DTB) | objdir
	$(call compile_asm,)
endif
