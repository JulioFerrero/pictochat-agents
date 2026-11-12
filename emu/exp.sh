#!/bin/bash
# usage: FPS=n exp.sh TAG [host args]  -> joins, then reports how long the client stays and the emulation speed
export LC_ALL=C
cd "$(dirname "$0")"
TAG=$1; shift
echo "starting the emulator and the host, waiting for the join (about 45 s; live log: emu/$TAG.log) ..."
./run_bridge.sh "$TAG" --gui-port ${GUI_PORT:-7790} "$@"
for i in $(seq 1 40); do sleep 2; grep -aq "joined" $TAG.log 2>/dev/null && break; done
if ! grep -aq "joined" $TAG.log; then echo "$TAG fps=${FPS:-60}: no join within 80 s"; exit; fi
f0=$(./ctl.sh frames; sleep 0.5; tail -1 harness.out | awk '{print $4}'); t0=$(date +%s.%N)
sleep 15
f1=$(./ctl.sh frames; sleep 0.5; tail -1 harness.out | awk '{print $4}'); t1=$(date +%s.%N)
jt=$(grep -a -m1 "joined" $TAG.log | awk '{print $1}')
dt=$(LC_ALL=C awk -v j=$jt '/deauthenticated/ && $1>j {print $1-j; exit}' $TAG.log)
echo "$TAG fps=${FPS:-60}: joined at ${jt}s, left after ${dt:-'>15 (still in)'} s; emulated $(awk -v a=$f0 -v b=$f1 -v x=$t0 -v y=$t1 'BEGIN{printf "%.1f", (b-a)/(y-x)}') frames per real second"
