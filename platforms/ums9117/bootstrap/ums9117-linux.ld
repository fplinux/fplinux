/* SPDX-License-Identifier: Unlicense */
/* UMS9117 fpdoom FDL2 layout with room for a Linux zImage and initramfs. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)

INCLUDE generated/fplinux-bootstrap-memory.ld

ENTRY(_start)
SECTIONS {
	.text IMAGE_START : {
		__image_start = .;
		*(.text._start) *(.text .text.*)
		. = ALIGN(4);
		__init_array_start = .;
		KEEP(*(.init_array.* .ctors.*))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		*(.data .data.* .gnu.linkonce.d.*)
		. = ALIGN(64);
		*(.fplinux_payload)
		. = ALIGN(64);
		__image_end = .;
	}
	.bss : {
		__bss_start = .;
		*(.bss .bss.* .gnu.linkonce.b.*) *(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	}

	__image_size = __image_end - __image_start;
	__bss_size = __bss_end - __image_end;
	__stack_top = EXTRA_START;
	__stack_bottom = __stack_top + EXTRA_SIZE;

	ASSERT(__bss_end - IMAGE_START <= IMAGE_SIZE, "image overflow")
	.dynamic : { *(.dynamic) }
	/DISCARD/ : { *(.ARM.exidx .ARM.exidx.*) }
}
