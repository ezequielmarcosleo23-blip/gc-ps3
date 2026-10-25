# Registro de rendimiento

Cada medición en consola se anota con el commit, la consola y su entorno (HEN/CFW, firmware, temperatura), y siempre con un antes y un después cuando se optimiza algo.

Las cifras de RPCS3 no van aquí: dependen de la PC (ver [fase0.md](fase0.md)).

## Mediciones pendientes en consola (Fase 0)

| Prueba | Qué se mide | Dónde sale en el log | Referencia |
| --- | --- | --- | --- |
| `udplog` | µs por línea sin pausa y con 4 hilos; pérdidas por ráfaga | `rafaga ...: N lineas en T us`; `tools/udplog_stats.py` | 0 perdidas a 1000/s y 100/s |
| `spu` | Mvert/s de hilo directo, spukit ×1 y ×4; µs en SPU | `exactos <modo>: mejor de 5` | — |
| `rsx` | ms por cuadro (media y máx.); tiempo de `rsxkit_init` | `600 cuadros ...`; `rsxkit: pantalla ...` | 16,7 ms a 60 Hz |
| `iso` | MB/s secuencial (HDD y USB); ms por lectura de 32 KB al azar | `secuencial: ...`; `aleatoria: ...` | Muy por encima de ~3 MB/s (lector de GameCube) |
| `jit` | µs para limpiar 64 KB de caché | `limpiar 64 KB de cache ...` | 12,2 µs en HEN según Salsa64 |

## Mediciones pendientes en consola (Fase 2)

| Prueba | Qué se mide | Dónde sale en el log | Referencia |
| --- | --- | --- | --- |
| `gx_escena` (en el menú de la app) | µs de GX por cuadro dibujado con el rasterizador de referencia en el PPU | `app: GX de gx_escena.dol: N us, 4 copias al XFB, K us por cuadro dibujado` | ~73 ms en la PC de la nube (runner en `-O2`); el RSX de la etapa D tiene que bajarlo a menos de 16,7 ms |

## Registro

| Fecha | Commit | Consola | Prueba | Antes | Después | Notas |
| --- | --- | --- | --- | --- | --- | --- |
