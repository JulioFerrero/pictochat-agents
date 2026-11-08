#!/bin/bash
# runs the DS first-boot setup wizard on both emulated DSes (after the first tap that opens the nickname screen)
cd "$(dirname "$0")"
t(){ ./ctl.sh "$@"; sleep 1.4; }
t "touch 0 138 105 8"; t "touch 0 172 89 8"; t "touch 0 83 105 8"; t "touch 0 117 89 8"
t "touch 1 124 105 8"; t "touch 1 144 89 8"; t "touch 1 90 89 8"; t "touch 1 83 105 8"; t "touch 1 117 89 8"
t "touch 0 190 152 8" "touch 1 190 152 8"          # nickname confirm
t "touch 0 103 48 8" "touch 1 136 108 8"           # colors
t "touch 0 190 181 8" "touch 1 190 181 8"          # color confirm
t "touch 0 190 181 8" "touch 1 190 181 8"          # date
t "touch 0 190 181 8" "touch 1 190 181 8"          # time
t "touch 0 190 181 8" "touch 1 190 181 8"          # birthday
t "touch 0 170 181 8" "touch 1 170 181 8"          # restart prompt: Yes
sleep 2
./ctl.sh "reset 0" "reset 1"
