# SPDX-License-Identifier: GPL-2.0-only
obj-m += rtl9617c-pe.o
RTL_NI_HDR := $(srctree)/drivers/net/ethernet/realtek/rtl9617c/rtl9617c_ni.h
ifneq ($(strip $(shell grep -s -l rtl9617c_ni_register_tx_hook $(RTL_NI_HDR))),)
ccflags-y += -DRTL9617C_PE_HAS_NI_TX_HOOK=1
endif
ccflags-y += -I$(src)/include -I$(srctree)/drivers/net/ethernet/realtek/rtl9617c -Werror
