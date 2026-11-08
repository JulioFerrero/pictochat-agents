#!/bin/bash
# Builds the headless two-DS test bed in this folder:
#   - clones melonDS (GPLv3) at the commit used during development and builds only its core library
#   - compiles harness.cpp against it
#   - unpacks bios7.bin / bios9.bin / firmware.bin from the BIOS zip into fw/ (not shipped here: copyrighted)
# usage: ./build.sh [path/to/"NDS BIOS & Firmware.zip"]
set -e
cd "$(dirname "$0")"
ZIP=${1:-"$HOME/Downloads/NDS BIOS & Firmware.zip"}
COMMIT=497aa6cdd26f45f8064b78916b00a773eab4ded7
if [ ! -d melonDS ]; then
    git clone https://github.com/melonDS-emu/melonDS.git
    git -C melonDS checkout -q $COMMIT
fi
cmake -S melonDS -B melonDS/build -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF -DENABLE_GDBSTUB=OFF -DENABLE_LTO_RELEASE=OFF
cmake --build melonDS/build -j"$(nproc)"
M=melonDS
g++ -O2 -std=gnu++17 -fwrapv -Wno-invalid-offsetof -DARCHITECTURE_x86_64=1 -DJIT_ENABLED \
    -I$M/src -I$M/src/net -I$M/build/src harness.cpp $M/src/net/LocalMP.cpp \
    $M/build/src/libcore.a $M/build/src/teakra/src/libteakra.a -lpthread -lrt -o harness
mkdir -p fw
[ -f fw/firmware.bin ] || (cd fw && unzip -o -q "$ZIP" bios7.bin bios9.bin firmware.bin)
echo "built ./harness; first run: ./start.sh out.pcap 2, then ./wizard.sh after the first tap (see ../pictochat.md, section 8)"
