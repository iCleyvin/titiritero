#!/usr/bin/env bash
# Compila plugin/titiritero.wps con la imagen fijada del Dockerfile.
set -euo pipefail
cd "$(dirname "$0")"
python3 plugin/web/empaquetar.py
python3 plugin/web/pantalla_qr.py
docker build -q -t titiritero-build . >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD/plugin:/project" titiritero-build make "$@"
ls -la plugin/titiritero.wps
