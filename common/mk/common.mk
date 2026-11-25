#
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Makefile fragment that adds the lab common library to an mm-iot-sdk (make flow) application.
#
# Include it from the application Makefile after BUILD_DIR, CC, CFLAGS and CONLYFLAGS are set
# (i.e. after the SDK mk/*.mk includes) and BEFORE the "$(ELF_FILE): $(OBJS) ..." rule, because
# make expands rule prerequisites when the rule is read:
#
#   LAB_COMMON_DIR := $(abspath $(APP_DIR)/../common)
#   include $(LAB_COMMON_DIR)/mk/common.mk
#
# Effects: appends the objects to OBJS, adds include/ to CFLAGS and provides the compile rule
# (objects go to $(BUILD_DIR)/lab_common/). Set LAB_COMMON_NO_RULES=1 to only get the variables
# below and integrate the sources yourself.
#
# Requires the SDK lwIP with LWIP_SOCKET=1 (the default in framework/src/lwip/port/lwipopts.h).

LAB_COMMON_DIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

LAB_COMMON_SRCS_C := \
	$(LAB_COMMON_DIR)/src/lab_platform_common.c \
	$(LAB_COMMON_DIR)/src/lab_sock.c \
	$(LAB_COMMON_DIR)/src/lab_util.c \
	$(LAB_COMMON_DIR)/src/linkproto.c \
	$(LAB_COMMON_DIR)/src/linkstats.c \
	$(LAB_COMMON_DIR)/src/msgstream.c \
	$(LAB_COMMON_DIR)/src/svc_tcp_stream.c \
	$(LAB_COMMON_DIR)/src/svc_udp_echo.c \
	$(LAB_COMMON_DIR)/port/mmosal/lab_platform_mmosal.c

LAB_COMMON_SRCS_H := $(wildcard $(LAB_COMMON_DIR)/include/*.h $(LAB_COMMON_DIR)/src/*.h)

# Public headers (for the application) and private ones (for the library sources only).
LAB_COMMON_INCLUDES := $(LAB_COMMON_DIR)/include
LAB_COMMON_PRIV_INCLUDES := $(LAB_COMMON_DIR)/src

# Selects the lwIP socket shim and the mmosal port.
LAB_COMMON_DEFINES := LAB_PLATFORM_MMOSAL=1

ifeq ($(LAB_COMMON_NO_RULES),)
LAB_COMMON_BUILD_DIR ?= $(BUILD_DIR)/lab_common
LAB_COMMON_OBJS := $(patsubst $(LAB_COMMON_DIR)/%.c,$(LAB_COMMON_BUILD_DIR)/%.o,$(LAB_COMMON_SRCS_C))

OBJS += $(LAB_COMMON_OBJS)
CFLAGS += $(addprefix -I,$(LAB_COMMON_INCLUDES))

$(LAB_COMMON_BUILD_DIR)/%.o: $(LAB_COMMON_DIR)/%.c $(LAB_COMMON_SRCS_H)
	@echo "$(MSGPFX)Compiling $<"
	@mkdir -p $(dir $@)
	$(QUIET)$(CC) -o $@ -c $(CFLAGS) $(CONLYFLAGS) \
		$(addprefix -I,$(LAB_COMMON_PRIV_INCLUDES)) $(addprefix -D,$(LAB_COMMON_DEFINES)) $<
endif
