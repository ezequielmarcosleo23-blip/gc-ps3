#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Receptor del log UDP de gc-ps3 (consola real o RPCS3).

Cada datagrama es una línea con el formato de src/core/logline.h:

    GCPS3 <sesión:8 hex> <secuencia> <t_us> <nivel> <prueba> | <mensaje>

Guarda cada ejecución (sesión) en logs/AAAAMMDD-HHMMSS_<prueba>_<sesión>.log,
descarta duplicados (la consola envía a broadcast y a 127.0.0.1) y cuenta las
líneas perdidas por los huecos en la secuencia. Las líneas de nivel D (detalle)
se guardan en el archivo sin mostrarlas; se muestra solo cuántas llegaron.

    python3 tools/logrecv.py                     # escucha hasta Ctrl+C
    python3 tools/logrecv.py --until-result      # termina con el RESULT: 0 OK, 1 FAIL, 2 tiempo agotado
    python3 tools/logrecv.py --selftest          # prueba interna (la corre `make test`)
"""
import argparse
import datetime
import os
import re
import socket
import sys
import tempfile
import time

PORT = 18194
LINE_RE = re.compile(r"^GCPS3 ([0-9a-f]{8}) (\d+) (\d+) ([IWERD]) ([a-z0-9_-]{1,31}) \| (.*)$", re.S)
RCVBUF = 8 << 20   # las ráfagas sin pausa llegan más rápido de lo que se escriben
# La misma línea de referencia la prueba tests/unit/test_logline.c.
FIXTURE = "GCPS3 0000beef 7 1500 I hello | hola mundo"


def parse(line):
    """Devuelve un dict con los campos de la línea, o None si no es válida."""
    m = LINE_RE.match(line)
    if not m:
        return None
    seq, t_us = int(m.group(2)), int(m.group(3))
    if seq > 0xFFFFFFFF or t_us > 0xFFFFFFFFFFFFFFFF:
        return None
    return {"session": m.group(1), "seq": seq, "t_us": t_us, "level": m.group(4),
            "test": m.group(5), "msg": m.group(6)}


class Session:
    def __init__(self, rec, out_dir):
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.test = rec["test"]
        self.path = os.path.join(out_dir, f"{stamp}_{rec['test']}_{rec['session']}.log")
        self.file = open(self.path, "a", encoding="utf-8")
        self.seen = set()
        self.max_seq = -1
        self.dups = 0
        self.result = None
        self.hidden = 0      # líneas D guardadas sin mostrar desde el último aviso

    @property
    def received(self):
        return len(self.seen)

    @property
    def lost(self):
        return self.max_seq + 1 - len(self.seen)

    def summary(self):
        return (f"{self.test}: {self.result or 'sin RESULT'} | {self.received} lineas, "
                f"{self.lost} perdidas, {self.dups} duplicadas -> {self.path}")


class Receiver:
    def __init__(self, out_dir, echo=True):
        self.out_dir = out_dir
        self.echo = echo
        self.sessions = {}
        os.makedirs(out_dir, exist_ok=True)

    def idle(self):
        """Sin datos por un momento: vuelca archivos y avisa de las líneas D ocultas."""
        for s in self.sessions.values():
            s.file.flush()
            self._report_hidden(s)

    def _report_hidden(self, s):
        if s.hidden and self.echo:
            print(f"[{s.test}] ... {s.hidden} lineas de detalle (D) guardadas en el archivo", flush=True)
        s.hidden = 0

    def handle(self, data, addr=None):
        """Procesa un datagrama; devuelve la sesión si trajo su RESULT."""
        text = data.decode("utf-8", "replace").rstrip("\r\n")
        rec = parse(text)
        if rec is None:
            if self.echo:
                print(f"[?] {addr[0] if addr else '-'}: {text}", flush=True)
            return None
        s = self.sessions.get(rec["session"])
        if s is None:
            s = self.sessions[rec["session"]] = Session(rec, self.out_dir)
            if self.echo:
                origin = f" desde {addr[0]}" if addr else ""
                print(f"== nueva ejecucion de '{s.test}'{origin} (sesion {rec['session']})", flush=True)
        if rec["seq"] in s.seen:
            s.dups += 1
            return None
        s.seen.add(rec["seq"])
        s.max_seq = max(s.max_seq, rec["seq"])
        s.file.write(text + "\n")
        if rec["level"] == "D":
            s.hidden += 1
            return None
        s.file.flush()
        self._report_hidden(s)
        if self.echo:
            mark = {"W": "AVISO ", "E": "ERROR ", "R": ""}.get(rec["level"], "")
            print(f"[{s.test} {rec['t_us'] / 1e6:8.3f}s] {mark}{rec['msg']}", flush=True)
        if rec["level"] == "R":
            s.result = rec["msg"]
            if self.echo:
                print("== " + s.summary(), flush=True)
            return s
        return None

    def close(self):
        for s in self.sessions.values():
            s.file.close()


def serve(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RCVBUF)
    except OSError:
        pass
    sock.bind((args.bind, args.port))
    sock.settimeout(0.5)
    rx = Receiver(args.out)
    print(f"Escuchando el log de gc-ps3 en UDP {args.bind}:{args.port}; logs en {args.out}"
          + ("" if args.until_result else " (Ctrl+C para salir)"), flush=True)
    deadline = time.monotonic() + args.timeout
    try:
        while True:
            if args.until_result and time.monotonic() > deadline:
                print(f"Tiempo agotado: no llego ningun RESULT en {args.timeout} s", flush=True)
                return 2
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                rx.idle()
                continue
            done = rx.handle(data, addr)
            if done and args.until_result:
                return 0 if done.result == "RESULT: OK" else 1
    except KeyboardInterrupt:
        print("", flush=True)
        for s in rx.sessions.values():
            print("== " + s.summary(), flush=True)
        return 0
    finally:
        rx.close()
        sock.close()


def selftest():
    def check(cond, what):
        if not cond:
            raise SystemExit(f"logrecv selftest: FALLO: {what}")

    r = parse(FIXTURE)
    check(r == {"session": "0000beef", "seq": 7, "t_us": 1500, "level": "I",
                "test": "hello", "msg": "hola mundo"}, "linea de referencia")
    check(parse("GCPS3 0000beef 9 2000 D udplog | r1 9/10000")["level"] == "D", "nivel D")
    for bad in ["hola", "GCPS3 beef 7 1500 I hello | m", "GCPS3 0000beef 7 1500 Q hello | m",
                "GCPS3 0000beef 7 1500 I hello m", "GCPS3 0000beef 99999999999 1500 I hello | m",
                "GCPS3 0000beef 7 1500 I Hola | m"]:
        check(parse(bad) is None, f"rechazar {bad!r}")

    # Envío real por UDP a 127.0.0.1: 0, 1, 1 (duplicada), 3 (falta la 2) y el RESULT.
    with tempfile.TemporaryDirectory() as tmp:
        rxsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rxsock.bind(("127.0.0.1", 0))
        rxsock.settimeout(2)
        port = rxsock.getsockname()[1]
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        lines = ["GCPS3 0000cafe 0 10 I hello | uno", "GCPS3 0000cafe 1 20 I hello | dos",
                 "GCPS3 0000cafe 1 20 I hello | dos", "GCPS3 0000cafe 3 40 W hello | cuatro",
                 "basura que no es del log", "GCPS3 0000cafe 4 45 D hello | detalle",
                 "GCPS3 0000cafe 5 50 R hello | RESULT: OK"]
        for line in lines:
            tx.sendto(line.encode(), ("127.0.0.1", port))
        rx = Receiver(tmp, echo=False)
        done = None
        while done is None:
            data, addr = rxsock.recvfrom(4096)
            done = rx.handle(data, addr)
        rx.close()
        check(done.result == "RESULT: OK", "RESULT recibido")
        check((done.received, done.lost, done.dups) == (5, 1, 1),
              f"conteo recibidas/perdidas/duplicadas = {(done.received, done.lost, done.dups)}")
        with open(done.path, encoding="utf-8") as f:
            saved = f.read().splitlines()
        check(len(saved) == 5 and saved[3].endswith("| detalle"), "archivo de log con 5 lineas, la D incluida")
        tx.close()
        rxsock.close()
    print("logrecv selftest: OK")
    return 0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Receptor del log UDP de gc-ps3")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(here), "logs"))
    ap.add_argument("--until-result", action="store_true", help="termina al llegar el RESULT")
    ap.add_argument("--timeout", type=float, default=120, help="segundos de espera con --until-result")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    return selftest() if args.selftest else serve(args)


if __name__ == "__main__":
    sys.exit(main())
