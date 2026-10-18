# Librerías de terceros

Cada librería se copia aquí sin su historial de git, fijada a un commit, y debe ser compatible con GPL-2.0-or-later. Para actualizar una: reemplazar la carpeta completa por la del nuevo commit y cambiar esta tabla en el mismo commit.

| Librería | Origen | Commit | Fecha | Licencia | Uso |
| --- | --- | --- | --- | --- | --- |
| ps3mapi-lib | https://github.com/bucanero/ps3mapi-lib | `234e3bf947e6260b5bbd874877d81a2825de8bb0` | 2022-11-15 | MIT | Detección de entorno (HEN, Cobra, Mamba, PS3MAPI), backends del JIT, temperatura |

Notas:

- **ps3mapi-lib** se compila directamente dentro de la app (ver `mk/ps3.mk`); su `Makefile` original se conserva solo como referencia. Sus archivos fuente dicen "It may be used for any purpose as long as this notice remains intact", así que no se modifican.
- **rsxkit** y **spukit** (Salsa64, MIT) se evalúan en las pruebas `rsx` y `spu` de la Fase 0; solo se agregan aquí si se adoptan.
