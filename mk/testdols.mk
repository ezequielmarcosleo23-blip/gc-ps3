# SPDX-License-Identifier: GPL-2.0-or-later
#
# DOLs de prueba (tests/dol, D-034): programas propios en C, sin libc,
# compilados con el compilador cruzado de PowerPC de 32 bits que ya usa el
# oráculo. Los .dol, .map y las salidas esperadas se versionan; `make testdols`
# los regenera (no hace falta para `make test` ni en el CI).
#
#   tests/dol/bin/<prueba>.dol, .map   lo que corre el emulador
#   tests/dol/esperado/<prueba>.txt    lo que tiene que imprimir; para hola, cpu y
#                                      pantalla sale de compilar el mismo programa en la PC

DOL_CC     ?= powerpc-linux-gnu-gcc
DOL_CFLAGS := -std=c11 -mcpu=750 -mhard-float -O2 -ffreestanding -fno-builtin -fno-pic \
              -msdata=none -G 0 -fno-stack-protector -fno-asynchronous-unwind-tables \
              -fno-tree-loop-distribute-patterns -ffp-contract=off -Wall -Wextra -Werror \
              -Wa,-m750cl -Itests/dol
DOL_LDFLAGS := -nostdlib -static -T tests/dol/link.ld -Wl,--build-id=none
TESTDOLS    := hola cpu timer pantalla contador hw gx_fifo
TESTDOLS_HOSTED := hola cpu pantalla       # los que también compilan en la PC
DOL_VIDEO   := pantalla contador           # los que usan tests/dol/video.c
DOL_GXLIB   := gx_fifo                     # los que usan tests/dol/gxlib.c (Fase 2)
TESTDOLS_EXIT := hola cpu timer hw gx_fifo # terminan con exit(0)
TESTDOLS_STAY := pantalla contador         # se quedan mostrando algo

DOL_OUT  := tests/dol/bin
DOL_TMP  := $(BUILD)/testdols
DOL_RT   := $(DOL_TMP)/crt0.o $(DOL_TMP)/rt.o

# Las reglas que compilan los DOLs solo existen con `make testdols`. En los demás
# objetivos los .dol, .map y .txt versionados son archivos fuente: la app de PS3
# y el .pkg los copian, y en un checkout nuevo (el CI) no hay .elf intermedios
# ni compilador cruzado para rehacerlos.
ifneq ($(filter testdols,$(MAKECMDGOALS)),)

# Antes de compilar: el compilador cruzado tiene que estar instalado
.PHONY: dol-cc-check
dol-cc-check:
	@command -v $(DOL_CC) >/dev/null 2>&1 || { \
	  echo "Falta $(DOL_CC). En Ubuntu: sudo apt install -y gcc-powerpc-linux-gnu"; exit 1; }

$(DOL_TMP)/%.o: tests/dol/%.c tests/dol/rt.h tests/dol/video.h tests/dol/gxlib.h | dol-cc-check
	@mkdir -p $(dir $@)
	$(DOL_CC) $(DOL_CFLAGS) -c $< -o $@

$(DOL_TMP)/crt0.o: tests/dol/crt0.S | dol-cc-check
	@mkdir -p $(dir $@)
	$(DOL_CC) $(DOL_CFLAGS) -c $< -o $@

$(DOL_TMP)/%.elf: $(DOL_RT) $(DOL_TMP)/%.o tests/dol/link.ld
	$(DOL_CC) $(DOL_LDFLAGS) $(filter %.o,$^) -lgcc -o $@

$(DOL_VIDEO:%=$(DOL_TMP)/%.elf): $(DOL_TMP)/video.o
$(DOL_GXLIB:%=$(DOL_TMP)/%.elf): $(DOL_TMP)/gxlib.o

$(DOL_OUT)/%.dol: $(DOL_TMP)/%.elf tools/elf2dol.py
	@mkdir -p $(dir $@)
	python3 tools/elf2dol.py $< $@ --map $(DOL_OUT)/$*.map

# La misma prueba compilada para la PC: su salida es la esperada del DOL
$(DOL_TMP)/host/%: tests/dol/%.c tests/dol/rt_host.c tests/dol/rt.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O2 -ffp-contract=off -Wall -Wextra -Werror -DGCPS3_HOST -Itests/dol $(filter %.c,$^) -o $@

$(DOL_TMP)/host/pantalla: tests/dol/video.c tests/dol/video.h

tests/dol/esperado/%.txt: $(DOL_TMP)/host/%
	@mkdir -p $(dir $@)
	$< > $@

endif
