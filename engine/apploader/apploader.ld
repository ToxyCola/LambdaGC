/* The header sits just before 0x81200000 so the code itself starts exactly there. */
ENTRY(apploader_entry)
SECTIONS
{
	. = 0x81200000 - 0x20;
	.header : { KEEP(*(.header)) }
	_code_start = .;
	.text : { *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) *(.sdata*) *(.sdata2*) }
	. = ALIGN(32);
	_code_end = .;
	_code_size = _code_end - _code_start;
	.bss : { *(.sbss*) *(.bss*) *(COMMON) }
	/DISCARD/ : { *(.comment) *(.eh_frame*) }
}
