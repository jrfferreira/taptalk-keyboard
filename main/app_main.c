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
#include "beeper.h"
#include "bsp/esp-bsp.h"
#include "config_store.h"
#include "diagnostics.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_lcd_touch.h"
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

/* The FT3168's I2C address. The BSP hands it to esp_lcd_touch_ft5x06, which
 * NACKs and gives up if the chip has not finished waking. */
#define TOUCH_I2C_ADDR 0x38
#define TOUCH_WAIT_STEP_MS 25
#define TOUCH_WAIT_TOTAL_MS 1500

/* TCA9554 P2 = TP_RESET, per the board schematic. P0 is LCD_RESET and P1 is
 * DSI_PWR_EN; we never touch those, because getting them wrong blanks the panel
 * and we only reach that code when something is already wrong. */
#define TP_RESET_PIN IO_EXPANDER_PIN_NUM_2

static bool touch_answers(i2c_master_bus_handle_t bus)
{
    return i2c_master_probe(bus, TOUCH_I2C_ADDR, TOUCH_WAIT_STEP_MS) == ESP_OK;
}

/* Wait for the touch controller to answer before the BSP tries to talk to it.
 *
 * BSP_LCD_TOUCH_RST is GPIO_NUM_NC on this board -- the reset line hangs off
 * the TCA9554 -- so the BSP neither resets the chip nor waits for it. On a cold
 * boot it NACKs, and bsp_display_indev_init() turns that into abort(), so the
 * board reboots forever. Probing first turns a crash loop into a short wait. */
static void wait_for_touch(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    /* i2c_master_probe logs an error on every NACK. Silence it, or the wait
     * buries the log in the very noise it exists to avoid. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    const int attempts = TOUCH_WAIT_TOTAL_MS / TOUCH_WAIT_STEP_MS;
    for (int i = 0; i < attempts; i++) {
        if (touch_answers(bus)) {
            esp_log_level_set("i2c.master", ESP_LOG_INFO);
            ESP_LOGI(TAG, "touch controller ready after %d ms", i * TOUCH_WAIT_STEP_MS);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_WAIT_STEP_MS));
    }

    /* Still silent. Release its reset through the expander -- the one thing the
     * BSP will never do for us. Only attempted once the chip has already failed
     * to answer, so a wrong guess about the pin cannot make a working board
     * worse. */
    ESP_LOGW(TAG, "touch silent after %d ms; pulsing TP_RESET via the expander",
             TOUCH_WAIT_TOTAL_MS);
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp != NULL && esp_io_expander_set_dir(exp, TP_RESET_PIN, IO_EXPANDER_OUTPUT) == ESP_OK) {
        esp_io_expander_set_level(exp, TP_RESET_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_io_expander_set_level(exp, TP_RESET_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(200));

        if (touch_answers(bus)) {
            esp_log_level_set("i2c.master", ESP_LOG_INFO);
            ESP_LOGI(TAG, "touch controller answered after a reset pulse");
            return;
        }
    }

    esp_log_level_set("i2c.master", ESP_LOG_INFO);
    /* We cannot save it from here. bsp_display_indev_init() will call abort().
     *
     * CONFIG_BSP_ERROR_CHECK=n would make that a plain return, but it does not
     * compile: with the option off, the BSP's own bsp_io_expander_init()
     * returns an esp_err_t from a function declared to return a pointer. So the
     * best we can do is say plainly what is about to happen, in the line just
     * above the panic, rather than leave a reboot loop to be guessed at. */
    ESP_LOGE(TAG, "touch controller never answered at 0x%02X -- the BSP is about "
                  "to abort. The board will reboot.", TOUCH_I2C_ADDR);
}

/* This FT3168 panel under-reports its coordinate range: a full-surface drag
 * measured raw X in 27..339 (nominal 0..368) and raw Y in 41..411 (nominal
 * 0..448), so it never reaches the display edges and the corner controls -- Wi-Fi,
 * Send, Undo -- were untappable. esp_lcd_touch calls this with the RAW panel
 * coordinates before swap/mirror; stretch the measured reachable band to the
 * full native range so the whole screen becomes reachable. Clamped to the panel
 * bounds: out-of-band samples were landing as negative coordinates and getting
 * dropped, which is what made the top edge dead. */
#define TOUCH_RAW_X_MIN 27
#define TOUCH_RAW_X_MAX 339
#define TOUCH_RAW_Y_MIN 41
#define TOUCH_RAW_Y_MAX 411
static void touch_scale(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                        uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;
    if (point_num == NULL) {
        return;
    }
    for (uint8_t i = 0; i < *point_num; i++) {
        int sx = ((int)x[i] - TOUCH_RAW_X_MIN) * (BSP_LCD_H_RES - 1) /
                 (TOUCH_RAW_X_MAX - TOUCH_RAW_X_MIN);
        int sy = ((int)y[i] - TOUCH_RAW_Y_MIN) * (BSP_LCD_V_RES - 1) /
                 (TOUCH_RAW_Y_MAX - TOUCH_RAW_Y_MIN);
        x[i] = (uint16_t)(sx < 0 ? 0 : (sx > BSP_LCD_H_RES - 1 ? BSP_LCD_H_RES - 1 : sx));
        y[i] = (uint16_t)(sy < 0 ? 0 : (sy > BSP_LCD_V_RES - 1 ? BSP_LCD_V_RES - 1 : sy));
    }
}

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

    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    ESP_ERROR_CHECK(disp != NULL ? ESP_OK : ESP_FAIL);

    /* Rotate the whole UI a quarter turn so the panel reads correctly when the
     * device is held with the USB-C port at the top. The logical canvas becomes
     * 448x368 (landscape); the corner layout follows via LV_ALIGN, the
     * background is baked at the matching size. 270, not 90: at 90 the USB-C
     * port came out the bottom. */
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_270);

    /* Rotate TOUCH to match. LVGL's sw-rotate turns the rendered image but does
     * NOT transform touch input -- esp_lvgl_port hands LVGL the raw panel
     * coordinates -- so without this the display is landscape while touch is
     * still portrait, and nothing is tappable.
     *
     * The port keeps the esp_lcd_touch handle as the first member of the indev's
     * driver-data struct, which is how we reach it without a public accessor.
     *
     * esp_lcd_touch applies mirror in raw panel space, THEN swaps, so with
     * swap on, screen-X comes from the panel-Y axis. mirror_y was flipping
     * screen-X: taps landed horizontally mirrored -- Send (bottom-right) fired
     * Undo (bottom-left), the Wi-Fi icon (top-right) hit the dead top-left
     * corner. This board needs swap-xy and NO mirror. All three flags are set
     * explicitly so the result does not depend on the driver's defaults. */
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev != NULL) {
        void *ctx = lv_indev_get_driver_data(indev);
        esp_lcd_touch_handle_t tp = ctx ? *(esp_lcd_touch_handle_t *)ctx : NULL;
        if (tp != NULL) {
            esp_lcd_touch_set_swap_xy(tp, true);
            esp_lcd_touch_set_mirror_x(tp, false);
            esp_lcd_touch_set_mirror_y(tp, false);
            /* Stretch the panel's under-reported coordinate range to the full
             * display, so the edges (and the controls that live there) are
             * reachable. See touch_scale. */
            tp->config.process_coordinates = touch_scale;
            ESP_LOGI(TAG, "touch rotated to match 270 deg display");
        } else {
            ESP_LOGE(TAG, "could not reach touch handle; touch will be misaligned");
        }
    }

    bsp_display_backlight_on();
}

void app_main(void)
{
    /* Our own tags to DEBUG; everyone else stays at INFO so the console is
     * readable. CONFIG_LOG_MAXIMUM_LEVEL_DEBUG is what keeps the ESP_LOGD calls
     * from being stripped at compile time. */
    for (const char *const *tag = (const char *const[]){"ui", "sm", "audio", "stt", "hid", "beep",
                                                        "diag", "prov", "net", "pmic", NULL};
         *tag != NULL; tag++) {
        esp_log_level_set(*tag, ESP_LOG_DEBUG);
    }

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

    wait_for_touch();

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

    /* After audio: bsp_audio_init() no-ops on a second call, so the speaker
     * inherits the microphone's 16 kHz rather than defaulting to 22050. */
    beeper_init();

#if CONFIG_TAPTALK_HID_DELAY_MS > 0
    /* Debug: keep the USB-Serial-JTAG console alive a while longer, because the
     * next line takes it away and everything interesting happens before then. */
    ESP_LOGW(TAG, "holding TinyUSB back for %d ms so the boot console survives",
             CONFIG_TAPTALK_HID_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_TAPTALK_HID_DELAY_MS));
#endif

    /* Last, and deliberately so: this is the line that costs us the
     * USB-Serial-JTAG console. Everything worth watching has already logged. */
    hid_kbd_set_layout(config_kbd_layout(&cfg));
    ESP_ERROR_CHECK(hid_kbd_start());

    ESP_ERROR_CHECK(diag_start());

    ESP_LOGI(TAG, "boot complete, free heap %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
