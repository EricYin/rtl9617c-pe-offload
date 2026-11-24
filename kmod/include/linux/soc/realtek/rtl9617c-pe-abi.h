/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared ARM64 <-> Taroko MIPS32 PE mailbox ABI for Realtek RTL9617C (Taurus).
 *
 * Included by both the Linux kernel driver (rtl9617c-pe.c) and the bare-metal
 * MIPS32 PE firmware (pe-main.c). Keep layout strictly fixed-width, naturally
 * aligned, and little-endian.
 */
#ifndef _RTL9617C_PE_ABI_H
#define _RTL9617C_PE_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define RTL_PE_SHM_MAGIC		0x50453936U /* "PE96" */
#define RTL_PE_ABI_VERSION		1U

/* Fixed offsets inside the 128 KiB DMA-coherent PE memory block */
#define RTL_PE_MEM_SIZE			0x20000U    /* 128 KiB */
#define RTL_PE_IMEM_MAX			0x08000U    /* 32 KiB: 0x00000..0x07FFF */
#define RTL_PE_DMEM_STACK_OFF		0x08000U    /* 8 KiB:  0x08000..0x09FFF */
#define RTL_PE_MAILBOX_OFF		0x0A000U    /* 8 KiB:  0x0A000..0x0BFFF */
#define RTL_PE_LOG_OFF			0x0C000U    /* 16 KiB: 0x0C000..0x0FFFF */
#define RTL_PE_LOG_SIZE			0x04000U
#define RTL_PE_RING_OFF			0x10000U    /* 64 KiB: 0x10000..0x1FFFF */

/* Non-exclusive wired offload feature bits (coexist inside unified PE image) */
#define RTL_PE_FEAT_FQ_CODEL_BIT	0
#define RTL_PE_FEAT_IPSEC_BIT		1
#define RTL_PE_FEAT_SRV6_BIT		2
#define RTL_PE_FEAT_MCAST_BIT		3
#define RTL_PE_FEAT_MAX			4

#define RTL_PE_FEAT_FQ_CODEL		(1U << RTL_PE_FEAT_FQ_CODEL_BIT)
#define RTL_PE_FEAT_IPSEC		(1U << RTL_PE_FEAT_IPSEC_BIT)
#define RTL_PE_FEAT_SRV6		(1U << RTL_PE_FEAT_SRV6_BIT)
#define RTL_PE_FEAT_MCAST		(1U << RTL_PE_FEAT_MCAST_BIT)
#define RTL_PE_FEAT_VALID_MASK		((1U << RTL_PE_FEAT_MAX) - 1U)

/* Operating modes reported/selected per PE core */
#define RTL_PE_MODE_OFF			0U
#define RTL_PE_MODE_OFFLOAD		1U
#define RTL_PE_MODE_WIFI_WFO		2U

/* Self-test commands (host <-> PE mailbox verification burst) */
#define RTL_PE_TEST_NONE		0U
#define RTL_PE_TEST_PING_CRC		1U

/* Per-feature live telemetry (32 bytes each, naturally aligned) */
struct rtl_pe_feat_stat {
	u32 util_permille;	/* 0..1000 (0.0%..100.0%) */
	u32 cycles;		/* CP0 Count ticks spent in last window */
	u32 qdepth;		/* Current queued packets */
	u32 flags;		/* Bit 0: active, Bit 1: hw_bound */
	u64 packets;		/* Cumulative packets processed */
	u64 bytes;		/* Cumulative bytes processed */
};

/*
 * Uncached mailbox at dma + RTL_PE_MAILBOX_OFF.
 * ARM writes ctrl_* fields; PE writes stat_* fields.
 * Exception vector writes exc_cause / exc_epc at fixed offsets +0x14 / +0x18.
 */
struct rtl_pe_shm {
	/* +0x00: PE status header (fixed offsets matched by pe-entry.S) */
	u32 magic;		/* +0x00: RTL_PE_SHM_MAGIC once pe_main is up */
	u32 abi_version;	/* +0x04: RTL_PE_ABI_VERSION */
	u32 fw_version;		/* +0x08: 0xMMmmpp00 */
	u32 state;		/* +0x0C: 0=init, 1=running, 2=stopping, 3=fault */
	u32 cp0_prid;		/* +0x10: MIPS CP0 $15 PrId */
	u32 exc_cause;		/* +0x14: CP0 $13 Cause on exception */
	u32 exc_epc;		/* +0x18: CP0 $14 EPC on exception */
	u32 cp0_cctl_init;	/* +0x1C: CP0 $20 CCTL before init */
	u32 cp0_cctl_run;	/* +0x20: CP0 $20 CCTL after IMEM/DMEM init */
	u32 boot_sp;		/* +0x24: initial $sp */
	u32 boot_pc;		/* +0x28: entry jump target */
	u32 reserved_hdr;	/* +0x2C */

	/* +0x30: ARM -> PE control block */
	u32 core_id;		/* +0x30: 0 or 1 */
	u32 pe_clk_hz;		/* +0x34: PE core clock in Hz (e.g. 800000000) */
	u32 feature_mask;	/* +0x38: active non-exclusive feature bitmask */
	u32 mode;		/* +0x3C: RTL_PE_MODE_* */
	u32 cmd_seq;		/* +0x40: incremented by ARM on config change */
	u32 stop_req;		/* +0x44: 1 = ARM requests clean halt */
	u32 test_cmd;		/* +0x48: RTL_PE_TEST_* */
	u32 test_len;		/* +0x4C: payload length for self-test */
	u32 test_iter;		/* +0x50: iteration count for self-test */
	u32 test_seq;		/* +0x54: incremented by ARM to trigger self-test */
	u32 reserved_ctrl[2];	/* +0x58..0x5F */

	/* +0x60: PE -> ARM live telemetry */
	u32 ack_seq;		/* +0x60: mirrors cmd_seq when applied */
	u32 stopped_ack;	/* +0x64: 1 when PE entered clean halt loop */
	u32 heartbeat;		/* +0x68: incremented every ~200 ms window */
	u32 uptime_ms;		/* +0x6C: monotonic PE uptime in ms */
	u32 count_now;		/* +0x70: latest CP0 $9 Count sample */
	u32 count_rate_hz;	/* +0x74: configured CP0 Count increment rate */
	u32 window_cycles;	/* +0x78: CP0 Count ticks in last ~200 ms window */
	u32 busy_cycles;	/* +0x7C: total work ticks in last window */
	u32 util_permille;	/* +0x80: 0..1000 total PE utilization */
	u32 log_wr;		/* +0x84: monotonic write cursor into log ring */
	u32 test_ack_seq;	/* +0x88: mirrors test_seq on completion */
	u32 test_crc;		/* +0x8C: FNV-1a/XOR digest from self-test */
	u32 test_cycles;	/* +0x90: CP0 Count ticks spent in self-test */
	u32 reserved_stat[3];	/* +0x94..0x9F */

	/* +0xA0: Per-feature telemetry array (4 * 32 = 128 bytes) */
	struct rtl_pe_feat_stat feat[RTL_PE_FEAT_MAX];

	/* +0x120: Small 1 KiB self-test scratch payload */
	u8 test_buf[1024];
};

#endif /* _RTL9617C_PE_ABI_H */
