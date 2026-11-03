/*
 * twloader: started by U-Boot (or QEMU) as a Linux arm64 Image: EL2 or EL1, MMU off, x0 = FDT.
 */
	.section .text.head, "ax"
	.globl	_start
_start:
	b	entry
	.long	0
	.quad	0			/* text_offset */
	.quad	_image_end - _start	/* image_size (with .bss) */
	.quad	0xa			/* LE, 4K, anywhere */
	.quad	0, 0, 0
	.ascii	"ARM\x64"
	.long	0

entry:
	msr	daifset, #0xf
	mov	x19, x0
	adrp	x1, boot_stack_top
	add	x1, x1, :lo12:boot_stack_top
	mov	sp, x1
	/* linked at 0, run wherever the firmware put us: apply our own R_AARCH64_RELATIVE relocations */
	adr	x4, _start
	adrp	x5, __rela_start
	add	x5, x5, :lo12:__rela_start
	adrp	x6, __rela_end
	add	x6, x6, :lo12:__rela_end
0:	cmp	x5, x6
	b.hs	9f
	ldp	x7, x8, [x5], #24	/* r_offset, r_info */
	ldur	x9, [x5, #-8]		/* r_addend */
	cmp	w8, #1027		/* R_AARCH64_RELATIVE */
	b.ne	0b
	add	x9, x9, x4
	str	x9, [x4, x7]
	b	0b
9:
	/* .bss is ours to clear (QEMU and U-Boot copy only the file) */
	adrp	x2, __bss_start
	add	x2, x2, :lo12:__bss_start
	adrp	x3, __bss_end
	add	x3, x3, :lo12:__bss_end
1:	cmp	x2, x3
	b.hs	2f
	str	xzr, [x2], #8
	b	1b
2:	mrs	x2, CurrentEL
	cmp	x2, #8
	b.ne	3f
	/* our own EL2 vectors: a stray exception there is reported instead of running into U-Boot's */
	adrp	x2, el2_vectors
	add	x2, x2, :lo12:el2_vectors
	msr	vbar_el2, x2
	isb
3:	mov	x0, x19
	adr	x1, _start
	bl	twl_main
4:	wfe
	b	4b

	.text
/* u64 smc_call(fn, a1, a2, a3) / hvc_call */
	.globl	smc_call
smc_call:
	smc	#0
	ret
	.globl	hvc_call
hvc_call:
	hvc	#0
	ret

	.globl	read_cntpct
read_cntpct:
	isb
	mrs	x0, cntpct_el0
	ret

	.globl	current_el
current_el:
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	ret

	.globl	read_mpidr
read_mpidr:
	mrs	x0, mpidr_el1
	ret

	.globl	read_cntfrq
read_cntfrq:
	mrs	x0, cntfrq_el0
	ret

/* void dcache_clean(start, end): to the point of coherency */
	.globl	dcache_clean
dcache_clean:
	bic	x0, x0, #63
1:	dc	cvac, x0
	add	x0, x0, #64
	cmp	x0, x1
	b.lo	1b
	dsb	sy
	ret

/* void dcache_inval(start, end) */
	.globl	dcache_inval
dcache_inval:
	bic	x0, x0, #63
1:	dc	civac, x0
	add	x0, x0, #64
	cmp	x0, x1
	b.lo	1b
	dsb	sy
	ret

/*
 * void enter_kernel(entry, boot_args): from EL2, the kernel gets a plain EL1; from EL1, a jump.
 */
/* EL2 set up for a plain EL1 below it: nothing trapped, timers and counters EL1's, MMU off at EL1 */
.macro	el1_setup
	mov	x2, #(1 << 31)		/* HCR_EL2.RW: EL1 is AArch64 */
	msr	hcr_el2, x2
	mrs	x2, cnthctl_el2
	orr	x2, x2, #3		/* EL1PCTEN, EL1PCEN */
	msr	cnthctl_el2, x2
	msr	cntvoff_el2, xzr
	mrs	x2, midr_el1
	msr	vpidr_el2, x2
	mrs	x2, mpidr_el1
	msr	vmpidr_el2, x2
	mov	x2, #0x33ff		/* CPTR_EL2: FP/SIMD not trapped */
	msr	cptr_el2, x2
	msr	hstr_el2, xzr
	mrs	x2, mdcr_el2
	and	x2, x2, #0x1f		/* HPMN kept, no debug or PMU traps */
	msr	mdcr_el2, x2
	ldr	x2, =0x30d00800		/* SCTLR_EL1: RES1 bits, MMU and caches off */
	msr	sctlr_el1, x2
	adrp	x2, el2_vectors
	add	x2, x2, :lo12:el2_vectors
	msr	vbar_el2, x2
	mov	x2, #0x3c5		/* EL1h, DAIF masked */
	msr	spsr_el2, x2
.endm

/* void enter_kernel(entry, boot_args): from EL2 the kernel gets a plain EL1; from EL1, a jump */
	.globl	enter_kernel
enter_kernel:
	msr	daifset, #0xf
	ic	iallu
	dsb	sy
	isb
	mrs	x2, CurrentEL
	cmp	x2, #8
	b.ne	1f
	el1_setup
	msr	elr_el2, x0
	mov	x0, x1
	isb
	eret
1:	mov	x2, x0
	mov	x0, x1
	br	x2

/*
 * The kernel's other cores (osfmk/arm64/psci_smp.c): PSCI starts a core at EL2, the kernel's
 * reset vector wants EL1. CPU_ON(core, here, the kernel's entry): x0 = where to go at EL1, with x0 = 0.
 */
	.globl	secondary_trampoline
secondary_trampoline:
	msr	daifset, #0xf
	mrs	x2, CurrentEL
	cmp	x2, #8
	b.ne	2f
	el1_setup
	msr	elr_el2, x0
	mov	x0, #0
	isb
	eret
2:	mov	x2, x0
	mov	x0, #0
	br	x2

/*
 * The kernel's way back (osfmk/arm64/loader_return.c): EL1, MMU on, this 2 MB block mapped 1:1
 * through TTBR0. x0 = log PA, x1 = length, x2 = reason, x3..x7 = what the reason says.
 */
	.globl	loader_return_stub
loader_return_stub:
	msr	daifset, #0xf
	mrs	x9, sctlr_el1
	bic	x9, x9, #1		/* MMU off */
	bic	x9, x9, #4		/* and the data cache */
	msr	sctlr_el1, x9
	isb
	ic	iallu
	dsb	sy
	isb
	adrp	x9, return_stack_top
	add	x9, x9, :lo12:return_stack_top
	mov	sp, x9
	bl	twl_kernel_returned
5:	wfe
	b	5b

/* the monitor core, started by PSCI CPU_ON at EL2 (or EL1) with x0 = its stack */
	.globl	monitor_entry
monitor_entry:
	msr	daifset, #0xf
	mov	sp, x0
	mrs	x2, CurrentEL
	cmp	x2, #8
	b.ne	6f
	adrp	x2, el2_vectors
	add	x2, x2, :lo12:el2_vectors
	msr	vbar_el2, x2
	isb
6:	bl	twl_monitor
7:	wfe
	b	7b

/* u64 el2_mmu_on(ttbr, tcr, mair): identity map with the caches on, for the monitor */
	.globl	el2_mmu_on
el2_mmu_on:
	msr	mair_el2, x2
	msr	tcr_el2, x1
	msr	ttbr0_el2, x0
	isb
	tlbi	alle2
	dsb	sy
	isb
	mrs	x3, sctlr_el2
	orr	x3, x3, #1		/* M */
	orr	x3, x3, #4		/* C */
	orr	x3, x3, #(1 << 12)	/* I */
	msr	sctlr_el2, x3
	isb
	ret

/* EL2 exception vectors: report and stop */
	.balign	2048
el2_vectors:
	.rept	16
	.balign	128
	mrs	x0, esr_el2
	mrs	x1, elr_el2
	mrs	x2, far_el2
	b	el2_trap
	.endr
el2_trap:
	adrp	x9, return_stack_top
	add	x9, x9, :lo12:return_stack_top
	mov	sp, x9
	bl	twl_el2_exception
8:	wfe
	b	8b

	.bss
	.balign	16
	.space	0x10000
boot_stack_top:
	.space	0x4000
return_stack_top:
