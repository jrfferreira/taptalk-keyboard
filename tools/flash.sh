#!/usr/bin/env bash
# Flash the container-built binaries from the host, because Docker on macOS
# cannot reach the USB serial port.
#
# Prerequisite (once):   python3 -m pip install --user esptool esp-idf-monitor
#
#   tools/flash.sh                 # auto-detect the port
#   tools/flash.sh /dev/cu.usbmodem101
#
# If no port appears, the board is not in download mode. This board has NO
# RESET button — CHIP_PU is driven by the AXP2101. To force the ROM bootloader:
#   hold BOOT, tap PWR (or unplug/replug USB-C), release BOOT.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${REPO_ROOT}/build"

command -v esptool.py >/dev/null 2>&1 || python3 -m esptool version >/dev/null 2>&1 || {
  echo "error: esptool not found. Run: python3 -m pip install --user esptool" >&2
  exit 1
}
ESPTOOL=$(command -v esptool.py >/dev/null 2>&1 && echo "esptool.py" || echo "python3 -m esptool")

for f in bootloader/bootloader.bin partition_table/partition-table.bin ota_data_initial.bin taptalk-keyboard.bin; do
  [ -f "${BUILD}/${f}" ] || { echo "error: ${BUILD}/${f} missing. Run tools/idf.sh build first." >&2; exit 1; }
done

PORT="${1:-}"
if [ -z "${PORT}" ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  [ -n "${PORT}" ] || { echo "error: no /dev/cu.usbmodem* found. Hold BOOT, tap PWR, retry." >&2; exit 1; }
fi
echo "flashing via ${PORT}"

# --after watchdog-reset re-samples the strapping pins. A plain USB-Serial-JTAG
# reset is only a core reset, which can leave the chip stuck in download mode.
${ESPTOOL} --chip esp32s3 --port "${PORT}" --baud 921600 \
  --before default-reset --after watchdog-reset \
  write-flash -z \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0     "${BUILD}/bootloader/bootloader.bin" \
  0x8000  "${BUILD}/partition_table/partition-table.bin" \
  0xf000  "${BUILD}/ota_data_initial.bin" \
  0x20000 "${BUILD}/taptalk-keyboard.bin"

echo
echo "flashed. monitor with:  python3 -m esp_idf_monitor --port ${PORT}"
