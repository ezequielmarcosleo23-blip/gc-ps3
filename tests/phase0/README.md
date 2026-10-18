# Pruebas de la Fase 0 en consola

Cada prueba será una app independiente que termina con una línea `RESULT: OK` o `RESULT: FAIL`, en pantalla y por UDP. Ese formato permite automatizarlas con `opentm_cli --pass-on "RESULT: OK" --fail-on "RESULT: FAIL"` en la consola DEX.

| Carpeta | Prueba | Estado |
| --- | --- | --- |
| `udplog/` | Log por UDP: pérdida y latencia | Etapa C |
| `jit/` | Escritura y ejecución de código | Etapa C |
| `spu/` | Trabajo en SPU y spukit | Etapa C |
| `rsx/` | Triángulo con rsxkit | Etapa C |
| `iso/` | Lectura de 1.4 GB desde HDD y USB | Etapa C |

Ver [docs/fase0.md](../../docs/fase0.md) para los criterios de éxito.
