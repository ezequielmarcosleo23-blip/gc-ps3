# SPDX-License-Identifier: GPL-2.0-or-later
# Configuración común del build

PS3DEV    ?= /usr/local/ps3dev
PSL1GHT   ?= $(PS3DEV)
# ppu-gcc lee $PSL1GHT del entorno al enlazar, así que se exportan aunque la
# terminal no tenga cargado ~/.ps3dev_env
export PS3DEV PSL1GHT
BUILD     ?= build

APP       := gc-ps3
TITLE     := GC-PS3 Fase 1
APPID     := GCPS00001
CONTENTID := UP0001-$(APPID)_00-0000000000000000

# Herramientas de PSL1GHT (vienen en el nightly de ps3dev)
PS3_TOOLS := $(PS3DEV)/bin
