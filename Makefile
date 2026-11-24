include $(TOPDIR)/rules.mk

PKG_NAME:=rtl9617c-pe-offload
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/rtl9617c-pe-offload
  SECTION:=net
  CATEGORY:=Network
  TITLE:=RTL9617C Taroko MIPS-I PE bare-metal offload firmware and manager
  DEPENDS:=@TARGET_rtl9617c +ucode +ucode-mod-fs +ucode-mod-uci +ucode-mod-ubus +rpcd
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
	mkdir -p $(PKG_BUILD_DIR)/firmware
	$(CP) ./firmware/. $(PKG_BUILD_DIR)/firmware/
endef

define Build/Compile
	if command -v clang >/dev/null 2>&1 && (command -v ld.lld-18 >/dev/null 2>&1 || command -v ld.lld >/dev/null 2>&1); then \
		$(MAKE) -C $(PKG_BUILD_DIR)/firmware clean all; \
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

$(eval $(call BuildPackage,rtl9617c-pe-offload))
