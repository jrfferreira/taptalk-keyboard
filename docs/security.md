# Security

Read this before you put a real API key on the device.

## The key is not protected at rest

The OpenAI API key is stored in **plaintext in NVS**, which lives in the
board's unencrypted SPI flash.

**Anyone with physical access to the board and a USB cable can read it:**

```sh
esptool --chip esp32s3 --port /dev/cu.usbmodem101 read-flash 0x9000 0x6000 nvs.bin
strings nvs.bin | grep sk-
```

That is not a bug. It is the consequence of a deliberate choice:

- ESP32-S3 flash encryption would protect the key, but it **burns eFuses
  irreversibly**. In Release mode it also stops plaintext reflashing, which is
  what the browser-based installer depends on, and it makes a bricked board
  unrecoverable.
- The alternative — a relay server holding the key, with the device carrying
  only a short-lived token — is the right answer for published firmware, and
  is on the roadmap. It is not the right answer for getting one device working
  on your desk this week.

### What follows from that

- **Treat the board as you would treat the key itself.** Do not lend it,
  leave it in a shared office, or sell it without erasing first.
- **Use "Erase stored credentials"** in the setup portal before the board
  leaves your hands. It clears the whole NVS namespace.
- **Never distribute a firmware binary built from a tree containing
  `main/secrets.h`.** That file is gitignored, and `config_store.c` only reads
  it to seed NVS on a development build, but a binary built with it has your
  credentials compiled in.
- **Scope the key.** Create a project-scoped OpenAI key used only by this
  device, with a spend limit, so a leak is bounded and revocable.

## The setup portal

When the device has no stored SSID it raises a Wi-Fi access point and serves a
form. Both your Wi-Fi password and your API key cross that link.

- The AP is **WPA2-PSK**, not open. The passphrase is **regenerated from the
  hardware RNG on every boot** and shown only on the device's screen. It is
  never stored and never reused.
- The form itself is served over **plain HTTP**, not HTTPS. A self-signed
  certificate on `192.168.4.1` produces a browser warning that trains users to
  click through warnings, which is worse than the thing it prevents. The WPA2
  link is the confidentiality boundary.
- At most **two clients** may associate.
- The portal is only reachable while the device is in setup mode. It does not
  run during normal operation.

### What the portal parses

Everything the portal reads is untrusted input from whatever joined the AP, so
the parsers are pure functions in `components/core/`, tested on the host rather
than trusted on the device:

| Code | Test | Guards against |
|---|---|---|
| `core/formdec.c` | `test_formdec.c` (66 checks) | buffer overflow; truncated `%` escapes reading past the body; `pass` matching `password` and swapping two credentials |
| `core/dnsreply.c` | `test_dnsreply.c` (8639 checks, incl. 4096 fuzz cases) | out-of-bounds writes; DNS compression-pointer loops; label lengths that run off the packet |

`form_get()` **refuses to truncate**. An over-long value is rejected with a 400
rather than silently clipped into a credential that then fails to authenticate
for reasons nobody can see.

## What is not logged

The API key is never written to the log. `config_mask_key()` renders it as
`sk-proj-…4a9f` for the status line and for `ESP_LOGI`. Serial console output
is readable by anyone with the board, so this matters.

The setup AP's own passphrase *is* logged — it is already displayed in 28-point
type on the front of the device.

The POST body is zeroed with `memset` immediately after parsing, since it holds
both the Wi-Fi password and the key in plaintext.

## Threat model, stated plainly

| Attacker | Outcome |
|---|---|
| On your Wi-Fi network | Cannot reach the key. The device runs no server in normal mode. |
| Near the device during setup | Must break WPA2 with a random per-boot passphrase they cannot see. |
| Holding the device for 60 seconds with a USB cable | **Has your API key.** |
| Holding the device, flash encryption enabled | Does not have the key. Not implemented; see above. |

## If a key leaks

Revoke it at <https://platform.openai.com/api-keys>. Then erase the device via
the setup portal and provision a new key. Nothing else on the device is
sensitive; the Wi-Fi password is the other thing worth rotating.
