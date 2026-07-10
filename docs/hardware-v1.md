# Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1)

## Check which board you have — this is not optional

Waveshare ships **two different boards under one product name**. Read the
sticker on the back:

| Revision | Display | Touch | BSP to use |
|---|---|---|---|
| **V1** | **SH8601** | **FT3168** | `waveshare/esp32_s3_touch_amoled_1_8: "^1.1.4"` |
| V2 | CO5300 | CST816 | `"^2.0.3"` |

This repo targets **V1**. `main/idf_component.yml` pins the `1.x` line on
purpose. BSP `2.x` depends on `esp_lcd_co5300` and drops `esp_lcd_sh8601`
entirely, so an unpinned dependency resolves to `2.x` and a V1 board boots to
a **black screen with no error in the log**.

Verify after any dependency change:

```sh
grep -A2 'esp_lcd_sh8601' dependencies.lock   # must be present
grep    'esp_lcd_co5300'  dependencies.lock   # must be absent
```

Waveshare's own examples in `waveshareteam/ESP32-S3-Touch-AMOLED-1.8` now pin
`^2.0.1`, i.e. they target V2. They are not a reliable reference for this board.

## What is actually on the board

From the sticker and the [schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf):

- **MCU** ESP32-S3R8 — 16 MB external flash (W25Q128), **8 MB in-package OCTAL PSRAM**
- **Display** SH8601, 368×448 AMOLED, QSPI
- **Touch** FT3168 on I²C, driven by the `esp_lcd_touch_ft5x06` component
- **Audio** ES8311 codec; **analog** microphone into its ADC; NS4150B amp + speaker
- **PMIC** AXP2101
- **IO expander** TCA9554 — owns `LCD_RESET`, `DSI_PWR_EN`, `TP_RESET`
- **IMU** QMI8658 · **RTC** PCF85063 · microSD
- **Buttons** BOOT and PWR. **There is no RESET button.**

### Pin map

Everything shares one I²C bus.

| Signal | GPIO |
|---|---|
| I²C SCL / SDA | 14 / 15 |
| I²S MCLK | 16 |
| I²S BCLK (SCLK) | 9 |
| I²S LRCK (WS) | 45 |
| I²S DOUT (S3→codec) | 8 |
| I²S DSIN (codec→S3) | 10 |
| Speaker amp enable | 46 |
| Touch INT | 21 |
| USB D− / D+ | 19 / 20 |

`LCD_RESET`, `TP_RESET`, `DSI_PWR_EN`, and the SD card CS are on the TCA9554
expander, not on GPIOs.

## The AXP2101 trap

**The BSP never configures the PMIC.** `grep -ri 'axp\|pmu\|aldo' ` over the
BSP source returns nothing. But per the schematic, **ALDO1 supplies the ES8311's
analog rail and the microphone**. If the PMU's power-on defaults leave ALDO1
off, the codec still enumerates on I²C and `esp_codec_dev_read()` still
succeeds — it just returns silence, with nothing in any log to explain why.

`main/pmic.c` asserts the rail itself: probe chip ID `0x03` (expect `0x4A`,
some dies report `0x47`), set `0x92[4:0] = 0x1C` (3.3 V), set `0x90` bit 0.

**Measured on real hardware, the rail is already up:**

```
pmic: AXP2101 found, chip id 0x4A
pmic: 0x90 = 0xFF    ALDO1=1 ALDO2=1 ALDO3=1 ALDO4=1
pmic: 0x92 = 0x1C    (3300 mV)
```

Both before and after our write. So on this board the write is a no-op, and the
schematic's warning turned out to be theoretical. It stays because it costs
nothing, it verifies the chip, it dumps the rails when the microphone misbehaves,
and it would rescue a unit whose power-on defaults differ. The read-modify-write
discipline is what made a pointless write harmless rather than a brownout.

**Registers that must never be blind-written:**

| Register | Why |
|---|---|
| `0x80` bit 0 | DCDC1 enable — the 3.3 V rail powering the ESP32-S3. Clearing it kills the board mid-instruction. |
| `0x82` | DCDC1 voltage — re-rails the whole system. |
| `0x10`, `0x12`, `0x14`, `0x22`, `0x23`, `0x24`, `0x25`, `0x27` | Power sequencing, BATFET, under-voltage shutdown. |

Every write in `pmic.c` is read-modify-write, and it refuses to write at all if
the chip ID does not match.

## USB, and why flashing gets awkward

The single USB-C port is wired **directly to the S3's native USB pins**
(GPIO19/20) through 22 Ω series resistors. There is **no CH343/CP2102 UART
bridge** anywhere on the board. That is what makes USB HID possible at all.

It also means the USB-Serial-JTAG controller and the USB-OTG controller share
one PHY. Once firmware calls `tinyusb_driver_install()` to become an HID
keyboard, **the serial console disappears** and you can no longer flash over
USB while the app is running.

Consequences:

- Chunk 1 deliberately ships **no TinyUSB**, so the console works during
  bring-up. HID lands last, not at "Phase 2".
- To reflash once HID firmware is running, force the ROM bootloader:
  **hold BOOT, tap PWR (or unplug/replug USB-C), release BOOT.**
  There is no RESET button — `CHIP_PU` is driven by the AXP2101's `PWROK`.
- `tools/flash.sh` passes `--after watchdog-reset`. A USB-Serial-JTAG reset is
  only a *core* reset and does not re-sample the strapping pins, so without
  this the chip can get stuck in download mode.
- A planned mitigation is an on-screen "Firmware Update" button that writes
  `RTC_CNTL_FORCE_DOWNLOAD_BOOT` in `RTC_CNTL_OPTION1_REG` and reboots
  straight into download mode, turning a hardware ritual into a tap.

## Two ways this board bites during boot

**The touch controller NACKs if you rush it.** Bring the display up the instant
power arrives and `esp_lcd_touch_new_i2c_ft5x06()` fails with an I2C NACK. The
BSP wraps that in `ESP_ERROR_CHECK`, so the whole application aborts and
reboots; it survives the warm retry, which is what makes it look random.

The driver hints at the cause first:

```
W i2c.master: Please check pull-up resistances whether be connected properly.
E i2c.master: I2C hardware NACK detected
E FT5x06: Touch controller FT5x06 initialization failed!
abort() ... Rebooting...
```

`app_main()` therefore settles for 200 ms, brings up I2C and the PMIC, and only
then starts the display. The shared bus also runs at **100 kHz**, not 400
(`CONFIG_BSP_I2C_FAST_MODE=n`). Nothing on it — PMIC, expander, codec, touch —
needs the speed.

**The BSP's default LVGL buffer is slow enough to trip the watchdog.**
`bsp_display_start()` asks for 20 rows in PSRAM with `buff_dma = false`. The SPI
driver then bounces every flush through an internal DMA buffer, and with a
nearly-full internal heap one screen took **twelve seconds** to paint:

```
E task_wdt: IDLE0 did not reset the watchdog ... CPU 0: taskLVGL
I ui: ui up                                     <- 12.5 s after backlight on
E ui: could not lock display for setup screen
```

Use `bsp_display_start_with_config()` with `buff_dma = true`,
`buff_spiram = false` and a larger buffer. See `display_start()` in
`main/app_main.c`. Wi-Fi and lwIP buffers move to PSRAM to pay for the internal
memory, which is safe precisely because the display no longer touches PSRAM.

## Audio

`bsp_audio_init(NULL)` defaults to **22050 Hz**, and it no-ops on a second
call. `bsp_audio_codec_microphone_init()` calls it with `NULL` if I²S is not
already up. So to record at 16 kHz you must call `bsp_audio_init(&cfg_16k)`
**first** — see `main/audio_capture.c`. Getting this wrong yields audio that
plays back at the wrong speed and transcribes as gibberish.

## After HID lands: the console moves, and so does reflashing

Once `hid_kbd_start()` installs TinyUSB, the USB PHY belongs to the OTG
controller. Two consequences, both permanent for that boot:

**The console moves.** Every log line before that call arrives on the
USB-Serial-JTAG port you flashed from. Everything after arrives on the
composite CDC interface, because `tinyusb_console_init()` redirects it. The
device re-enumerates at that moment, so your monitor will drop; reattach it to
the new port. This is why HID is the *last* thing boot does — the AXP2101
register dump, the audio bring-up and the Wi-Fi association are all already on
the wire before the port changes.

**Reflashing needs the button.** esptool can no longer reset the board into the
ROM bootloader over USB, because the port it would talk to is a keyboard.

    hold BOOT, tap PWR, release BOOT

Then flash. This applies to the browser installer too.

To skip all of this during bring-up, build with HID off:

    idf.py menuconfig   # TapTalk -> Enumerate as a USB HID keyboard = n

The USB-Serial-JTAG console then survives, and transcripts are logged instead
of typed.
