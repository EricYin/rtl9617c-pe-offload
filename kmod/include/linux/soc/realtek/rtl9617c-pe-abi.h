/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared ARM64 <-> Taroko MIPS32 PE mailbox & data-plane ABI for Realtek
 * RTL9617C (Taurus).
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
#define RTL_PE_ABI_VERSION		2U

/* Fixed offsets inside the 128 KiB DMA-coherent PE memory block */
#define RTL_PE_MEM_SIZE			0x20000U    /* 128 KiB */
#define RTL_PE_IMEM_MAX			0x08000U    /* 32 KiB: 0x00000..0x07FFF */
#define RTL_PE_DMEM_STACK_OFF		0x08000U    /* 8 KiB:  0x08000..0x09FFF */
#define RTL_PE_MAILBOX_OFF		0x0A000U    /* 8 KiB:  0x0A000..0x0BFFF */
#define RTL_PE_LOG_OFF			0x0C000U    /* 16 KiB: 0x0C000..0x0FFFF */
#define RTL_PE_LOG_SIZE			0x04000U
#define RTL_PE_RING_OFF			0x10000U    /* 64 KiB: 0x10000..0x1FFFF */

/* Sub-layout inside RTL_PE_RING_OFF (64 KiB coherent ring region) */
#define RTL_PE_DMA_DESC_OFF		0x10000U    /* 4 KiB: 512 * 8B DMA LSO desc */
#define RTL_PE_TX_RING_OFF		0x11000U    /* 8 KiB: 256 * 32B Host->PE ring */
#define RTL_PE_FREE_RING_OFF		0x13000U    /* 1 KiB: 256 * 4B PE->Host free */

/* Ring dimensions (strict powers of 2) */
#define RTL_PE_DMA_ORDER		9U
#define RTL_PE_DMA_ENTRIES		(1U << RTL_PE_DMA_ORDER) /* 512 */
#define RTL_PE_DMA_MASK			(RTL_PE_DMA_ENTRIES - 1U)
#define RTL_PE_DMA_GUARD		2U

#define RTL_PE_TX_RING_SIZE		256U
#define RTL_PE_TX_RING_MASK		(RTL_PE_TX_RING_SIZE - 1U)
#define RTL_PE_FREE_RING_SIZE		256U
#define RTL_PE_FREE_RING_MASK		(RTL_PE_FREE_RING_SIZE - 1U)

/* Dedicated packet buffer pool per PE core (256 * 2 KiB = 512 KiB) */
#define RTL_PE_POOL_SLOTS		256U
#define RTL_PE_SLOT_SIZE		2048U
#define RTL_PE_POOL_BYTES		(RTL_PE_POOL_SLOTS * RTL_PE_SLOT_SIZE)
#define RTL_PE_DMA_HDR_LEN		16U

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

/* Per-feature status flags */
#define RTL_PE_FEAT_FLAG_ACTIVE		(1U << 0)
#define RTL_PE_FEAT_FLAG_HW_BOUND	(1U << 1)

/* Operating modes reported/selected per PE core */
#define RTL_PE_MODE_OFF			0U
#define RTL_PE_MODE_OFFLOAD		1U
#define RTL_PE_MODE_WIFI_WFO		2U

/* Self-test commands (host <-> PE mailbox verification burst) */
#define RTL_PE_TEST_NONE		0U
#define RTL_PE_TEST_PING_CRC		1U

/* Submission descriptor flags */
#define RTL_PE_TX_FLAG_OAM		(1U << 0)
#define RTL_PE_TX_FLAG_MCAST		(1U << 1)
#define RTL_PE_TX_FLAG_WAN		(1U << 2)

/* DMA LSO TX descriptor (8 bytes, matches hardware DMA LSO ring layout) */
struct rtl_pe_dma_desc {
	u32 addr;
	u32 ctrl;
};

/* Host -> PE packet submission descriptor (32 bytes, naturally aligned) */
struct rtl_pe_tx_desc {
	u32 slot_id;		/* +0x00: buffer slot index 0..255 */
	u32 wire_len;		/* +0x04: prefix_len + padded Ethernet len */
	u32 hdr_low;		/* +0x08: Header A high word */
	u32 hdr_high;		/* +0x0C: Header A low word */
	u32 desc_ctrl;		/* +0x10: pre-encoded DMA LSO ctrl word */
	u16 feat_id;		/* +0x14: RTL_PE_FEAT_*_BIT */
	u16 prefix_len;		/* +0x16: offset past 16B DMA hdr to Eth hdr */
	u32 flow_hint;		/* +0x18: optional host hash hint (0=PE hash) */
	u32 flags;		/* +0x1C: RTL_PE_TX_FLAG_* */
};

/* Per-feature live telemetry (48 bytes each, naturally 8-byte aligned) */
struct rtl_pe_feat_stat {
	u32 util_permille;	/* 0..1000 (0.0%..100.0%) */
	u32 cycles;		/* busy ns spent in last 200 ms window */
	u32 qdepth;		/* current queued packets */
	u32 flags;		/* RTL_PE_FEAT_FLAG_* */
	u64 packets;		/* cumulative packets transmitted */
	u64 bytes;		/* cumulative bytes transmitted */
	u64 drops;		/* cumulative AQM / tail drops */
	u64 ecn_marks;		/* cumulative ECN CE marks */
};

/*
 * Uncached mailbox at dma + RTL_PE_MAILBOX_OFF.
 * ARM writes ctrl_* / dp_cfg / tx_prod / free_cons; PE writes stat_* / tx_cons / free_prod.
 * Fixed header offsets +0x00..+0x28 MUST remain unchanged for pe-entry.S!
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
	u32 count_now;		/* +0x70: latest hardware 1 ns PTP timer sample */
	u32 count_rate_hz;	/* +0x74: timer rate in Hz (1000000000) */
	u32 window_cycles;	/* +0x78: elapsed ns in last ~200 ms window */
	u32 busy_cycles;	/* +0x7C: total work ns in last window */
	u32 util_permille;	/* +0x80: 0..1000 total PE utilization */
	u32 log_wr;		/* +0x84: monotonic write cursor into log ring */
	u32 test_ack_seq;	/* +0x88: mirrors test_seq on completion */
	u32 test_crc;		/* +0x8C: FNV-1a digest from self-test */
	u32 test_cycles;	/* +0x90: ns spent in self-test */
	u32 dp_ack_seq;		/* +0x94: mirrors dp_seq after DMA VP reset */
	u32 dma_tx_head;	/* +0x98: diagnostic PE DMA TX head */
	u32 dma_tx_tail;	/* +0x9C: diagnostic PE DMA TX tail */

	/* +0xA0: Per-feature telemetry array (4 * 48 = 192 bytes, +0xA0..+0x15F) */
	struct rtl_pe_feat_stat feat[RTL_PE_FEAT_MAX];

	/* +0x160: Data-plane ring pointers & FQ-CoDel / Mcast parameters */
	u32 dma_vp_id;		/* +0x160: DMA LSO VP index (4=PE0, 5=PE1, 0xff=none) */
	u32 dma_vp_ready;	/* +0x164: 1 when Host armed DMA VP & NI TX hook */
	u32 pool_dma;		/* +0x168: physical base of 512 KiB packet pool */
	u32 pool_slots;		/* +0x16C: number of 2 KiB slots (256) */
	u32 dp_seq;		/* +0x170: incremented by ARM on DMA VP reset */
	u32 fq_rate_kbps;	/* +0x174: 0=unshaped FQ-CoDel, >0=token bucket kbps */
	u32 fq_target_us;	/* +0x178: CoDel sojourn target us (default 5000) */
	u32 fq_interval_us;	/* +0x17C: CoDel interval us (default 100000) */
	u32 fq_quantum;		/* +0x180: DRR quantum bytes (default 1514) */
	u32 fq_limit;		/* +0x184: max queued packets (default 224) */
	u32 fq_ecn;		/* +0x188: 1=ECN CE mark ECT packets before drop */
	u32 mcast_rate_kbps;	/* +0x18C: 0=unpaced, >0=mcast pacer kbps */
	u32 tx_prod;		/* +0x190: Host -> PE submission producer cursor */
	u32 tx_cons;		/* +0x194: PE -> Host submission consumer cursor */
	u32 free_prod;		/* +0x198: PE -> Host reclaimed slot producer cursor */
	u32 free_cons;		/* +0x19C: Host -> PE reclaimed slot consumer cursor */

	/* +0x1A0: Small 1 KiB self-test scratch payload */
	u8 test_buf[1024];
};

#endif /* _RTL9617C_PE_ABI_H */
