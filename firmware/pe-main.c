/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Realtek RTL9617C (Taurus) unified bare-metal Taroko MIPS32 PE firmware.
 *
 * Executes zero-wait in 32 KiB IMEM (0x8fc00400) with private 8 KiB DMEM stack.
 * Implements:
 *   - Hardware DMA LSO VP direct TX (VP4 on PE0, VP5 on PE1 via ATU 0xb3001000)
 *   - 64-queue RFC 8290 FQ-CoDel (1 ns PTP sojourn, ECN CE, optional shaper)
 *   - Multicast/broadcast FIFO with optional token-bucket pacer
 *   - Non-exclusive feature slots & clean DMA-reset resync via dp_seq/dp_ack_seq
 */

#include "rtl9617c-pe-abi.h"

#define PE_FW_VERSION		0x00020000U	/* v2.0.0 */
#define PE_LOG_MASK		(RTL_PE_LOG_SIZE - 1U)

/*
 * ATU entry 1: 0x1000 -> 0xf430 (NE PTP 1 ns timer at 0xb000e01c)
 * ATU entry 2: 0x1100 -> 512 KiB coherent pool at 0xb1000000 + (pool_dma & 0xfffff)
 * ATU entry 3: 0x1200 -> 128 KiB PE SHM/rings at 0xb2000000 + (pe_dma & 0xfffff)
 * ATU entry 4: 0x1300 -> 0xf700 (DMA_LSO at 0xb3001000)
 */
#define PE_PTP_TIMER_VA		0xb000e01cU
#define PE_PTP_NS_MASK		0x3fffffffU
#define PE_NS_PER_SEC		1000000000U
#define PE_WIN_TARGET_NS	200000000U	/* 200 ms sampling window */
#define PE_IDLE_PAUSE_ITERS	2000U		/* ~5 us pure-IMEM backoff */
#define PE_PTP_POLL_DIV		200U		/* ~1 ms NE APB read cadence */

#define PE_POOL_KSEG1_BASE	0xb1000000U
#define PE_DMA_BASE_VA		0xb3001000U
#define PE_DMA_CTRL_OFF		0x000U
#define PE_DMA_VP_BASE(vp)	(PE_DMA_BASE_VA + 0x100U + ((vp) * 0xa0U))
#define PE_DMA_VP_WRITE_OFF	0x18U
#define PE_DMA_VP_READ_OFF	0x1cU
#define PE_DMA_VP_MISC_IRQ_OFF	0x68U
#define PE_DMA_VP_MISC_IRQ_TX	1U

#define PE_FQ_FLOWS		64U
#define PE_FQ_FLOW_MASK		(PE_FQ_FLOWS - 1U)
#define PE_SLOT_NONE		0xffffU
#define PE_FLOW_NONE		0xffU

#define PE_FLOW_IN_NONE		0U
#define PE_FLOW_IN_NEW		1U
#define PE_FLOW_IN_OLD		2U

#define PE_CODEL_ABOVE		(1U << 0)
#define PE_CODEL_DROPPING	(1U << 1)

#define PE_PKT_FLAG_MCAST	(1U << 0)
#define PE_PKT_FLAG_WAN		(1U << 1)

#define PE_ETH_P_IP		0x0800U
#define PE_ETH_P_IPV6		0x86ddU
#define PE_ETH_P_8021Q		0x8100U
#define PE_ETH_P_8021AD		0x88a8U
#define PE_ETH_P_QINQ_OLD	0x9100U
#define PE_ETH_P_PPP_SES	0x8864U
#define PE_PPP_IP		0x0021U
#define PE_PPP_IPV6		0x0057U

typedef signed int s32;
typedef signed short s16;

struct pe_dp_state {
	/* Per-slot packet metadata (256 slots * 10B = 2560B) */
	u16 pkt_next[RTL_PE_POOL_SLOTS];
	u32 pkt_enq_ns[RTL_PE_POOL_SLOTS];
	u32 pkt_desc_ctrl[RTL_PE_POOL_SLOTS];
	u8  pkt_prefix[RTL_PE_POOL_SLOTS];
	u8  pkt_flags[RTL_PE_POOL_SLOTS];

	/* 64-flow FQ-CoDel queues & DRR state (64 * 20B = 1280B) */
	u16 flow_head[PE_FQ_FLOWS];
	u16 flow_tail[PE_FQ_FLOWS];
	u16 flow_qlen[PE_FQ_FLOWS];
	s16 flow_deficit[PE_FQ_FLOWS];
	u8  flow_qnext[PE_FQ_FLOWS];
	u8  flow_list[PE_FQ_FLOWS];

	u32 codel_first_above[PE_FQ_FLOWS];
	u32 codel_drop_next[PE_FQ_FLOWS];
	u16 codel_count[PE_FQ_FLOWS];
	u16 codel_lastcount[PE_FQ_FLOWS];
	u8  codel_flags[PE_FQ_FLOWS];

	/* DMA LSO descriptor-to-slot completion tracking (512 * 2B = 1024B) */
	u16 dma_slot[RTL_PE_DMA_ENTRIES];

	/* DRR lists + mcast FIFO + token buckets */
	u8  new_head, new_tail, old_head, old_tail;
	u16 mcast_head, mcast_tail, mcast_qlen;
	u16 fq_total_q;
	u16 fq_wan_q;
	u8  dma_idle_cnt;
	u8  pad0;

	u32 tx_cons;
	u32 free_prod;
	u32 dma_head;
	u32 dma_tail;

	u32 fq_tb_ns;
	u8  fq_tb_valid;
	u32 mcast_tb_ns;
	u8  mcast_tb_valid;
};

static inline void pe_sync(void)
{
	__asm__ volatile(".word 0x0000000f" ::: "memory");
}

static inline void pe_idle_pause(u32 iters)
{
	while (iters--)
		__asm__ volatile("nop");
}

static inline u32 pe_rd32(u32 va)
{
	return *(volatile u32 *)va;
}

static inline void pe_wr32(u32 va, u32 val)
{
	*(volatile u32 *)va = val;
}

static inline u32 read_hw_ns(void)
{
	return (*(volatile u32 *)PE_PTP_TIMER_VA) & PE_PTP_NS_MASK;
}

static inline u32 delta_hw_ns(u32 now, u32 prev)
{
	return (now >= prev) ? (now - prev) : (now + PE_NS_PER_SEC - prev);
}

static inline s32 ptp_diff_ns(u32 a, u32 b)
{
	s32 d = (s32)a - (s32)b;

	if (d > (s32)(PE_NS_PER_SEC / 2U))
		d -= (s32)PE_NS_PER_SEC;
	else if (d < -(s32)(PE_NS_PER_SEC / 2U))
		d += (s32)PE_NS_PER_SEC;
	return d;
}

static inline u32 ptp_add_ns(u32 t, u32 delta)
{
	u32 r = t + delta;

	while (r >= PE_NS_PER_SEC)
		r -= PE_NS_PER_SEC;
	return r;
}

static inline u32 read_cp0_prid(void)
{
	u32 v;

	__asm__ volatile("mfc0 %0, $15" : "=r"(v));
	return v;
}

static inline void log_putc(volatile struct rtl_pe_shm *shm, volatile char *ring, char c)
{
	u32 wr = shm->log_wr;

	ring[wr & PE_LOG_MASK] = c;
	shm->log_wr = wr + 1U;
}

static void log_puts(volatile struct rtl_pe_shm *shm, volatile char *ring, const char *s)
{
	while (*s)
		log_putc(shm, ring, *s++);
}

static void log_hex32(volatile struct rtl_pe_shm *shm, volatile char *ring, u32 v)
{
	static const char hex[16] = "0123456789abcdef";
	int shift;

	log_putc(shm, ring, '0');
	log_putc(shm, ring, 'x');
	for (shift = 28; shift >= 0; shift -= 4)
		log_putc(shm, ring, hex[(v >> shift) & 0xfU]);
}

static void log_u32(volatile struct rtl_pe_shm *shm, volatile char *ring, u32 v)
{
	char tmp[12];
	int n = 0;

	if (!v) {
		log_putc(shm, ring, '0');
		return;
	}
	while (v && n < 11) {
		tmp[n++] = (char)('0' + (v % 10U));
		v /= 10U;
	}
	while (n--)
		log_putc(shm, ring, tmp[n]);
}

static u32 calc_permille(u32 busy, u32 total)
{
	u32 p;

	if (!total)
		return 0;
	if (busy >= total)
		return 1000;
	if (busy <= 0x00400000U) {
		p = (busy * 1000U) / total;
	} else {
		u32 t = total >> 8;

		p = t ? (((busy >> 8) * 1000U) / t) : 0;
	}
	return p > 1000U ? 1000U : p;
}

static u32 pe_fnv1a(volatile const u8 *buf, u32 len, u32 iter)
{
	u32 h = 0x811c9dc5U;
	u32 r, i;

	for (r = 0; r < iter; r++) {
		for (i = 0; i < len; i++) {
			h ^= buf[i];
			h *= 0x01000193U;
		}
	}
	return h;
}

static u32 pe_isqrt(u32 x)
{
	u32 res = 0;
	u32 bit = (x > 65535U) ? (1U << 30) : (1U << 14);

	while (bit > x)
		bit >>= 2;
	while (bit) {
		if (x >= res + bit) {
			x -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return res ? res : 1U;
}

static u32 calc_pkt_ns(u32 wire_len, u32 rate_kbps)
{
	u32 q, r;

	if (!rate_kbps)
		return 0;
	if (rate_kbps < 64U)
		rate_kbps = 64U;
	if (rate_kbps >= 100000U) {
		u32 kbps_div8 = rate_kbps >> 3;

		return kbps_div8 ? ((wire_len * 1000000U) / kbps_div8) : 0U;
	}
	q = 8000000U / rate_kbps;
	r = 8000000U % rate_kbps;
	return wire_len * q + (wire_len * r) / rate_kbps;
}

static inline u32 slot_wire_len(const struct pe_dp_state *dp, u16 slot_id)
{
	u32 len = (dp->pkt_desc_ctrl[slot_id] >> 8) & 0xffffU;

	return (len > RTL_PE_DMA_HDR_LEN) ? (len - RTL_PE_DMA_HDR_LEN) : len;
}

static inline void dp_return_slot(volatile struct rtl_pe_shm *shm,
				  volatile u32 *free_ring,
				  struct pe_dp_state *dp, u16 slot_id)
{
	free_ring[dp->free_prod & RTL_PE_FREE_RING_MASK] = slot_id;
	pe_sync();
	dp->free_prod++;
	shm->free_prod = dp->free_prod;
}

static u32 dp_reclaim_dma(volatile struct rtl_pe_shm *shm,
			  volatile u32 *free_ring,
			  struct pe_dp_state *dp, u32 vp_va)
{
	u32 hw = pe_rd32(vp_va + PE_DMA_VP_READ_OFF) & RTL_PE_DMA_MASK;
	u32 reclaimed = 0;

	while (((hw - dp->dma_tail) & RTL_PE_DMA_MASK) > RTL_PE_DMA_GUARD) {
		u16 slot_id = dp->dma_slot[dp->dma_tail];

		if (slot_id != PE_SLOT_NONE) {
			dp->dma_slot[dp->dma_tail] = PE_SLOT_NONE;
			dp_return_slot(shm, free_ring, dp, slot_id);
			reclaimed++;
		}
		dp->dma_tail = (dp->dma_tail + 1U) & RTL_PE_DMA_MASK;
		dp->dma_idle_cnt = 0;
	}

	/* When DMA HW has caught up completely and remained idle, drain final <=2 guard slots */
	if (dp->dma_tail != dp->dma_head && hw == dp->dma_head) {
		if (++dp->dma_idle_cnt >= 40U) {
			while (dp->dma_tail != dp->dma_head) {
				u16 slot_id = dp->dma_slot[dp->dma_tail];

				if (slot_id != PE_SLOT_NONE) {
					dp->dma_slot[dp->dma_tail] = PE_SLOT_NONE;
					dp_return_slot(shm, free_ring, dp, slot_id);
					reclaimed++;
				}
				dp->dma_tail = (dp->dma_tail + 1U) & RTL_PE_DMA_MASK;
			}
			dp->dma_idle_cnt = 0;
		}
	} else {
		dp->dma_idle_cnt = 0;
	}

	shm->dma_tx_tail = dp->dma_tail;
	return reclaimed;
}

static inline u32 dp_dma_avail(const struct pe_dp_state *dp)
{
	u32 free = (dp->dma_tail - dp->dma_head - 1U) & RTL_PE_DMA_MASK;

	return (free > RTL_PE_DMA_GUARD) ? (free - RTL_PE_DMA_GUARD) : 0U;
}

static int dp_submit_dma(volatile struct rtl_pe_shm *shm,
			 volatile struct rtl_pe_dma_desc *dma_desc,
			 struct pe_dp_state *dp, u32 vp_va, u32 pool_dma,
			 u16 slot_id)
{
	u32 idx = dp->dma_head;
	u32 next_head;

	if (!dp_dma_avail(dp))
		return -1;

	dma_desc[idx].addr = pool_dma + ((u32)slot_id * RTL_PE_SLOT_SIZE);
	dma_desc[idx].ctrl = dp->pkt_desc_ctrl[slot_id];
	dp->dma_slot[idx] = slot_id;

	next_head = (idx + 1U) & RTL_PE_DMA_MASK;
	pe_sync();
	dp->dma_head = next_head;
	pe_wr32(vp_va + PE_DMA_VP_WRITE_OFF, next_head);
	pe_wr32(vp_va + PE_DMA_VP_MISC_IRQ_OFF, PE_DMA_VP_MISC_IRQ_TX);
	shm->dma_tx_head = next_head;
	return 0;
}

static void fq_list_push(struct pe_dp_state *dp, u8 f, u8 target_list)
{
	if (target_list == PE_FLOW_IN_NONE)
		return;
	dp->flow_qnext[f] = PE_FLOW_NONE;
	dp->flow_list[f] = target_list;
	if (target_list == PE_FLOW_IN_NEW) {
		if (dp->new_head == PE_FLOW_NONE)
			dp->new_head = f;
		else
			dp->flow_qnext[dp->new_tail] = f;
		dp->new_tail = f;
	} else {
		if (dp->old_head == PE_FLOW_NONE)
			dp->old_head = f;
		else
			dp->flow_qnext[dp->old_tail] = f;
		dp->old_tail = f;
	}
}

static u8 fq_list_pop(struct pe_dp_state *dp, u8 from_list)
{
	u8 f;

	if (from_list == PE_FLOW_IN_NEW) {
		f = dp->new_head;
		if (f != PE_FLOW_NONE) {
			dp->new_head = dp->flow_qnext[f];
			if (dp->new_head == PE_FLOW_NONE)
				dp->new_tail = PE_FLOW_NONE;
			dp->flow_qnext[f] = PE_FLOW_NONE;
			dp->flow_list[f] = PE_FLOW_IN_NONE;
		}
	} else {
		f = dp->old_head;
		if (f != PE_FLOW_NONE) {
			dp->old_head = dp->flow_qnext[f];
			if (dp->old_head == PE_FLOW_NONE)
				dp->old_tail = PE_FLOW_NONE;
			dp->flow_qnext[f] = PE_FLOW_NONE;
			dp->flow_list[f] = PE_FLOW_IN_NONE;
		}
	}
	return f;
}

static u16 fq_flow_pop_pkt(struct pe_dp_state *dp, u8 f)
{
	u16 slot = dp->flow_head[f];

	if (slot == PE_SLOT_NONE)
		return PE_SLOT_NONE;
	dp->flow_head[f] = dp->pkt_next[slot];
	if (dp->flow_head[f] == PE_SLOT_NONE)
		dp->flow_tail[f] = PE_SLOT_NONE;
	dp->pkt_next[slot] = PE_SLOT_NONE;
	dp->flow_qlen[f]--;
	dp->fq_total_q--;
	if (dp->pkt_flags[slot] & PE_PKT_FLAG_WAN)
		dp->fq_wan_q--;
	return slot;
}

/*
 * Parse optional VLAN / PPPoE L2 tags and set ECN Congestion Experienced (CE)
 * bit on IPv4/IPv6 ECT(0)/ECT(1) packets without unaligned loads.
 * Returns 1 if ECN-capable (already CE or newly marked CE), 0 if Not-ECT / non-IP.
 */
static int pe_try_ecn_mark(u32 pool_va, u16 slot_id, u32 wire_len, u32 prefix_len)
{
	volatile u8 *pkt = (volatile u8 *)(pool_va + ((u32)slot_id * RTL_PE_SLOT_SIZE) +
					   RTL_PE_DMA_HDR_LEN);
	u32 off = prefix_len + 12U;
	u16 proto;
	u32 tags = 0;

	if (wire_len < prefix_len + 14U)
		return 0;

	proto = ((u16)pkt[off] << 8) | pkt[off + 1U];
	off += 2U;

	while (proto == PE_ETH_P_8021Q || proto == PE_ETH_P_8021AD ||
	       proto == PE_ETH_P_QINQ_OLD) {
		if (wire_len < off + 4U || tags++ >= 2U)
			return 0;
		proto = ((u16)pkt[off + 2U] << 8) | pkt[off + 3U];
		off += 4U;
	}

	if (proto == PE_ETH_P_PPP_SES) {
		u16 ppp;

		if (wire_len < off + 8U)
			return 0;
		ppp = ((u16)pkt[off + 6U] << 8) | pkt[off + 7U];
		off += 8U;
		if (ppp == PE_PPP_IP)
			proto = PE_ETH_P_IP;
		else if (ppp == PE_PPP_IPV6)
			proto = PE_ETH_P_IPV6;
		else
			return 0;
	}

	if (proto == PE_ETH_P_IP) {
		u8 tos, ecn, new_tos;
		u16 old_csum, new_csum;
		u32 check;

		if (wire_len < off + 20U || (pkt[off] >> 4) != 4U)
			return 0;
		tos = pkt[off + 1U];
		ecn = tos & 0x03U;
		if (!ecn)
			return 0;
		if (ecn == 3U)
			return 1;
		new_tos = tos | 0x03U;
		old_csum = ((u16)pkt[off + 10U] << 8) | pkt[off + 11U];
		check = ((~old_csum) & 0xffffU) + (u32)(new_tos - tos);
		check = (check & 0xffffU) + (check >> 16);
		new_csum = (u16)((~check) & 0xffffU);
		pkt[off + 1U] = new_tos;
		pkt[off + 10U] = (u8)(new_csum >> 8);
		pkt[off + 11U] = (u8)new_csum;
		pe_sync();
		return 1;
	}

	if (proto == PE_ETH_P_IPV6) {
		u8 b1, ecn;

		if (wire_len < off + 40U || (pkt[off] >> 4) != 6U)
			return 0;
		b1 = pkt[off + 1U];
		ecn = (b1 >> 4) & 0x03U;
		if (!ecn)
			return 0;
		if (ecn == 3U)
			return 1;
		pkt[off + 1U] = b1 | 0x30U;
		pe_sync();
		return 1;
	}

	return 0;
}

static u8 pe_hash_slot(u32 pool_va, u16 slot_id, u32 wire_len, u32 prefix_len, u32 hint)
{
	if (hint) {
		u32 h = hint ^ (hint >> 6) ^ (hint >> 12);

		return (u8)(h & PE_FQ_FLOW_MASK);
	}
	{
		volatile u8 *pkt = (volatile u8 *)(pool_va +
						   ((u32)slot_id * RTL_PE_SLOT_SIZE) +
						   RTL_PE_DMA_HDR_LEN + prefix_len);
		u32 h = 0x811c9dc5U;
		u32 i, n = (wire_len > prefix_len + 14U) ? 14U :
			   ((wire_len > prefix_len) ? (wire_len - prefix_len) : 0U);

		for (i = 0; i < n; i++) {
			h ^= pkt[i];
			h *= 0x01000193U;
		}
		return (u8)((h ^ (h >> 6)) & PE_FQ_FLOW_MASK);
	}
}

static void fq_drop_fattest(volatile struct rtl_pe_shm *shm,
			    volatile u32 *free_ring,
			    struct pe_dp_state *dp)
{
	u8 f, best = 0;
	u16 best_len = 0;
	u16 dropped;

	for (f = 0; f < PE_FQ_FLOWS; f++) {
		if (dp->flow_qlen[f] > best_len) {
			best_len = dp->flow_qlen[f];
			best = f;
		}
	}
	if (!best_len)
		return;
	dropped = fq_flow_pop_pkt(dp, best);
	if (dropped != PE_SLOT_NONE) {
		dp_return_slot(shm, free_ring, dp, dropped);
		shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].drops++;
	}
}

static void dp_reset_queues(volatile struct rtl_pe_shm *shm,
			    volatile u32 *free_ring,
			    struct pe_dp_state *dp, int drop_queued)
{
	u32 i;

	for (i = 0; i < RTL_PE_DMA_ENTRIES; i++) {
		u16 s = dp->dma_slot[i];

		if (s != PE_SLOT_NONE) {
			dp->dma_slot[i] = PE_SLOT_NONE;
			dp_return_slot(shm, free_ring, dp, s);
		}
	}
	dp->dma_head = 0;
	dp->dma_tail = 0;
	dp->fq_tb_valid = 0;
	dp->mcast_tb_valid = 0;

	if (drop_queued) {
		for (i = 0; i < PE_FQ_FLOWS; i++) {
			while (dp->flow_head[i] != PE_SLOT_NONE) {
				u16 s = fq_flow_pop_pkt(dp, (u8)i);

				dp_return_slot(shm, free_ring, dp, s);
			}
		}
		while (dp->mcast_head != PE_SLOT_NONE) {
			u16 s = dp->mcast_head;

			dp->mcast_head = dp->pkt_next[s];
			dp->pkt_next[s] = PE_SLOT_NONE;
			dp->mcast_qlen--;
			dp_return_slot(shm, free_ring, dp, s);
		}
		dp->mcast_tail = PE_SLOT_NONE;
		shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].qdepth = 0;
		shm->feat[RTL_PE_FEAT_MCAST_BIT].qdepth = 0;
	}
}

static void dp_init(struct pe_dp_state *dp)
{
	u32 i;

	for (i = 0; i < RTL_PE_POOL_SLOTS; i++) {
		dp->pkt_next[i] = PE_SLOT_NONE;
		dp->pkt_enq_ns[i] = 0;
		dp->pkt_desc_ctrl[i] = 0;
		dp->pkt_prefix[i] = 0;
		dp->pkt_flags[i] = 0;
	}
	for (i = 0; i < PE_FQ_FLOWS; i++) {
		dp->flow_head[i] = PE_SLOT_NONE;
		dp->flow_tail[i] = PE_SLOT_NONE;
		dp->flow_qlen[i] = 0;
		dp->flow_deficit[i] = 0;
		dp->flow_qnext[i] = PE_FLOW_NONE;
		dp->flow_list[i] = PE_FLOW_IN_NONE;
		dp->codel_first_above[i] = 0;
		dp->codel_drop_next[i] = 0;
		dp->codel_count[i] = 0;
		dp->codel_lastcount[i] = 0;
		dp->codel_flags[i] = 0;
	}
	for (i = 0; i < RTL_PE_DMA_ENTRIES; i++)
		dp->dma_slot[i] = PE_SLOT_NONE;

	dp->new_head = PE_FLOW_NONE;
	dp->new_tail = PE_FLOW_NONE;
	dp->old_head = PE_FLOW_NONE;
	dp->old_tail = PE_FLOW_NONE;
	dp->mcast_head = PE_SLOT_NONE;
	dp->mcast_tail = PE_SLOT_NONE;
	dp->mcast_qlen = 0;
	dp->fq_total_q = 0;
	dp->fq_wan_q = 0;
	dp->dma_idle_cnt = 0;
	dp->tx_cons = 0;
	dp->free_prod = 0;
	dp->dma_head = 0;
	dp->dma_tail = 0;
	dp->fq_tb_ns = 0;
	dp->fq_tb_valid = 0;
	dp->mcast_tb_ns = 0;
	dp->mcast_tb_valid = 0;
}

static u32 dp_drain_tx_ring(volatile struct rtl_pe_shm *shm,
			    volatile struct rtl_pe_tx_desc *tx_ring,
			    volatile u32 *free_ring,
			    struct pe_dp_state *dp, u32 pool_va,
			    u32 feature_mask, u32 fq_limit, u32 fq_quantum,
			    u32 now_ns)
{
	u32 prod = shm->tx_prod;
	u32 count = 0;

	while (dp->tx_cons != prod && count < 32U) {
		volatile struct rtl_pe_tx_desc *td = &tx_ring[dp->tx_cons & RTL_PE_TX_RING_MASK];
		u32 slot_id32 = td->slot_id;
		u32 desc_ctrl = td->desc_ctrl;
		u16 feat_id = td->feat_id;
		u16 prefix_len = td->prefix_len;
		u32 flow_hint = td->flow_hint;
		u32 flags = td->flags;
		u16 slot_id;

		dp->tx_cons++;
		count++;

		if (slot_id32 >= RTL_PE_POOL_SLOTS)
			continue;
		slot_id = (u16)slot_id32;
		dp->pkt_desc_ctrl[slot_id] = desc_ctrl;
		dp->pkt_enq_ns[slot_id] = now_ns;
		dp->pkt_prefix[slot_id] = (prefix_len <= 64U) ? (u8)prefix_len : 64U;
		dp->pkt_next[slot_id] = PE_SLOT_NONE;

		if (feat_id == RTL_PE_FEAT_MCAST_BIT &&
		    ((feature_mask & RTL_PE_FEAT_MCAST) || !dp->mcast_qlen)) {
			if (dp->mcast_qlen >= 64U) {
				dp_return_slot(shm, free_ring, dp, slot_id);
				shm->feat[RTL_PE_FEAT_MCAST_BIT].drops++;
				continue;
			}
			dp->pkt_flags[slot_id] = PE_PKT_FLAG_MCAST;
			if (dp->mcast_head == PE_SLOT_NONE)
				dp->mcast_head = slot_id;
			else
				dp->pkt_next[dp->mcast_tail] = slot_id;
			dp->mcast_tail = slot_id;
			dp->mcast_qlen++;
		} else {
			u32 wire_len = slot_wire_len(dp, slot_id);
			u8 f = pe_hash_slot(pool_va, slot_id, wire_len,
					    dp->pkt_prefix[slot_id], flow_hint);

			dp->pkt_flags[slot_id] = (flags & RTL_PE_TX_FLAG_WAN) ? PE_PKT_FLAG_WAN : 0U;
			if (dp->fq_total_q >= fq_limit)
				fq_drop_fattest(shm, free_ring, dp);

			if (dp->flow_head[f] == PE_SLOT_NONE)
				dp->flow_head[f] = slot_id;
			else
				dp->pkt_next[dp->flow_tail[f]] = slot_id;
			dp->flow_tail[f] = slot_id;
			dp->flow_qlen[f]++;
			dp->fq_total_q++;
			if (dp->pkt_flags[slot_id] & PE_PKT_FLAG_WAN)
				dp->fq_wan_q++;

			if (dp->flow_list[f] == PE_FLOW_IN_NONE) {
				dp->flow_deficit[f] = (s16)fq_quantum;
				fq_list_push(dp, f, PE_FLOW_IN_NEW);
			}
		}
	}

	if (count) {
		pe_sync();
		shm->tx_cons = dp->tx_cons;
	}
	return count;
}

static u32 dp_service_mcast(volatile struct rtl_pe_shm *shm,
			    volatile struct rtl_pe_dma_desc *dma_desc,
			    struct pe_dp_state *dp, u32 vp_va, u32 pool_dma,
			    u32 mcast_rate_kbps, u32 now_ns)
{
	u32 sent = 0;

	while (dp->mcast_head != PE_SLOT_NONE && sent < 8U) {
		u16 slot_id = dp->mcast_head;
		u32 wire_len = slot_wire_len(dp, slot_id);
		u32 pkt_ns = calc_pkt_ns(wire_len, mcast_rate_kbps);

		if (pkt_ns) {
			if (!dp->mcast_tb_valid) {
				dp->mcast_tb_ns = now_ns;
				dp->mcast_tb_valid = 1;
			} else {
				s32 diff = ptp_diff_ns(now_ns, dp->mcast_tb_ns);

				if (diff < 0)
					break;
				if (diff > 500000)
					dp->mcast_tb_ns = ptp_add_ns(now_ns, PE_NS_PER_SEC - 500000U);
			}
		}

		if (dp_submit_dma(shm, dma_desc, dp, vp_va, pool_dma, slot_id) < 0)
			break;

		dp->mcast_head = dp->pkt_next[slot_id];
		if (dp->mcast_head == PE_SLOT_NONE)
			dp->mcast_tail = PE_SLOT_NONE;
		dp->pkt_next[slot_id] = PE_SLOT_NONE;
		dp->mcast_qlen--;

		if (pkt_ns)
			dp->mcast_tb_ns = ptp_add_ns(dp->mcast_tb_ns, pkt_ns);

		shm->feat[RTL_PE_FEAT_MCAST_BIT].packets++;
		shm->feat[RTL_PE_FEAT_MCAST_BIT].bytes += wire_len;
		sent++;
	}
	return sent;
}

/* Dequeue one packet from flow f applying CoDel AQM + optional ECN CE */
static u16 fq_codel_dequeue_flow(volatile struct rtl_pe_shm *shm,
				 volatile u32 *free_ring,
				 struct pe_dp_state *dp, u8 f,
				 u32 pool_va, u32 target_ns, u32 interval_ns,
				 u32 ecn_en, u32 now_ns)
{
	u16 slot = fq_flow_pop_pkt(dp, f);
	u32 sojourn;
	u8 cflags;

	if (slot == PE_SLOT_NONE) {
		dp->codel_flags[f] &= ~(PE_CODEL_ABOVE | PE_CODEL_DROPPING);
		return PE_SLOT_NONE;
	}

	sojourn = delta_hw_ns(now_ns, dp->pkt_enq_ns[slot]);
	cflags = dp->codel_flags[f];

	if (sojourn < target_ns || dp->fq_total_q <= 1U) {
		dp->codel_flags[f] &= ~(PE_CODEL_ABOVE | PE_CODEL_DROPPING);
		return slot;
	}

	if (!(cflags & PE_CODEL_ABOVE)) {
		dp->codel_first_above[f] = ptp_add_ns(now_ns, interval_ns);
		dp->codel_flags[f] |= PE_CODEL_ABOVE;
		return slot;
	}

	if (ptp_diff_ns(now_ns, dp->codel_first_above[f]) < 0)
		return slot;

	if (cflags & PE_CODEL_DROPPING) {
		while (ptp_diff_ns(now_ns, dp->codel_drop_next[f]) >= 0) {
			u32 wlen = slot_wire_len(dp, slot);

			if (ecn_en && pe_try_ecn_mark(pool_va, slot, wlen,
						      dp->pkt_prefix[slot])) {
				dp->codel_count[f]++;
				dp->codel_drop_next[f] = ptp_add_ns(dp->codel_drop_next[f],
								    interval_ns / pe_isqrt(dp->codel_count[f]));
				shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].ecn_marks++;
				return slot;
			}

			dp_return_slot(shm, free_ring, dp, slot);
			shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].drops++;
			dp->codel_count[f]++;
			dp->codel_drop_next[f] = ptp_add_ns(dp->codel_drop_next[f],
							    interval_ns / pe_isqrt(dp->codel_count[f]));

			slot = fq_flow_pop_pkt(dp, f);
			if (slot == PE_SLOT_NONE) {
				dp->codel_flags[f] &= ~(PE_CODEL_ABOVE | PE_CODEL_DROPPING);
				return PE_SLOT_NONE;
			}
			sojourn = delta_hw_ns(now_ns, dp->pkt_enq_ns[slot]);
			if (sojourn < target_ns) {
				dp->codel_flags[f] &= ~(PE_CODEL_ABOVE | PE_CODEL_DROPPING);
				return slot;
			}
		}
		return slot;
	}

	/* Enter dropping state */
	{
		u32 delta;
		u32 wlen = slot_wire_len(dp, slot);

		if (ecn_en && pe_try_ecn_mark(pool_va, slot, wlen,
					      dp->pkt_prefix[slot])) {
			shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].ecn_marks++;
		} else {
			dp_return_slot(shm, free_ring, dp, slot);
			shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].drops++;
			slot = fq_flow_pop_pkt(dp, f);
			if (slot == PE_SLOT_NONE) {
				dp->codel_flags[f] &= ~(PE_CODEL_ABOVE | PE_CODEL_DROPPING);
				return PE_SLOT_NONE;
			}
		}

		dp->codel_flags[f] |= PE_CODEL_DROPPING;
		delta = (dp->codel_count[f] > dp->codel_lastcount[f]) ?
			(u32)(dp->codel_count[f] - dp->codel_lastcount[f]) : 1U;
		if (dp->codel_count[f] > 2U &&
		    ptp_diff_ns(now_ns, ptp_add_ns(dp->codel_drop_next[f], interval_ns << 2)) < 0)
			dp->codel_count[f] = (u16)((delta > 1U) ? delta : 1U);
		else
			dp->codel_count[f] = 1U;
		dp->codel_lastcount[f] = dp->codel_count[f];
		dp->codel_drop_next[f] = ptp_add_ns(now_ns,
						    interval_ns / pe_isqrt(dp->codel_count[f]));
		return slot;
	}
}

static u32 dp_service_fq_codel(volatile struct rtl_pe_shm *shm,
			       volatile struct rtl_pe_dma_desc *dma_desc,
			       volatile u32 *free_ring,
			       struct pe_dp_state *dp, u32 vp_va, u32 pool_dma,
			       u32 pool_va, u32 rate_kbps, u32 target_ns,
			       u32 interval_ns, u32 quantum, u32 ecn_en,
			       u32 now_ns)
{
	u32 sent = 0;
	u32 steps = 0;

	while (dp->fq_total_q && sent < 8U && steps++ < 128U) {
		u8 from_list, f;
		u16 slot_id, peek_slot;
		u32 wire_len, pkt_ns, is_wan;
		u32 wan_gated = 0;

		if (!dp_dma_avail(dp))
			break;

		/* WAN-only token bucket gate (leaves LAN download TX unthrottled) */
		if (rate_kbps && dp->fq_wan_q) {
			if (!dp->fq_tb_valid) {
				dp->fq_tb_ns = now_ns;
				dp->fq_tb_valid = 1;
			} else {
				s32 diff = ptp_diff_ns(now_ns, dp->fq_tb_ns);

				if (diff < 0)
					wan_gated = 1;
				else if (diff > 250000)
					dp->fq_tb_ns = ptp_add_ns(now_ns, PE_NS_PER_SEC - 250000U);
			}
		}

		if (wan_gated && dp->fq_wan_q >= dp->fq_total_q)
			break;

		from_list = (dp->new_head != PE_FLOW_NONE) ? PE_FLOW_IN_NEW : PE_FLOW_IN_OLD;
		if (from_list == PE_FLOW_IN_OLD && dp->old_head == PE_FLOW_NONE)
			break;

		f = fq_list_pop(dp, from_list);
		if (f == PE_FLOW_NONE)
			break;

		peek_slot = dp->flow_head[f];
		if (wan_gated && peek_slot != PE_SLOT_NONE &&
		    (dp->pkt_flags[peek_slot] & PE_PKT_FLAG_WAN)) {
			fq_list_push(dp, f, PE_FLOW_IN_OLD);
			continue;
		}

		if (dp->flow_deficit[f] <= 0) {
			dp->flow_deficit[f] += (s16)quantum;
			fq_list_push(dp, f, PE_FLOW_IN_OLD);
			continue;
		}

		slot_id = fq_codel_dequeue_flow(shm, free_ring, dp, f, pool_va,
						target_ns, interval_ns, ecn_en, now_ns);
		if (slot_id == PE_SLOT_NONE) {
			if (dp->flow_head[f] != PE_SLOT_NONE)
				fq_list_push(dp, f, PE_FLOW_IN_OLD);
			else if (from_list == PE_FLOW_IN_NEW)
				fq_list_push(dp, f, PE_FLOW_IN_OLD);
			continue;
		}

		wire_len = slot_wire_len(dp, slot_id);
		is_wan = dp->pkt_flags[slot_id] & PE_PKT_FLAG_WAN;
		dp->flow_deficit[f] -= (s16)wire_len;
		if (dp->flow_head[f] != PE_SLOT_NONE)
			fq_list_push(dp, f, from_list);
		else if (from_list == PE_FLOW_IN_NEW)
			fq_list_push(dp, f, PE_FLOW_IN_OLD);

		if (dp_submit_dma(shm, dma_desc, dp, vp_va, pool_dma, slot_id) < 0) {
			dp_return_slot(shm, free_ring, dp, slot_id);
			shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].drops++;
			break;
		}

		if (is_wan && rate_kbps) {
			pkt_ns = calc_pkt_ns(wire_len, rate_kbps);
			if (pkt_ns)
				dp->fq_tb_ns = ptp_add_ns(dp->fq_tb_ns, pkt_ns);
		}

		shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].packets++;
		shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].bytes += wire_len;
		sent++;
	}
	return sent;
}

static void refresh_feature_flags(volatile struct rtl_pe_shm *shm,
				  u32 feature_mask, u32 vp_ready)
{
	u32 i;

	for (i = 0; i < RTL_PE_FEAT_MAX; i++) {
		u32 f = 0;

		if (feature_mask & (1U << i)) {
			f |= RTL_PE_FEAT_FLAG_ACTIVE;
			if (vp_ready && (i == RTL_PE_FEAT_FQ_CODEL_BIT ||
					 i == RTL_PE_FEAT_MCAST_BIT))
				f |= RTL_PE_FEAT_FLAG_HW_BOUND;
		}
		shm->feat[i].flags = f;
	}
}

void __attribute__((section(".text.pe_main"), noreturn))
pe_main(volatile struct rtl_pe_shm *shm)
{
	volatile char *ring = (volatile char *)((u32)shm + (RTL_PE_LOG_OFF - RTL_PE_MAILBOX_OFF));
	volatile struct rtl_pe_dma_desc *dma_desc =
		(volatile struct rtl_pe_dma_desc *)((u32)shm + (RTL_PE_DMA_DESC_OFF - RTL_PE_MAILBOX_OFF));
	volatile struct rtl_pe_tx_desc *tx_ring =
		(volatile struct rtl_pe_tx_desc *)((u32)shm + (RTL_PE_TX_RING_OFF - RTL_PE_MAILBOX_OFF));
	volatile u32 *free_ring =
		(volatile u32 *)((u32)shm + (RTL_PE_FREE_RING_OFF - RTL_PE_MAILBOX_OFF));

	struct pe_dp_state dp;
	u32 prid = read_cp0_prid();
	u32 pe_clk = shm->pe_clk_hz ? shm->pe_clk_hz : 800000000U;
	u32 feature_mask = shm->feature_mask & RTL_PE_FEAT_VALID_MASK;
	u32 last_cmd_seq = shm->cmd_seq;
	u32 last_dp_seq = shm->dp_seq;
	u32 last_test_seq = shm->test_seq;
	u32 vp_id = shm->dma_vp_id;
	u32 vp_ready = shm->dma_vp_ready;
	u32 vp_va = (vp_id < 12U) ? PE_DMA_VP_BASE(vp_id) : 0U;
	u32 pool_dma = shm->pool_dma;
	u32 pool_va = pool_dma ? (PE_POOL_KSEG1_BASE | (pool_dma & 0xfffffU)) : 0U;
	u32 fq_rate_kbps = shm->fq_rate_kbps;
	u32 fq_target_ns, fq_interval_ns, fq_quantum, fq_limit, fq_ecn;
	u32 mcast_rate_kbps = shm->mcast_rate_kbps;
	u32 win_start, prev_ns, now_ns;
	u32 win_busy = 0;
	u32 feat_busy[RTL_PE_FEAT_MAX];
	u32 fallback_loops = 0;
	u32 idle_ticks = 0;
	u32 i;

	dp_init(&dp);
	if (vp_ready && vp_va) {
		u32 hw_rd = pe_rd32(vp_va + PE_DMA_VP_READ_OFF) & RTL_PE_DMA_MASK;

		dp.dma_head = hw_rd;
		dp.dma_tail = hw_rd;
	}

	fq_target_ns = (shm->fq_target_us ? shm->fq_target_us : 5000U) * 1000U;
	fq_interval_ns = (shm->fq_interval_us ? shm->fq_interval_us : 100000U) * 1000U;
	if (fq_target_ns < 100000U)
		fq_target_ns = 100000U;
	if (fq_target_ns > 100000000U)
		fq_target_ns = 100000000U;
	if (fq_interval_ns < 1000000U)
		fq_interval_ns = 1000000U;
	if (fq_interval_ns > 400000000U)
		fq_interval_ns = 400000000U;
	fq_quantum = shm->fq_quantum ? shm->fq_quantum : 1514U;
	if (fq_quantum < 256U)
		fq_quantum = 256U;
	fq_limit = shm->fq_limit ? shm->fq_limit : 224U;
	if (fq_limit > 224U)
		fq_limit = 224U;
	fq_ecn = shm->fq_ecn;

	for (i = 0; i < RTL_PE_FEAT_MAX; i++) {
		feat_busy[i] = 0;
		shm->feat[i].util_permille = 0;
		shm->feat[i].cycles = 0;
		shm->feat[i].qdepth = 0;
		shm->feat[i].packets = 0;
		shm->feat[i].bytes = 0;
		shm->feat[i].drops = 0;
		shm->feat[i].ecn_marks = 0;
	}
	refresh_feature_flags(shm, feature_mask, vp_ready && vp_va && pool_va);

	shm->abi_version = RTL_PE_ABI_VERSION;
	shm->fw_version = PE_FW_VERSION;
	shm->cp0_prid = prid;
	shm->count_rate_hz = PE_NS_PER_SEC;
	shm->ack_seq = last_cmd_seq;
	shm->dp_ack_seq = last_dp_seq;
	shm->stopped_ack = 0;
	shm->heartbeat = 1;
	shm->uptime_ms = 0;
	shm->util_permille = 0;
	shm->tx_cons = 0;
	shm->free_prod = 0;
	shm->dma_tx_head = dp.dma_head;
	shm->dma_tx_tail = dp.dma_tail;
	shm->state = 1; /* running */
	pe_sync();

	log_puts(shm, ring, "PE");
	log_u32(shm, ring, shm->core_id);
	log_puts(shm, ring, " Taroko v2 prid=");
	log_hex32(shm, ring, prid);
	log_puts(shm, ring, " clk=");
	log_u32(shm, ring, pe_clk / 1000000U);
	log_puts(shm, ring, "MHz mask=");
	log_hex32(shm, ring, feature_mask);
	log_puts(shm, ring, " vp=");
	log_u32(shm, ring, vp_id);
	if (vp_ready && vp_va) {
		log_puts(shm, ring, " rd=");
		log_u32(shm, ring, dp.dma_head);
		log_puts(shm, ring, " dmactrl=");
		log_hex32(shm, ring, pe_rd32(PE_DMA_BASE_VA + PE_DMA_CTRL_OFF));
	}
	log_putc(shm, ring, '\n');

	/* Publish magic last so host observes fully initialized SHM */
	shm->magic = RTL_PE_SHM_MAGIC;
	pe_sync();

	win_start = read_hw_ns();
	prev_ns = win_start;

	for (;;) {
		u32 did_work = 0;

		/* 1. Handle clean stop request from host */
		if (shm->stop_req) {
			if (vp_ready && vp_va) {
				u32 wait_it = 10000U;

				while (wait_it--) {
					u32 hw = pe_rd32(vp_va + PE_DMA_VP_READ_OFF) & RTL_PE_DMA_MASK;

					if (hw == dp.dma_head)
						break;
					pe_idle_pause(50U);
				}
			}
			dp_reset_queues(shm, free_ring, &dp, 1);
			log_puts(shm, ring, "PE");
			log_u32(shm, ring, shm->core_id);
			log_puts(shm, ring, " clean stop ack\n");
			shm->state = 2;
			pe_sync();
			shm->stopped_ack = 1;
			pe_sync();
			for (;;)
				__asm__ volatile("nop");
		}

		/* 2. Hot-apply live feature_mask & shaper / CoDel params (zero reboot) */
		if (shm->cmd_seq != last_cmd_seq) {
			u32 new_seq = shm->cmd_seq;

			feature_mask = shm->feature_mask & RTL_PE_FEAT_VALID_MASK;
			fq_rate_kbps = shm->fq_rate_kbps;
			fq_target_ns = (shm->fq_target_us ? shm->fq_target_us : 5000U) * 1000U;
			fq_interval_ns = (shm->fq_interval_us ? shm->fq_interval_us : 100000U) * 1000U;
			if (fq_target_ns < 100000U)
				fq_target_ns = 100000U;
			if (fq_target_ns > 100000000U)
				fq_target_ns = 100000000U;
			if (fq_interval_ns < 1000000U)
				fq_interval_ns = 1000000U;
			if (fq_interval_ns > 400000000U)
				fq_interval_ns = 400000000U;
			fq_quantum = shm->fq_quantum ? shm->fq_quantum : 1514U;
			if (fq_quantum < 256U)
				fq_quantum = 256U;
			fq_limit = shm->fq_limit ? shm->fq_limit : 224U;
			if (fq_limit > 224U)
				fq_limit = 224U;
			fq_ecn = shm->fq_ecn;
			mcast_rate_kbps = shm->mcast_rate_kbps;
			dp.fq_tb_valid = 0;
			dp.mcast_tb_valid = 0;

			refresh_feature_flags(shm, feature_mask, vp_ready && vp_va && pool_va);
			for (i = 0; i < RTL_PE_FEAT_MAX; i++) {
				if (!(feature_mask & (1U << i))) {
					shm->feat[i].util_permille = 0;
					shm->feat[i].cycles = 0;
					shm->feat[i].qdepth = 0;
					feat_busy[i] = 0;
				}
			}
			last_cmd_seq = new_seq;
			shm->ack_seq = new_seq;
			pe_sync();
			did_work = 1;

			log_puts(shm, ring, "PE");
			log_u32(shm, ring, shm->core_id);
			log_puts(shm, ring, " cfg seq=");
			log_u32(shm, ring, new_seq);
			log_puts(shm, ring, " mask=");
			log_hex32(shm, ring, feature_mask);
			log_puts(shm, ring, " rate=");
			log_u32(shm, ring, fq_rate_kbps);
			log_putc(shm, ring, '\n');
		}

		/* 3. Handle DMA VP arm / reset notification */
		if (shm->dp_seq != last_dp_seq) {
			u32 new_dp = shm->dp_seq;

			vp_id = shm->dma_vp_id;
			vp_ready = shm->dma_vp_ready;
			vp_va = (vp_id < 12U) ? PE_DMA_VP_BASE(vp_id) : 0U;
			pool_dma = shm->pool_dma;
			pool_va = pool_dma ? (PE_POOL_KSEG1_BASE | (pool_dma & 0xfffffU)) : 0U;

			/* Host already reset tx_prod/free_cons and freed in-flight slots. */
			dp_init(&dp);
			if (vp_ready && vp_va) {
				u32 hw_rd = pe_rd32(vp_va + PE_DMA_VP_READ_OFF) & RTL_PE_DMA_MASK;

				dp.dma_head = hw_rd;
				dp.dma_tail = hw_rd;
			}
			shm->tx_cons = 0;
			shm->free_prod = 0;
			shm->dma_tx_head = dp.dma_head;
			shm->dma_tx_tail = dp.dma_tail;
			shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].qdepth = 0;
			shm->feat[RTL_PE_FEAT_MCAST_BIT].qdepth = 0;
			refresh_feature_flags(shm, feature_mask, vp_ready && vp_va && pool_va);

			last_dp_seq = new_dp;
			shm->dp_ack_seq = new_dp;
			pe_sync();
			did_work = 1;

			log_puts(shm, ring, "PE");
			log_u32(shm, ring, shm->core_id);
			log_puts(shm, ring, " dp seq=");
			log_u32(shm, ring, new_dp);
			log_puts(shm, ring, " ready=");
			log_u32(shm, ring, vp_ready);
			if (vp_ready && vp_va) {
				log_puts(shm, ring, " rd=");
				log_u32(shm, ring, dp.dma_head);
				log_puts(shm, ring, " dmactrl=");
				log_hex32(shm, ring, pe_rd32(PE_DMA_BASE_VA + PE_DMA_CTRL_OFF));
			}
			log_putc(shm, ring, '\n');
		}

		/* 4. On-demand host <-> PE verification burst */
		if (shm->test_seq != last_test_seq) {
			u32 tseq = shm->test_seq;
			u32 t0 = read_hw_ns();
			u32 len = shm->test_len;
			u32 iter = shm->test_iter;
			u32 crc = 0;
			u32 dt;

			if (len > sizeof(shm->test_buf))
				len = sizeof(shm->test_buf);
			if (shm->test_cmd == RTL_PE_TEST_PING_CRC && len && iter)
				crc = pe_fnv1a(shm->test_buf, len, iter);

			dt = delta_hw_ns(read_hw_ns(), t0);
			win_busy += dt;
			shm->test_crc = crc;
			shm->test_cycles = dt;
			pe_sync();
			shm->test_ack_seq = tseq;
			last_test_seq = tseq;
			pe_sync();
			did_work = 1;
		}

		/* 5. Real HW Data Plane: reclaim -> ingest -> service mcast -> service fq_codel */
		if (vp_ready && vp_va && pool_va) {
			u32 has_tx = (shm->tx_prod != dp.tx_cons);
			u32 has_dma = (dp.dma_head != dp.dma_tail);
			u32 has_q = (dp.fq_total_q || dp.mcast_qlen);

			if (has_tx || has_dma || has_q) {
				u32 t0 = read_hw_ns();
				u32 rx_n = 0, mc_n = 0, fq_n = 0, rec_n = 0;

				if (has_dma)
					rec_n = dp_reclaim_dma(shm, free_ring, &dp, vp_va);
				if (has_tx)
					rx_n = dp_drain_tx_ring(shm, tx_ring, free_ring, &dp,
								pool_va, feature_mask,
								fq_limit, fq_quantum, t0);
				if (dp.mcast_qlen)
					mc_n = dp_service_mcast(shm, dma_desc, &dp, vp_va,
								pool_dma, mcast_rate_kbps, t0);
				if (dp.fq_total_q)
					fq_n = dp_service_fq_codel(shm, dma_desc, free_ring, &dp,
								   vp_va, pool_dma, pool_va,
								   fq_rate_kbps, fq_target_ns,
								   fq_interval_ns, fq_quantum,
								   fq_ecn, t0);

				shm->feat[RTL_PE_FEAT_FQ_CODEL_BIT].qdepth = dp.fq_total_q;
				shm->feat[RTL_PE_FEAT_MCAST_BIT].qdepth = dp.mcast_qlen;

				if (rx_n || mc_n || fq_n || rec_n) {
					u32 dt = delta_hw_ns(read_hw_ns(), t0);

					win_busy += dt;
					if (mc_n)
						feat_busy[RTL_PE_FEAT_MCAST_BIT] += dt;
					if (fq_n || rx_n)
						feat_busy[RTL_PE_FEAT_FQ_CODEL_BIT] += dt;
					did_work = 1;
				}
			}
		}

		/*
		 * 6. Pure-IMEM idle backoff when queues are completely empty, OR
		 *    short IMEM yield while waiting on shaper / DMA TX completion.
		 */
		if (!did_work) {
			if (dp.fq_total_q || dp.mcast_qlen || dp.dma_head != dp.dma_tail) {
				pe_idle_pause(40U);
				if (++idle_ticks < 200U)
					continue;
			} else {
				pe_idle_pause(PE_IDLE_PAUSE_ITERS);
				if (++idle_ticks < PE_PTP_POLL_DIV)
					continue;
			}
			idle_ticks = 0;
		}

		/* 7. Publish 200 ms telemetry window + watchdog heartbeat */
		now_ns = read_hw_ns();
		if (now_ns == prev_ns) {
			if (++fallback_loops >= 200U) {
				shm->heartbeat = shm->heartbeat + 1U;
				shm->uptime_ms = shm->uptime_ms + 200U;
				shm->count_now = now_ns;
				fallback_loops = 0;
				pe_sync();
			}
		} else {
			u32 elapsed = delta_hw_ns(now_ns, win_start);

			prev_ns = now_ns;
			fallback_loops = 0;
			if (elapsed >= PE_WIN_TARGET_NS) {
				shm->count_now = now_ns;
				shm->window_cycles = elapsed;
				shm->busy_cycles = win_busy;
				shm->util_permille = calc_permille(win_busy, elapsed);

				for (i = 0; i < RTL_PE_FEAT_MAX; i++) {
					if (feature_mask & (1U << i)) {
						shm->feat[i].cycles = feat_busy[i];
						shm->feat[i].util_permille =
							calc_permille(feat_busy[i], elapsed);
					}
					feat_busy[i] = 0;
				}

				shm->uptime_ms = shm->uptime_ms + (elapsed / 1000000U);
				shm->heartbeat = shm->heartbeat + 1U;
				pe_sync();

				win_busy = 0;
				win_start = now_ns;
			}
		}
	}
}
