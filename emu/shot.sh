#!/bin/bash
cd "$(dirname "$0")"
timeout 3 bash -c 'echo "shot $1 $2.ppm" > cmd.fifo' _ "$1" "$2"; sleep 0.6
python3 -I ./ppm2png.py $2.ppm $2.png
