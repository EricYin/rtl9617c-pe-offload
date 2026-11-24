/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Realtek RTL9617C (Taurus) unified bare-metal Taroko MIPS32 PE firmware.
 *
 * Runs zero-wait in 32 KiB IMEM (0x8fc00400) with private 8 KiB DMEM stack.
 * All mutable state lives on the DMEM stack or uncached kseg1 mailbox ($a0).
 * Non-exclusive offload features coexist via shm->feature_mask bitmask.
 */

#include "rtl9617c-pe-abi.h"

#define PE_FW_VERSION		0x00010001U	/* v1.0.1 */
#define PE_LOG_MASK		(RTL_PE_LOG_SIZE - 1U)
/*
 * Realtek Taroko RX5281 (PrId 0xdc02) omits standard MIPS32 CP0 $9 Count.
 * Use the SoC's free-running 1 ns NI PTP timer (0xf430e01c), already mapped
 * uncached at 0xb000e01c through ATU entry 1 (0x10000000 -> 0xf4300000).
 */
#define PE_PTP_TIMER_VA		0xb000e01cU
#define PE_PTP_NS_MASK		0x3fffffffU
#define PE_NS_PER_SEC		1000000000U
#define PE_WIN_TARGET_NS	200000000U	/* 200 ms sampling window */
#define PE_IDLE_PAUSE_ITERS	2000U		/* ~5 us pure-IMEM backoff */
#define PE_PTP_POLL_DIV		200U		/* ~1 ms NE APB read cadence */

static inline void pe_sync(void)
{
	__asm__ volatile(".word 0x0000000f" ::: "memory");
}

static inline void pe_idle_pause(u32 iters)
{
	while (iters--)
		__asm__ volatile("nop");
}

static inline u32 read_hw_ns(void)
{
	return (*(volatile u32 *)PE_PTP_TIMER_VA) & PE_PTP_NS_MASK;
}

static inline u32 delta_hw_ns(u32 now, u32 prev)
{
	return (now >= prev) ? (now - prev) : (now + PE_NS_PER_SEC - prev);
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

/* Overflow-safe 32-bit permille calculation: (busy * 1000) / total */
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

/*
 * Feature slot polling hooks (Phase 1 unified non-exclusive framework).
 * Each slot returns >0 if it processed work in this poll cycle; Phase 2 plugs
 * L3QM EPP/EQ packet rings directly into these slots without changing the ABI.
 */
static u32 poll_feature_slot(volatile struct rtl_pe_shm *shm, u32 idx)
{
	(void)shm;
	(void)idx;
	return 0;
}

void __attribute__((section(".text.pe_main"), noreturn))
pe_main(volatile struct rtl_pe_shm *shm)
{
	volatile char *ring = (volatile char *)((u32)shm + (RTL_PE_LOG_OFF - RTL_PE_MAILBOX_OFF));
	u32 prid = read_cp0_prid();
	u32 pe_clk = shm->pe_clk_hz ? shm->pe_clk_hz : 800000000U;
	u32 feature_mask = shm->feature_mask & RTL_PE_FEAT_VALID_MASK;
	u32 last_cmd_seq = shm->cmd_seq;
	u32 last_test_seq = shm->test_seq;
	u32 win_start, prev_ns, now_ns;
	u32 win_busy = 0;
	u32 feat_busy[RTL_PE_FEAT_MAX];
	u32 fallback_loops = 0;
	u32 idle_ticks = 0;
	u32 i;

	for (i = 0; i < RTL_PE_FEAT_MAX; i++) {
		feat_busy[i] = 0;
		shm->feat[i].util_permille = 0;
		shm->feat[i].cycles = 0;
		shm->feat[i].qdepth = 0;
		shm->feat[i].flags = (feature_mask & (1U << i)) ? 1U : 0U;
		shm->feat[i].packets = 0;
		shm->feat[i].bytes = 0;
	}

	shm->abi_version = RTL_PE_ABI_VERSION;
	shm->fw_version = PE_FW_VERSION;
	shm->cp0_prid = prid;
	shm->count_rate_hz = PE_NS_PER_SEC;
	shm->ack_seq = last_cmd_seq;
	shm->stopped_ack = 0;
	shm->heartbeat = 1;
	shm->uptime_ms = 0;
	shm->util_permille = 0;
	shm->state = 1; /* running */
	pe_sync();

	log_puts(shm, ring, "PE");
	log_u32(shm, ring, shm->core_id);
	log_puts(shm, ring, " Taroko RX5281 online prid=");
	log_hex32(shm, ring, prid);
	log_puts(shm, ring, " clk=");
	log_u32(shm, ring, pe_clk / 1000000U);
	log_puts(shm, ring, "MHz mask=");
	log_hex32(shm, ring, feature_mask);
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

		/* 2. Hot-apply live feature_mask / mode updates (zero reboot) */
		if (shm->cmd_seq != last_cmd_seq) {
			u32 new_seq = shm->cmd_seq;

			feature_mask = shm->feature_mask & RTL_PE_FEAT_VALID_MASK;
			for (i = 0; i < RTL_PE_FEAT_MAX; i++) {
				if (feature_mask & (1U << i)) {
					shm->feat[i].flags |= 1U;
				} else {
					shm->feat[i].flags &= ~1U;
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
			log_puts(shm, ring, " config seq=");
			log_u32(shm, ring, new_seq);
			log_puts(shm, ring, " mask=");
			log_hex32(shm, ring, feature_mask);
			log_putc(shm, ring, '\n');
		}

		/* 3. On-demand host <-> PE verification burst (counts real busy ns) */
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

		/* 4. Poll active non-exclusive offload feature slots (no idle NE MMIO) */
		if (feature_mask) {
			for (i = 0; i < RTL_PE_FEAT_MAX; i++) {
				u32 done;

				if (!(feature_mask & (1U << i)))
					continue;
				done = poll_feature_slot(shm, i);
				if (done) {
					did_work = 1;
					feat_busy[i] += done;
					win_busy += done;
				}
			}
		}

		/*
		 * 5. Pure-IMEM idle backoff so PE never floods the shared DDR AXI
		 *    or Network Engine (0xf4300000) APB register bus when idle.
		 *    Sample the NE PTP timer at ~1 ms cadence rather than every loop.
		 */
		if (!did_work) {
			pe_idle_pause(PE_IDLE_PAUSE_ITERS);
			if (++idle_ticks < PE_PTP_POLL_DIV)
				continue;
			idle_ticks = 0;
		}

		/* 6. Publish 200 ms telemetry window + watchdog heartbeat */
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
