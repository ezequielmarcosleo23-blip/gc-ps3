# Pruebas de la Fase 0 en consola

Cada carpeta (salvo `common/`) es una prueba independiente que se compila a `build/phase0/<prueba>.self`. Todas usan la base común de `common/harness.h`: log por UDP y TTY, entorno detectado, línea final `RESULT: OK` o `RESULT: FAIL` y, en consola con HEN/CFW, buzzer (1 pitido OK, 3 FAIL).

```c
#include "common/harness.h"

static int mi_prueba(const ps3_env_t *env)
{
    LOGI("algo que medir: %d", 42);
    return 0;               /* 0 = OK */
}

int main(void) { return phase0_main("mi_prueba", mi_prueba); }
```

| Carpeta | Prueba | Etapa | Estado |
| --- | --- | --- | --- |
| `hello/` | El log llega a la PC (5 líneas + entorno + RESULT) | B | Lista para RPCS3 |
| `udplog/` | Log por UDP: pérdida por ráfaga y costo por línea; logger con 4 hilos | C | Lista para RPCS3 |
| `jit/` | Código generado en la cola NOBITS (`nobits.ld`), marca de crash | C | Compilada; necesita consola |
| `spu/` | 64 000 vértices × 4×4 en SPU: hilo directo y spukit (`src/spu/xform*`) | C | Lista para RPCS3 |
| `rsx/` | Destino 640×480 a 4:3 en 720p con rsxkit, lectura de píxeles | C | Lista para RPCS3 (con Write Color Buffers) |
| `iso/` | Lectura verificada de 1.4 GB desde HDD y USB (`tools/mkiso.py`) | C | Lista para RPCS3 (con el archivo) |

**Ejecutar en RPCS3:** abrir `tools\logrecv.cmd` en Windows y luego File → Boot (S)ELF → `build\phase0\<prueba>.self` (con Network status: Connected). Ver [docs/log-udp.md](../../docs/log-udp.md).

**Ejecutar en consola:** `make run-test TEST=<prueba> PS3LOAD=tcp:<IP>` con ps3loadx abierto en la PS3.

`LOGD(...)` escribe líneas de detalle: van por UDP al archivo del log pero no a la TTY ni a la pantalla del receptor. Para ráfagas de datos.

Una prueba puede pedir librerías u opciones de enlace propias en `mk/ps3.mk` (`PHASE0_LIBS_<prueba>`, `PHASE0_LDFLAGS_<prueba>`).

Los criterios de éxito y cómo correr cada prueba en RPCS3 están en [docs/fase0.md](../../docs/fase0.md).
