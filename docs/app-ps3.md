# La app de PS3

`build/ps3/gc-ps3.self` (y el paquete `build/ps3/gc-ps3.pkg`) corre los DOLs de GameCube en el mismo núcleo que el runner de PC y muestra su imagen. Código en `src/platform/ps3/main.c`.

## Qué hace

| Pantalla | Qué hay | Botones |
| --- | --- | --- |
| Menú | Los DOLs encontrados, por nombre (los de `/dev_hdd0/gcps3/dol` dicen "(hdd)"), y abajo lo que llega del mando | Arriba y abajo (cruceta o palanca izquierda; mantenido repite): elegir. Cruz: correr. Triángulo: salir |
| Corriendo | El último cuadro de la VI a 4:3, y encima: nombre, campo, MIPS y porcentaje de la velocidad de la GameCube | Círculo: volver al menú |

Los DOLs se buscan en tres carpetas: `dol` al lado del ejecutable (`build/ps3/dol` cuando se bootea el `.self`), `USRDIR/dol` del paquete instalado (`/dev_hdd0/game/GCPS00001/USRDIR/dol`) y `/dev_hdd0/gcps3/dol` (en RPCS3: `<carpeta de RPCS3>/dev_hdd0/gcps3/dol`). Con cada `.dol` se lee el `.map` del mismo nombre si está. Un nombre que ya apareció en una carpeta anterior no se repite (si se bootea el `.self` con el paquete instalado, valen los de `build/ps3/dol`).

El núcleo corre en el PPU, sin JIT: avanza hasta dos campos de video o 15 ms por cuadro de la PS3. Con el intérprete va más lento que una GameCube, así que el contador sube despacio; lo que imprimen los programas no cambia, porque miden tiempo emulado. Cada cuadro de la VI pasa a RGB en el PPU con el mismo código que el runner (`src/core/xfb.c`).

## Log

Sale por UDP y por la TTY ([log-udp.md](log-udp.md)):

| Línea | Qué es |
| --- | --- |
| `app: corriendo pantalla.dol (reloj 845791192)` | Empieza un programa; el reloj es el que quedó en el IPL emulado (segundos desde 2000) |
| `pantalla: ...` (sin prefijo) | Lo que imprime el programa (OSReport, printf, UART del IPL) |
| `app: cuadro 1 (campo 2) 640x480, crc32 RGB 0x10589e8c` | Un cuadro distinto al terminar un campo de video, con el CRC-32 de sus bytes RGB |
| `app: fin de pantalla.dol, 123200000 ciclos, 17 campos, 1 cuadros distintos` | Volvió al menú: ciclos corridos |
| `emu: ...` | Mensajes del núcleo (carga del DOL, accesos raros) |
| `app: mando en el puerto 1` | El mando que se usa (el primero conectado); `app: sin mando` si se desconecta |
| `app: botones 0x0040 (mando 0x0000, palanca 128,255)` | Cada cambio de botones: lo que usa la app, lo que llegó del mando (DIGITAL1 \| DIGITAL2 << 8) y la palanca izquierda (128 al centro) |

`build/host/gc-ps3-runner --cuadros` da la misma lista de cuadros con "runner:" delante.

## Probar en RPCS3

1. `make ps3 pkg` (en WSL).
2. En RPCS3: Configuration → Network → **Network status: Connected**, y abrir `tools\logrecv.cmd` en Windows.
3. Una de dos:
   - File → Boot (S)ELF → `build\ps3\gc-ps3.self` (toma los DOLs de `build\ps3\dol`), o
   - File → Install Packages → `build\ps3\gc-ps3.pkg` y arrancar "GC-PS3 Fase 1" desde la lista de juegos.
4. En el menú (si no se mueve, la línea de abajo dice qué llega del mando; qué tecla o botón es cada uno se ve en RPCS3 → Configuration → Pads), correr `pantalla.dol`, `contador.dol`, `cpu.dol` y `hw.dol` (y los demás si se quiere). Dejar `pantalla` y `contador` unos segundos y volver con Círculo. Sacar una captura de `pantalla` y de `contador`.
5. Salir con Triángulo y comparar el log con el runner:

```bash
python3 tools/compare_log.py logs/<el último>_gc-ps3_<sesión>.log
```

Por cada programa corre el runner con el mismo DOL, el mismo reloj y los mismos ciclos, y compara la salida línea por línea y la lista de cuadros (CRC incluido). Dice también si la salida es la completa de `tests/dol/esperado`. Sale con 0 si todo es igual. Sirve igual con el `TTY.log` de RPCS3 en vez del log UDP.

Lo que se espera: `pantalla` con un solo cuadro de CRC `0x10589e8c`; `contador` con un cuadro nuevo por campo; `cpu` y `hw` con su salida completa y `exit(0)`.

## Probada en la PC

`make test-app` (también corre en `make test` y en el CI) compila `main.c` en la PC con versiones de prueba de lv2, el mando, rsxkit y el log (`tests/appsim/stubs.c`) y la corre con un guion de botones (`tests/appsim/run.py`): baja por el menú, corre `contador`, `cpu`, `gx_fifo`, `gx_raster`, `gx_tri`, `hw`, `pantalla` y `timer`, entra dos veces al menú del sistema y el sistema la cierra con `timer` corriendo. El reloj de microsegundos avanza fijo, así que cada cuadro de la PS3 corre 3 millones de ciclos y el log sale siempre igual. Se revisa:

- `tools/compare_log.py`: las ocho corridas iguales al runner;
- el `exit(0)` de `cpu`, `gx_fifo`, `gx_raster`, `gx_tri`, `hw` y `timer`, el cuadro de `pantalla` (`0x10589e8c`) y varios de `contador`;
- que cada cuadro avisado se subió a rsxkit y que la subida, vuelta a RGB, tiene el mismo CRC;
- el menú: topes, cruceta y palanca, un botón mantenido que cuenta una vez y que repite si se mantiene más, y un DOL en dos carpetas que aparece una sola vez;
- la vuelta del menú del sistema y la salida por EXIT_GAME.

El log queda en `build/host/appsim/app.log`. En la PS3 falta lo que la simulación no cubre: el RSX, el mando y los archivos de verdad.
