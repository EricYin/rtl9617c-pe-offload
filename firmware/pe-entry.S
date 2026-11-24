/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Realtek RTL9617C (Taurus) Taroko MIPS32 PE bare-metal entry & exception vector.
 *
 * The Linux host driver patches the four immediate instructions at:
 *   - 0x0000 / 0x0004: lui/ori $a0 with uncached kseg1 mailbox VA
 *   - 0x0380 / 0x0384: lui/ori $t0 with uncached kseg1 mailbox VA
 *
 * Stack lives at 0x8fc09ff0 (8 KiB CP3 DMEM window 0x0fc08000..0x0fc09fff),
 * strictly inside the 28-bit ATU entry 5 window so any 512 MiB DMA address works.
 */

.set noreorder
.set noat

.section .entry,"ax"
.globl pe_entry
pe_entry:
	/* Patched by kernel to uncached mailbox VA: (dma + 0xA000) | 0xa0000000 */
	lui	$a0, 0
	ori	$a0, $a0, 0

	/* Private 8 KiB stack inside ATU entry 5 (0x0fc08000..0x0fc09fff) */
	lui	$sp, 0x8fc0
	ori	$sp, $sp, 0x9ff0

	/* Save initial CP0 $20 (Taroko CCTL) to shm->cp0_cctl_init (+0x1C) */
	mfc0	$t0, $20
	sw	$t0, 0x1c($a0)

	/* Invalidate I-cache / D-cache via Taroko CCTL bit 1 */
	li	$t1, -3
	and	$t1, $t0, $t1
	mtc0	$t1, $20
	nop
	nop
	nop
	nop
	nop
	nop
	ori	$t1, $t1, 2
	mtc0	$t1, $20
	nop
	nop
	nop
	nop
	nop
	nop

	/* Enable CP3 (Status.CU3 = bit 31) for IMEM/DMEM window programming */
	mfc0	$t0, $12
	lui	$t1, 0x8000
	or	$t0, $t0, $t1
	mtc0	$t0, $12
	nop
	nop
	nop
	nop
	nop
	nop

	/* CP3 IMEM window: 32 KiB at 28-bit 0x0fc00000..0x0fc07fff */
	lui	$t0, 0x0fc0
	.word	0x4c880000		/* mtc3 $t0, $0 (IMEM base) */
	ori	$t0, $t0, 0x7fff
	.word	0x4c880800		/* mtc3 $t0, $1 (IMEM top)  */

	/* CP3 DMEM window: 8 KiB at 28-bit 0x0fc08000..0x0fc09fff */
	lui	$t0, 0x0fc0
	ori	$t0, $t0, 0x8000
	.word	0x4c882000		/* mtc3 $t0, $4 (DMEM base) */
	ori	$t0, $t0, 0x1fff
	.word	0x4c882800		/* mtc3 $t0, $5 (DMEM top)  */

	/* Trigger Taroko IMEM fill + enable IMEM & DMEM */
	mfc0	$t0, $20
	li	$t1, -113
	and	$t0, $t0, $t1
	mtc0	$t0, $20
	nop
	nop
	nop
	ori	$t0, $t0, 0x10
	mtc0	$t0, $20
	nop
	nop
	nop
	ori	$t0, $t0, 0x440
	mtc0	$t0, $20
	nop
	nop
	nop
	nop
	nop
	nop

	/* Record post-init CCTL (+0x20), stack (+0x24), and entry PC (+0x28) */
	mfc0	$t0, $20
	sw	$t0, 0x20($a0)
	sw	$sp, 0x24($a0)

	/* Jump into zero-wait IMEM alias 0x8fc00400 (pe_main) with $a0 = mailbox */
	lui	$t9, 0x8fc0
	ori	$t9, $t9, 0x0400
	sw	$t9, 0x28($a0)
	jr	$t9
	nop

halt_loop:
	b	halt_loop
	nop

.section .exception,"ax"
pe_exception:
	/* Patched by kernel to uncached mailbox VA */
	lui	$t0, 0
	ori	$t0, $t0, 0
	mfc0	$t1, $13		/* CP0 Cause */
	sw	$t1, 0x14($t0)
	mfc0	$t1, $14		/* CP0 EPC */
	sw	$t1, 0x18($t0)
	li	$t1, 3			/* state = FAULT */
	sw	$t1, 0x0c($t0)
	.word	0x0000000f		/* sync */
exc_halt:
	b	exc_halt
	nop
