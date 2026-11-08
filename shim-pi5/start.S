/*
 * Pi 5 boot shim entry. The Pi firmware loads this as the "kernel"
 * (arm64 Linux Image header) and jumps here with x0 = DTB, MMU and
 * D-cache off. On Pi 5 the firmware's TF-A hands over at EL2; XNU wants
 * EL1, so drop there first. PSCI CPU_ON also starts secondaries at EL2.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
/*
 * If at EL2: make EL1 AArch64, stop trapping timers/FP, eret to x0
 * at EL1h with DAIF masked. If already at EL1: branch to x0.
 * Clobbers x1, x2. x3 = entry EL (1 or 2), for reporting.
 */
drop_to_el1:
	mrs	x1, CurrentEL
	lsr	x3, x1, #2
	cmp	x3, #2
	b.ne	1f
	mov	x1, #(1 << 31)		/* HCR_EL2.RW: EL1 is AArch64, nothing else */
	msr	hcr_el2, x1
	mov	x1, #3			/* CNTHCTL_EL2: EL1PCTEN | EL1PCEN */
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	mov	x1, #0x33ff		/* CPTR_EL2: no FP/SIMD traps */
	msr	cptr_el2, x1
	msr	hstr_el2, xzr
	mrs	x1, midr_el1		/* EL1 sees the real IDs */
	msr	vpidr_el2, x1
	mrs	x1, mpidr_el1
	msr	vmpidr_el2, x1
	ldr	x1, =0x30d00800		/* SCTLR_EL1: RES1 bits, MMU/caches off */
	msr	sctlr_el1, x1
	mov	x1, #0x3c5		/* SPSR: EL1h, D A I F masked */
	msr	spsr_el2, x1
	msr	elr_el2, x0
	isb
	eret
1:	br	x0

	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer from the firmware */
	adr	x0, primary_el1
	b	drop_to_el1
primary_el1:
	mov	x20, x3			/* entry EL */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	mov	x1, #(3 << 20)		/* CPACR_EL1.FPEN */
	msr	cpacr_el1, x1
	isb
	adr	x1, _bss_start
	adr	x2, _bss_end
2:	cmp	x1, x2
	b.hs	3f
	str	xzr, [x1], #8
	b	2b
3:	mov	x0, x19
	mov	x1, x20
	bl	shim_main
4:	wfe
	b	4b

/* PSCI CPU_ON target. x0 = context id = cpu index (1..3). */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mov	x19, x0
	adr	x0, secondary_el1
	b	drop_to_el1
secondary_el1:
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x19, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	mov	x0, x19
	mov	x1, x3
	bl	secondary_main
5:	wfe
	b	5b

/*
 * Exception vectors: each slot saves x29/x30, loads its number, then the
 * common path saves x0-x28 and calls exc_handler(slot, regs). Returns
 * with eret (IRQs); fatal ones never return.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret

	.ltorg
