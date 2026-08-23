# System detection
UNAME_M := $(shell uname -m)
ARCH ?= arm64
PWD := $(shell pwd)

# Architecture-specific settings
ifeq ($(UNAME_M),x86_64)
    # Cross-compilation for ARM64 from x86
    KERNEL_DIR ?= linux_headers/usr/src/linux-headers-5.15.0-1046-xilinx-zynqmp
    CROSS_COMPILE ?= aarch64-linux-gnu-
else ifeq ($(UNAME_M),aarch64)
    # Native compilation on ARM64
    KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
    CROSS_COMPILE ?= 
else
    $(error Unsupported architecture: $(UNAME_M))
endif

# Kernel modules
obj-m := xil-isp-lite.o
obj-m += xil-vip.o
obj-m += mipi-rx-to-video.o
obj-m += ar1335.o
obj-m += xlnx_imx219.o

# Device Tree files
DTS_FILE := pl.dtsi
DTBO_FILE := $(DTS_FILE:.dtsi=.dtbo)
DTC_FLAGS ?= -@ -O dtb

BIT_STREAM_FILE := design_1_wrapper_imx219.bin

# Default target
all: modules dtbo

# Build kernel modules
modules:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
	CONFIG_MODULE_SIG=y EXTRA_CFLAGS="-I$(PWD)" modules

# Compile device tree
dtbo: $(DTBO_FILE)

$(DTBO_FILE): $(DTS_FILE)
	dtc $(DTC_FLAGS) -o $@ $<

# Clean everything
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f *.dtbo

# Installation targets (ARM64 only)
# Keep deployment ordered even when the caller uses "make -j".  Loading the
# modules before applying the overlay is intentional: the drivers bind as the
# overlay creates their devices.
install: check_root check_arch
	$(MAKE) install_bitstream
	$(MAKE) install_modules
	$(MAKE) install_dtbo

install_modules: modules
	$(MAKE) load_modules

load_modules: check_root
	insmod xil-isp-lite.ko
	insmod xil-vip.ko
	insmod mipi-rx-to-video.ko
	insmod ar1335.ko
	insmod xlnx_imx219.ko

# The Xilinx 5.15 composite driver has no async-notifier unbind callback.  If a
# subdevice module is removed while the composite remains bound, its stale
# entity pointer makes the next probe fail with "duplicate subdev".  Tear down
# both composite graphs before removing any of our modules.
unbind_composites: check_root check_devices_idle
	@set -eu; \
	unbind_composite() { \
		driver_path="$$1"; \
		device_name="$$2"; \
		[ -d "$$driver_path" ] || return 0; \
		[ -L "$$driver_path/$$device_name" ] || return 0; \
		echo "Unbinding composite $$device_name"; \
		printf '%s\n' "$$device_name" > "$$driver_path/unbind"; \
	}; \
	unbind_composite /sys/bus/platform/drivers/xilinx-video \
		axi:vcap_mipi_csi2_rx_rpi; \
	unbind_composite /sys/bus/platform/drivers/xilinx-video \
		axi:isp_pipe_video_dev

# Bind ISP first to preserve the established numbering: ISP is /dev/media0
# and the IMX219 capture graph is /dev/media1.
bind_composites: check_root
	@set -eu; \
	bind_composite() { \
		driver_path="$$1"; \
		device_path="$$2"; \
		device_name="$$3"; \
		[ -d "$$driver_path" ] || return 0; \
		[ -e "$$device_path" ] || return 0; \
		[ ! -L "$$driver_path/$$device_name" ] || return 0; \
		echo "Binding composite $$device_name"; \
		printf '%s\n' "$$device_name" > "$$driver_path/bind"; \
	}; \
	bind_composite /sys/bus/platform/drivers/xilinx-video \
		/sys/bus/platform/devices/axi:isp_pipe_video_dev \
		axi:isp_pipe_video_dev; \
	bind_composite /sys/bus/platform/drivers/xilinx-video \
		/sys/bus/platform/devices/axi:vcap_mipi_csi2_rx_rpi \
		axi:vcap_mipi_csi2_rx_rpi

install_dtbo: dtbo
	-rmdir /sys/kernel/config/device-tree/overlays/*
	mkdir /sys/kernel/config/device-tree/overlays/Infinite_ISP
	echo 0 > /sys/class/fpga_manager/fpga0/flags
	cat $(DTBO_FILE) > /sys/kernel/config/device-tree/overlays/Infinite_ISP/dtbo

install_bitstream: $(BIT_STREAM_FILE)
	install -C -m 644 $(BIT_STREAM_FILE) /lib/firmware/

check_devices_idle:
	@if command -v fuser >/dev/null 2>&1 && \
	   fuser /dev/media* /dev/video* /dev/v4l-subdev* >/dev/null 2>&1; then \
		echo "Refusing module reload: a media/video device is still open" >&2; \
		fuser -v /dev/media* /dev/video* /dev/v4l-subdev* >&2 || true; \
		exit 1; \
	fi

unload_modules: check_root unbind_composites
	@set -eu; \
	for module_name in xlnx_imx219 ar1335 mipi_rx_to_video xil_vip xil_isp_lite; do \
		grep -q "^$$module_name " /proc/modules || continue; \
		echo "Removing $$module_name"; \
		rmmod "$$module_name"; \
	done

# Reload only the software drivers.  The FPGA image and DT overlay stay in
# place, which makes iterative driver debugging possible without a reboot.
reload: check_root check_arch modules
	$(MAKE) unload_modules
	$(MAKE) load_modules
	$(MAKE) bind_composites
	@command -v udevadm >/dev/null 2>&1 && udevadm settle --timeout=10 || true
	@echo "Modules reloaded; reapply the media-ctl formats before testing"

uninstall: check_root check_arch
	$(MAKE) unload_modules
	-rmdir /sys/kernel/config/device-tree/overlays/Infinite_ISP
	rm -f /lib/firmware/$(BIT_STREAM_FILE)

# Check for root privileges
check_root:
ifeq ($(shell id -u),0)
    # We're root, continue
else
	$(error This target must be run as root. Use 'sudo make install' or 'sudo make uninstall')
endif

# Check architecture for install/uninstall
check_arch:
ifneq ($(UNAME_M),aarch64)
	$(error This target can only be run on aarch64 architecture)
endif

.PHONY: all modules dtbo clean install install_modules load_modules \
	install_dtbo install_bitstream reload unload_modules unbind_composites \
	bind_composites \
	check_devices_idle uninstall check_arch check_root
