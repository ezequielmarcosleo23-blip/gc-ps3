# Librerías de terceros

Cada librería se copia aquí sin su historial de git, fijada a un commit, y debe ser compatible con GPL-2.0-or-later. Para actualizar una: reemplazar la carpeta completa por la del nuevo commit y cambiar esta tabla en el mismo commit.

| Librería | Origen | Commit | Fecha | Licencia | Uso |
| --- | --- | --- | --- | --- | --- |
| ps3mapi-lib | https://github.com/bucanero/ps3mapi-lib | `234e3bf947e6260b5bbd874877d81a2825de8bb0` | 2022-11-15 | MIT | Detección de entorno (HEN, Cobra, Mamba, PS3MAPI), backends del JIT, temperatura |
| spukit | https://github.com/ricowp81/spukit (tag `v0.1.0`) | `1bdff0e2d11b85600a957b7104236c96a1b21356` | 2026-10-18 | MIT | Trabajadores SPU persistentes con cola de trabajos (prueba `spu`) |
| rsxkit | https://github.com/ricowp81/rsxkit (tag `v0.1.0`) | `0235a022752111862f038e7d40146e088f85b681` | 2026-10-18 | MIT | Video, destinos de render, fragment programs generados y texto (prueba `rsx`) |

Notas:

- **ps3mapi-lib** se compila directamente dentro de la app (ver `mk/ps3.mk`); su `Makefile` original se conserva solo como referencia. Sus archivos fuente dicen "It may be used for any purpose as long as this notice remains intact", así que no se modifican.
- **spukit** y **rsxkit** nacieron en [Salsa64](https://github.com/ricowp81/Salsa64) (que las lleva como subtree, con el mismo código) y se copian de sus repos propios sin la carpeta `examples/`. Se compilan con su `Makefile` original en `build/lib/<nombre>` (ver `mk/ps3.mk`) y no se modifican. Adoptadas tras las pruebas `spu` y `rsx` de la Fase 0 (D-024); el rendimiento se confirma en consola.
- **rsxkit** necesita `cgcomp` con `libCg.so` para sus shaders precompilados: `sudo apt install libcg` (Ubuntu multiverse; ya está en la imagen Docker). Ver D-020.
