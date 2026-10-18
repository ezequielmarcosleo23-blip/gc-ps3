# SPDX-License-Identifier: GPL-2.0-or-later
#
# App de PS3 (PPU) con PSL1GHT: .elf -> .self (+ .fake.self) -> .pkg
# Replica los pasos de $(PSL1GHT)/ppu_rules sin el make recursivo de los samples.

PPU_BIN     := $(PS3DEV)/ppu/bin
PPU_CC      := $(PPU_BIN)/ppu-gcc
PPU_AS      := $(PPU_BIN)/ppu-as
PPU_STRIP   := $(PPU_BIN)/ppu-strip

PPU_MACHDEP := -mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections
PPU_INC     := -I$(PSL1GHT)/ppu/include -I$(PSL1GHT)/ppu/include/simdmath \
               -Isrc -Iinclude -Ilib/ps3mapi-lib/include -I$(BUILD)/ps3/gen
PPU_CFLAGS  := -O2 -g -Wall -Wextra -std=gnu11 $(PPU_MACHDEP) $(PPU_INC)
# Código de terceros en lib/: sin -Wextra y con aliasing relajado (como su Makefile original)
LIB_CFLAGS  := -O2 -g -Wall -std=gnu11 $(PPU_MACHDEP) $(PPU_INC) -DBIGENDIAN -fno-strict-aliasing
PPU_LDFLAGS := $(PPU_MACHDEP) -L$(PSL1GHT)/ppu/lib
PPU_LIBS    := -lsysutil -lrt -llv2 -lm

PS3_SRCS  := $(wildcard src/platform/ps3/*.c) $(wildcard src/core/*.c)
LIB_SRCS  := $(wildcard lib/ps3mapi-lib/source/*.c)
PS3_OBJS  := $(PS3_SRCS:%.c=$(BUILD)/ps3/obj/%.o)
LIB_OBJS  := $(LIB_SRCS:%.c=$(BUILD)/ps3/obj/%.o)
SPU_BLOBS := $(SPU_PROGS:%=$(BUILD)/ps3/gen/spu_%.bin.o)
SPU_HDRS  := $(SPU_PROGS:%=$(BUILD)/ps3/gen/spu_%_bin.h)

PS3_ELF      := $(BUILD)/ps3/$(APP).elf
PS3_STRIPPED := $(BUILD)/ps3/$(APP).stripped.elf
PS3_SELF     := $(BUILD)/ps3/$(APP).self
PS3_PKG      := $(BUILD)/ps3/$(APP).pkg

# --- programas SPE incrustados ------------------------------------------------
$(BUILD)/ps3/gen/spu_%.bin: $(BUILD)/spu/%.elf
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/ps3/gen/spu_%.bin.o: $(BUILD)/ps3/gen/spu_%.bin
	cd $(dir $<) && $(PS3_TOOLS)/bin2s -a 64 $(notdir $<) | $(PPU_AS) -o $(notdir $@)

$(BUILD)/ps3/gen/spu_%_bin.h:
	@mkdir -p $(dir $@)
	@printf '#pragma once\n#include <ppu-types.h>\nextern const u8 spu_%s_bin[];\nextern const u8 spu_%s_bin_end[];\nextern const u32 spu_%s_bin_size;\n' $* $* $* > $@

# --- objetos del PPU ----------------------------------------------------------
$(BUILD)/ps3/obj/lib/%.o: lib/%.c
	@mkdir -p $(dir $@)
	$(PPU_CC) $(LIB_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/ps3/obj/%.o: %.c | $(SPU_HDRS)
	@mkdir -p $(dir $@)
	$(PPU_CC) $(PPU_CFLAGS) -MMD -MP -c $< -o $@

-include $(PS3_OBJS:.o=.d) $(LIB_OBJS:.o=.d)

# --- enlazado, SELF y PKG -----------------------------------------------------
$(PS3_ELF): $(PS3_OBJS) $(LIB_OBJS) $(SPU_BLOBS)
	$(PPU_CC) $^ $(PPU_LDFLAGS) $(PPU_LIBS) -Wl,-Map,$@.map -o $@

$(PS3_SELF): $(PS3_ELF)
	$(PPU_STRIP) $< -o $(PS3_STRIPPED)
	$(PS3_TOOLS)/sprxlinker $(PS3_STRIPPED)
	$(PS3_TOOLS)/make_self $(PS3_STRIPPED) $@
	$(PS3_TOOLS)/fself $(PS3_STRIPPED) $(basename $@).fake.self

$(PS3_PKG): $(PS3_SELF)
	rm -rf $(BUILD)/ps3/pkg && mkdir -p $(BUILD)/ps3/pkg/USRDIR
	cp $(PS3_TOOLS)/ICON0.PNG $(BUILD)/ps3/pkg/ICON0.PNG
	$(PS3_TOOLS)/make_self_npdrm $(PS3_STRIPPED) $(BUILD)/ps3/pkg/USRDIR/EBOOT.BIN $(CONTENTID) > /dev/null
	$(PS3_TOOLS)/sfo --title "$(TITLE)" --appid "$(APPID)" -f $(PS3_TOOLS)/sfo.xml $(BUILD)/ps3/pkg/PARAM.SFO
	$(PS3_TOOLS)/pkg --contentid $(CONTENTID) $(BUILD)/ps3/pkg/ $@ > /dev/null
	cp $@ $(basename $@).gnpdrm.pkg
	$(PS3_TOOLS)/package_finalize $(basename $@).gnpdrm.pkg
