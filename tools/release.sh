#!/usr/bin/env bash
# Reproduce the CI release image locally, into web/firmware/.
#
# CI is the source of truth (.github/workflows/deploy-pages.yml); this exists so
# you can inspect the exact bytes before they are published.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION="0.2.0"
OUT="web/firmware/taptalk-v${VERSION}-esp32s3-v1-merged.bin"

if [ -f main/secrets.h ]; then
  echo "error: main/secrets.h is present. A binary built from this tree would embed" >&2
  echo "       your Wi-Fi password and API key. Move it aside first." >&2
  exit 1
fi

tools/idf.sh build

mkdir -p web/firmware
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "${REPO_ROOT}:/project" -w /project espressif/idf:v5.5 bash -lc "
esptool.py --chip esp32s3 merge_bin \
  -o ${OUT} \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xf000  build/ota_data_initial.bin \
  0x20000 build/taptalk-keyboard.bin"

echo
ls -l "${OUT}"
echo "flash at offset 0x0; ESP Web Tools reads web/manifest.json"
