# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runner en PC y pruebas unitarias del núcleo portable, con ASan + UBSan.

HOST_CC     ?= cc
# La FPU del Gekko se emula con la de la máquina: sin fusionar a*b+c y
# respetando el modo de redondeo (ver src/cpu/fpu_util.h). Igual en ps3.mk.
FP_CFLAGS   := -ffp-contract=off -frounding-math
HOST_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Werror $(FP_CFLAGS) -Isrc -Iinclude
HOST_SAN    := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

HOST_CORE_SRCS   := $(wildcard src/core/*.c src/cpu/*.c src/hw/*.c src/gx/*.c)
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

$(HOST_BE_TESTS): $(HOST_TEST_SRCS) $(wildcard src/core/*.h src/cpu/*.h src/hw/*.h src/gx/*.h tests/unit/*.h)
	@mkdir -p $(dir $@)
	$(BE_CC) $(BE_CFLAGS) $(HOST_TEST_SRCS) -static -lm -o $@

# Oráculo: instrucciones ejecutadas en un PowerPC 750CXe emulado (base de Gekko).
# Regenera tests/unit/data/oracle_*.bin; necesita gcc-powerpc-linux-gnu y qemu-user.
ORACLE_CC   ?= powerpc-linux-gnu-gcc
ORACLE_QEMU ?= qemu-ppc -cpu 750cxe
ORACLE_SRCS := $(wildcard tools/ppc-oracle/oracle_*.c)
ORACLE_BINS := $(ORACLE_SRCS:tools/ppc-oracle/%.c=$(BUILD)/oracle/%)

.PHONY: oracle-cc-check
oracle-cc-check:
	@command -v $(ORACLE_CC) >/dev/null 2>&1 || { \
	  echo "Falta $(ORACLE_CC). En Ubuntu: sudo apt install -y gcc-powerpc-linux-gnu qemu-user"; exit 1; }

$(BUILD)/oracle/%: tools/ppc-oracle/%.c | oracle-cc-check
	@mkdir -p $(dir $@)
	$(ORACLE_CC) -static -O1 -mcpu=750 -Wall -Wextra -o $@ $<

# La app de PS3 en la PC (tests/appsim): src/platform/ps3/main.c con versiones
# de prueba del mando, lv2, rsxkit y el log, y el mismo núcleo que el runner.
# tests/appsim/run.py la corre con un guion de botones y revisa el log.
APPSIM_DIR    := $(BUILD)/host/appsim
APPSIM        := $(APPSIM_DIR)/gc-ps3-appsim
APPSIM_SRCS   := src/platform/ps3/main.c src/platform/ps3/files.c tests/appsim/stubs.c
APPSIM_OBJS   := $(APPSIM_SRCS:%.c=$(APPSIM_DIR)/obj/%.o)
APPSIM_CFLAGS := $(filter-out -std=c11,$(HOST_CFLAGS)) -std=gnu11 -Itests/appsim/include -Ilib/rsxkit/include

$(APPSIM_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(APPSIM_CFLAGS) $(HOST_SAN) -MMD -MP -c $< -o $@

$(APPSIM): $(APPSIM_OBJS) $(HOST_CORE_SRCS:%.c=$(BUILD)/host/obj/%.o)
	$(HOST_CC) $(HOST_SAN) $^ -lm -o $@

-include $(APPSIM_OBJS:.o=.d)
