# TapTalk Keyboard

Hold a button, speak, release. The device transcribes your speech and types it
into whatever computer it is plugged into, as a USB keyboard.

Firmware for the **Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1)**. Read
[docs/hardware-v1.md](docs/hardware-v1.md) before doing anything else — there
are two incompatible board revisions sold under this name.

## Status: chunk 1 — board bring-up

| | |
|---|---|
| Pure logic (WAV, keymap, multipart, form + DNS parsers, state machine) | done, unit + interop tested |
| Wi-Fi / API-key setup via SoftAP captive portal | written, **compiles**, unverified on hardware |
| PMIC, display, touch, LVGL, Wi-Fi, SNTP, mic capture | written, **compiles**, unverified on hardware |
| STT upload | chunk 2 |
| USB HID keyboard | chunk 2 |
| Web flasher | later |

Nothing here has run on a physical board yet. The build is green and the
host tests pass; that is all that is currently claimed.

## Build

Needs Docker. There is no ESP-IDF on the host. No credentials are compiled in.

```sh
tools/idf.sh build
```

Optionally, `cp main/secrets.example.h main/secrets.h` and fill it in to skip
the setup portal on a bench build. That file is gitignored; read
[docs/security.md](docs/security.md) before using it.

## Test

Needs only clang and Python 3. Runs in about a second.

```sh
cd test/host && make test
```

`components/core/` deliberately includes no `esp_*` headers, which is what lets
the same sources compile natively. `interop.py` re-checks the generated WAV and
multipart body with Python's `wave` and `email` parsers — parsers that did not
write the bytes.

## Flash

Docker on macOS cannot see the USB port, so building and flashing are separate.

```sh
python3 -m pip install --user esptool esp-idf-monitor   # once
tools/flash.sh
```

If no serial port appears, the board is not in download mode. **This board has
no RESET button.** Hold **BOOT**, tap **PWR**, release BOOT.

## Setup

No credentials are baked into the firmware. On first boot the device has no
stored SSID, so it raises a WPA2 access point and shows a QR code:

1. Scan the QR code with your phone, or join `TapTalk-XXXX` using the
   eight-digit password on screen. The password is regenerated every boot.
2. A setup page opens by itself. Enter your Wi-Fi and paste your OpenAI key.
3. The device saves both to NVS and restarts.

Tap **Setup** on the main screen to change them later, or to erase them.

The key is stored **unencrypted**. Anyone holding the board and a USB cable can
read it. That is a deliberate trade-off, explained in
[docs/security.md](docs/security.md).

## What chunk 1 is for

One question: **does the AXP2101 actually power the microphone?**

The BSP never configures the PMIC, but the schematic says its ALDO1 rail feeds
the ES8311's analog supply. If the power-on defaults leave that rail off, the
codec still enumerates and reads still succeed — they just return silence.

So the screen shows a live microphone level bar. Speak at the board:

- **Bar moves** → the rail is up, and chunks 2+ can be built on solid ground.
- **Bar is flat** → `main/pmic.c` needs the schematic's real LDO map, and we
  found out after one flash instead of after three thousand lines of code.

The status line also reports Wi-Fi, whether SNTP synced the clock (without it
every TLS handshake fails, since certificates are validated against a clock
that reads 1970 at boot), and the AXP2101's chip ID and ALDO1 state.

## Layout

```
components/core/   pure C, no esp_* headers, host-testable
  sm.c             transition function: (state, event, guards) -> (next, actions)
  keymap*.c        codepoint -> HID keystroke *sequence* (dead keys need two)
  formdec.c        form parsing; untrusted input from the setup portal
  dnsreply.c       captive-portal DNS; untrusted input, fuzzed
  wav.c multipart.c textnorm.c
main/              hardware. app_sm.c runs the actions sm.c returns.
  pmic.c           AXP2101; the BSP does not touch it
  config_store.c   credentials in NVS, plaintext (see docs/security.md)
  provisioning.c   SoftAP + captive portal + form
  audio_capture.c  16 kHz mono into one static PSRAM clip
test/host/         Makefile + clang; no cmake on the dev machine
tools/             idf.sh (docker build), flash.sh (native esptool)
```
