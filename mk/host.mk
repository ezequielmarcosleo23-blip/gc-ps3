# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runner en PC y pruebas unitarias del núcleo portable, con ASan + UBSan.

HOST_CC     ?= cc
# La FPU del Gekko se emula con la de la máquina: sin fusionar a*b+c y
# respetando el modo de redondeo (ver src/cpu/fpu_util.h). Igual en ps3.mk.
FP_CFLAGS   := -ffp-contract=off -frounding-math
HOST_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Werror $(FP_CFLAGS) -Isrc -Iinclude
HOST_SAN    := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

HOST_CORE_SRCS   := $(wildcard src/core/*.c src/cpu/*.c)
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
	$(HOST_CC) $(HOST_SAN) $^ -lm -o $@

$(HOST_TESTS): $(HOST_TEST_OBJS)
	$(HOST_CC) $(HOST_SAN) $^ -lm -o $@

-include $(HOST_RUNNER_OBJS:.o=.d) $(HOST_TEST_OBJS:.o=.d)

# Las mismas pruebas en big-endian (PowerPC de 64 bits, como el PPU) con QEMU:
# en la PS3 core/endian.h no invierte bytes y ese camino también se prueba.
BE_CC      ?= powerpc64-linux-gnu-gcc
BE_QEMU    ?= qemu-ppc64
BE_CFLAGS  := -std=c11 -O1 -Wall -Wextra -Wpedantic -Werror $(FP_CFLAGS) -Isrc -Iinclude -mcpu=cell
HOST_BE_TESTS := $(BUILD)/host-be/unit-tests

$(HOST_BE_TESTS): $(HOST_TEST_SRCS) $(wildcard src/core/*.h src/cpu/*.h tests/unit/*.h)
	@mkdir -p $(dir $@)
	$(BE_CC) $(BE_CFLAGS) $(HOST_TEST_SRCS) -static -lm -o $@

# Oráculo: instrucciones ejecutadas en un PowerPC 750CXe emulado (base de Gekko).
# Regenera tests/unit/data/oracle_*.bin; necesita gcc-powerpc-linux-gnu y qemu-user.
ORACLE_CC   ?= powerpc-linux-gnu-gcc
ORACLE_QEMU ?= qemu-ppc -cpu 750cxe
ORACLE_SRCS := $(wildcard tools/ppc-oracle/oracle_*.c)
ORACLE_BINS := $(ORACLE_SRCS:tools/ppc-oracle/%.c=$(BUILD)/oracle/%)

$(BUILD)/oracle/%: tools/ppc-oracle/%.c
	@mkdir -p $(dir $@)
	$(ORACLE_CC) -static -O1 -mcpu=750 -Wall -Wextra -o $@ $<
