# TapTalk Keyboard

**Hold a button. Speak. Let go.** TapTalk turns the final transcript into USB
keystrokes. To the connected host, it is simply a standard USB keyboard.

### [→ Install it from your browser](https://jrfferreira.github.io/taptalk-keyboard/)

No toolchain, no command line. Chrome or Edge, a USB-C data cable, about a
minute. After setup, there is no driver, desktop app, browser tab, or companion
software on the computer: plug it into any host that accepts a standard USB
keyboard and it types there. Configure Wi-Fi and your own transcription endpoint
(plus an API key when it needs one) on the device.

---

## The board

Firmware for the **Waveshare ESP32-S3-Touch-AMOLED-1.8** — a 1.8" round-cornered
AMOLED with touch, a microphone, and USB wired straight to the ESP32-S3.

> [!IMPORTANT]
> **Two incompatible boards ship under this one name.** Check the sticker on the
> back before you buy or flash:
>
> | | Display | Touch | This firmware |
> |---|---|---|---|
> | **V1** | **SH8601** | **FT3168** | ✅ supported |
> | V2 | CO5300 | CST816 | ❌ blank screen |
>
> Read [docs/hardware-v1.md](docs/hardware-v1.md) before doing anything else.

[**Buy the V1 on AliExpress**](https://s.click.aliexpress.com/e/_c349By9V) —
affiliate link. It costs you nothing extra and sends a small commission my way.
Confirm the listing says `SH8601` / `FT3168`; sellers relist V2 under the same
title.

## How it works

```
hold the on-screen button   →  16 kHz mono WAV into PSRAM
release                     →  multipart HTTP(S) POST to a transcription endpoint
transcript comes back       →  typed as USB HID keystrokes
```

It types only after the final transcript arrives. Streaming speech-to-text
revises its guesses, and a keyboard cannot take a word back without a volley of
backspaces.

## Your transcription, your choice

TapTalk is not tied to an account or a hosted service. Its setup screen accepts
any OpenAI-compatible `POST /v1/audio/transcriptions` endpoint, model name, an
optional language, and an optional bearer key.

- Use the included OpenAI default with your own key.
- Point it at a compatible provider.
- Point it at your own server on your LAN; a local server can omit the key.

The ESP32 records and sends the audio, then behaves as a keyboard. It does not
run a general-purpose speech-to-text model itself. See
[docs/custom-endpoints.md](docs/custom-endpoints.md) for the exact request
format and local-network security guidance.

## Setup

No credentials are compiled in. On first boot the device has no stored SSID, so
it raises its own Wi-Fi network and shows a QR code.

1. Scan the QR code, or join `TapTalk-XXXX` with the eight-digit password on
   screen. That password is regenerated from the hardware RNG every boot.
2. A setup page opens by itself. Enter your Wi-Fi, transcription endpoint,
   model, the keyboard layout your computer is set to, and (if required) an
   API key.
3. The device saves the settings and restarts.

Tap the **cog** on the main screen to change them later, or to erase them.

> [!WARNING]
> The endpoint and API key are stored **unencrypted**. Sixty seconds with the
> board and a USB cable is enough to read them back. Use a dedicated, limited
> key where your service requires one, and erase it before the board leaves your
> hands.
> [docs/security.md](docs/security.md) explains the trade-off rather than
> pretending it away.

## Status

Confirmed on hardware, end to end: **hold → speak → release → the transcript is
typed into the host.**

| | |
|---|---|
| Display, touch, LVGL UI | ✅ on hardware |
| PMIC, Wi-Fi, SNTP, microphone capture | ✅ on hardware |
| Setup portal (SoftAP + captive portal) | ✅ on hardware |
| Transcription over HTTPS (multipart, OpenAI) | ✅ on hardware |
| Configurable OpenAI-compatible endpoint | ✅ implemented; hardware validation pending |
| USB HID typing, composite with a CDC console | ✅ on hardware |
| Browser installer | ✅ works |
| Driver or companion software on the host | ✅ none required |
| Confirmation beeps, diagnostics heartbeat | ⚠️ shipped, not yet re-tested on the board |
| Selectable keyboard layouts (US, ABNT2, Portuguese) | ✅ implemented; hardware validation pending |

The setup page asks which layout the *computer* is set to — the host decodes
our keystrokes through its own layout, so the two must match. On the Portuguese
layouts accents are composed with dead keys and `ação` types as written; on US
it falls back to the bare letters and arrives as `acao`. Emoji are skipped
rather than mangled. The layout tables are transcribed from Microsoft's KBDBR
and KBDPO definitions; AltGr symbols assume a Windows or Linux host, since
macOS arranges its Option combinations differently. Adding another language is
one table in `components/core/` plus a `<option>` in the portal.

## Develop

### Build

Needs Docker. There is no ESP-IDF on the host, and no credentials are compiled
in.

```sh
tools/idf.sh build
```

### Test

Needs only a C compiler and Python 3. Runs in about a second.

```sh
cd test/host && make test        # 13,061 checks across 10 suites
```

`components/core/` deliberately includes no `esp_*` headers, which is what lets
the same sources compile natively. `interop.py` re-checks the generated WAV and
multipart body with Python's `wave` and `email` modules — parsers that did not
write the bytes.

### Flash

The browser installer is the easy path. From a terminal, note that Docker on
macOS cannot see the USB port, so building and flashing are separate steps:

```sh
python3 -m pip install --user esptool esp-idf-monitor   # once
tools/flash.sh
```

> [!NOTE]
> **This board has no RESET button** — `CHIP_PU` is driven by the AXP2101. And
> once the firmware runs, it owns the USB port as a keyboard, so nothing can
> reset it into the bootloader on its own. To flash: **hold BOOT, tap PWR,
> release BOOT.**

## Layout

```
components/core/   pure C, no esp_* headers, host-testable
  sm.c             transition function: (state, event, guards) -> (next, actions)
  keymap*.c        codepoint -> HID keystroke sequence (dead keys need two)
  typeplan.c       press/release framing; "aa" is not one keypress
  textnorm.c       UTF-8, de-accenting, whitespace (a stray '\n' would press Enter)
  formdec.c        setup-portal form parsing; untrusted input
  dnsreply.c       captive-portal DNS; untrusted input, fuzzed
  wav.c  multipart.c  jsonesc.c

main/              hardware. app_sm.c runs the actions sm.c returns.
  pmic.c           AXP2101; the BSP never touches it
  config_store.c   Wi-Fi and transcription settings in NVS, plaintext (see docs/security.md)
  provisioning.c   SoftAP + captive portal + form
  audio_capture.c  16 kHz mono into one static PSRAM clip
  stt_client.c     streams multipart straight out of PSRAM; never copies the WAV
  hid_kbd.c        composite CDC + HID, so installing HID does not blind the console
  beeper.c         press/release tones; the board has no haptics
  diagnostics.c    heartbeat: names whichever task has wedged

test/host/         Makefile + a C compiler; no cmake needed
tools/             idf.sh (docker build), flash.sh (native esptool), release.sh
web/               the installer page, published to GitHub Pages
```

## Docs

- [docs/hardware-v1.md](docs/hardware-v1.md) — V1 vs V2, the pin map, the boot
  order, and the two ways this board bites during bring-up
- [docs/security.md](docs/security.md) — where the key lives, who can read it,
  and the threat model stated plainly

## License

Original source is [CC0](LICENSE) — public domain. Binaries built from this repo
statically link Apache-2.0 and MIT components; see [NOTICE](NOTICE).
