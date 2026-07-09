#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app_sm.h"
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ui";

#if LV_FONT_MONTSERRAT_28
#define FONT_BIG &lv_font_montserrat_28
#else
#define FONT_BIG LV_FONT_DEFAULT
#endif

typedef struct {
    app_state_t state;
    int level;
    uint32_t clip_ms;
    int clip_peak;
    bool wifi_up;
    char ip[20];
    bool time_ok;
    char iso[24];
    pmic_status_t pmic;
    char msg[64];
    char api_key[24]; /* masked */
} ui_model_t;

static ui_model_t s_model;
static SemaphoreHandle_t s_lock;

/* Main screen */
static lv_obj_t *s_main, *s_btn, *s_btn_label, *s_state_label, *s_bar, *s_status_label,
    *s_msg_label, *s_setup_btn;
/* Setup screen, built lazily */
static lv_obj_t *s_setup;

static void model_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void model_unlock(void) { xSemaphoreGive(s_lock); }

#define MODEL_SET(field, value)                                                                    \
    do {                                                                                           \
        model_lock();                                                                              \
        s_model.field = (value);                                                                   \
        model_unlock();                                                                            \
    } while (0)

void ui_set_state(app_state_t state) { MODEL_SET(state, state); }
void ui_set_level(int percent) { MODEL_SET(level, percent); }

void ui_set_clip(uint32_t ms, int peak)
{
    model_lock();
    s_model.clip_ms   = ms;
    s_model.clip_peak = peak;
    model_unlock();
}

void ui_set_wifi(bool up, const char *ip)
{
    model_lock();
    s_model.wifi_up = up;
    snprintf(s_model.ip, sizeof(s_model.ip), "%s", ip != NULL ? ip : "");
    model_unlock();
}

void ui_set_time(bool ok, const char *iso)
{
    model_lock();
    s_model.time_ok = ok;
    snprintf(s_model.iso, sizeof(s_model.iso), "%s", iso != NULL ? iso : "");
    model_unlock();
}

void ui_set_pmic(const pmic_status_t *status)
{
    model_lock();
    s_model.pmic = *status;
    model_unlock();
}

void ui_set_msg(const char *msg)
{
    model_lock();
    snprintf(s_model.msg, sizeof(s_model.msg), "%s", msg != NULL ? msg : "");
    model_unlock();
}

void ui_set_api_key(const char *masked)
{
    model_lock();
    snprintf(s_model.api_key, sizeof(s_model.api_key), "%s", masked != NULL ? masked : "");
    model_unlock();
}

/* Runs in the LVGL task, which already holds the LVGL lock. */
static void ui_tick(lv_timer_t *timer)
{
    (void)timer;

    ui_model_t m;
    model_lock();
    m = s_model;
    model_unlock();

    if (m.state == ST_PROVISIONING) {
        return; /* the setup screen is static */
    }

    lv_label_set_text(s_state_label, sm_state_name(m.state));
    lv_bar_set_value(s_bar, m.level, LV_ANIM_OFF);

    const bool recording = (m.state == ST_RECORDING);
    lv_obj_set_style_bg_color(s_btn, recording ? lv_color_hex(0xC62828) : lv_color_hex(0x2E7D32),
                              LV_PART_MAIN);
    if (recording) {
        lv_label_set_text_fmt(s_btn_label, "%u.%us", (unsigned)(m.clip_ms / 1000),
                              (unsigned)((m.clip_ms % 1000) / 100));
    } else {
        lv_label_set_text(s_btn_label, "HOLD\nTO TALK");
    }

    lv_label_set_text_fmt(s_status_label, "WiFi %s %s\nTime %s\nKey %s\nALDO1 %s %umV",
                          m.wifi_up ? "up" : "--", m.ip, m.time_ok ? m.iso : "unsynced",
                          m.api_key[0] ? m.api_key : "<unset>", m.pmic.aldo1_on ? "on" : "OFF",
                          (unsigned)m.pmic.aldo1_mv);

    lv_label_set_text(s_msg_label, m.msg);
}

/* LVGL event callbacks fire in the LVGL task. They only enqueue. */
static void on_press(lv_event_t *e) { (void)e; app_sm_post(EV_BTN_PRESS); }
static void on_release(lv_event_t *e) { (void)e; app_sm_post(EV_BTN_RELEASE); }
static void on_press_lost(lv_event_t *e) { (void)e; app_sm_post(EV_PRESS_LOST); }
static void on_setup(lv_event_t *e) { (void)e; app_sm_post(EV_ENTER_SETUP); }

static void build_main(void)
{
    s_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_main, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_main);
    lv_label_set_text(title, "TapTalk");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0E0), LV_PART_MAIN);

    s_state_label = lv_label_create(s_main);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x9E9E9E), LV_PART_MAIN);

    /* Reaching setup must not require a working network. */
    s_setup_btn = lv_button_create(s_main);
    lv_obj_set_size(s_setup_btn, 64, 32);
    lv_obj_align(s_setup_btn, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(s_setup_btn, lv_color_hex(0x37474F), LV_PART_MAIN);
    lv_obj_add_event_cb(s_setup_btn, on_setup, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_setup_btn);
    lv_label_set_text(sl, "Setup");
    lv_obj_center(sl);

    s_btn = lv_button_create(s_main);
    lv_obj_set_size(s_btn, 220, 220);
    lv_obj_align(s_btn, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn, on_release, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_btn, on_press_lost, LV_EVENT_PRESS_LOST, NULL);

    s_btn_label = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_btn_label, FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_btn_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(s_btn_label);

    /* If this bar never moves, ALDO1 is not powering the microphone and no
     * amount of codec debugging will help. */
    s_bar = lv_bar_create(s_main);
    lv_obj_set_size(s_bar, 280, 14);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 118);
    lv_bar_set_range(s_bar, 0, 100);

    s_msg_label = lv_label_create(s_main);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, 144);
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFFB300), LV_PART_MAIN);

    s_status_label = lv_label_create(s_main);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x757575), LV_PART_MAIN);
    lv_label_set_text(s_status_label, "");
}

void ui_show_setup(const prov_info_t *info)
{
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "could not lock display for setup screen");
        return;
    }

    s_setup = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_setup, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(s_setup);
    lv_label_set_text(t, "Setup");
    lv_obj_set_style_text_font(t, FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *hint = lv_label_create(s_setup);
    lv_label_set_text(hint, "Join this Wi-Fi from your phone.\nA setup page opens by itself.");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 52);

#if LV_USE_QRCODE
    /* Scanning this joins the AP directly, so nobody has to retype an
     * eight-digit password on a phone keyboard. */
    char wifi_uri[80];
    snprintf(wifi_uri, sizeof(wifi_uri), "WIFI:T:WPA;S:%s;P:%s;;", info->ssid, info->pass);

    lv_obj_t *qr = lv_qrcode_create(s_setup);
    lv_qrcode_set_size(qr, 180);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, wifi_uri, strlen(wifi_uri));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_border_width(qr, 6, LV_PART_MAIN);
    lv_obj_set_style_border_color(qr, lv_color_white(), LV_PART_MAIN);
#endif

    lv_obj_t *creds = lv_label_create(s_setup);
    lv_label_set_text_fmt(creds, "%s\npassword  %s\n%s", info->ssid, info->pass, info->url);
    lv_obj_set_style_text_align(creds, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(creds, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_align(creds, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_screen_load(s_setup);
    bsp_display_unlock();

    ESP_LOGI(TAG, "setup screen shown");
}

esp_err_t ui_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");

    ESP_RETURN_ON_FALSE(bsp_display_lock(0), ESP_FAIL, TAG, "display lock");
    build_main();
    lv_screen_load(s_main);
    lv_timer_create(ui_tick, 100, NULL);
    bsp_display_unlock();

    ESP_LOGI(TAG, "ui up");
    return ESP_OK;
}
