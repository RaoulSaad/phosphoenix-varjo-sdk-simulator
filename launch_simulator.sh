#!/usr/bin/env sh
# POSIX twin of "Launch Simulator.bat". Adjust ENV_PY if your env lives elsewhere.
ENV_PY="${ENV_PY:-$HOME/miniconda3/envs/phos-rtcv/bin/python}"
[ -x "$ENV_PY" ] || ENV_PY=python
cd "$(dirname "$0")" && exec "$ENV_PY" launcher/app.py
