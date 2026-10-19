# Mapa de memoria de la GameCube en el emulador

Implementado en `src/core/mem.c` y `src/core/mmio.c` (Fase 1, etapa A). Las direcciones son las efectivas de Gekko con el mapa por omisión de los BAT, que es el que dejan el IPL y casi todos los juegos.

| Dirección | Qué es | En el emulador |
| --- | --- | --- |
| `0x80000000`–`0x817FFFFF` | RAM principal, 24 MB, con caché | Búfer de 32 MB alineado a 64 KB; dirección `& 0x01FFFFFF` |
| `0xC0000000`–`0xC17FFFFF` | La misma RAM, sin caché | El mismo búfer |
| `0x00000000`–`0x017FFFFF` | La misma RAM, física (traducción apagada, DMA) | El mismo búfer |
| `0x81800000`–`0x81FFFFFF` | Fuera de los 24 MB | Cola del búfer: se lee y escribe sin fallar (D-008) |
| `0xCC000000`–`0xCC00FFFF` | Registros de hardware (física `0x0C000000`) | Tabla de manejadores por páginas de 256 bytes |
| `0xC8000000` | Acceso directo al EFB | Todavía no: lee 0, ignora escrituras, se cuenta |
| `0xE0000000`–`0xE0003FFF` | Caché bloqueada, 16 KB | Búfer aparte |
| — | ARAM, 16 MB | Búfer aparte; sin dirección, la usa el DMA del DSP |
| Cualquier otra | — | Lee 0, ignora escrituras, se cuenta en `gc_mem_t.bad`; las primeras 16 van al log |

## Registros de hardware

| Bloque | Desplazamiento | Para qué |
| --- | --- | --- |
| CP | `0x0000` | Command Processor (FIFO de GX) |
| PE | `0x1000` | Pixel Engine |
| VI | `0x2000` | Video |
| PI | `0x3000` | Interrupciones y reset |
| MI | `0x4000` | Protección de memoria |
| DSP | `0x5000` | DSP, ARAM y DMA de audio |
| DI | `0x6000` | Lector de disco |
| SI | `0x6400` | Mandos |
| EXI | `0x6800` | Tarjetas de memoria, RTC, IPL |
| AI | `0x6C00` | Audio |
| GX FIFO | `0x8000` | Write gather pipe |

Cada bloque registra un manejador con `gc_mmio_register()`; recibe el desplazamiento dentro del bloque y el tamaño (1, 2 o 4 bytes). Un acceso de 64 bits son dos de 32, la palabra alta primero. Un acceso sin manejador lee 0, se cuenta en `gc_mmio_t.unmapped` y las primeras 16 van al log.

## Por qué así

- **Máscara en vez de comprobar límites:** la RAM se direcciona con `& 0x01FFFFFF` sobre un búfer de 32 MB, así que un juego que lee fuera de sus 24 MB no tumba el emulador. El JIT podrá usar la misma base y la misma máscara para los accesos que sabe que son RAM; todo lo demás va por el camino lento (`gc_read32`...).
- **Sin fallos de página:** lv2 no deja recibir la notificación de un fallo (D-008), así que nada depende de proteger páginas.
- **Big-endian en los dos lados:** en la PS3 los accesos son copias directas; en la PC se invierten bytes. `make test-be` corre las mismas pruebas en PowerPC de 64 bits con QEMU para cubrir el camino de la consola.
- **Fuera de alcance por ahora:** juegos que cambian el mapa con la MMU (tablas de páginas, por ejemplo Rogue Squadron) y el acceso directo al EFB.
