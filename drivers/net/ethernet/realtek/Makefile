# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Realtek network device drivers.
#

obj-$(CONFIG_8139CP) += 8139cp.o
obj-$(CONFIG_8139TOO) += 8139too.o
obj-$(CONFIG_ATP) += atp.o
obj-${CONFIG_R8125} += r8125_vendor/
obj-${CONFIG_R8126} += r8126_vendor/
obj-${CONFIG_R8168} += r8168_vendor/
r8169-y += r8169_main.o r8169_firmware.o r8169_phy_config.o
r8169-$(CONFIG_R8169_LEDS) += r8169_leds.o
obj-$(CONFIG_R8169) += r8169.o
obj-$(CONFIG_RTASE) += rtase/
