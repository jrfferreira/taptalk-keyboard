#!/usr/bin/env bash
# Run idf.py inside the pinned ESP-IDF container.
#
#   tools/idf.sh build
#   tools/idf.sh size
#   tools/idf.sh menuconfig
#
# Flashing is NOT possible through this script: Docker Desktop on macOS cannot
# pass a USB serial device into the container. Build here, flash with
# tools/flash.sh on the host.
set -euo pipefail

IMAGE="espressif/idf:v5.5"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "${1:-}" in
  flash|monitor|erase-flash|erase_flash)
    echo "error: '$1' needs the USB device, which Docker on macOS cannot see." >&2
    echo "       Build with 'tools/idf.sh build', then run 'tools/flash.sh'." >&2
    exit 2
    ;;
esac

exec docker run --rm -t \
  -v "${REPO_ROOT}:/project" \
  -w /project \
  -u "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  "${IMAGE}" \
  idf.py "$@"
