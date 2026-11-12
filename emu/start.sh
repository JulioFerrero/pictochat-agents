#!/bin/bash
# usage: start.sh OUT.pcap N [BRIDGE_PORT]   (detached harness + command fifo holder)
cd "$(dirname "$0")"
# stop only what this script started last time (pid files), never other processes
for f in harness.pid fifo.pid; do [ -f $f ] && kill "$(cat $f)" 2>/dev/null; rm -f $f; done
sleep 0.5
rm -f cmd.fifo; mkfifo cmd.fifo
setsid nohup sleep 1000000 > cmd.fifo 2>/dev/null < /dev/null &
echo $! > fifo.pid
setsid nohup env ${NOJIT:+NOJIT=1} ./harness fw/bios7.bin fw/bios9.bin fw/firmware.bin "$1" "$2" ${3:+$3} < cmd.fifo > harness.out 2> harness.err &
echo $! > harness.pid
sleep 1
