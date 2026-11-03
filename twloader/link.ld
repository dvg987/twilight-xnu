ENTRY(_start)
SECTIONS {
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.dynamic : { *(.dynamic) }
	.got : { *(.got*) }
	. = ALIGN(8);
	__rela_start = .;
	.rela.dyn : { *(.rela*) }
	__rela_end = .;
	. = ALIGN(4096);
	__bss_start = .;
	.bss (NOLOAD) : { *(.bss*) *(COMMON) }
	. = ALIGN(4096);
	__bss_end = .;
	_image_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.interp) }
}
