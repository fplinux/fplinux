# SPDX-License-Identifier: GPL-2.0-only
# The bootstrap is a fixed RAM-only closure. Command-line make variables must
# not be able to add storage code or another payload.
override NAME := ta1618_linux_bootstrap
override TWO_STAGE := 0
override T117 := 1
override LIBC_SDIO := 0

override VENDOR := ../vendor/fpdoom
override SYSDIR := $(VENDOR)/fpdoom
override SYSDIR_T117 := $(VENDOR)/ums9117
override PACK_RELOC := $(VENDOR)/pack_reloc/pack_reloc
override LDSCRIPT := $(CURDIR)/ums9117-bootstrap/ums9117-linux.ld
override OBJDIR := obj

override SYS_EXTRA :=
override SYS_SRCS := asmcode usbio common libc syscomm syscode
override SYS_SRCS2 := start entry $(SYS_SRCS)
override APP_SRCS := main fplinux-boot-screen/boot-screen ums9117-bootstrap/boot-main ums9117-bootstrap/bootstrap ums9117-bootstrap/handoff payload
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

ifneq ($(LIBC_SDIO),0)
$(error FPLinux bootstrap requires LIBC_SDIO=0)
endif
ifneq ($(strip $(SYS_SRCS)),asmcode usbio common libc syscomm syscode)
$(error FPLinux bootstrap source closure changed: $(SYS_SRCS))
endif
ifneq ($(strip $(APP_SRCS)),main fplinux-boot-screen/boot-screen ums9117-bootstrap/boot-main ums9117-bootstrap/bootstrap ums9117-bootstrap/handoff payload)
$(error FPLinux bootstrap application closure changed: $(APP_SRCS))
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

$(OBJDIR)/app/payload.o: payload.s ../out/zImage ../out/ta1618.dtb | objdir
	$(call compile_asm,)
