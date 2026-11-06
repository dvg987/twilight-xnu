ENTRY(_start)
SECTIONS {
	. = 0;
	.text : { *(.text.head) *(.text*) *(.rodata*) }
	.data : { *(.data*) *(.bss*) *(COMMON) }
	_image_size = ALIGN(4096);
}
