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
} ui_model_t;

static ui_model_t s_model;
static SemaphoreHandle_t s_lock;

static lv_obj_t *s_btn, *s_btn_label, *s_state_label, *s_bar, *s_status_label, *s_msg_label;

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

/* Runs in the LVGL task, which already holds the LVGL lock. */
static void ui_tick(lv_timer_t *timer)
{
    (void)timer;

    ui_model_t m;
    model_lock();
    m = s_model;
    model_unlock();

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

    lv_label_set_text_fmt(s_status_label, "WiFi %s %s\nTime %s\nALDO1 %s %umV  id 0x%02X",
                          m.wifi_up ? "up" : "--", m.ip, m.time_ok ? m.iso : "unsynced",
                          m.pmic.aldo1_on ? "on" : "OFF", (unsigned)m.pmic.aldo1_mv,
                          m.pmic.chip_id);

    lv_label_set_text(s_msg_label, m.msg);
}

/* LVGL event callbacks fire in the LVGL task. They only enqueue. */
static void on_press(lv_event_t *e) { (void)e; app_sm_post(EV_BTN_PRESS); }
static void on_release(lv_event_t *e) { (void)e; app_sm_post(EV_BTN_RELEASE); }
static void on_press_lost(lv_event_t *e) { (void)e; app_sm_post(EV_PRESS_LOST); }

esp_err_t ui_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");

    ESP_RETURN_ON_FALSE(bsp_display_lock(0), ESP_FAIL, TAG, "display lock");

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "TapTalk");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0E0), LV_PART_MAIN);

    s_state_label = lv_label_create(scr);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x9E9E9E), LV_PART_MAIN);

    s_btn = lv_button_create(scr);
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

    /* The whole point of chunk 1: if this bar never moves, ALDO1 is not
     * powering the microphone and no amount of codec debugging will help. */
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 280, 14);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 118);
    lv_bar_set_range(s_bar, 0, 100);

    s_msg_label = lv_label_create(scr);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, 144);
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFFB300), LV_PART_MAIN);

    s_status_label = lv_label_create(scr);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x757575), LV_PART_MAIN);
    lv_label_set_text(s_status_label, "");

    lv_timer_create(ui_tick, 100, NULL);

    bsp_display_unlock();
    ESP_LOGI(TAG, "ui up");
    return ESP_OK;
}
