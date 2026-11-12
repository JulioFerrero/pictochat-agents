#!/bin/bash
# usage: run_bridge.sh TAG [extra host args...]   fresh harness + python host, client joins room A
cd "$(dirname "$0")"
TAG=$1; shift
[ -f host.pid ] && kill "$(cat host.pid)" 2>/dev/null; rm -f host.pid      # the host this script started last time
./start.sh mp_$TAG.pcap 1 7778
sleep 8; ./ctl.sh "touch 0 5 5 6"; sleep 4; ./ctl.sh "touch 0 80 72 8"; sleep 4; ./ctl.sh "fps ${FPS:-60}"
setsid nohup python3 ../pictochat.py host --bridge 7778 --channel 1 --out $PWD/$TAG --name pyhost "$@" > $TAG.out 2>&1 < /dev/null &
echo $! > host.pid
sleep 8; ./ctl.sh "touch 0 110 48 8"
