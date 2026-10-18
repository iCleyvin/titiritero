#!/usr/bin/env python3
"""Mete el panel web dentro del plugin: inserta cripto.js en panel.html, comprime con gzip y escribe
src/panel_web.h con los bytes. Lo llama compilar.sh antes de compilar."""
import gzip
from pathlib import Path

WEB = Path(__file__).resolve().parent
SALIDA = WEB.parent / "src" / "panel_web.h"


def comprimido(datos: bytes) -> bytes:
    return gzip.compress(datos, compresslevel=9, mtime=0)   # mtime=0: misma entrada, mismos bytes


def arreglo(nombre: str, datos: bytes) -> str:
    filas = [", ".join(f"0x{b:02x}" for b in datos[i:i + 20]) for i in range(0, len(datos), 20)]
    return f"static const uint8_t {nombre}[] = {{\n    " + ",\n    ".join(filas) + "\n};\n"


def main():
    pagina = (WEB / "panel.html").read_text(encoding="utf-8")
    cripto = (WEB / "cripto.js").read_text(encoding="utf-8")
    if "/*CRIPTO*/" not in pagina:
        raise SystemExit("panel.html no tiene el hueco /*CRIPTO*/")
    pagina = pagina.replace("/*CRIPTO*/", cripto)
    piezas = {
        "PANEL_HTML_GZ": comprimido(pagina.encode("utf-8")),
        "ICONO_SVG_GZ": comprimido((WEB / "icono.svg").read_bytes()),
        "MANIFIESTO_GZ": comprimido((WEB / "manifest.webmanifest").read_bytes()),
    }
    cuerpo = "".join(arreglo(n, d) for n, d in piezas.items())
    SALIDA.write_text("#pragma once\n/* Generado por web/empaquetar.py: no editar a mano. */\n#include <cstdint>\n\n" + cuerpo)
    print(f"{SALIDA.name}: " + ", ".join(f"{n} {len(d)} B" for n, d in piezas.items()))


if __name__ == "__main__":
    main()
