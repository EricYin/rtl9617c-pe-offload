# rtl9617c-pe-offload

Fully decoupled out-of-tree kernel module (`kmod-rtl9617c-pe`), bare-metal Taroko RX5281 (MIPS-I) firmware (`pe-offload.bin`), UCI orchestrator, and `rpcd`/`ubus` backend for the dual Packet Engines (PE0 / PE1) on Realtek RTL9617C (Taurus) OpenWrt / ImmortalWrt platforms.

> Requires **zero** patches or DTS changes in the main OpenWrt / ImmortalWrt kernel tree.

## Packages Built

- **`kmod-rtl9617c-pe`**: Out-of-tree Linux kernel driver (`rtl9617c-pe.ko`, autoloaded at priority 60) handling ATU translation, SRAM/reset sequencing, `PD_OFFLOAD0/1` clock power-gating, watchdog fault recovery, warm-reboot quiescing, and `/sys/kernel/rtl9617c_pe/` sysfs export.
- **`rtl9617c-pe-offload`**: Bare-metal MIPS-I firmware (`3.8 KiB` zero-wait IMEM), `/usr/sbin/rtl9617c-pe-ctl`, `/usr/share/ucode/rtl9617c/pe.uc`, and `ubus` service `luci.rtl9617c-pe`.

## Companion UI

Install [luci-app-rtl9617c-pe](https://github.com/crysister/luci-app-rtl9617c-pe) for the PE dashboard and one-click controls, and [luci-app-rtl9617c-qos](https://github.com/crysister/luci-app-rtl9617c-qos) for soft-decoupled toggles inside the QoS / VPN / Offload pages.
