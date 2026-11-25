#
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# MM6108-EKH05 S1G relay repeater firmware for the A -> relay -> B HaLow lab.
#
# Out-of-tree mm-iot-sdk 2.14.3 application built with the SDK make flow and its bundled
# STM32U5 BSP (no STM32CubeIDE / CubeMX). Modelled on
# third_party/mm-iot-sdk/examples/relay_mode/targets/mm-mm6108-ekh05/Makefile.
#
# Targets:
#   all (default)  build/relay_ekh05.elf
#   bin            build/relay_ekh05.bin   (raw image from 0x08000000, gaps filled with 0xFF)
#   hex            build/relay_ekh05.hex   (Intel HEX, loaded sections only)
#   mbin           build/relay_ekh05.mbin
#   images         elf + bin + hex + size report
#   configbin      build/configstore.bin from config.hjson (no device access needed)
#   bin-with-config build/relay_ekh05_with_config.bin = bin with configstore.bin at 0x08004000
#   size           arm-none-eabi-size and the memory regions
#   flash          program the existing ELF with OpenOCD (needs USB access to the ST-LINK);
#                  never rebuilds, so it cannot pick up an SDK tree whose patches were reverted
#   config         write config.hjson to the device config store via OpenOCD
#   clean
#
# Usually run through ./build.sh (Docker image ekh05-builder:2.14.3). Never build with a
# BUILD_DIR inside the SDK submodule.
#
# Linking checks that the patches in ../patches/mm-iot-sdk are applied to the SDK tree, because
# morselib is compiled from it and the firmware needs patch 0001 to survive a disassociation.
# ALLOW_UNPATCHED=1 skips the check (build.sh --no-patches sets it).

APP_NAME = relay_ekh05

PLATFORM = mm-mm6108-ekh05

APP_DIR = .

# Path to the SDK framework directory (submodule third_party/mm-iot-sdk).
MMIOT_ROOT ?= ../third_party/mm-iot-sdk/framework

-include $(MMIOT_ROOT)/mk/misc.mk

# Build type: production (default, -Os) or debug.
BUILDTYPE ?= production

ifeq ($(BUILDTYPE),production)
BUILD_DIR ?= build
else
BUILD_DIR ?= build_$(BUILDTYPE)
endif

ELF_FILE = $(BUILD_DIR)/$(APP_NAME).elf
BIN_FILE = $(BUILD_DIR)/$(APP_NAME).bin
HEX_FILE = $(BUILD_DIR)/$(APP_NAME).hex
MBIN_FILE = $(BUILD_DIR)/$(APP_NAME).mbin
CONFIGSTORE_BIN = $(BUILD_DIR)/configstore.bin
BIN_WITH_CONFIG = $(BUILD_DIR)/$(APP_NAME)_with_config.bin

ifeq ($(VERBOSE),)
QUIET = @
endif

BUILD_MORSELIB_FROM_SOURCE ?= y
BUILD_SUPPLICANT_CRYPTO_FROM_SOURCE ?= y
BUILD_WITH_MBEDTLS_ECC_HW_CRYPTO ?= y

# The repeater runs an AP interface, so the supplicant needs AP (SAE authenticator) support.
ENABLE_AP_MODE ?= y

# Downstream stations on the relay AP. The lab needs one (node A); keep a little headroom.
MAX_STAS ?= 4

IP_STACK ?= lwip
MMIPAL_IPV4_ENABLED ?= 1
MMIPAL_IPV6_ENABLED ?= 0

# Board Configuration File linked into the application, so a fresh board works without
# programming the config store first (and a drag-and-drop .bin is self-contained). The EKH05
# module is an MM6108-MF08651. Set BCF_MBIN_REL= (empty) to read "bcf_file" from the config
# store instead, as the SDK examples do.
BCF_MBIN_REL ?= mm6108/bcfs/bcf_mf08651_us.mbin

# Extra FreeRTOS heap for this application (UDP echo service context, event queue, tasks).
APP_HEAP ?= 32768

ifneq ($(ENABLE_MBEDTLS_P384),)
BUILD_DEFINES += MBEDTLS_ECP_DP_SECP384R1_ENABLED
endif
ifneq ($(ENABLE_MBEDTLS_P521),)
BUILD_DEFINES += MBEDTLS_ECP_DP_SECP521R1_ENABLED
endif

SRCS_C = $(wildcard $(APP_DIR)/src/*.c)
SRCS_CPP =
SRCS_H = $(wildcard $(APP_DIR)/src/*.h)

INCLUDES += $(APP_DIR)/src

BUILD_DEFINES += MAX_STAS=$(MAX_STAS)
BUILD_DEFINES += ENABLE_MMWLAN_RELAY=1
BUILD_DEFINES += APP_HEAP=$(APP_HEAP)
# lwIP counters for the ip_tx_pkts/ip_rx_pkts fields of the health line
# (mmipal_get_link_packet_counts() returns 0 without them; the SDK default is off).
BUILD_DEFINES += LWIP_STATS=1

# Local, git-ignored lab settings (../lab_local.conf, see ../lab_local.conf.example): compiled in
# through a header generated from it, unless LAB_CONFIG_OVERRIDE_HEADER is given.
LAB_LOCAL_CONF ?= $(abspath $(APP_DIR)/../lab_local.conf)
LAB_LOCAL_HEADER :=
ifeq ($(LAB_CONFIG_OVERRIDE_HEADER),)
ifneq ($(wildcard $(LAB_LOCAL_CONF)),)
LAB_LOCAL_HEADER := $(abspath $(BUILD_DIR))/lab_local_config.h
LAB_CONFIG_OVERRIDE_HEADER := $(LAB_LOCAL_HEADER)
endif
endif

# Optional build-time override of the lab defaults, e.g. LAB_CONFIG_OVERRIDE_HEADER=my_lab.h
# (searched on the include path; put it in src/ or pass an absolute path).
ifneq ($(LAB_CONFIG_OVERRIDE_HEADER),)
CFLAGS += -DLAB_CONFIG_OVERRIDE_HEADER='"$(LAB_CONFIG_OVERRIDE_HEADER)"'
endif

CFLAGS += -Werror -Wall -Wextra
CONLYFLAGS += -Wc++-compat
CONLYFLAGS += -std=gnu11

ifeq ($(EXCLUDE_MMCONFIG),1)
BUILD_DEFINES += EXCLUDE_MMCONFIG=$(EXCLUDE_MMCONFIG)
endif

ifneq ($(ENABLE_AP_MODE),)
BUILD_SUPPLICANT_WITH_AP=y

ifneq ($(shell echo $(MAX_STAS) | grep "[^0-9]"),)
$(error MAX_STAS='$(MAX_STAS)' is not an integer)
endif

# Same sizing rules as the SDK relay_mode example.
BUILD_DEFINES += AP_MODE_HEAP=$(shell echo $$(($(MAX_STAS) * 5000)))
DEBUG_BUILD_DEFINES += MAX_ALLOCATIONS=$(shell echo $$((250 + ($(MAX_STAS) * 40))))
MMPKTMEM_TX_POOL_N_BLOCKS = $(shell echo $$((32 + ($(MAX_STAS) * 2))))
MMPKTMEM_RX_POOL_N_BLOCKS = $(shell echo $$((32 + ($(MAX_STAS) * 2))))
BUILD_DEFINES += MMPKTMEM_TX_MGMT_POOL_N_BLOCKS=4
endif

ifneq ($(BUILD_SUPPLICANT_WITH_AP)$(BUILD_SUPPLICANT_WITH_DPP)$(BUILD_MORSELIB_FROM_SOURCE),)
BUILD_SUPPLICANT_FROM_SOURCE=y
endif

all:

include $(MMIOT_ROOT)/mk/utils.mk

include $(MMIOT_ROOT)/mk/platform-$(PLATFORM).mk

include $(MMIOT_ROOT)/mk/core-$(CORE).mk

include $(MMIOT_ROOT)/mk/mmconfig.mk
include $(MMIOT_ROOT)/mk/slip.mk
include $(MMIOT_ROOT)/mk/mmutils.mk
include $(MMIOT_ROOT)/mk/littlefs.mk
include $(MMIOT_ROOT)/mk/freertos-common.mk
include $(MMIOT_ROOT)/mk/freertos-$(CORE).mk
include $(MMIOT_ROOT)/mk/morselib.mk
include $(MMIOT_ROOT)/mk/morsefirmware.mk
include $(MMIOT_ROOT)/mk/wpa_supplicant.mk
include $(MMIOT_ROOT)/mk/mbedtls.mk
include $(MMIOT_ROOT)/mk/mmipal.mk
include $(MMIOT_ROOT)/mk/mmpktmem.mk
include $(MMIOT_ROOT)/mk/$(IP_STACK).mk
include $(MMIOT_ROOT)/mk/mmregdb.mk

MMIOT_INCLUDES += src

SRCS_H += $(addprefix $(MMIOT_ROOT)/,$(MMIOT_SRCS_H))
$(eval $(call check_for_files,$(SRCS_H)))

OBJS += $(patsubst $(APP_DIR)/%.S,$(BUILD_DIR)/%.o,$(patsubst $(APP_DIR)/%.s,$(BUILD_DIR)/%.o,$(SRCS_S)))
OBJS += $(patsubst $(APP_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS_C))
OBJS += $(patsubst %.c,$(BUILD_DIR)/%.o,$(MMIOT_SRCS_C))
OBJS += $(patsubst %.S,$(BUILD_DIR)/%.o,$(patsubst %.s,$(BUILD_DIR)/%.o,$(MMIOT_SRCS_S)))

LIBS += $(addprefix $(MMIOT_ROOT)/,$(MMIOT_LIBS))

CFLAGS += $(addprefix -I,$(INCLUDES))
CFLAGS += $(addprefix -I$(MMIOT_ROOT)/,$(MMIOT_INCLUDES))
CFLAGS += $(addprefix -D,$(BUILD_DEFINES))
CFLAGS += $(CSPECS)

ifeq ($(DISABLE_FILEID_GENERATION),)
FILEHASHES_LIST=$(BUILD_DIR)/filehashes.txt
CFLAGS += -DMMOSAL_FILEID=$(call uint32_hash,$<)
endif

ifeq ($(BUILDTYPE),debug)
DEBUG_CFLAGS += -Og
DEBUG_CFLAGS += -g3
DEBUG_CFLAGS += -gdwarf-5
DEBUG_CFLAGS += -fvar-tracking-assignments
DEBUG_BUILD_DEFINES += MMOSAL_TRACK_ALLOCATIONS
DEBUG_BUILD_DEFINES += ENABLE_MMOSAL_DEV_ASSERT
DEBUG_BUILD_DEFINES += HALT_ON_ASSERT
DEBUG_BUILD_DEFINES += RESET_MM_ON_HALT
CFLAGS += $(DEBUG_CFLAGS)
LINKFLAGS += $(DEBUG_LINKFLAGS)
BUILD_DEFINES += $(DEBUG_BUILD_DEFINES)
else
CFLAGS += -Os
CFLAGS += -DMMLOG_PREFIX_FMT="\"%c %8lu \""
BUILD_DEFINES += "MMLOG_PREFIX_ARGS=lvl,mmosal_get_time_ms()"
endif
$(info Building $(APP_NAME) for $(BUILDTYPE) in $(BUILD_DIR))

# Lab common library (UDP echo service, wire format, statistics, platform layer). It appends its
# objects to OBJS, so it must come before the $(ELF_FILE) rule below.
LAB_COMMON_DIR := $(abspath $(APP_DIR)/../common)
include $(LAB_COMMON_DIR)/mk/common.mk

# The app and common objects include halow_lab_config.h, which includes the generated header:
# rebuild them when lab_local.conf changes. The tool rewrites the header only if it changed.
ifneq ($(LAB_LOCAL_HEADER),)
$(patsubst $(APP_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS_C)) $(LAB_COMMON_OBJS): $(LAB_LOCAL_HEADER)
$(LAB_LOCAL_HEADER): $(LAB_LOCAL_CONF) $(abspath $(APP_DIR)/../tools/lab_local_header.py)
	@mkdir -p $(dir $@)
	$(QUIET)$(PYTHON) $(abspath $(APP_DIR)/../tools/lab_local_header.py) $< $@
endif

# Sanity check (see the SDK example): every OBJS entry must be an object file.
NON_OBJ_OBJS := $(filter-out %.o,$(OBJS))
ifneq ($(NON_OBJ_OBJS),)
$(error OBJS contains non-.o entries (source path likely not under its expected root): $(NON_OBJ_OBJS))
endif

LINKER ?= $(CC)
SIZE := "$(TOOLCHAIN_BASE)size"

LINKFLAG_PREFIX=-Wl,-T
LD_FILES += $(addprefix $(MMIOT_ROOT)/,$(BSP_LD_FILES))
LINKFLAGS += $(addprefix $(LINKFLAG_PREFIX),$(LD_FILES))
LINKFLAGS += -Wl,-Map=$(BUILD_DIR)/$(APP_NAME).map

ELF_TO_MBIN = $(MMIOT_ROOT)/tools/buildsystem/convert-bin-to-mbin.py
PROGRAM_CONFIGSTORE = $(MMIOT_ROOT)/tools/platform/program-configstore.py
OPENOCD_CFG ?= $(MMIOT_ROOT)/src/platforms/$(PLATFORM)/openocd.cfg
CONFIGFILE ?= $(APP_DIR)/config.hjson
PYTHON ?= python3

SDK_PATCH_TOOL := $(abspath $(APP_DIR)/../tools/apply_sdk_patches.sh)
SDK_TREE := $(abspath $(MMIOT_ROOT)/..)
ALLOW_UNPATCHED ?=

# Fails unless every patch is applied. git runs as root in the container on a host-owned
# checkout, so mark it safe for the check (build.sh does the same).
ifeq ($(BUILD_MORSELIB_FROM_SOURCE)$(ALLOW_UNPATCHED),y)
define check_sdk_patches
	$(QUIET)state=$$(GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*' \
		$(SDK_PATCH_TOOL) --check $(SDK_TREE) 2>&1) || true; \
	if ! echo "$$state" | grep -q '^   applied ' || echo "$$state" | grep -qE 'not applied|CONFLICT'; then \
		echo "$$state" >&2; \
		echo "error: SDK patches are not all applied to $(SDK_TREE); build with relay_ekh05/build.sh" \
			"(applies them) or pass ALLOW_UNPATCHED=1 to link an unpatched firmware" >&2; \
		exit 1; \
	fi
endef
endif

# The SDK Python tools live in the submodule; do not leave __pycache__ directories there.
export PYTHONDONTWRITEBYTECODE = 1

# Config store partition 0 of the STM32U585 (framework/tools/platform/configstore.json).
CONFIGSTORE_OFFSET = 0x4000
CONFIGSTORE_PART_SIZE = 0x2000

.PHONY: all bin hex mbin images size clean flash config configbin bin-with-config
all: $(ELF_FILE)
bin: $(BIN_FILE)
hex: $(HEX_FILE)
mbin: $(MBIN_FILE)
images: $(ELF_FILE) $(BIN_FILE) $(HEX_FILE) size
configbin: $(CONFIGSTORE_BIN)
bin-with-config: $(BIN_WITH_CONFIG)

size: $(ELF_FILE)
	$(QUIET)$(SIZE) -A -x $< | grep -E '^(section|\.isr_vector|\.bootloader|\.text|\.rodata|\.data|\.bss|\._user_heap_stack|\.ARM)' || true
	$(QUIET)$(SIZE) -B $<

clean:
	-rm -r $(BUILD_DIR)

#
# Rules to compile/assemble sources in $(MMIOT_ROOT)
#
$(BUILD_DIR)/%.o: $(MMIOT_ROOT)/%.s $(SRCS_H)
	@echo "$(MSGPFX)Assembling $<"
	@mkdir -p $(dir $@)
	$(QUIET)$(AS) -o $@ -c $(CFLAGS) $<

$(BUILD_DIR)/%.o: $(MMIOT_ROOT)/%.S $(SRCS_H)
	@echo "$(MSGPFX)Assembling $<"
	@mkdir -p $(dir $@)
	$(QUIET)$(AS) -o $@ -c $(CFLAGS) $<

$(BUILD_DIR)/%.o: $(MMIOT_ROOT)/%.c $(SRCS_H)
	@echo "$(MSGPFX)Compiling $<"
	@mkdir -p $(dir $@)
	$(QUIET)$(CC) -o $@ -c $(CFLAGS) $(CONLYFLAGS) $(call file_cflags,$(patsubst $(MMIOT_ROOT)/%,%,$<)) $<

#
# Rules to compile sources in $(APP_DIR)
#
$(BUILD_DIR)/%.o: $(APP_DIR)/%.c $(SRCS_H) $(LAB_COMMON_SRCS_H)
	@echo "$(MSGPFX)Compiling $<"
	@mkdir -p $(dir $@)
	$(QUIET)$(CC) -o $@ -c $(CFLAGS) $(CONLYFLAGS) $(APP_CFLAGS) $<

#
# Link and convert
#
$(ELF_FILE): $(OBJS) $(LIBS) $(LD_FILES) $(FILEHASHES_LIST)
	$(check_sdk_patches)
	@echo "$(MSGPFX)Linking $@"
	$(QUIET)$(LINKER) -o $@ $(OBJS) $(CFLAGS) $(LINKFLAGS) $(LIBS)

# Raw image for ST-LINK drag-and-drop. It starts at 0x08000000 (the reset-vector stub in the
# BOOTLOADER region) and spans the 16 KiB config store gap, which is filled with 0xFF (erased
# flash) rather than zeros.
$(BIN_FILE): $(ELF_FILE)
	@echo "$(MSGPFX)Generating $@"
	$(QUIET)$(OBJCOPY) -Obinary --gap-fill 0xff $< $@

$(HEX_FILE): $(ELF_FILE)
	@echo "$(MSGPFX)Generating $@"
	$(QUIET)$(OBJCOPY) -Oihex $< $@

$(MBIN_FILE): $(ELF_FILE)
	@echo "Convert $< to $@"
	$(QUIET)$(ELF_TO_MBIN) -s -o $@ $<

# Config store partition image generated offline from config.hjson (same tool and format the
# SDK uses over OpenOCD, written to a file instead of the device).
$(CONFIGSTORE_BIN): $(CONFIGFILE)
	@echo "$(MSGPFX)Generating $@ from $<"
	@mkdir -p $(dir $@)
	$(QUIET)$(PYTHON) $(PROGRAM_CONFIGSTORE) -T "$(TOOLCHAIN_BASE)" -o $@ -d write-json $<
	$(QUIET)test $$(wc -c < $@) -le $$(($(CONFIGSTORE_PART_SIZE))) || \
		{ echo "error: $@ is larger than one config store partition" >&2; rm -f $@; exit 1; }

$(BIN_WITH_CONFIG): $(BIN_FILE) $(CONFIGSTORE_BIN)
	@echo "$(MSGPFX)Generating $@"
	$(QUIET)cp $(BIN_FILE) $@.tmp
	$(QUIET)dd if=$(CONFIGSTORE_BIN) of=$@.tmp bs=1 seek=$$(($(CONFIGSTORE_OFFSET))) conv=notrunc status=none
	$(QUIET)mv $@.tmp $@

# Programs what build.sh produced. It deliberately does not depend on the ELF: after
# "build.sh --revert-patches" the unpatched SDK sources are newer than the objects, and a rebuild
# here would silently flash a firmware without patch 0001.
flash:
	@test -f $(ELF_FILE) || { echo "error: $(ELF_FILE) missing; run relay_ekh05/build.sh first" >&2; exit 1; }
	openocd -c "set MM_PROGRAM 1" -f $(OPENOCD_CFG) -c "program $(ELF_FILE) verify reset exit"

# Starts OpenOCD in the background, writes the config store and stops OpenOCD again.
config: $(CONFIGFILE)
	@openocd -c "set CONNECT_ASSERT_SRST 1" -f $(OPENOCD_CFG) & \
	OPENOCD_PID=$$!; \
	trap 'kill $$OPENOCD_PID 2>/dev/null || true; wait $$OPENOCD_PID 2>/dev/null || true' EXIT INT TERM; \
	sleep 2; \
	$(PYTHON) $(PROGRAM_CONFIGSTORE) -T "$(TOOLCHAIN_BASE)" -H localhost -p $(PLATFORM) -d write-json $<

ifeq ($(DISABLE_FILEID_GENERATION),)
$(FILEHASHES_LIST): $(SRCS_C) $(addprefix $(MMIOT_ROOT)/,$(MMIOT_SRCS_C))
	@mkdir -p $(dir $@)
	@bash -c 'echo -e $(foreach src,$^,$(call uint32_hash,$(src)) $(src)\\n)' > $@
endif
