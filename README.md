# rtl9617c-pe-offload

Standalone bare-metal Taroko RX5281 (MIPS-I) firmware, UCI orchestrator, and `rpcd`/`ubus` backend for the dual Packet Engines (PE0 / PE1) on Realtek RTL9617C (Taurus) OpenWrt / ImmortalWrt platforms.

## Highlights

- **Zero-wait bare-metal MIPS-I firmware** (`pe-offload.bin`, ~3.8 KiB, runs inside the 32 KiB on-chip IMEM with private 8 KiB DMEM stack).
- **Non-exclusive feature bitmask** (`FQ-CoDel`, `IPsec ESP`, `SRv6`, `MCAST`): multiple wired offload roles coexist inside one unified firmware and hot-switch without reboot.
- **1 ns hardware telemetry**: real-time PE / PE-AXI frequencies (`800 MHz / 400 MHz`) and per-core / per-feature utilization measured via the SoC's 1 ns PTP hardware clock.
- **Auto power-gating**: idle PE cores automatically assert reset and gate `PD_OFFLOAD0/1` clocks to save power.
- **Wireless WFO ready**: detects `/sys/class/ieee80211/phy*` and `/lib/firmware/rtl9617c/pe-wfo.bin` for seamless switching on wireless RTL9617C siblings.

## Companion UI

Install [luci-app-rtl9617c-pe](https://github.com/crysister/luci-app-rtl9617c-pe) for the PE dashboard and one-click controls, and [luci-app-rtl9617c-qos](https://github.com/crysister/luci-app-rtl9617c-qos) for integrated toggles in the QoS / VPN / Offload pages.
