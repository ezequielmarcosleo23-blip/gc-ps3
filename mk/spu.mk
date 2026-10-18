# SPDX-License-Identifier: GPL-2.0-or-later
#
# Programas SPE: cada carpeta src/spu/<nombre>/ se compila a $(BUILD)/spu/<nombre>.elf
# y se incrusta en el ELF del PPU como spu_<nombre>_bin (con bin2s).

SPU_CC      := $(PS3DEV)/spu/bin/spu-gcc
SPU_MACHDEP := -mdual-nops -fmodulo-sched -ffunction-sections -fdata-sections
SPU_CFLAGS  := -O2 -g -Wall -Wextra -std=gnu11 $(SPU_MACHDEP) \
               -I$(PSL1GHT)/spu/include -Isrc
SPU_LDFLAGS := $(SPU_MACHDEP) -L$(PSL1GHT)/spu/lib
SPU_LIBS    := -lsputhread

SPU_PROGS := $(notdir $(patsubst %/,%,$(wildcard src/spu/*/)))
SPU_ELFS  := $(SPU_PROGS:%=$(BUILD)/spu/%.elf)

define SPU_PROG_template
SPU_OBJS_$(1) := $$(patsubst src/spu/$(1)/%.c,$(BUILD)/spu/$(1)/%.o,$$(wildcard src/spu/$(1)/*.c))
$(BUILD)/spu/$(1).elf: $$(SPU_OBJS_$(1))
	@mkdir -p $$(dir $$@)
	$$(SPU_CC) $$^ $$(SPU_LDFLAGS) $$(SPU_LIBS) -Wl,-Map,$$@.map -o $$@
-include $$(SPU_OBJS_$(1):.o=.d)
endef
$(foreach p,$(SPU_PROGS),$(eval $(call SPU_PROG_template,$(p))))

$(BUILD)/spu/%.o: src/spu/%.c
	@mkdir -p $(dir $@)
	$(SPU_CC) $(SPU_CFLAGS) -MMD -MP -c $< -o $@
