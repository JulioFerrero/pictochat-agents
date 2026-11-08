#!/bin/bash
# usage: ctl.sh "cmd" ...   (sends commands to the harness fifo; never blocks for long)
cd "$(dirname "$0")"
for c in "$@"; do timeout 3 bash -c 'echo "$1" > cmd.fifo' _ "$c"; done
