include settings.make
all:
	@printf '%s\n' '$(NAME)' '$(FPLINUX_BOOTSTRAP_MAP)' '$(FPLINUX_BOOTSTRAP_DTB)' '$(FPLINUX_BOOTSTRAP_MODE)' '$(FPLINUX_BOOT_FONT)'
