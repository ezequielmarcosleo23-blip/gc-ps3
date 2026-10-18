# Log por UDP

Las pruebas de la consola (y las de RPCS3) mandan su log a la PC por UDP, una línea por datagrama, al puerto **18194**. También lo escriben en la TTY.

## Formato

```
GCPS3 <sesión> <secuencia> <t_us> <nivel> <prueba> | <mensaje>
GCPS3 0000beef 7 1500 I hello | hola mundo
```

| Campo | Contenido |
| --- | --- |
| sesión | 8 dígitos hex; identifica una ejecución |
| secuencia | desde 0, de 1 en 1; un hueco = una línea perdida |
| t_us | microsegundos desde que arrancó el log en la consola (timebase del PPU) |
| nivel | `I` info, `W` aviso, `E` error, `R` resultado final |
| prueba | `[a-z0-9_-]`, hasta 31 caracteres |

La implementación de referencia es `src/core/logline.c` (con pruebas en `tests/unit/test_logline.c`). `tools/logrecv.py` y `tools/logrecv.ps1` leen el mismo formato y comparten la línea de referencia de las pruebas.

## Destinos

| Situación | La app envía a |
| --- | --- |
| Existe `/dev_hdd0/tmp/gcps3/log.cfg` con `IP` o `IP:puerto` | Solo esa dirección |
| No existe | `255.255.255.255` (red local) y `127.0.0.1` (sirve en RPCS3) |

Como la misma línea puede llegar por dos caminos, el receptor descarta duplicados por número de secuencia.

## Receptores

| Dónde | Comando | Notas |
| --- | --- | --- |
| Windows | `tools\logrecv.cmd` | Sin instalar nada. Para RPCS3 en la misma PC basta así; para la consola, Windows pedirá permitir PowerShell en la red privada |
| Ubuntu (WSL) | `make logrecv` | Para recibir desde la consola necesita WSL en modo *mirrored* y una regla UDP 18194 en el firewall de Hyper-V |
| Ubuntu (WSL) | `make run-test TEST=hello PS3LOAD=tcp:IP` | Arranca el receptor, envía la prueba con ps3load y sale con 0 si llega `RESULT: OK`, 1 si `FAIL`, 2 si se agota el tiempo |

Cada ejecución queda en `logs/AAAAMMDD-HHMMSS_<prueba>_<sesión>.log` (ignorado por git). Al llegar el `RESULT` el receptor muestra el resumen: líneas recibidas, perdidas y duplicadas.

## RPCS3

1. En RPCS3: Configuration → Network → **Network status: Connected** (sin esto no hay sockets).
2. Abrir `tools\logrecv.cmd` en Windows.
3. File → Boot (S)ELF → `build\phase0\<prueba>.self`.
