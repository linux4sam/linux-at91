# SPDX-License-Identifier: GPL-2.0
#
# Makefile for Synopsys DWC Platform drivers
#
dw-csi-objs := dw-csi-plat.o dw-mipi-csi.o
ifeq ($(CONFIG_DWC_MIPI_TC_DPHY_GEN3),y)
	dw-csi-objs += dw-csi-sysfs.o
endif
obj-$(CONFIG_DWC_MIPI_CSI2_HOST) += dw-csi.o
