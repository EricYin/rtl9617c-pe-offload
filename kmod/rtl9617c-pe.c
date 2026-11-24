// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL9617C (Taurus) dual Taroko MIPS32 PE (PE0 / PE1) base driver.
 *
 * Provides safe ATU mapping, SRAM/reset/clock power-gating, firmware loading,
 * live feature_mask control, CP0 Count utilization telemetry, ring log export,
 * watchdog recovery, and warm-reboot cleanup.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include <linux/soc/realtek/rtl9617c-pe-abi.h>

#define RTL_PE_NUM_CORES		2
#define RTL_PE_DEFAULT_FW		"rtl9617c/pe-offload.bin"
#define RTL_PE_WDT_INTERVAL_MS		2000
#define RTL_PE_WDT_STALL_LIMIT		3

/* Taurus fixed physical register addresses (used as fallback when DT absent) */
#define TAURUS_GLB_BASE			0xf4320000UL
#define TAURUS_GLB_SIZE			0x300UL
#define TAURUS_GLB_BIST_OFF		0x004
#define TAURUS_GLB_RST_OFF		0x098
#define TAURUS_GLB_CFG_OFF		0x0c0
#define TAURUS_GLB_PEDIV_OFF		0x210
#define TAURUS_GLB_CPLL0_OFF		0x230
#define TAURUS_GLB_FPLL0_OFF		0x240

#define TAURUS_ATU0_BASE		0x90300000UL
#define TAURUS_ATU_STRIDE		0x100UL
#define TAURUS_ATU_SIZE			0x20UL
#define TAURUS_SRAM_BASE		0x90303068UL
#define TAURUS_SRAM_SIZE		0x18UL
#define TAURUS_SRAM_CORE_OFF(c)		(0x04 + (c) * 8)

#define TAURUS_BIST_PE_SRAM_BIT		BIT(18)
#define TAURUS_CFG_PD_OFFLOAD(c)	BIT(28 + (c))
#define TAURUS_RST_PE_BIT(c)		BIT(18 + (c))
#define TAURUS_RST_RCPU_BIT(c)		BIT(20 + (c))
#define TAURUS_SRAM_ISO_BIT		BIT(5)

enum rtl_pe_state {
	RTL_PE_STOPPED = 0,
	RTL_PE_RUNNING = 1,
	RTL_PE_FAULT = 2,
};

struct rtl_pe_ctrl;

struct rtl_pe_core {
	struct rtl_pe_ctrl *ctrl;
	unsigned int id;
	struct kobject kobj;

	void __iomem *atu;
	void __iomem *sram;

	void *mem;
	dma_addr_t dma;
	bool hw_saved;
	bool configured;
	bool clk_gated;

	u32 reset_saved;
	u32 sram_saved;
	u32 glb_saved;
	u32 atu_staging[8];
	u32 atu_entries[6][7];

	enum rtl_pe_state state;
	u32 mode;
	u32 feature_mask;
	u32 cmd_seq;
	u32 test_seq;
	u32 last_hb;
	unsigned int hb_stalls;
	u32 last_exc_cause;
	u32 last_exc_epc;
	char fw_name[96];
};

struct rtl_pe_ctrl {
	struct device *dev;
	void __iomem *glb;
	struct clk *pe_clk;
	struct clk *peaxi_clk;
	struct mutex lock;
	struct kobject *root_kobj;
	struct delayed_work wdt_work;
	struct notifier_block reboot_nb;
	struct rtl_pe_core cores[RTL_PE_NUM_CORES];
};

static struct rtl_pe_ctrl *g_pe_ctrl;
static struct platform_device *g_fallback_pdev;

static inline struct rtl_pe_shm *pe_shm(struct rtl_pe_core *pe)
{
	return pe->mem ? (struct rtl_pe_shm *)((u8 *)pe->mem + RTL_PE_MAILBOX_OFF) : NULL;
}

static inline const char *pe_log_buf(struct rtl_pe_core *pe)
{
	return pe->mem ? (const char *)((u8 *)pe->mem + RTL_PE_LOG_OFF) : NULL;
}

static void rtl_pe_read_clks(struct rtl_pe_ctrl *ctrl, u32 *pe_hz, u32 *peaxi_hz)
{
	u32 ref = 25000000U;
	u32 cpll0, fpll0, pediv, cpll, fpll, pclk, aclk;

	if (ctrl->pe_clk && ctrl->peaxi_clk) {
		*pe_hz = (u32)clk_get_rate(ctrl->pe_clk);
		*peaxi_hz = (u32)clk_get_rate(ctrl->peaxi_clk);
		return;
	}

	cpll0 = readl(ctrl->glb + TAURUS_GLB_CPLL0_OFF);
	fpll0 = readl(ctrl->glb + TAURUS_GLB_FPLL0_OFF);
	pediv = readl(ctrl->glb + TAURUS_GLB_PEDIV_OFF);

	if (cpll0 & BIT(14))
		cpll = ref * ((cpll0 & 0xff) + 3);
	else
		cpll = ref / (((cpll0 >> 12) & 0x3) + 2) * ((cpll0 & 0xff) + 3);

	if (fpll0 & BIT(14))
		fpll = ref * ((fpll0 & 0xff) + 3);
	else
		fpll = ref / (((fpll0 >> 12) & 0x3) + 2) * ((fpll0 & 0xff) + 3);

	if (pediv & BIT(6)) {
		u32 div = (pediv >> 8) & 0x3f;
		pclk = div ? (cpll * 2 / div) : cpll;
	} else {
		u32 div = pediv & 0x3f;
		pclk = div ? (fpll * 2 / div) : fpll;
	}

	aclk = (pediv & BIT(22)) ? pclk : (pclk / 2);
	*pe_hz = pclk;
	*peaxi_hz = aclk;
}

static int rtl_pe_atu_cmd(struct rtl_pe_core *pe, u32 cmd)
{
	u32 st;

	writel(cmd, pe->atu);
	return readl_poll_timeout(pe->atu, st, !(st & (BIT(30) | BIT(31))), 1, 10000);
}

static void rtl_pe_set_reset(struct rtl_pe_core *pe, u32 bits)
{
	void __iomem *rst = pe->ctrl->glb + TAURUS_GLB_RST_OFF;
	u32 mask = TAURUS_RST_PE_BIT(pe->id) | TAURUS_RST_RCPU_BIT(pe->id);
	u32 v = (readl(rst) & ~mask) | (bits & mask);

	writel(v, rst);
	readl(rst);
}

static void rtl_pe_set_clk_pd(struct rtl_pe_core *pe, bool power_down)
{
	void __iomem *cfg = pe->ctrl->glb + TAURUS_GLB_CFG_OFF;
	u32 bit = TAURUS_CFG_PD_OFFLOAD(pe->id);
	u32 v = readl(cfg);

	if (power_down)
		v |= bit;
	else
		v &= ~bit;
	writel(v, cfg);
	readl(cfg);
	pe->clk_gated = power_down;
}

static int rtl_pe_hw_save_once(struct rtl_pe_core *pe)
{
	u32 rst_val;
	unsigned int idx, i;
	int ret;

	if (pe->hw_saved)
		return 0;

	pe->glb_saved = readl(pe->ctrl->glb + TAURUS_GLB_CFG_OFF);
	rst_val = readl(pe->ctrl->glb + TAURUS_GLB_RST_OFF);
	pe->reset_saved = rst_val;
	pe->sram_saved = readl(pe->sram);

	/* Ensure PE clock is enabled briefly while reading ATU staging/entries */
	rtl_pe_set_clk_pd(pe, false);
	udelay(20);

	for (i = 0; i < 8; i++)
		pe->atu_staging[i] = readl(pe->atu + i * 4);

	if (!(rst_val & TAURUS_RST_RCPU_BIT(pe->id)) || (pe->atu_staging[0] & 0x0e)) {
		dev_warn(pe->ctrl->dev, "pe%u appears busy (rst=0x%08x atu0=0x%08x)\n",
			 pe->id, rst_val, pe->atu_staging[0]);
		return -EBUSY;
	}

	for (idx = 0; idx < 6; idx++) {
		ret = rtl_pe_atu_cmd(pe, (pe->atu_staging[0] & 0xff) | (idx << 24) | BIT(30));
		if (ret)
			return ret;
		for (i = 0; i < 7; i++)
			pe->atu_entries[idx][i] = readl(pe->atu + 4 + i * 4);
	}

	pe->hw_saved = true;
	return 0;
}

static void rtl_pe_hw_stop_locked(struct rtl_pe_core *pe, bool power_down_clk)
{
	struct rtl_pe_shm *shm = pe_shm(pe);
	u32 ack;

	if (pe->state == RTL_PE_RUNNING && shm) {
		shm->stop_req = 1;
		dma_wmb();
		read_poll_timeout(READ_ONCE, ack, ack != 0, 100, 50000, false,
				  shm->stopped_ack);
	}

	if (pe->configured) {
		rtl_pe_set_reset(pe, TAURUS_RST_PE_BIT(pe->id) | TAURUS_RST_RCPU_BIT(pe->id));
		writel(readl(pe->sram) | TAURUS_SRAM_ISO_BIT, pe->sram);
		readl(pe->sram);
		udelay(1000);
		pe->configured = false;
	}

	if (power_down_clk && pe->hw_saved)
		rtl_pe_set_clk_pd(pe, true);

	if (pe->state != RTL_PE_FAULT)
		pe->state = RTL_PE_STOPPED;
	pe->hb_stalls = 0;
}

static void rtl_pe_hw_restore_locked(struct rtl_pe_core *pe)
{
	unsigned int idx, i;

	rtl_pe_hw_stop_locked(pe, false);
	if (!pe->hw_saved)
		return;

	rtl_pe_set_clk_pd(pe, false);
	udelay(20);

	for (idx = 0; idx < 6; idx++) {
		for (i = 0; i < 7; i++)
			writel(pe->atu_entries[idx][i], pe->atu + 4 + i * 4);
		rtl_pe_atu_cmd(pe, (pe->atu_staging[0] & 0xff) | (idx << 24) | BIT(31));
	}
	for (i = 1; i < 8; i++)
		writel(pe->atu_staging[i], pe->atu + i * 4);
	writel(pe->atu_staging[0] & ~(BIT(30) | BIT(31)), pe->atu);

	writel(pe->sram_saved, pe->sram);
	rtl_pe_set_reset(pe, pe->reset_saved);

	/* Restore original clock power-down bit */
	rtl_pe_set_clk_pd(pe, !!(pe->glb_saved & TAURUS_CFG_PD_OFFLOAD(pe->id)));
	pe->hw_saved = false;
}

static bool rtl_pe_fw_name_valid(const char *name)
{
	if (!name || !name[0] || strlen(name) >= 90)
		return false;
	if (name[0] == '/' || strstr(name, ".."))
		return false;
	return true;
}

static int rtl_pe_start_locked(struct rtl_pe_core *pe)
{
	static const u32 atu_in[6]  = { 0x1fc0, 0x1000, 0x1100, 0x1200, 0x1300, 0x0fc0 };
	static const u32 atu_out[6] = { 0,      0xf430, 0xa040, 0xb040, 0xf700, 0      };
	const struct firmware *fw = NULL;
	struct rtl_pe_shm *shm;
	u32 mbox_va, pe_hz, peaxi_hz, magic, w0, w1, e0, e1;
	void __iomem *bist;
	unsigned int idx, i;
	int ret;

	if (pe->state == RTL_PE_RUNNING)
		return 0;

	if (!rtl_pe_fw_name_valid(pe->fw_name))
		return -EINVAL;

	ret = rtl_pe_hw_save_once(pe);
	if (ret)
		return ret;

	if (!pe->mem) {
		pe->mem = dma_alloc_coherent(pe->ctrl->dev, RTL_PE_MEM_SIZE,
					     &pe->dma, GFP_KERNEL);
		if (!pe->mem)
			return -ENOMEM;
		if (!IS_ALIGNED(pe->dma, RTL_PE_MEM_SIZE) ||
		    (u64)pe->dma + RTL_PE_MEM_SIZE > 0x20000000ULL) {
			dev_err(pe->ctrl->dev,
				"pe%u DMA %pad out of kseg1 512M/128K alignment\n",
				pe->id, &pe->dma);
			dma_free_coherent(pe->ctrl->dev, RTL_PE_MEM_SIZE,
					  pe->mem, pe->dma);
			pe->mem = NULL;
			return -EINVAL;
		}
	}

	ret = request_firmware(&fw, pe->fw_name, pe->ctrl->dev);
	if (ret) {
		dev_err(pe->ctrl->dev, "pe%u firmware '%s' load failed: %d\n",
			pe->id, pe->fw_name, ret);
		return ret;
	}
	if (fw->size < 0x404 || fw->size > RTL_PE_IMEM_MAX) {
		dev_err(pe->ctrl->dev, "pe%u firmware size %zu invalid (0x404..0x%x)\n",
			pe->id, fw->size, RTL_PE_IMEM_MAX);
		release_firmware(fw);
		return -EINVAL;
	}

	/* Hold reset + isolate SRAM + ungate PE clock + exit PE SRAM BIST */
	rtl_pe_set_clk_pd(pe, false);
	rtl_pe_set_reset(pe, TAURUS_RST_PE_BIT(pe->id) | TAURUS_RST_RCPU_BIT(pe->id));
	writel(readl(pe->sram) | TAURUS_SRAM_ISO_BIT, pe->sram);
	readl(pe->sram);
	udelay(1000);

	bist = pe->ctrl->glb + TAURUS_GLB_BIST_OFF;
	writel(readl(bist) & ~TAURUS_BIST_PE_SRAM_BIT, bist);
	readl(bist);

	/* Prepare coherent memory: zero full 128 KiB, copy IMEM firmware */
	memset(pe->mem, 0, RTL_PE_MEM_SIZE);
	memcpy(pe->mem, fw->data, fw->size);
	release_firmware(fw);

	/*
	 * Patch the 4 mailbox immediate instructions (lui/ori $a0 at 0x00/0x04
	 * and lui/ori $t0 at 0x380/0x384) with uncached kseg1 mailbox VA.
	 */
	mbox_va = (u32)(pe->dma + RTL_PE_MAILBOX_OFF) | 0xa0000000U;
	w0 = get_unaligned_le32((u8 *)pe->mem + 0x000) & 0xffff0000U;
	w1 = get_unaligned_le32((u8 *)pe->mem + 0x004) & 0xffff0000U;
	e0 = get_unaligned_le32((u8 *)pe->mem + 0x380) & 0xffff0000U;
	e1 = get_unaligned_le32((u8 *)pe->mem + 0x384) & 0xffff0000U;
	put_unaligned_le32(w0 | (mbox_va >> 16), (u8 *)pe->mem + 0x000);
	put_unaligned_le32(w1 | (mbox_va & 0xffffU), (u8 *)pe->mem + 0x004);
	put_unaligned_le32(e0 | (mbox_va >> 16), (u8 *)pe->mem + 0x380);
	put_unaligned_le32(e1 | (mbox_va & 0xffffU), (u8 *)pe->mem + 0x384);

	rtl_pe_read_clks(pe->ctrl, &pe_hz, &peaxi_hz);
	shm = pe_shm(pe);
	shm->core_id = pe->id;
	shm->pe_clk_hz = pe_hz;
	shm->feature_mask = pe->feature_mask;
	shm->mode = pe->mode ? pe->mode : RTL_PE_MODE_OFFLOAD;
	shm->cmd_seq = ++pe->cmd_seq;
	shm->stop_req = 0;
	dma_wmb();

	/* Program all 6 ATU translation windows */
	for (idx = 0; idx < 6; idx++) {
		bool is_code = (idx == 0 || idx == 5);

		writel(atu_in[idx], pe->atu + 4);
		writel(is_code ? 0xffffU : 0xfff0U, pe->atu + 8);
		writel(is_code ? ((u32)pe->dma >> 16) : atu_out[idx], pe->atu + 12);
		for (i = 3; i < 7; i++)
			writel(pe->atu_entries[0][i], pe->atu + 4 + i * 4);
		ret = rtl_pe_atu_cmd(pe, (pe->atu_staging[0] & 0xff) | 0x3fU |
				     (idx << 24) | BIT(31));
		if (ret) {
			dev_err(pe->ctrl->dev, "pe%u ATU[%u] write timeout\n", pe->id, idx);
			rtl_pe_set_clk_pd(pe, true);
			return ret;
		}
	}

	pe->configured = true;

	/* Release SRAM isolation -> PE wrapper reset -> RCPU reset */
	writel(readl(pe->sram) & ~TAURUS_SRAM_ISO_BIT, pe->sram);
	readl(pe->sram);
	rtl_pe_set_reset(pe, TAURUS_RST_RCPU_BIT(pe->id));
	udelay(1000);
	rtl_pe_set_reset(pe, 0);

	/* Wait up to 150 ms for PE magic handshake */
	ret = read_poll_timeout(READ_ONCE, magic, magic == RTL_PE_SHM_MAGIC,
				100, 150000, false, shm->magic);
	if (ret) {
		pe->last_exc_cause = READ_ONCE(shm->exc_cause);
		pe->last_exc_epc = READ_ONCE(shm->exc_epc);
		dev_err(pe->ctrl->dev,
			"pe%u boot timeout (magic=0x%08x state=%u cause=0x%08x epc=0x%08x cctl=0x%08x/0x%08x)\n",
			pe->id, magic, READ_ONCE(shm->state),
			pe->last_exc_cause, pe->last_exc_epc,
			READ_ONCE(shm->cp0_cctl_init), READ_ONCE(shm->cp0_cctl_run));
		pe->state = RTL_PE_FAULT;
		rtl_pe_hw_stop_locked(pe, true);
		return -ETIMEDOUT;
	}

	pe->state = RTL_PE_RUNNING;
	pe->last_hb = READ_ONCE(shm->heartbeat);
	pe->hb_stalls = 0;
	pe->last_exc_cause = 0;
	pe->last_exc_epc = 0;

	dev_info(pe->ctrl->dev,
		 "pe%u online: fw=%s prid=0x%08x clk=%u MHz axi=%u MHz dma=%pad mask=0x%x\n",
		 pe->id, pe->fw_name, READ_ONCE(shm->cp0_prid),
		 pe_hz / 1000000U, peaxi_hz / 1000000U, &pe->dma, pe->feature_mask);

	schedule_delayed_work(&pe->ctrl->wdt_work,
			      msecs_to_jiffies(RTL_PE_WDT_INTERVAL_MS));
	return 0;
}

static int rtl_pe_push_config_locked(struct rtl_pe_core *pe)
{
	struct rtl_pe_shm *shm = pe_shm(pe);
	u32 seq, ack;

	if (pe->state != RTL_PE_RUNNING || !shm)
		return 0;

	seq = ++pe->cmd_seq;
	shm->feature_mask = pe->feature_mask;
	shm->mode = pe->mode;
	dma_wmb();
	shm->cmd_seq = seq;
	dma_wmb();

	return read_poll_timeout(READ_ONCE, ack, ack == seq, 100, 50000, false,
				 shm->ack_seq);
}

static void rtl_pe_wdt_fn(struct work_struct *work)
{
	struct rtl_pe_ctrl *ctrl = container_of(to_delayed_work(work),
						struct rtl_pe_ctrl, wdt_work);
	bool any_running = false;
	unsigned int i;

	mutex_lock(&ctrl->lock);
	for (i = 0; i < RTL_PE_NUM_CORES; i++) {
		struct rtl_pe_core *pe = &ctrl->cores[i];
		struct rtl_pe_shm *shm = pe_shm(pe);
		u32 hb, st;

		if (pe->state != RTL_PE_RUNNING || !shm)
			continue;

		st = READ_ONCE(shm->state);
		hb = READ_ONCE(shm->heartbeat);
		if (st == 3) {
			pe->last_exc_cause = READ_ONCE(shm->exc_cause);
			pe->last_exc_epc = READ_ONCE(shm->exc_epc);
			dev_err(ctrl->dev, "pe%u fault exception cause=0x%08x epc=0x%08x, stopping\n",
				pe->id, pe->last_exc_cause, pe->last_exc_epc);
			pe->state = RTL_PE_FAULT;
			rtl_pe_hw_stop_locked(pe, true);
			continue;
		}
		if (hb == pe->last_hb) {
			if (++pe->hb_stalls >= RTL_PE_WDT_STALL_LIMIT) {
				dev_err(ctrl->dev, "pe%u heartbeat stalled at %u, stopping\n",
					pe->id, hb);
				pe->state = RTL_PE_FAULT;
				rtl_pe_hw_stop_locked(pe, true);
				continue;
			}
		} else {
			pe->last_hb = hb;
			pe->hb_stalls = 0;
		}
		any_running = true;
	}
	mutex_unlock(&ctrl->lock);

	if (any_running)
		schedule_delayed_work(&ctrl->wdt_work,
				      msecs_to_jiffies(RTL_PE_WDT_INTERVAL_MS));
}

/* ===================== Sysfs Interface ===================== */

struct pe_core_attr {
	struct attribute attr;
	ssize_t (*show)(struct rtl_pe_core *pe, char *buf);
	ssize_t (*store)(struct rtl_pe_core *pe, const char *buf, size_t count);
};

static ssize_t pe_core_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct rtl_pe_core *pe = container_of(kobj, struct rtl_pe_core, kobj);
	struct pe_core_attr *pa = container_of(attr, struct pe_core_attr, attr);

	if (!pa->show)
		return -EIO;
	return pa->show(pe, buf);
}

static ssize_t pe_core_attr_store(struct kobject *kobj, struct attribute *attr,
				  const char *buf, size_t count)
{
	struct rtl_pe_core *pe = container_of(kobj, struct rtl_pe_core, kobj);
	struct pe_core_attr *pa = container_of(attr, struct pe_core_attr, attr);

	if (!pa->store)
		return -EIO;
	return pa->store(pe, buf, count);
}

static const struct sysfs_ops pe_core_sysfs_ops = {
	.show = pe_core_attr_show,
	.store = pe_core_attr_store,
};

static const char *rtl_pe_state_name(enum rtl_pe_state st)
{
	switch (st) {
	case RTL_PE_RUNNING: return "running";
	case RTL_PE_FAULT:   return "fault";
	default:             return "stopped";
	}
}

static const char *rtl_pe_mode_name(u32 mode)
{
	switch (mode) {
	case RTL_PE_MODE_OFFLOAD:  return "offload";
	case RTL_PE_MODE_WIFI_WFO: return "wifi_wfo";
	default:                   return "off";
	}
}

static ssize_t state_show(struct rtl_pe_core *pe, char *buf)
{
	ssize_t n;

	mutex_lock(&pe->ctrl->lock);
	n = sysfs_emit(buf, "%s\n", rtl_pe_state_name(pe->state));
	mutex_unlock(&pe->ctrl->lock);
	return n;
}

static ssize_t enable_show(struct rtl_pe_core *pe, char *buf)
{
	ssize_t n;

	mutex_lock(&pe->ctrl->lock);
	n = sysfs_emit(buf, "%u\n", pe->state == RTL_PE_RUNNING ? 1U : 0U);
	mutex_unlock(&pe->ctrl->lock);
	return n;
}

static ssize_t enable_store(struct rtl_pe_core *pe, const char *buf, size_t count)
{
	bool en;
	int ret;

	ret = kstrtobool(buf, &en);
	if (ret)
		return ret;

	mutex_lock(&pe->ctrl->lock);
	if (en) {
		if (pe->mode == RTL_PE_MODE_OFF)
			pe->mode = RTL_PE_MODE_OFFLOAD;
		ret = rtl_pe_start_locked(pe);
	} else {
		rtl_pe_hw_stop_locked(pe, true);
		pe->state = RTL_PE_STOPPED;
		ret = 0;
	}
	mutex_unlock(&pe->ctrl->lock);
	return ret ? ret : count;
}

static ssize_t mode_show(struct rtl_pe_core *pe, char *buf)
{
	ssize_t n;

	mutex_lock(&pe->ctrl->lock);
	n = sysfs_emit(buf, "%s\n", rtl_pe_mode_name(pe->mode));
	mutex_unlock(&pe->ctrl->lock);
	return n;
}

static ssize_t mode_store(struct rtl_pe_core *pe, const char *buf, size_t count)
{
	u32 mode;
	int ret = 0;

	if (sysfs_streq(buf, "off"))
		mode = RTL_PE_MODE_OFF;
	else if (sysfs_streq(buf, "offload"))
		mode = RTL_PE_MODE_OFFLOAD;
	else if (sysfs_streq(buf, "wifi_wfo"))
		mode = RTL_PE_MODE_WIFI_WFO;
	else
		return -EINVAL;

	mutex_lock(&pe->ctrl->lock);
	pe->mode = mode;
	if (mode == RTL_PE_MODE_OFF) {
		pe->feature_mask = 0;
		rtl_pe_hw_stop_locked(pe, true);
		pe->state = RTL_PE_STOPPED;
	} else if (pe->state == RTL_PE_RUNNING) {
		ret = rtl_pe_push_config_locked(pe);
	}
	mutex_unlock(&pe->ctrl->lock);
	return ret ? ret : count;
}

static ssize_t firmware_show(struct rtl_pe_core *pe, char *buf)
{
	ssize_t n;

	mutex_lock(&pe->ctrl->lock);
	n = sysfs_emit(buf, "%s\n", pe->fw_name);
	mutex_unlock(&pe->ctrl->lock);
	return n;
}

static ssize_t firmware_store(struct rtl_pe_core *pe, const char *buf, size_t count)
{
	char tmp[96];
	size_t len;

	len = min(count, sizeof(tmp) - 1);
	memcpy(tmp, buf, len);
	tmp[len] = '\0';
	strim(tmp);

	if (!rtl_pe_fw_name_valid(tmp))
		return -EINVAL;

	mutex_lock(&pe->ctrl->lock);
	strscpy(pe->fw_name, tmp, sizeof(pe->fw_name));
	mutex_unlock(&pe->ctrl->lock);
	return count;
}

static ssize_t feature_mask_show(struct rtl_pe_core *pe, char *buf)
{
	ssize_t n;

	mutex_lock(&pe->ctrl->lock);
	n = sysfs_emit(buf, "0x%x\n", pe->feature_mask);
	mutex_unlock(&pe->ctrl->lock);
	return n;
}

static ssize_t feature_mask_store(struct rtl_pe_core *pe, const char *buf, size_t count)
{
	u32 mask;
	int ret;

	ret = kstrtou32(buf, 0, &mask);
	if (ret)
		return ret;
	if (mask & ~RTL_PE_FEAT_VALID_MASK)
		return -EINVAL;

	mutex_lock(&pe->ctrl->lock);
	pe->feature_mask = mask;
	if (pe->state == RTL_PE_RUNNING)
		ret = rtl_pe_push_config_locked(pe);
	mutex_unlock(&pe->ctrl->lock);
	return ret ? ret : count;
}

static ssize_t stats_show(struct rtl_pe_core *pe, char *buf)
{
	struct rtl_pe_shm *shm;
	u32 pe_hz, peaxi_hz;
	int len = 0;
	unsigned int i;

	mutex_lock(&pe->ctrl->lock);
	rtl_pe_read_clks(pe->ctrl, &pe_hz, &peaxi_hz);
	shm = pe_shm(pe);

	len += sysfs_emit_at(buf, len, "core=%u\n", pe->id);
	len += sysfs_emit_at(buf, len, "state=%s\n", rtl_pe_state_name(pe->state));
	len += sysfs_emit_at(buf, len, "mode=%s\n", rtl_pe_mode_name(pe->mode));
	len += sysfs_emit_at(buf, len, "feature_mask=0x%x\n", pe->feature_mask);
	len += sysfs_emit_at(buf, len, "pe_clk_hz=%u\n", pe_hz);
	len += sysfs_emit_at(buf, len, "peaxi_clk_hz=%u\n", peaxi_hz);
	len += sysfs_emit_at(buf, len, "clk_gated=%u\n", pe->clk_gated ? 1U : 0U);
	len += sysfs_emit_at(buf, len, "exc_cause=0x%08x\n", pe->last_exc_cause);
	len += sysfs_emit_at(buf, len, "exc_epc=0x%08x\n", pe->last_exc_epc);

	if (shm && pe->state == RTL_PE_RUNNING) {
		dma_rmb();
		len += sysfs_emit_at(buf, len, "abi_version=%u\n", READ_ONCE(shm->abi_version));
		len += sysfs_emit_at(buf, len, "fw_version=0x%08x\n", READ_ONCE(shm->fw_version));
		len += sysfs_emit_at(buf, len, "cp0_prid=0x%08x\n", READ_ONCE(shm->cp0_prid));
		len += sysfs_emit_at(buf, len, "cp0_cctl_init=0x%08x\n", READ_ONCE(shm->cp0_cctl_init));
		len += sysfs_emit_at(buf, len, "cp0_cctl_run=0x%08x\n", READ_ONCE(shm->cp0_cctl_run));
		len += sysfs_emit_at(buf, len, "heartbeat=%u\n", READ_ONCE(shm->heartbeat));
		len += sysfs_emit_at(buf, len, "uptime_ms=%u\n", READ_ONCE(shm->uptime_ms));
		len += sysfs_emit_at(buf, len, "count_now=%u\n", READ_ONCE(shm->count_now));
		len += sysfs_emit_at(buf, len, "count_rate_hz=%u\n", READ_ONCE(shm->count_rate_hz));
		len += sysfs_emit_at(buf, len, "window_cycles=%u\n", READ_ONCE(shm->window_cycles));
		len += sysfs_emit_at(buf, len, "busy_cycles=%u\n", READ_ONCE(shm->busy_cycles));
		len += sysfs_emit_at(buf, len, "util_permille=%u\n", READ_ONCE(shm->util_permille));
		len += sysfs_emit_at(buf, len, "test_crc=0x%08x\n", READ_ONCE(shm->test_crc));
		len += sysfs_emit_at(buf, len, "test_cycles=%u\n", READ_ONCE(shm->test_cycles));

		for (i = 0; i < RTL_PE_FEAT_MAX; i++) {
			len += sysfs_emit_at(buf, len,
					     "feat%u=%u %u %llu %llu %u 0x%x\n",
					     i,
					     READ_ONCE(shm->feat[i].util_permille),
					     READ_ONCE(shm->feat[i].cycles),
					     READ_ONCE(shm->feat[i].packets),
					     READ_ONCE(shm->feat[i].bytes),
					     READ_ONCE(shm->feat[i].qdepth),
					     READ_ONCE(shm->feat[i].flags));
		}
	} else {
		len += sysfs_emit_at(buf, len, "util_permille=0\n");
		len += sysfs_emit_at(buf, len, "heartbeat=0\n");
		len += sysfs_emit_at(buf, len, "uptime_ms=0\n");
	}
	mutex_unlock(&pe->ctrl->lock);
	return len;
}

static ssize_t log_show(struct rtl_pe_core *pe, char *buf)
{
	struct rtl_pe_shm *shm;
	const char *ring;
	u32 wr, avail, take, start, first, second;

	mutex_lock(&pe->ctrl->lock);
	shm = pe_shm(pe);
	ring = pe_log_buf(pe);
	if (!shm || !ring) {
		mutex_unlock(&pe->ctrl->lock);
		return 0;
	}

	dma_rmb();
	wr = READ_ONCE(shm->log_wr);
	avail = min_t(u32, wr, RTL_PE_LOG_SIZE);
	take = min_t(u32, avail, PAGE_SIZE - 1);
	if (!take) {
		mutex_unlock(&pe->ctrl->lock);
		return 0;
	}

	start = (wr - take) & (RTL_PE_LOG_SIZE - 1);
	first = min_t(u32, take, RTL_PE_LOG_SIZE - start);
	second = take - first;
	memcpy(buf, ring + start, first);
	if (second)
		memcpy(buf + first, ring, second);
	buf[take] = '\0';
	mutex_unlock(&pe->ctrl->lock);
	return take;
}

static u32 rtl_pe_fnv1a(const u8 *data, u32 len, u32 iter)
{
	u32 h = 0x811c9dc5U;
	u32 r, i;

	for (r = 0; r < iter; r++) {
		for (i = 0; i < len; i++) {
			h ^= data[i];
			h *= 0x01000193U;
		}
	}
	return h;
}

static ssize_t selftest_store(struct rtl_pe_core *pe, const char *buf, size_t count)
{
	struct rtl_pe_shm *shm;
	u32 iter, seq, ack, got_crc, expect_crc, cyc;
	unsigned int i;
	int ret;

	ret = kstrtou32(buf, 0, &iter);
	if (ret || iter == 0 || iter > 50000)
		return ret ? ret : -EINVAL;

	mutex_lock(&pe->ctrl->lock);
	shm = pe_shm(pe);
	if (pe->state != RTL_PE_RUNNING || !shm) {
		mutex_unlock(&pe->ctrl->lock);
		return -ENODEV;
	}

	for (i = 0; i < 256; i++)
		shm->test_buf[i] = (u8)((i * 37U + pe->id * 13U + iter) & 0xffU);
	expect_crc = rtl_pe_fnv1a(shm->test_buf, 256, iter);

	seq = ++pe->test_seq;
	shm->test_cmd = RTL_PE_TEST_PING_CRC;
	shm->test_len = 256;
	shm->test_iter = iter;
	dma_wmb();
	shm->test_seq = seq;
	dma_wmb();

	ret = read_poll_timeout(READ_ONCE, ack, ack == seq, 200, 2000000, false,
				shm->test_ack_seq);
	if (ret) {
		dev_err(pe->ctrl->dev, "pe%u selftest timeout (iter=%u)\n", pe->id, iter);
		mutex_unlock(&pe->ctrl->lock);
		return -ETIMEDOUT;
	}

	dma_rmb();
	got_crc = READ_ONCE(shm->test_crc);
	cyc = READ_ONCE(shm->test_cycles);
	mutex_unlock(&pe->ctrl->lock);

	if (got_crc != expect_crc) {
		dev_err(pe->ctrl->dev, "pe%u selftest CRC mismatch: got=0x%08x expect=0x%08x\n",
			pe->id, got_crc, expect_crc);
		return -EIO;
	}

	dev_info(pe->ctrl->dev, "pe%u selftest PASS: iter=%u len=256 crc=0x%08x cycles=%u\n",
		 pe->id, iter, got_crc, cyc);
	return count;
}

#define PE_ATTR_RO(_name) \
	static struct pe_core_attr pe_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = _name##_show, \
	}

#define PE_ATTR_RW(_name) \
	static struct pe_core_attr pe_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = 0644 }, \
		.show = _name##_show, \
		.store = _name##_store, \
	}

#define PE_ATTR_WO(_name) \
	static struct pe_core_attr pe_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = 0200 }, \
		.store = _name##_store, \
	}

PE_ATTR_RO(state);
PE_ATTR_RW(enable);
PE_ATTR_RW(mode);
PE_ATTR_RW(firmware);
PE_ATTR_RW(feature_mask);
PE_ATTR_RO(stats);
PE_ATTR_RO(log);
PE_ATTR_WO(selftest);

static struct attribute *pe_core_attrs[] = {
	&pe_attr_state.attr,
	&pe_attr_enable.attr,
	&pe_attr_mode.attr,
	&pe_attr_firmware.attr,
	&pe_attr_feature_mask.attr,
	&pe_attr_stats.attr,
	&pe_attr_log.attr,
	&pe_attr_selftest.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pe_core);

static void pe_core_release(struct kobject *kobj) { }

static const struct kobj_type pe_core_ktype = {
	.sysfs_ops = &pe_core_sysfs_ops,
	.release = pe_core_release,
	.default_groups = pe_core_groups,
};

static ssize_t pe_clk_hz_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u32 pe_hz = 0, peaxi_hz = 0;

	if (g_pe_ctrl)
		rtl_pe_read_clks(g_pe_ctrl, &pe_hz, &peaxi_hz);
	return sysfs_emit(buf, "%u\n", pe_hz);
}

static ssize_t peaxi_clk_hz_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u32 pe_hz = 0, peaxi_hz = 0;

	if (g_pe_ctrl)
		rtl_pe_read_clks(g_pe_ctrl, &pe_hz, &peaxi_hz);
	return sysfs_emit(buf, "%u\n", peaxi_hz);
}

static struct kobj_attribute pe_clk_hz_attr = __ATTR_RO(pe_clk_hz);
static struct kobj_attribute peaxi_clk_hz_attr = __ATTR_RO(peaxi_clk_hz);

static struct attribute *pe_root_attrs[] = {
	&pe_clk_hz_attr.attr,
	&peaxi_clk_hz_attr.attr,
	NULL,
};

static const struct attribute_group pe_root_group = {
	.attrs = pe_root_attrs,
};

static void rtl_pe_quiesce_all(struct rtl_pe_ctrl *ctrl)
{
	unsigned int i;

	cancel_delayed_work_sync(&ctrl->wdt_work);
	mutex_lock(&ctrl->lock);
	for (i = 0; i < RTL_PE_NUM_CORES; i++)
		rtl_pe_hw_restore_locked(&ctrl->cores[i]);
	mutex_unlock(&ctrl->lock);
}

static int rtl_pe_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct rtl_pe_ctrl *ctrl = container_of(nb, struct rtl_pe_ctrl, reboot_nb);

	rtl_pe_quiesce_all(ctrl);
	return NOTIFY_DONE;
}

static int rtl_pe_probe(struct platform_device *pdev)
{
	struct rtl_pe_ctrl *ctrl;
	void __iomem *sram_base;
	u32 pe_hz, peaxi_hz;
	unsigned int i;
	int ret;

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = &pdev->dev;
	mutex_init(&ctrl->lock);
	INIT_DELAYED_WORK(&ctrl->wdt_work, rtl_pe_wdt_fn);

	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ctrl->glb = devm_ioremap(&pdev->dev, TAURUS_GLB_BASE, TAURUS_GLB_SIZE);
	sram_base = devm_ioremap(&pdev->dev, TAURUS_SRAM_BASE, TAURUS_SRAM_SIZE);
	if (!ctrl->glb || !sram_base)
		return -ENOMEM;

	ctrl->pe_clk = devm_clk_get_optional(&pdev->dev, "pe");
	if (IS_ERR(ctrl->pe_clk))
		ctrl->pe_clk = NULL;
	ctrl->peaxi_clk = devm_clk_get_optional(&pdev->dev, "peaxi");
	if (IS_ERR(ctrl->peaxi_clk))
		ctrl->peaxi_clk = NULL;

	ctrl->root_kobj = kobject_create_and_add("rtl9617c_pe", kernel_kobj);
	if (!ctrl->root_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(ctrl->root_kobj, &pe_root_group);
	if (ret)
		goto err_root;

	for (i = 0; i < RTL_PE_NUM_CORES; i++) {
		struct rtl_pe_core *pe = &ctrl->cores[i];

		pe->ctrl = ctrl;
		pe->id = i;
		pe->state = RTL_PE_STOPPED;
		pe->mode = RTL_PE_MODE_OFF;
		strscpy(pe->fw_name, RTL_PE_DEFAULT_FW, sizeof(pe->fw_name));

		pe->atu = devm_ioremap(&pdev->dev,
				       TAURUS_ATU0_BASE + i * TAURUS_ATU_STRIDE,
				       TAURUS_ATU_SIZE);
		if (!pe->atu) {
			ret = -ENOMEM;
			goto err_cores;
		}
		pe->sram = sram_base + TAURUS_SRAM_CORE_OFF(i);
		pe->clk_gated = !!(readl(ctrl->glb + TAURUS_GLB_CFG_OFF) &
				   TAURUS_CFG_PD_OFFLOAD(i));

		ret = kobject_init_and_add(&pe->kobj, &pe_core_ktype,
					   ctrl->root_kobj, "pe%u", i);
		if (ret) {
			kobject_put(&pe->kobj);
			goto err_cores;
		}
	}

	ctrl->reboot_nb.notifier_call = rtl_pe_reboot_notify;
	register_reboot_notifier(&ctrl->reboot_nb);

	g_pe_ctrl = ctrl;
	platform_set_drvdata(pdev, ctrl);

	rtl_pe_read_clks(ctrl, &pe_hz, &peaxi_hz);
	dev_info(&pdev->dev, "RTL9617C dual Taroko MIPS32 PE base ready (PE %u MHz, AXI %u MHz)\n",
		 pe_hz / 1000000U, peaxi_hz / 1000000U);
	return 0;

err_cores:
	while (i--)
		kobject_put(&ctrl->cores[i].kobj);
	sysfs_remove_group(ctrl->root_kobj, &pe_root_group);
err_root:
	kobject_put(ctrl->root_kobj);
	return ret;
}

static void rtl_pe_remove(struct platform_device *pdev)
{
	struct rtl_pe_ctrl *ctrl = platform_get_drvdata(pdev);
	unsigned int i;

	unregister_reboot_notifier(&ctrl->reboot_nb);
	rtl_pe_quiesce_all(ctrl);
	g_pe_ctrl = NULL;

	for (i = 0; i < RTL_PE_NUM_CORES; i++) {
		kobject_put(&ctrl->cores[i].kobj);
		if (ctrl->cores[i].mem) {
			dma_free_coherent(ctrl->dev, RTL_PE_MEM_SIZE,
					  ctrl->cores[i].mem, ctrl->cores[i].dma);
			ctrl->cores[i].mem = NULL;
		}
	}
	sysfs_remove_group(ctrl->root_kobj, &pe_root_group);
	kobject_put(ctrl->root_kobj);
}

static void rtl_pe_shutdown(struct platform_device *pdev)
{
	struct rtl_pe_ctrl *ctrl = platform_get_drvdata(pdev);

	if (ctrl)
		rtl_pe_quiesce_all(ctrl);
}

static const struct of_device_id rtl_pe_of_match[] = {
	{ .compatible = "realtek,taurus-pe" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl_pe_of_match);

static struct platform_driver rtl_pe_driver = {
	.probe = rtl_pe_probe,
	.remove = rtl_pe_remove,
	.shutdown = rtl_pe_shutdown,
	.driver = {
		.name = "rtl9617c-pe",
		.of_match_table = rtl_pe_of_match,
	},
};

static int __init rtl_pe_init(void)
{
	int ret;

	ret = platform_driver_register(&rtl_pe_driver);
	if (ret)
		return ret;

	/* Allow live insmod on existing RTL9617C DTBs lacking realtek,taurus-pe */
	if (!g_pe_ctrl && of_machine_is_compatible("realtek,rtl9617c")) {
		g_fallback_pdev = platform_device_register_simple("rtl9617c-pe", -1, NULL, 0);
		if (IS_ERR(g_fallback_pdev)) {
			ret = PTR_ERR(g_fallback_pdev);
			g_fallback_pdev = NULL;
			platform_driver_unregister(&rtl_pe_driver);
			return ret;
		}
	}
	return 0;
}

static void __exit rtl_pe_exit(void)
{
	if (g_fallback_pdev) {
		platform_device_unregister(g_fallback_pdev);
		g_fallback_pdev = NULL;
	}
	platform_driver_unregister(&rtl_pe_driver);
}

module_init(rtl_pe_init);
module_exit(rtl_pe_exit);

MODULE_AUTHOR("crysister");
MODULE_DESCRIPTION("Realtek RTL9617C (Taurus) dual Taroko MIPS32 PE base driver");
MODULE_LICENSE("GPL");
