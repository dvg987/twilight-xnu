// Linux arm64 Image header, then a probe that records the boot state and reboots to fastboot.
	.section .text.head, "ax"
	.globl _start
_start:
	b	entry			// code0
	.long	0			// code1
	.quad	0			// text_offset
	.quad	_image_size		// image_size
	.quad	0xa			// flags: LE, 4K, anywhere
	.quad	0, 0, 0			// res2..4
	.ascii	"ARM\x64"		// magic
	.long	0			// res5
entry:
#ifdef STAGE0
	movz	x0, #0x0009
	movk	x0, #0x8400, lsl #16
	mov	x1, #1
	mov	x2, #0
	mov	x3, #0
	smc	#0
	b	.
#endif
	mov	x19, x0
	mov	x20, x1
	mov	x21, x2
	mov	x22, x3
	adr	x23, _start
	ldr	x1, =0x6f00f000		// a stack in the probe's own window
	mov	sp, x1
	mov	x0, x19
	mov	x1, x23
	bl	emmc_main
1:	wfe
	b	1b

	.globl	read_sysregs
read_sysregs:			// x0 = u64[16] out
	mrs	x1, CurrentEL;		str	x1, [x0, #0]
	mrs	x1, midr_el1;		str	x1, [x0, #8]
	mrs	x1, mpidr_el1;		str	x1, [x0, #16]
	mrs	x1, id_aa64mmfr0_el1;	str	x1, [x0, #24]
	mrs	x1, id_aa64mmfr1_el1;	str	x1, [x0, #32]
	mrs	x1, id_aa64pfr0_el1;	str	x1, [x0, #40]
	mrs	x1, id_aa64isar0_el1;	str	x1, [x0, #48]
	mrs	x1, cntfrq_el0;		str	x1, [x0, #56]
	mrs	x1, daif;		str	x1, [x0, #64]
	mrs	x2, CurrentEL
	cmp	x2, #8
	b.ne	2f
	mrs	x1, sctlr_el2;		str	x1, [x0, #72]
	mrs	x1, hcr_el2;		str	x1, [x0, #80]
	mrs	x1, vbar_el2;		str	x1, [x0, #88]
	b	3f
2:	mrs	x1, sctlr_el1;		str	x1, [x0, #72]
3:	mrs	x1, id_aa64mmfr2_el1;	str	x1, [x0, #96]
	mrs	x1, cntpct_el0;		str	x1, [x0, #104]
	mrs	x1, id_aa64isar1_el1;	str	x1, [x0, #112]
	mrs	x1, id_aa64dfr0_el1;	str	x1, [x0, #120]
	ret

	.globl	cntpct
cntpct:
	isb
	mrs	x0, cntpct_el0
	ret

	.globl	smc_call
smc_call:
	smc	#0
	ret

	.globl	dcache_clean_range	// x0 start, x1 end
dcache_clean_range:
	bic	x0, x0, #63
4:	dc	cvac, x0
	add	x0, x0, #64
	cmp	x0, x1
	b.lo	4b
	dsb	sy
	ret
