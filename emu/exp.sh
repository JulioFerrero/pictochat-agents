#!/bin/bash
# usage: FPS=n exp.sh TAG [host args]  -> joins, then reports how long the client stays and the emulation speed
export LC_ALL=C
cd "$(dirname "$0")"
TAG=$1; shift
ps -eo pid,user,args | grep "[p]ictochat.py ho" | grep " $(id -un) " | awk "{print \$1}" | xargs -r kill 2>/dev/null
./run_bridge.sh "$TAG" --gui-port 7790 "$@"
for i in $(seq 1 40); do sleep 2; grep -q "joined" $TAG.log 2>/dev/null && break; done
if ! grep -q "joined" $TAG.log; then echo "$TAG fps=${FPS:-60}: no join within 80 s"; exit; fi
f0=$(./ctl.sh frames; sleep 0.5; tail -1 harness.out | awk '{print $4}'); t0=$(date +%s.%N)
sleep 15
f1=$(./ctl.sh frames; sleep 0.5; tail -1 harness.out | awk '{print $4}'); t1=$(date +%s.%N)
jt=$(grep -m1 "joined" $TAG.log | awk '{print $1}')
dt=$(awk -v j=$jt '/deauthenticated/ && $1>j {print $1-j; exit}' $TAG.log)
echo "$TAG fps=${FPS:-60}: joined at ${jt}s, left after ${dt:-'>15 (still in)'} s; emulated $(awk -v a=$f0 -v b=$f1 -v x=$t0 -v y=$t1 'BEGIN{printf "%.1f", (b-a)/(y-x)}') frames per real second"
