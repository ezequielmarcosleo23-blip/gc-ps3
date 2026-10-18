/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Carpetas de datos en la PS3.
 *
 * Nada va en /dev_hdd0/tmp: RPCS3 la vacía cada vez que arranca un programa
 * (lo documentó Salsa64 y lo confirmó la prueba iso de la Fase 0), así que
 * un archivo de prueba, la configuración del log o la marca de crash del JIT
 * no sobrevivirían de una ejecución a otra.
 */
#ifndef GCPS3_PLATFORM_PS3_PATHS_H
#define GCPS3_PLATFORM_PS3_PATHS_H

#define GCPS3_DATA_DIR "/dev_hdd0/gcps3"   /* en RPCS3: <carpeta de RPCS3>/dev_hdd0/gcps3 */
#define GCPS3_USB_DIR  "/dev_usb000/gcps3"

#endif
