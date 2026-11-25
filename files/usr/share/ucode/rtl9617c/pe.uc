/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared ucode library for RTL9617C PE0/PE1 orchestration and telemetry. */

import * as fs from 'fs';
import * as uci from 'uci';

const SYS_ROOT = '/sys/kernel/rtl9617c_pe';
const FW_OFFLOAD = 'rtl9617c/pe-offload.bin';
const FW_WFO = 'rtl9617c/pe-wfo.bin';

export const FEATURES = [
	{
		id: 'fq_codel',
		bit: 0,
		name: 'FQ-CoDel 硬件公平队列',
		desc: '在 PE 旁路运行 64 队列 FQ-CoDel（支持 1ns PTP 定时整形与 ECN 标记），消除上行/代理路径缓冲膨胀',
		module_title: '网口总限速与排队',
		module_url: 'admin/network/rtl9617c_qos/ports',
		default_pe: 0,
		hw_capable: true,
		note: '已接入 NI TX 钩子与 DMA LSO VP 直发引擎，可与其他有线卸载同核共存'
	},
	{
		id: 'ipsec',
		bit: 1,
		name: 'IPsec ESP 卸载框架',
		desc: '框架保留；当前 0x90500000 密码引擎由内核 rtk-taurus-crypto 管理，加解密由 ARMv8 AES-CE 承担',
		module_title: 'VPN 硬件加速',
		module_url: 'admin/network/rtl9617c_vpn',
		default_pe: 0,
		hw_capable: false,
		note: '仅状态占位（实机密码由内核 ARMv8 AES-CE 执行，未绑定 PE DMA TX 路径）'
	},
	{
		id: 'srv6',
		bit: 2,
		name: 'SRv6 / 变长隧道头框架',
		desc: '框架保留；当前变长 SRv6 封装由内核网络栈与 FE 表项协同承担',
		module_title: 'VPN 硬件加速',
		module_url: 'admin/network/rtl9617c_vpn',
		default_pe: 1,
		hw_capable: false,
		note: '仅状态占位（当前未绑定 PE DMA TX 路径）'
	},
	{
		id: 'mcast',
		bit: 3,
		name: '组播队列硬件直发',
		desc: '接管主机侧发送的组播/广播报文，经 PE 与独立 DMA LSO VP 发送并支持可选速率平滑',
		module_title: '硬件加速与卸载',
		module_url: 'admin/network/rtl9617c_offload',
		default_pe: 1,
		hw_capable: true,
		note: '已接入 NI TX 钩子与 DMA LSO VP 直发引擎，可与其他有线卸载同核共存'
	}
];

const PARAM_DEFAULTS = {
	fq_rate_kbps:    { sec: 'fq_codel', opt: 'rate_kbps',   def: 0,      min: 0,    max: 10000000 },
	fq_target_us:    { sec: 'fq_codel', opt: 'target_us',   def: 5000,   min: 100,  max: 100000 },
	fq_interval_us:  { sec: 'fq_codel', opt: 'interval_us', def: 100000, min: 1000, max: 400000 },
	fq_quantum:      { sec: 'fq_codel', opt: 'quantum',     def: 1514,   min: 256,  max: 9000 },
	fq_limit:        { sec: 'fq_codel', opt: 'limit',       def: 224,    min: 16,   max: 224 },
	fq_ecn:          { sec: 'fq_codel', opt: 'ecn',         def: 1,      min: 0,    max: 1 },
	mcast_rate_kbps: { sec: 'mcast',    opt: 'rate_kbps',   def: 0,      min: 0,    max: 10000000 }
};

function clamp_int(v, spec) {
	let n = int(v);
	if (n < spec.min)
		return spec.min;
	if (n > spec.max)
		return spec.max;
	return n;
}

function is_dir(path) {
	let st = fs.stat(path);
	return !!(st && st.type == 'directory');
}

function read_str(path) {
	let s = fs.readfile(path);
	return s != null ? trim(s) : null;
}

function write_str(path, val) {
	let n = fs.writefile(path, sprintf('%s\n', val));
	return n != null && n > 0;
}

function find_feature(id) {
	for (let f in FEATURES) {
		if (f.id == id)
			return f;
	}
	return null;
}

function detect_wireless() {
	let phys = [];
	let entries = fs.lsdir('/sys/class/ieee80211');
	if (entries) {
		for (let e in entries) {
			if (match(e, /^phy[0-9]+$/))
				push(phys, e);
		}
	}
	let wfo_fw = fs.access('/lib/firmware/' + FW_WFO, 'r');
	return {
		present: length(phys) > 0,
		phys: phys,
		wfo_fw_present: !!wfo_fw,
		wfo_available: (length(phys) > 0) && !!wfo_fw
	};
}

function load_cfg() {
	let cur = uci.cursor();
	let mode = cur.get('rtl9617c_pe', 'global', 'mode') || 'offload';
	if (mode != 'off' && mode != 'offload' && mode != 'wifi_wfo')
		mode = 'offload';

	let feat_cfg = {};
	for (let f in FEATURES) {
		let en = cur.get('rtl9617c_pe', f.id, 'enabled');
		let pe = int(cur.get('rtl9617c_pe', f.id, 'pe') ?? f.default_pe);
		if (pe != 0 && pe != 1)
			pe = f.default_pe;
		feat_cfg[f.id] = {
			enabled: (en == '1' || en == 'true'),
			pe: pe
		};
	}

	let params = {};
	for (let k, spec in PARAM_DEFAULTS) {
		let raw = cur.get('rtl9617c_pe', spec.sec, spec.opt);
		params[k] = clamp_int(raw != null ? raw : spec.def, spec);
	}
	return { mode: mode, features: feat_cfg, params: params };
}

function save_cfg(cfg) {
	let cur = uci.cursor();
	if (!cur.get('rtl9617c_pe', 'global'))
		cur.set('rtl9617c_pe', 'global', 'global');
	cur.set('rtl9617c_pe', 'global', 'mode', cfg.mode);

	for (let f in FEATURES) {
		let fc = cfg.features[f.id] || { enabled: false, pe: f.default_pe };
		if (!cur.get('rtl9617c_pe', f.id))
			cur.set('rtl9617c_pe', f.id, 'feature');
		cur.set('rtl9617c_pe', f.id, 'enabled', fc.enabled ? '1' : '0');
		cur.set('rtl9617c_pe', f.id, 'pe', sprintf('%d', fc.pe));
	}

	if (cfg.params) {
		for (let k, spec in PARAM_DEFAULTS) {
			if (!(k in cfg.params))
				continue;
			if (!cur.get('rtl9617c_pe', spec.sec))
				cur.set('rtl9617c_pe', spec.sec, 'feature');
			cur.set('rtl9617c_pe', spec.sec, spec.opt,
				sprintf('%d', clamp_int(cfg.params[k], spec)));
		}
	}
	cur.commit('rtl9617c_pe');
}

function parse_stats(core_id) {
	let raw = read_str(sprintf('%s/pe%d/stats', SYS_ROOT, core_id));
	let out = {
		core: core_id,
		state: 'unavailable',
		mode: 'off',
		feature_mask: 0,
		pe_clk_hz: 0,
		peaxi_clk_hz: 0,
		clk_gated: 1,
		util_permille: 0,
		heartbeat: 0,
		uptime_ms: 0,
		cp0_prid: '0x00000000',
		window_cycles: 0,
		busy_cycles: 0,
		exc_cause: '0x00000000',
		exc_epc: '0x00000000',
		test_crc: '0x00000000',
		test_cycles: 0,
		dma_vp_id: 255,
		dma_vp_ready: 0,
		dma_tx_head: 0,
		dma_tx_tail: 0,
		dp_enq: 0,
		dp_bypass: 0,
		fq_rate_kbps: 0,
		fq_target_us: 5000,
		fq_interval_us: 100000,
		fq_quantum: 1514,
		fq_limit: 224,
		fq_ecn: 1,
		mcast_rate_kbps: 0,
		feat_stats: {}
	};
	if (!raw)
		return out;

	for (let line in split(raw, '\n')) {
		let m = match(line, /^([a-z0-9_]+)=(.*)$/);
		if (!m)
			continue;
		let k = m[1], v = m[2];
		if (k == 'state' || k == 'mode' || k == 'cp0_prid' ||
		    k == 'exc_cause' || k == 'exc_epc' || k == 'test_crc') {
			out[k] = v;
		} else if (k == 'feature_mask') {
			out.feature_mask = hex(v);
		} else if (match(k, /^feat([0-9]+)$/)) {
			let fm = match(k, /^feat([0-9]+)$/);
			let p = split(v, ' ');
			if (length(p) >= 6) {
				let fl = hex(p[5]);
				out.feat_stats[int(fm[1])] = {
					util_permille: int(p[0]),
					cycles: int(p[1]),
					packets: int(p[2]),
					bytes: int(p[3]),
					qdepth: int(p[4]),
					flags: fl,
					hw_bound: (fl & 2) != 0,
					drops: length(p) >= 7 ? int(p[6]) : 0,
					ecn_marks: length(p) >= 8 ? int(p[7]) : 0
				};
			}
		} else if (k in out) {
			out[k] = int(v);
		}
	}
	return out;
}

export function apply() {
	if (!is_dir(SYS_ROOT))
		return { ok: false, error: 'kernel driver rtl9617c-pe not loaded' };

	let cfg = load_cfg();
	let wifi = detect_wireless();

	if (cfg.mode == 'wifi_wfo' && !wifi.wfo_available) {
		cfg.mode = 'offload';
		save_cfg(cfg);
	}

	if (cfg.mode == 'off') {
		for (let c = 0; c < 2; c++) {
			write_str(sprintf('%s/pe%d/mode', SYS_ROOT, c), 'off');
			write_str(sprintf('%s/pe%d/feature_mask', SYS_ROOT, c), '0x0');
			write_str(sprintf('%s/pe%d/enable', SYS_ROOT, c), '0');
		}
		return { ok: true, mode: 'off' };
	}

	if (cfg.mode == 'wifi_wfo') {
		for (let c = 0; c < 2; c++) {
			write_str(sprintf('%s/pe%d/enable', SYS_ROOT, c), '0');
			write_str(sprintf('%s/pe%d/firmware', SYS_ROOT, c), FW_WFO);
			write_str(sprintf('%s/pe%d/mode', SYS_ROOT, c), 'wifi_wfo');
			write_str(sprintf('%s/pe%d/feature_mask', SYS_ROOT, c), '0x0');
			if (!write_str(sprintf('%s/pe%d/enable', SYS_ROOT, c), '1'))
				return { ok: false, error: sprintf('failed to start pe%d in wifi_wfo mode', c) };
		}
		return { ok: true, mode: 'wifi_wfo' };
	}

	/* General non-exclusive wired offload mode */
	let masks = [ 0, 0 ];
	for (let f in FEATURES) {
		let fc = cfg.features[f.id];
		if (fc && fc.enabled)
			masks[fc.pe] |= (1 << f.bit);
	}

	for (let c = 0; c < 2; c++) {
		let m = masks[c];
		if (m == 0) {
			/* Auto power-down idle PE clock to save energy */
			write_str(sprintf('%s/pe%d/feature_mask', SYS_ROOT, c), '0x0');
			write_str(sprintf('%s/pe%d/mode', SYS_ROOT, c), 'off');
			write_str(sprintf('%s/pe%d/enable', SYS_ROOT, c), '0');
		} else {
			let cur_state = read_str(sprintf('%s/pe%d/state', SYS_ROOT, c));
			let cur_fw = read_str(sprintf('%s/pe%d/firmware', SYS_ROOT, c));
			if (cur_state == 'running' && cur_fw != FW_OFFLOAD)
				write_str(sprintf('%s/pe%d/enable', SYS_ROOT, c), '0');

			write_str(sprintf('%s/pe%d/firmware', SYS_ROOT, c), FW_OFFLOAD);
			write_str(sprintf('%s/pe%d/mode', SYS_ROOT, c), 'offload');
			for (let pk, _spec in PARAM_DEFAULTS)
				write_str(sprintf('%s/pe%d/%s', SYS_ROOT, c, pk), sprintf('%d', cfg.params[pk]));
			write_str(sprintf('%s/pe%d/feature_mask', SYS_ROOT, c), sprintf('0x%x', m));
			if (cur_state != 'running' || cur_fw != FW_OFFLOAD) {
				if (!write_str(sprintf('%s/pe%d/enable', SYS_ROOT, c), '1'))
					return { ok: false, error: sprintf('failed to start pe%d', c) };
			}
		}
	}

	return { ok: true, mode: 'offload', masks: masks, params: cfg.params };
}

export function status() {
	let driver_loaded = is_dir(SYS_ROOT);
	let cfg = load_cfg();
	let wifi = detect_wireless();
	let pe_clk_hz = driver_loaded ? int(read_str(SYS_ROOT + '/pe_clk_hz') || 0) : 0;
	let peaxi_clk_hz = driver_loaded ? int(read_str(SYS_ROOT + '/peaxi_clk_hz') || 0) : 0;

	let cores = [ parse_stats(0), parse_stats(1) ];
	let feat_list = [];

	for (let f in FEATURES) {
		let fc = cfg.features[f.id] || { enabled: false, pe: f.default_pe };
		let core = cores[fc.pe];
		let fs_hw = core.feat_stats[f.bit] || {
			util_permille: 0, cycles: 0, packets: 0, bytes: 0,
			qdepth: 0, flags: 0, hw_bound: false, drops: 0, ecn_marks: 0
		};
		let active = (cfg.mode == 'offload') && fc.enabled &&
			     (core.state == 'running') && ((core.feature_mask & (1 << f.bit)) != 0);

		push(feat_list, {
			id: f.id,
			bit: f.bit,
			name: f.name,
			desc: f.desc,
			note: f.note,
			module_title: f.module_title,
			module_url: f.module_url,
			hw_capable: !!f.hw_capable,
			hw_bound: active && !!fs_hw.hw_bound,
			enabled: fc.enabled,
			active: active,
			pe: fc.pe,
			util_permille: active ? fs_hw.util_permille : 0,
			packets: active ? fs_hw.packets : 0,
			bytes: active ? fs_hw.bytes : 0,
			qdepth: active ? fs_hw.qdepth : 0,
			drops: active ? fs_hw.drops : 0,
			ecn_marks: active ? fs_hw.ecn_marks : 0
		});
	}

	/* Strip raw feat_stats map from core objects to keep JSON tidy */
	for (let c in cores)
		delete c.feat_stats;

	return {
		driver_loaded: driver_loaded,
		arch: 'Taroko RX5281 (MIPS-I)',
		pe_clk_hz: pe_clk_hz,
		peaxi_clk_hz: peaxi_clk_hz,
		mode: cfg.mode,
		params: cfg.params,
		wireless: wifi,
		cores: cores,
		features: feat_list
	};
}

export function set_mode(mode) {
	if (mode != 'off' && mode != 'offload' && mode != 'wifi_wfo')
		return { ok: false, error: 'invalid mode' };

	let wifi = detect_wireless();
	if (mode == 'wifi_wfo' && !wifi.wfo_available)
		return { ok: false, error: wifi.present ? 'WFO firmware not installed' : 'no wireless hardware detected' };

	let cfg = load_cfg();
	cfg.mode = mode;
	if (mode == 'off' || mode == 'wifi_wfo') {
		for (let f in FEATURES)
			cfg.features[f.id].enabled = false;
	}
	save_cfg(cfg);
	return apply();
}

export function set_feature(id, enabled, pe) {
	let f = find_feature(id);
	if (!f)
		return { ok: false, error: 'unknown feature: ' + (id || '') };

	let cfg = load_cfg();
	let en = !!enabled;
	if (pe != null) {
		let p = int(pe);
		if (p != 0 && p != 1)
			return { ok: false, error: 'invalid PE core index' };
		cfg.features[f.id].pe = p;
	}
	cfg.features[f.id].enabled = en;
	if (en && cfg.mode != 'offload')
		cfg.mode = 'offload';

	save_cfg(cfg);
	return apply();
}

export function set_params(params) {
	let cfg = load_cfg();
	let changed = false;

	if (params) {
		for (let k, spec in PARAM_DEFAULTS) {
			if (k in params && params[k] != null) {
				cfg.params[k] = clamp_int(params[k], spec);
				changed = true;
			}
		}
	}
	if (!changed)
		return { ok: false, error: 'no valid parameters provided' };

	save_cfg(cfg);
	return apply();
}

export function disable_all() {
	let cfg = load_cfg();
	cfg.mode = 'off';
	for (let f in FEATURES)
		cfg.features[f.id].enabled = false;
	save_cfg(cfg);
	return apply();
}

export function run_selftest(core_id, iter) {
	let c = int(core_id);
	let n = int(iter || 5000);
	if (c != 0 && c != 1)
		return { ok: false, error: 'invalid core' };
	if (n < 100 || n > 30000)
		n = 5000;
	let path = sprintf('%s/pe%d/selftest', SYS_ROOT, c);
	if (!write_str(path, sprintf('%d', n)))
		return { ok: false, error: 'selftest failed (ensure PE core is running)' };
	let st = parse_stats(c);
	return {
		ok: true,
		core: c,
		iter: n,
		crc: st.test_crc,
		cycles_ns: st.test_cycles,
		util_permille: st.util_permille
	};
}

export function get_log(core_id) {
	let c = int(core_id);
	if (c != 0 && c != 1)
		return { ok: false, error: 'invalid core', log: '' };
	let s = fs.readfile(sprintf('%s/pe%d/log', SYS_ROOT, c));
	return { ok: true, core: c, log: s || '' };
}
