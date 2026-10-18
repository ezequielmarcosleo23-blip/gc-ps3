# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runner en PC y pruebas unitarias del núcleo portable, con ASan + UBSan.

HOST_CC     ?= cc
HOST_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Werror -Isrc -Iinclude
HOST_SAN    := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

HOST_CORE_SRCS   := $(wildcard src/core/*.c)
HOST_RUNNER_SRCS := $(wildcard src/platform/host/*.c) $(HOST_CORE_SRCS)
HOST_TEST_SRCS   := $(wildcard tests/unit/*.c) $(HOST_CORE_SRCS)

HOST_RUNNER_OBJS := $(HOST_RUNNER_SRCS:%.c=$(BUILD)/host/obj/%.o)
HOST_TEST_OBJS   := $(HOST_TEST_SRCS:%.c=$(BUILD)/host/obj/%.o)

HOST_RUNNER := $(BUILD)/host/gc-ps3-runner
HOST_TESTS  := $(BUILD)/host/unit-tests

$(BUILD)/host/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_SAN) -MMD -MP -c $< -o $@

$(HOST_RUNNER): $(HOST_RUNNER_OBJS)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

$(HOST_TESTS): $(HOST_TEST_OBJS)
	$(HOST_CC) $(HOST_SAN) $^ -o $@

-include $(HOST_RUNNER_OBJS:.o=.d) $(HOST_TEST_OBJS:.o=.d)
