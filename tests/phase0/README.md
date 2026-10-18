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
| `udplog/` | Log por UDP: pérdida y ritmo | C | Pendiente |
| `jit/` | Escritura y ejecución de código | C | Pendiente (necesita consola) |
| `spu/` | Trabajo en SPU y spukit | C | Pendiente |
| `rsx/` | Triángulo con rsxkit | C | Pendiente |
| `iso/` | Lectura de 1.4 GB desde HDD y USB | C | Pendiente |

**Ejecutar en RPCS3:** abrir `tools\logrecv.cmd` en Windows y luego File → Boot (S)ELF → `build\phase0\<prueba>.self` (con Network status: Connected). Ver [docs/log-udp.md](../../docs/log-udp.md).

**Ejecutar en consola:** `make run-test TEST=<prueba> PS3LOAD=tcp:<IP>` con ps3loadx abierto en la PS3.

Los criterios de éxito están en [docs/fase0.md](../../docs/fase0.md).
