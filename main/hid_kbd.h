/* USB HID keyboard, on a composite device that also carries a CDC console.
 *
 * Installing TinyUSB hands the USB PHY from the USB-Serial-JTAG controller to
 * the OTG controller, which kills the serial console. Rather than lose it, we
 * enumerate as CDC + HID and redirect the log to the CDC interface. The cost
 * is that esptool can no longer reset the board into the bootloader over USB:
 * reflashing means holding BOOT and tapping PWR. See docs/hardware-v1.md. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Installs TinyUSB and starts the typing task. Call once, late in boot: it
 * takes the console with it. */
esp_err_t hid_kbd_start(void);

/* Selects the keymap transcripts are typed with, by its stored settings name
 * ("us", "abnt2", ...). Call before hid_kbd_start(); an unknown name falls
 * back to US rather than fail. */
void hid_kbd_set_layout(const char *name);

/* True once a host has enumerated us. There is no point recording with nobody
 * to type into, so this gates the record button. */
bool hid_kbd_mounted(void);

/* Types `utf8` on the typing task and posts EV_TYPE_DONE, or EV_TYPE_ABORT if
 * the host went away mid-string. Safe to call from the state machine task. */
esp_err_t hid_kbd_type(const char *utf8);

/* Strikes one chord (hold `mod`, tap `key`, release) on the typing task and
 * reports back with the same EV_TYPE_DONE / EV_TYPE_ABORT events. This is how
 * Send submits a dictation -- typically Enter, or a modifier + Enter. */
esp_err_t hid_kbd_send(uint8_t mod, uint8_t key);

/* Backspaces away exactly what the last hid_kbd_type() put on the host: one
 * Backspace per character that advanced the cursor, including any trailing
 * space. Reports back with EV_TYPE_DONE / EV_TYPE_ABORT. A no-op (still posts
 * EV_TYPE_DONE) if nothing has been typed since boot. */
esp_err_t hid_kbd_undo(void);

void hid_kbd_abort(void);
