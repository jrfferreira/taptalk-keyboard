/* TapTalk Keyboard.
 *
 * Boot order is load-bearing, and every step of it was learned the hard way.
 *
 * The PMIC and the I2C bus come up BEFORE the display. Bringing the panel up
 * the instant power arrives makes the FT3168 touch controller NACK on I2C, and
 * the BSP wraps that in ESP_ERROR_CHECK, so the whole application aborts and
 * reboots. It survives the warm retry, which is exactly what makes it look
 * random rather than like a race.
 *
 * TinyUSB comes up LAST. Installing it hands the USB PHY from the
 * USB-Serial-JTAG controller to the OTG controller, so every log line up to
 * that point arrives on the port you flashed from, and everything after it on
 * the composite CDC interface. Reattach the monitor when the device
 * re-enumerates. */
#include "app_sm.h"
#include "audio_capture.h"
#include "bsp/esp-bsp.h"
#include "config_store.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_kbd.h"
#include "net_wifi.h"
#include "pmic.h"
#include "ui.h"

static const char *TAG = "main";

/* The rails, the I2C bus and the touch controller all need a moment after the
 * AXP2101 lets go. Cheaper than a crash loop. */
#define POWER_SETTLE_MS 200

/* 40 rows of 368 px in RGB565: 29 KB, internal and DMA-capable.
 *
 * The BSP's default is 20 rows in PSRAM with buff_dma = false, which forces
 * the SPI driver to bounce every flush through an internal DMA buffer. With a
 * nearly-full internal heap that took twelve seconds to paint one screen,
 * starved the idle task into a watchdog reset, and held the LVGL lock so long
 * that the setup screen never drew. An internal DMA buffer removes the bounce. */
#define LVGL_BUF_ROWS 40

static void display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_H_RES * LVGL_BUF_ROWS,
        .double_buffer = false,
        .flags         = {.buff_dma = true, .buff_spiram = false},
    };
    /* Core 1, away from Wi-Fi and lwIP. bsp_display_start_with_config() spawns
     * the LVGL task itself; we must not create a second one. */
    cfg.lvgl_port_cfg.task_affinity = 1;

    ESP_ERROR_CHECK(bsp_display_start_with_config(&cfg) != NULL ? ESP_OK : ESP_FAIL);
    bsp_display_backlight_on();
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "TapTalk %s (idf %s)", app->version, app->idf_ver);
    ESP_LOGI(TAG, "PSRAM free: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    vTaskDelay(pdMS_TO_TICKS(POWER_SETTLE_MS));

    /* Brings up I2C and asserts ALDO1, the codec's analog rail. Idempotent:
     * the state machine calls it again as its first action and gets the cached
     * result, so the UI can still report what happened. */
    pmic_status_t pmic;
    if (pmic_init(&pmic) != ESP_OK) {
        ESP_LOGE(TAG, "PMIC not found; continuing, but the microphone may be silent");
    }

    display_start();
    ESP_LOGI(TAG, "internal heap after display: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_ERROR_CHECK(ui_init());

    /* NVS must be up before config_load(); net_init_common() does that, plus
     * netif, the event loop, and esp_wifi_init(). Whether the radio then comes
     * up as a station or as the setup access point is the state machine's
     * decision, driven by whether an SSID is stored. */
    ESP_ERROR_CHECK(net_init_common());

    app_config_t cfg;
    ESP_ERROR_CHECK(config_load(&cfg));

    app_sm_start(&cfg);

    /* ALDO1 is already asserted, so the codec can be opened straight away. */
    const esp_err_t err = audio_capture_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio bring-up failed: %s", esp_err_to_name(err));
        ui_set_error("Microphone error");
    }

    /* Last, and deliberately so: this is the line that costs us the
     * USB-Serial-JTAG console. Everything worth watching has already logged. */
    ESP_ERROR_CHECK(hid_kbd_start());

    ESP_LOGI(TAG, "boot complete, free heap %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
