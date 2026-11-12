#!/bin/bash
# usage: stop.sh   stops the harness, its command-fifo holder and a host started by run_bridge.sh (pid files only)
cd "$(dirname "$0")"
for f in host.pid harness.pid fifo.pid; do
    [ -f $f ] && kill "$(cat $f)" 2>/dev/null && echo "stopped $f ($(cat $f))"
    rm -f $f
done
