#!/bin/bash
# usage: start.sh OUT.pcap N [BRIDGE_PORT]   (detached harness + command fifo holder)
cd "$(dirname "$0")"
pgrep -x harness | xargs -r kill 2>/dev/null
pgrep -x sleep | xargs -r kill 2>/dev/null
sleep 0.5
rm -f cmd.fifo; mkfifo cmd.fifo
setsid nohup sleep 1000000 > cmd.fifo 2>/dev/null < /dev/null &
setsid nohup env ${NOJIT:+NOJIT=1} ./harness fw/bios7.bin fw/bios9.bin fw/firmware.bin "$1" "$2" ${3:+$3} < cmd.fifo > harness.out 2> harness.err &
sleep 1
