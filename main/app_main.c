/* TapTalk Keyboard.
 *
 * Boot order matters here. The PMIC runs first because ALDO1 feeds the codec's
 * analog rail; the display comes up before it only so the screen can narrate
 * what happens next. Audio comes last, since it needs the rail the PMIC just
 * asserted.
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
#include "hid_kbd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "TapTalk %s (idf %s)", app->version, app->idf_ver);
    ESP_LOGI(TAG, "PSRAM free: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* Display and LVGL. bsp_display_start() spawns the LVGL task itself; we
     * must not create a second one. */
    ESP_ERROR_CHECK(bsp_display_start() != NULL ? ESP_OK : ESP_FAIL);
    bsp_display_backlight_on();
    ESP_ERROR_CHECK(ui_init());

    /* NVS must be up before config_load(); net_init_common() does that, plus
     * netif, the event loop, and esp_wifi_init(). Whether the radio then comes
     * up as a station or as the setup access point is the state machine's
     * decision, driven by whether an SSID is stored. */
    ESP_ERROR_CHECK(net_init_common());

    app_config_t cfg;
    ESP_ERROR_CHECK(config_load(&cfg));

    app_sm_start(&cfg);

    /* pmic_init() is the state machine's first action, so ALDO1 is up before
     * we open the codec. Wait for that step to land. */
    while (app_sm_state() == ST_BOOT || app_sm_state() == ST_PMIC_INIT) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* The capture task drives the on-screen level meter, which is how we find
     * out whether the microphone rail is actually powered. Worth having even
     * on the setup screen: a flat bar there means the same thing. */
    const esp_err_t err = audio_capture_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio bring-up failed: %s", esp_err_to_name(err));
        ui_set_msg("Microphone error");
    }

    /* Last, and deliberately so: this is the line that costs us the
     * USB-Serial-JTAG console. Everything worth watching has already logged. */
    ESP_ERROR_CHECK(hid_kbd_start());

    ESP_LOGI(TAG, "boot complete, free heap %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
