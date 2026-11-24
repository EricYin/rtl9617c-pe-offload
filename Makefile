include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rtl9617c-pe-offload
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/rtl9617c-pe
  SUBMENU:=Other modules
  TITLE:=Realtek RTL9617C (Taurus) dual Taroko PE base driver
  DEPENDS:=@TARGET_rtl9617c
  FILES:=$(PKG_BUILD_DIR)/kmod/rtl9617c-pe.ko
  AUTOLOAD:=$(call AutoLoad,60,rtl9617c-pe)
endef

define KernelPackage/rtl9617c-pe/description
  Out-of-tree kernel driver for the dual Taroko RX5281 (MIPS-I) Packet Engines
  (PE0 / PE1) on Realtek RTL9617C (Taurus) SoCs. Works with stock RTL9617C DTBs
  without requiring any main-tree kernel patches or DTS changes.
endef

define Package/rtl9617c-pe-offload
  SECTION:=net
  CATEGORY:=Network
  TITLE:=RTL9617C Taroko MIPS-I PE bare-metal offload firmware and manager
  DEPENDS:=@TARGET_rtl9617c +kmod-rtl9617c-pe +ucode +ucode-mod-fs +ucode-mod-uci +ucode-mod-ubus +rpcd
endef

define Package/rtl9617c-pe-offload/description
  Standalone bare-metal Taroko MIPS-I firmware, UCI orchestrator, and rpcd/ubus
  telemetry service for Realtek RTL9617C (Taurus) PE0/PE1 coprocessors.
  Supports non-exclusive feature bitmask offloading, live utilization telemetry,
  automatic clock power-gating when idle, and wireless WFO mode handoff.
endef

define Package/rtl9617c-pe-offload/conffiles
/etc/config/rtl9617c_pe
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/kmod $(PKG_BUILD_DIR)/firmware
	$(CP) ./kmod/. $(PKG_BUILD_DIR)/kmod/
	$(CP) ./firmware/. $(PKG_BUILD_DIR)/firmware/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)/kmod" modules
	if command -v clang >/dev/null 2>&1 \
	   && (command -v ld.lld-18 >/dev/null 2>&1 || command -v ld.lld >/dev/null 2>&1) \
	   && (command -v llvm-objcopy-18 >/dev/null 2>&1 || command -v llvm-objcopy >/dev/null 2>&1) \
	   && (command -v llvm-objdump-18 >/dev/null 2>&1 || command -v llvm-objdump >/dev/null 2>&1); then \
		env -u CFLAGS -u ASFLAGS -u LDFLAGS -u MAKEFLAGS $(MAKE) -C $(PKG_BUILD_DIR)/firmware clean && \
		env -u CFLAGS -u ASFLAGS -u LDFLAGS -u MAKEFLAGS $(MAKE) -C $(PKG_BUILD_DIR)/firmware all; \
	else \
		test -f $(PKG_BUILD_DIR)/firmware/pe-offload.bin; \
	fi
endef

define Package/rtl9617c-pe-offload/install
	$(INSTALL_DIR) $(1)/lib/firmware/rtl9617c
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/firmware/pe-offload.bin $(1)/lib/firmware/rtl9617c/pe-offload.bin

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/rtl9617c_pe $(1)/etc/config/rtl9617c_pe

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/rtl9617c-pe $(1)/etc/init.d/rtl9617c-pe

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/etc/uci-defaults/96-rtl9617c-pe-perms $(1)/etc/uci-defaults/96-rtl9617c-pe-perms

	$(INSTALL_DIR) $(1)/usr/share/ucode/rtl9617c
	$(INSTALL_DATA) ./files/usr/share/ucode/rtl9617c/pe.uc $(1)/usr/share/ucode/rtl9617c/pe.uc

	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/usr/sbin/rtl9617c-pe-ctl $(1)/usr/sbin/rtl9617c-pe-ctl

	$(INSTALL_DIR) $(1)/usr/share/rpcd/ucode
	$(INSTALL_BIN) ./files/usr/share/rpcd/ucode/luci.rtl9617c-pe $(1)/usr/share/rpcd/ucode/luci.rtl9617c-pe

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/usr/share/rpcd/acl.d/rtl9617c-pe-offload.json $(1)/usr/share/rpcd/acl.d/rtl9617c-pe-offload.json
endef

$(eval $(call KernelPackage,rtl9617c-pe))
$(eval $(call BuildPackage,rtl9617c-pe-offload))
