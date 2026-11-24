/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(pe_entry)

SECTIONS
{
	. = 0xbfc00000;
	.entry : { KEEP(*(.entry)) }

	. = 0xbfc00380;
	.exception : { KEEP(*(.exception)) }

	/* Zero-wait IMEM execution alias at 0x8fc00400 (file offset 0x400) */
	.text 0x8fc00400 : AT(0xbfc00400) {
		*(.text.pe_main)
		*(.text*)
	}
	ASSERT(pe_main == 0x8fc00400, "pe_main must be at 0x8fc00400")

	.rodata : {
		*(.rodata*)
	}

	.data : {
		*(.data*)
	}

	.bss : {
		*(.bss*)
		*(COMMON)
	}

	ASSERT(. <= 0x8fc08000, "PE firmware exceeds 32 KiB IMEM window")

	/DISCARD/ : {
		*(.MIPS.abiflags)
		*(.reginfo)
		*(.pdr)
		*(.comment)
		*(.note*)
		*(.got*)
		*(.eh_frame*)
	}
}
