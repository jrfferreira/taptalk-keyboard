#include "pmic.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pmic";

#define AXP2101_ADDR 0x34
#define I2C_TIMEOUT_MS 100

/* Register map cross-checked against XPowersLib (MIT), which X-Powers
 * endorses, and the AXP2101 datasheet v1.4. */
#define REG_STATUS1   0x00
#define REG_STATUS2   0x01
#define REG_CHIP_ID   0x03
#define REG_PWRON_STS 0x20
#define REG_PWROFF_STS 0x21
#define REG_DCDC_ONOFF 0x80 /* DO NOT WRITE: bit0 is DCDC1 = the 3.3 V rail
                             * powering the ESP32-S3 itself. Clearing it kills
                             * the board mid-instruction. Read only. */
#define REG_DCDC1_VOL 0x82  /* DO NOT WRITE: re-rails the whole system. */
#define REG_LDO_ONOFF0 0x90 /* bit0=ALDO1 .. bit3=ALDO4, bit4=BLDO1, ... */
#define REG_ALDO1_VOL 0x92  /* bits[4:0]; bits[7:5] reserved, must be preserved */
#define REG_ALDO2_VOL 0x93
#define REG_ALDO3_VOL 0x94
#define REG_ALDO4_VOL 0x95
#define REG_BAT_PCT   0xA4

#define CHIP_ID_A 0x4A /* what XPowersLib requires */
#define CHIP_ID_B 0x47 /* reported by some dies */

#define ALDO1_EN_BIT 0x01
#define ALDO_VOL_MASK 0x1F
#define ALDO_VOL_RESERVED 0xE0

/* Vout(mV) = 500 + N * 100, N in [0, 31]. */
static uint8_t aldo_mv_to_code(uint16_t mv) { return (uint8_t)((mv - 500u) / 100u); }
static uint16_t aldo_code_to_mv(uint8_t code) { return (uint16_t)(500u + (code & ALDO_VOL_MASK) * 100u); }

static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

/* Preserve every bit we are not deliberately changing. A blind byte write to
 * the wrong register here powers the board off. */
static esp_err_t reg_update(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(reg_read(reg, &v), TAG, "read 0x%02x", reg);
    const uint8_t nv = (uint8_t)((v & (uint8_t)~clear_mask) | set_mask);
    if (nv == v) {
        return ESP_OK;
    }
    return reg_write(reg, nv);
}

static void dump_rails(const char *when)
{
    static const uint8_t regs[] = {REG_STATUS1,   REG_STATUS2,    REG_PWRON_STS, REG_PWROFF_STS,
                                   REG_DCDC_ONOFF, REG_DCDC1_VOL, REG_LDO_ONOFF0, REG_ALDO1_VOL,
                                   REG_ALDO2_VOL,  REG_ALDO3_VOL, REG_ALDO4_VOL,  REG_BAT_PCT};
    ESP_LOGI(TAG, "--- AXP2101 registers (%s) ---", when);
    for (size_t i = 0; i < sizeof(regs); i++) {
        uint8_t v = 0;
        if (reg_read(regs[i], &v) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X = 0x%02X", regs[i], v);
        } else {
            ESP_LOGW(TAG, "  0x%02X = <read failed>", regs[i]);
        }
    }
    uint8_t ldo = 0;
    if (reg_read(REG_LDO_ONOFF0, &ldo) == ESP_OK) {
        ESP_LOGI(TAG, "  ALDO1=%d ALDO2=%d ALDO3=%d ALDO4=%d", !!(ldo & 0x01), !!(ldo & 0x02),
                 !!(ldo & 0x04), !!(ldo & 0x08));
    }
}

esp_err_t pmic_init(pmic_status_t *out)
{
    if (out != NULL) {
        *out = (pmic_status_t){0};
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev_cfg, &s_dev), TAG,
                        "add axp2101");

    uint8_t id = 0;
    if (reg_read(REG_CHIP_ID, &id) != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 did not answer at 0x%02X", AXP2101_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    if (id != CHIP_ID_A && id != CHIP_ID_B) {
        /* Refuse to write registers on a chip we have not identified. */
        ESP_LOGE(TAG, "unexpected chip id 0x%02X (want 0x%02X or 0x%02X); not writing", id,
                 CHIP_ID_A, CHIP_ID_B);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "AXP2101 found, chip id 0x%02X", id);

    dump_rails("before");

    /* The only two writes we make. ALDO1 = 3.3 V, then enable it.
     * Voltage first, so the rail never comes up at the wrong level. */
    const uint8_t code = aldo_mv_to_code(3300); /* == 0x1C */
    ESP_RETURN_ON_ERROR(reg_update(REG_ALDO1_VOL, ALDO_VOL_MASK, code), TAG, "set ALDO1 voltage");
    ESP_RETURN_ON_ERROR(reg_update(REG_LDO_ONOFF0, 0x00, ALDO1_EN_BIT), TAG, "enable ALDO1");

    dump_rails("after");

    uint8_t ldo = 0, vol = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_LDO_ONOFF0, &ldo), TAG, "verify enable");
    ESP_RETURN_ON_ERROR(reg_read(REG_ALDO1_VOL, &vol), TAG, "verify voltage");

    if (out != NULL) {
        out->present   = true;
        out->chip_id   = id;
        out->aldo1_on  = (ldo & ALDO1_EN_BIT) != 0;
        out->aldo1_mv  = aldo_code_to_mv(vol);
    }
    ESP_LOGI(TAG, "ALDO1 (codec + mic analog): %s @ %u mV",
             (ldo & ALDO1_EN_BIT) ? "on" : "OFF", aldo_code_to_mv(vol));

    (void)ALDO_VOL_RESERVED; /* documented above; reg_update preserves it via clear_mask */
    return ESP_OK;
}

#define STATUS1_VBUS_GOOD 0x20 /* bit 5 */

bool pmic_vbus_present(void)
{
    if (s_dev == NULL) {
        return false;
    }
    uint8_t v = 0;
    if (reg_read(REG_STATUS1, &v) != ESP_OK) {
        return false;
    }
    return (v & STATUS1_VBUS_GOOD) != 0;
}
