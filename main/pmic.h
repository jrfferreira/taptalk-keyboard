/* AXP2101 power management.
 *
 * The Waveshare BSP never touches this chip — grep its source for "axp" and
 * you get nothing — yet per the board schematic ALDO1 supplies the ES8311's
 * analog rail and the microphone. If the PMU's power-on defaults happen to
 * leave ALDO1 off, the codec enumerates on I2C and records pure silence, with
 * nothing in any log to explain why. So we assert the rail ourselves. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool present;      /* chip answered on I2C with a plausible ID */
    uint8_t chip_id;   /* register 0x03 */
    bool aldo1_on;     /* after our write */
    uint16_t aldo1_mv; /* after our write */
} pmic_status_t;

/* Probes the AXP2101, dumps its rail configuration to the log, then ensures
 * ALDO1 is enabled at 3.3 V. Every write is read-modify-write.
 *
 * Calls bsp_i2c_init() itself, so it is safe (and intended) to run before the
 * display comes up.
 *
 * Returns ESP_ERR_NOT_FOUND if the chip does not answer, in which case no
 * write is attempted. */
esp_err_t pmic_init(pmic_status_t *out);

/* Is USB-C delivering power right now?
 *
 * TinyUSB's tud_mounted() is configured bus-powered (self_powered = false), so
 * it assumes VBUS is present whenever the firmware is running -- and this board
 * keeps running on battery after the cable is pulled, so tud_mounted() never
 * reports the unplug and the "USB" icon would stay lit forever. The AXP2101, as
 * the power-path controller, always knows: register 0x00 ("PMU status 1")
 * bit 5 is VBUS-good. Returns false if the chip is absent or the read fails. */
bool pmic_vbus_present(void);
