# SPDX-License-Identifier: GPL-2.0-or-later
#
# DOLs de prueba (tests/dol, D-034): programas propios en C, sin libc,
# compilados con el compilador cruzado de PowerPC de 32 bits que ya usa el
# oráculo. Los .dol, .map y las salidas esperadas se versionan; `make testdols`
# los regenera (no hace falta para `make test` ni en el CI).
#
#   tests/dol/bin/<prueba>.dol, .map   lo que corre el emulador
#   tests/dol/esperado/<prueba>.txt    lo que tiene que imprimir; para hola y cpu
#                                      sale de compilar el mismo programa en la PC

DOL_CC     ?= powerpc-linux-gnu-gcc
DOL_CFLAGS := -std=c11 -mcpu=750 -mhard-float -O2 -ffreestanding -fno-builtin -fno-pic \
              -msdata=none -G 0 -fno-stack-protector -fno-asynchronous-unwind-tables \
              -fno-tree-loop-distribute-patterns -ffp-contract=off -Wall -Wextra -Werror \
              -Wa,-m750cl -Itests/dol
DOL_LDFLAGS := -nostdlib -static -T tests/dol/link.ld -Wl,--build-id=none
TESTDOLS    := hola cpu timer
TESTDOLS_HOSTED := hola cpu                # los que también compilan en la PC

DOL_OUT  := tests/dol/bin
DOL_TMP  := $(BUILD)/testdols
DOL_RT   := $(DOL_TMP)/crt0.o $(DOL_TMP)/rt.o

$(DOL_TMP)/%.o: tests/dol/%.c tests/dol/rt.h
	@mkdir -p $(dir $@)
	$(DOL_CC) $(DOL_CFLAGS) -c $< -o $@

$(DOL_TMP)/crt0.o: tests/dol/crt0.S
	@mkdir -p $(dir $@)
	$(DOL_CC) $(DOL_CFLAGS) -c $< -o $@

$(DOL_TMP)/%.elf: $(DOL_RT) $(DOL_TMP)/%.o tests/dol/link.ld
	$(DOL_CC) $(DOL_LDFLAGS) $(DOL_RT) $(DOL_TMP)/$*.o -lgcc -o $@

$(DOL_OUT)/%.dol: $(DOL_TMP)/%.elf tools/elf2dol.py
	@mkdir -p $(dir $@)
	python3 tools/elf2dol.py $< $@ --map $(DOL_OUT)/$*.map

# La misma prueba compilada para la PC: su salida es la esperada del DOL
$(DOL_TMP)/host/%: tests/dol/%.c tests/dol/rt_host.c tests/dol/rt.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O2 -ffp-contract=off -Wall -Wextra -Werror -DGCPS3_HOST -Itests/dol $< tests/dol/rt_host.c -o $@

tests/dol/esperado/%.txt: $(DOL_TMP)/host/%
	@mkdir -p $(dir $@)
	$< > $@
