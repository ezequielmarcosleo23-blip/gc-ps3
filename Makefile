# SPDX-License-Identifier: GPL-2.0-or-later
#
# gc-ps3 — Makefile principal
#
#   make            compila la app de PS3 (.self) y el runner de PC
#   make test       compila y ejecuta las pruebas unitarias en PC (ASan + UBSan)
#   make pkg        genera el .pkg instalable
#   make run        envía el .self a la consola con ps3load (PS3LOAD=tcp:IP_DE_LA_PS3)
#   make docker-make  compila todo dentro de la imagen Docker (igual que el CI)
#   make help       muestra esta ayuda

include mk/config.mk
include tools/toolchain.env

.DEFAULT_GOAL := all
.SUFFIXES:
.DELETE_ON_ERROR:
.SECONDARY:

include mk/spu.mk
include mk/ps3.mk
include mk/host.mk

.PHONY: all ps3 pkg host test run clean toolchain-check docker-image docker-make help

all: ps3 host

ps3: $(PS3_SELF)

pkg: $(PS3_PKG)

host: $(HOST_RUNNER) $(HOST_TESTS)

test: $(HOST_TESTS) $(HOST_RUNNER)
	$(HOST_TESTS)
	$(HOST_RUNNER) --version

run: $(PS3_SELF)
	@if [ -z "$(PS3LOAD)" ]; then echo "Falta PS3LOAD, por ejemplo: make run PS3LOAD=tcp:192.168.1.50"; exit 1; fi
	PS3LOAD=$(PS3LOAD) $(PS3DEV)/bin/ps3load $(PS3_SELF)

clean:
	rm -rf $(BUILD)

# Comprueba que el toolchain instalado es el que fija tools/toolchain.env
toolchain-check:
	@tag=$$(cat $(PS3DEV)/.ps3dev_tag 2>/dev/null || echo "?"); \
	echo "PS3DEV       : $(PS3DEV)"; \
	echo "tag esperado : $(PS3DEV_TAG)"; \
	echo "tag instalado: $$tag"; \
	$(PPU_CC) --version | head -n1; \
	$(SPU_CC) --version | head -n1; \
	[ "$$tag" = "$(PS3DEV_TAG)" ] || { echo "ERROR: el toolchain no coincide con tools/toolchain.env"; exit 1; }

DOCKER_IMAGE := gc-ps3-build:$(PS3DEV_TAG)

docker-image:
	docker build -f docker/Dockerfile -t $(DOCKER_IMAGE) .

docker-make: docker-image
	docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR)":/src -w /src $(DOCKER_IMAGE) \
		make -j$$(nproc) BUILD=build-docker all test pkg

help:
	@sed -n '3,11p' Makefile | sed 's/^# \{0,1\}//'
