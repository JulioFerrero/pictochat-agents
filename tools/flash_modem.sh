#!/bin/sh
# Build and flash the radio modem (esp32c3_modem/) onto the board on PORT, for whichever chip it is:
# - ESP32-C3 or ESP32-S3: the modem uses the chip's native USB port (USB Serial/JTAG), /dev/cu.usbmodem* on macOS;
# - classic ESP32 (e.g. the ESP32-2432S028 "Cheap Yellow Display"): the modem uses UART0 through the board's
#   USB-to-UART chip at 921600 baud, /dev/cu.usbserial* or /dev/cu.wchusbserial*.
# Usage: tools/flash_modem.sh [PORT]      (default: the first such port; ESP-IDF from $IDF_PATH or ~/esp/esp-idf-v5.4.1)
set -e
IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf-v5.4.1}"
# ESP-IDF 5.4 on macOS's Python 3.9 can't find ruamel.yaml's metadata, so prefer Homebrew's Python 3.13.
[ -d /opt/homebrew/opt/python@3.13/libexec/bin ] && PATH="/opt/homebrew/opt/python@3.13/libexec/bin:$PATH"
PORT="${1:-$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "No board found. Plug the ESP32 into the Mac (on two-port boards: the port marked USB)."; exit 1; }
. "$IDF_DIR/export.sh" >/dev/null

# Which chip answers on PORT -> the ESP-IDF target to build for.
chip=$(esptool.py --port "$PORT" chip_id 2>&1 | sed -n 's/^Chip is \(ESP32[-A-Z0-9]*\).*/\1/p' | head -1)
case "$chip" in
  ESP32-C3) target=esp32c3 ;;
  ESP32-S3) target=esp32s3 ;;
  ESP32-D0*|ESP32-PICO*|ESP32-U4*|ESP32-S0*) target=esp32 ;;
  "") echo "$PORT: no ESP32 answered. Hold BOOT, tap RESET, release BOOT, then run this again."; exit 1 ;;
  *) echo "$PORT is an $chip, which the modem doesn't support (ESP32, ESP32-C3 or ESP32-S3)."; exit 1 ;;
esac
# A C3 or S3 flashed through its USB-to-UART port would then talk on the native USB port: refuse it.
case "$target:$PORT" in
  esp32:*|*:*/cu.usbmodem*) ;;
  *) echo "$PORT is the $chip's USB-to-UART port. The modem talks over the native USB port: use the other connector."
     exit 1 ;;
esac

echo "$PORT: $chip -> building and flashing the modem for $target"
cd "$(dirname "$0")/../esp32c3_modem"
# One build folder and sdkconfig per target; set-target only the first time.
[ -f "sdkconfig.$target" ] || idf.py -B "build-$target" -D SDKCONFIG="sdkconfig.$target" set-target "$target"
idf.py -B "build-$target" -D SDKCONFIG="sdkconfig.$target" build flash -p "$PORT"
