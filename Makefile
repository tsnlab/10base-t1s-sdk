# ========================
#  Build Configuration
# ========================
DRIVER_NAME := lan865x
BUILD_DIR   := $(CURDIR)/build
SRC_DIR     := src/kernel/lan865x
INCLUDE_DIR := include
BOARD       ?= rpi4  # Supported: rpi4, rpi5 (both arm64) or x64 (x86_64)
MACADDR 	?= d0:d1:95:30:23:00 # Default MAC address

# Architecture-specific settings
ifeq ($(BOARD),rpi5)
    ARCH := arm64
    CROSS_COMPILE := aarch64-linux-gnu-
	KERNEL_DIR := $(RPI5_KERNEL_DIR)
    TOOLCHAIN := $(shell which $(CROSS_COMPILE)gcc)
    EXTRA_CFLAGS += -DCONFIG_RPI5=1
else
    # Default: RPi4
    ARCH := arm64
    CROSS_COMPILE := aarch64-linux-gnu-
	KERNEL_DIR := $(RPI4_KERNEL_DIR)
    TOOLCHAIN := $(shell which $(CROSS_COMPILE)gcc)
endif

# ========================
#  MAC Address Handling
# ========================
# Usage examples:
#   make BOARD=rpi4 MACADDR="de:ad:be:ef:12:34"     # Fixed MAC, Enable DiP switch
#   make BOARD=rpi4 MACADDR=none                    # Random MAC + Enable DIP switch
#   make BOARD=rpi4                                 # Default MAC + Enable DIP switch
#   make BOARD=rpi4 DISABLE_DIPSWITCH=1             # Default MAC + Disable DIP switch -> Default MAC

ifeq ($(MACADDR),none)
    EXTRA_CFLAGS += -DUSE_RANDOM_MAC
else ifneq ($(MACADDR),)
    EXTRA_CFLAGS += -DMACADDR='\"$(strip $(MACADDR))\"'
endif

# Enable MAC ADDRESS DIP switch (Default)
EXTRA_CFLAGS += -DENABLE_DIPSWITCH

ifeq ($(DISABLE_DIPSWITCH),1)
    EXTRA_CFLAGS := $(filter-out -DENABLE_DIPSWITCH,$(EXTRA_CFLAGS))
endif

# Test configuration (always built for host)
TEST_DIR    := $(CURDIR)/tests/mock/lan865x
TEST_TARGET := $(BUILD_DIR)/test_runner
MOCK_INCLUDES := -I$(CURDIR)/$(INCLUDE_DIR) \
                -I$(CURDIR)/$(INCLUDE_DIR)/linux \
                -I$(TEST_DIR)

# Common includes for driver
DRIVER_INCLUDES := -I$(CURDIR)/$(INCLUDE_DIR) \
                  -I$(CURDIR)/$(INCLUDE_DIR)/linux \
                  -I$(CURDIR)/$(INCLUDE_DIR)/10baset1s \
                  -I$(KERNEL_DIR)/include \
                  -I$(KERNEL_DIR)/include/uapi \
                  -I$(KERNEL_DIR)/include/linux \
                  -I$(KERNEL_DIR)/drivers/net/phy \
                  -I$(KERNEL_DIR)/drivers/spi

# RPi5-specific optimizations
ifeq ($(BOARD),rpi5)
    DRIVER_INCLUDES += -I$(KERNEL_DIR)/arch/arm64/include
    EXTRA_CFLAGS += -march=armv8.2-a+crc+simd -mtune=cortex-a76
endif

# ========================
#  Build Targets
# ========================
all: print_config $(BUILD_DIR)/Makefile
	@echo "[$(DRIVER_NAME)] Building driver for $(BOARD)..."
	$(MAKE) -C $(KERNEL_DIR) M=$(BUILD_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) EXTRA_CFLAGS="$(DRIVER_INCLUDES) $(EXTRA_CFLAGS)" modules
	@cp $(BUILD_DIR)/$(DRIVER_NAME).ko ./

print_config:
	@echo "========================================"
	@echo "  Build Configuration Summary"
	@echo "========================================"
	@echo "- Target Board   : $(BOARD)"
	@echo "- Architecture   : $(ARCH)"
	@echo "- Cross Compile  : $(if $(CROSS_COMPILE),$(CROSS_COMPILE),None (native build))"
	@echo "- Kernel Version : $(shell cat $(KERNEL_DIR)/include/config/kernel.release 2>/dev/null || echo 'N/A')"
	@echo "- Kernel Source  : $(KERNEL_DIR)"
	@echo "- Toolchain      : $(TOOLCHAIN)"
	@echo "- Extra CFLAGS   : $(EXTRA_CFLAGS)"
ifeq ($(MACADDR),none)
	@echo "- MAC Address    : random (runtime generated)"
else
	@echo "- MAC Address    : $(MACADDR)"
endif
	@echo "========================================"
	@test -d $(KERNEL_DIR) || { \
		echo "[ERROR] Kernel directory not found!"; \
		exit 1; \
	}

$(BUILD_DIR)/Makefile:
	@mkdir -p $(BUILD_DIR)
	@echo "obj-m := $(DRIVER_NAME).o" > $@
	@echo "$(DRIVER_NAME)-y :=\
    ../$(SRC_DIR)/lan865x.o\
		../$(SRC_DIR)/lan865x_sysfs.o\
		../$(SRC_DIR)/lan865x_interrupt.o\
		../$(SRC_DIR)/t1s_hat_fxl6408.o\
		../src/kernel/oa_tc6.o\
" >> $@
	@echo "ccflags-y := $(DRIVER_INCLUDES) $(EXTRA_CFLAGS)" >> $@

# ========================
#  Clean Target
# ========================
clean:
	@echo "[$(DRIVER_NAME)] Cleaning build artifacts..."
	rm -rf $(BUILD_DIR) $(DRIVER_NAME).ko \
	       *.o *.o.cmd *.o.d *.mod.c *.symvers *.order .tmp_versions \
		   src/kernel/.oa_tc6.o.cmd	\
		   src/kernel/lan865x/.lan865x.o.cmd \
		   src/kernel/.oa_tc6.o.d \
	       src/kernel/*.o src/kernel/*.o.cmd src/kernel/*.o.d \
	       src/kernel/.tmp_versions \
	       src/kernel/lan865x/*.o src/kernel/lan865x/*.o.cmd src/kernel/lan865x/*.o.d \
	       src/kernel/lan865x/.tmp_versions

.PHONY: all clean print_config
