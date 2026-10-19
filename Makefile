# SPDX-License-Identifier: GPL-2.0-or-later
#
# gc-ps3 — Makefile principal
#
#   make            compila la app de PS3 (.self) y el runner de PC
#   make test       compila y ejecuta las pruebas unitarias en PC (ASan + UBSan)
#   make test-be    las mismas pruebas en big-endian (PowerPC 64 con QEMU)
#   make phase0     compila las pruebas de la Fase 0 (build/phase0/<prueba>.self)
#   make logrecv    escucha el log UDP de la consola o de RPCS3 (Ctrl+C para salir)
#   make pkg        genera el .pkg instalable
#   make run        envía el .self a la consola con ps3load (PS3LOAD=tcp:IP_DE_LA_PS3)
#   make run-test TEST=hello PS3LOAD=tcp:IP   envía una prueba y espera su RESULT
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

.PHONY: all ps3 phase0 pkg host test test-be run run-test logrecv clean toolchain-check docker-image docker-make help

all: ps3 phase0 host

ps3: $(PS3_SELF)

phase0: $(PHASE0_SELFS)

pkg: $(PS3_PKG)

host: $(HOST_RUNNER) $(HOST_TESTS)

test: $(HOST_TESTS) $(HOST_RUNNER)
	$(HOST_TESTS)
	$(HOST_RUNNER) --version
	python3 tools/logrecv.py --selftest
	python3 tools/udplog_stats.py --selftest
	python3 tools/mkiso.py --selftest

# Pruebas unitarias en big-endian con QEMU (necesita gcc-powerpc64-linux-gnu y qemu-user)
test-be: $(HOST_BE_TESTS)
	$(BE_QEMU) $(HOST_BE_TESTS)

run: $(PS3_SELF)
	@if [ -z "$(PS3LOAD)" ]; then echo "Falta PS3LOAD, por ejemplo: make run PS3LOAD=tcp:192.168.1.50"; exit 1; fi
	PS3LOAD=$(PS3LOAD) $(PS3DEV)/bin/ps3load $(PS3_SELF)

LOGRECV_TIMEOUT ?= 300   # udplog dura unos 2 minutos

logrecv:
	python3 tools/logrecv.py --out logs

# Arranca el receptor, envía la prueba con ps3load y espera su RESULT (sale con 0 si es OK)
run-test: phase0
	@if [ -z "$(TEST)" ] || [ ! -f $(BUILD)/phase0/$(TEST).self ]; then echo "Uso: make run-test TEST=<$(PHASE0_TESTS)> PS3LOAD=tcp:IP"; exit 1; fi
	@if [ -z "$(PS3LOAD)" ]; then echo "Falta PS3LOAD, por ejemplo: PS3LOAD=tcp:192.168.1.50"; exit 1; fi
	python3 tools/logrecv.py --out logs --until-result --timeout $(LOGRECV_TIMEOUT) & pid=$$!; \
	sleep 1; PS3LOAD=$(PS3LOAD) $(PS3DEV)/bin/ps3load $(BUILD)/phase0/$(TEST).self || { kill $$pid; exit 1; }; \
	wait $$pid

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
	[ "$$tag" = "$(PS3DEV_TAG)" ] || { echo "ERROR: el toolchain no coincide con tools/toolchain.env"; exit 1; }; \
	printf 'void main(float4 p : POSITION, out float4 o : POSITION) { o = p; }\n' > /tmp/gcps3-cg.vcg; \
	if $(PS3DEV)/bin/cgcomp -v /tmp/gcps3-cg.vcg /tmp/gcps3-cg.vpo > /dev/null 2>&1; then echo "cgcomp      : OK (libCg)"; \
	else echo "ERROR: cgcomp no carga libCg.so; instalar con: sudo apt install libcg"; rm -f /tmp/gcps3-cg.*; exit 1; fi; \
	rm -f /tmp/gcps3-cg.*

DOCKER_IMAGE := gc-ps3-build:$(PS3DEV_TAG)

docker-image:
	docker build -f docker/Dockerfile -t $(DOCKER_IMAGE) .

docker-make: docker-image
	docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR)":/src -w /src $(DOCKER_IMAGE) \
		make -j$$(nproc) BUILD=build-docker all test test-be pkg

help:
	@sed -n '3,14p' Makefile | sed 's/^# \{0,1\}//'
