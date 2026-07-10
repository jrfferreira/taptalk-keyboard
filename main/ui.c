#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app_sm.h"
#include "beeper.h"
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

/* Panel is 368x448. */
#define BTN_D 216
#define WAVE_BARS 27
#define WAVE_BAR_W 4
#define WAVE_BAR_GAP 4
#define WAVE_H 64
#define WAVE_MIN 4
#define ICON_HIT 56 /* transparent touch target around the settings glyph */

#define C_BG_TOP   0x05070A
#define C_BG_BOT   0x141E28
#define C_IDLE_TOP 0x5CF0B0
#define C_IDLE_BOT 0x1B8560
#define C_REC_TOP  0xFF7A6E
#define C_REC_BOT  0xB3241C
/* Pressed = the same hue, lifted. A different hue would read as a different
 * button; a lift reads as the same button, depressed. */
#define C_IDLE_TOP_P 0x8CFFCB
#define C_IDLE_BOT_P 0x27A87A
#define C_REC_TOP_P  0xFFA096
#define C_REC_BOT_P  0xD13A30
#define PRESS_MS 90
#define C_INK      0x04150E
#define C_ON       0x3DDC97
#define C_OFF      0x39434D
#define C_MSG      0xF5B638
#define C_STATUS   0x8A97A3

typedef struct {
    app_state_t state;
    int level;
    uint32_t clip_ms;
    bool wifi;
    bool usb;
    char msg[80];
    bool msg_is_error;
} ui_model_t;

static ui_model_t s_model;
static SemaphoreHandle_t s_lock;

static lv_obj_t *s_main, *s_setup;
static lv_obj_t *s_btn, *s_timer, *s_status;
static lv_obj_t *s_ico_usb, *s_ico_wifi, *s_badge, *s_badge_hit;
static lv_obj_t *s_sheet, *s_sheet_text;
static lv_obj_t *s_wave[WAVE_BARS];

/* Rolling level history, oldest first. Shifting it each tick is what makes
 * the bars travel instead of just pulsing in place. */
static uint8_t s_hist[WAVE_BARS];

static void model_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void model_unlock(void) { xSemaphoreGive(s_lock); }

#define MODEL_SET(field, value)                                                                    \
    do {                                                                                           \
        model_lock();                                                                              \
        s_model.field = (value);                                                                   \
        model_unlock();                                                                            \
    } while (0)

void ui_set_state(app_state_t state) { MODEL_SET(state, state); }
void ui_set_level(int percent) { MODEL_SET(level, percent < 0 ? 0 : (percent > 100 ? 100 : percent)); }
void ui_set_wifi(bool connected) { MODEL_SET(wifi, connected); }
void ui_set_usb(bool connected) { MODEL_SET(usb, connected); }

void ui_set_clip(uint32_t ms, int peak)
{
    (void)peak; /* the wave shows level; the number added nothing */
    MODEL_SET(clip_ms, ms);
}

static void set_msg(const char *text, bool is_error)
{
    model_lock();
    snprintf(s_model.msg, sizeof(s_model.msg), "%s", text != NULL ? text : "");
    s_model.msg_is_error = is_error && s_model.msg[0] != '\0';
    model_unlock();
}

void ui_set_status(const char *text) { set_msg(text, false); }
void ui_set_error(const char *text) { set_msg(text, true); }
void ui_clear_msg(void) { set_msg("", false); }

/* ------------------------------------------------------------------ paint */

static void set_grad(lv_obj_t *o, uint32_t top, uint32_t bot)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(top), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(bot), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
}

/* The pressed look is a STYLE on LV_STATE_PRESSED, not something ui_tick()
 * paints. LVGL swaps and animates it in the render task on the touch event
 * itself, so the feedback is immediate and the tick stays free -- which is the
 * whole lesson of the twelve-second screen. */
static void set_grad_pressed(lv_obj_t *o, uint32_t top, uint32_t bot)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(top), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(bot), LV_PART_MAIN | LV_STATE_PRESSED);
}

/* Runs in the LVGL task, which already holds the LVGL lock.
 *
 * Every LVGL setter invalidates the widget it touches, whether or not the
 * value changed. Rewriting the button's gradient on each tick invalidated a
 * 216 px circle plus its shadow seventeen times a second, which is how the
 * render task ended up starving the idle task. So: touch nothing that has not
 * changed. The waveform is the exception -- it genuinely changes every tick,
 * which is what makes it a waveform. */
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

    static int last_rec = -1; /* neither true nor false, so the first tick paints */
    static int last_wifi = -1, last_usb = -1;
    static uint32_t last_timer_tenths = UINT32_MAX;
    static char last_msg[sizeof(m.msg)] = {1};

    const bool rec = (m.state == ST_RECORDING);
    if ((int)rec != last_rec) {
        last_rec = rec;
        set_grad(s_btn, rec ? C_REC_TOP : C_IDLE_TOP, rec ? C_REC_BOT : C_IDLE_BOT);
        set_grad_pressed(s_btn, rec ? C_REC_TOP_P : C_IDLE_TOP_P,
                         rec ? C_REC_BOT_P : C_IDLE_BOT_P);
        lv_obj_set_style_shadow_color(s_btn, lv_color_hex(rec ? C_REC_TOP : C_IDLE_TOP),
                                      LV_PART_MAIN);
        if (rec) {
            lv_obj_remove_flag(s_timer, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_timer, LV_OBJ_FLAG_HIDDEN);
            last_timer_tenths = UINT32_MAX;
        }
    }

    if (rec) {
        const uint32_t tenths = m.clip_ms / 100;
        if (tenths != last_timer_tenths) {
            last_timer_tenths = tenths;
            lv_label_set_text_fmt(s_timer, "%u.%u", (unsigned)(tenths / 10),
                                  (unsigned)(tenths % 10));
        }
    }

    /* Scroll the history one step and append the newest level. */
    memmove(s_hist, s_hist + 1, WAVE_BARS - 1);
    s_hist[WAVE_BARS - 1] = (uint8_t)m.level;

    for (int i = 0; i < WAVE_BARS; i++) {
        const int h = WAVE_MIN + (s_hist[i] * (WAVE_H - WAVE_MIN)) / 100;
        if (lv_obj_get_height(s_wave[i]) != h) {
            lv_obj_set_height(s_wave[i], h);
            /* Quieter bars fade rather than vanish, so the line always reads. */
            lv_obj_set_style_bg_opa(s_wave[i], (lv_opa_t)(70 + (s_hist[i] * 185) / 100),
                                    LV_PART_MAIN);
        }
    }

    if ((int)m.wifi != last_wifi) {
        last_wifi = m.wifi;
        lv_obj_set_style_text_color(s_ico_wifi, lv_color_hex(m.wifi ? C_ON : C_OFF), LV_PART_MAIN);
    }
    if ((int)m.usb != last_usb) {
        last_usb = m.usb;
        lv_obj_set_style_text_color(s_ico_usb, lv_color_hex(m.usb ? C_ON : C_OFF), LV_PART_MAIN);
    }

    static bool last_was_error = false;
    if (strcmp(m.msg, last_msg) != 0 || m.msg_is_error != last_was_error) {
        snprintf(last_msg, sizeof(last_msg), "%s", m.msg);
        last_was_error = m.msg_is_error;

        const bool have = m.msg[0] != '\0';

        /* An error becomes a badge you can tap, not a line of text that
         * scrolls past. A status is just dim text. */
        if (have && m.msg_is_error) {
            lv_label_set_text(s_sheet_text, m.msg);
            lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_badge_hit, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_badge_hit, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN); /* the error is gone */
        }

        if (have && !m.msg_is_error) {
            lv_label_set_text(s_status, m.msg);
            lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void sheet_open(lv_event_t *e)
{
    (void)e;
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
}

static void sheet_close(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
}

/* LVGL event callbacks fire in the LVGL task. They only enqueue. */
/* The beep goes out from the touch event, not from the state machine, so the
 * confirmation is bound to the finger rather than to whatever the machine
 * decides to do about it. A press that is refused still clicks. */
static void on_press(lv_event_t *e)
{
    (void)e;
    beeper_play(BEEP_PRESS);
    app_sm_post(EV_BTN_PRESS);
}

static void on_release(lv_event_t *e)
{
    (void)e;
    beeper_play(BEEP_RELEASE);
    app_sm_post(EV_BTN_RELEASE);
}

static void on_press_lost(lv_event_t *e)
{
    (void)e;
    beeper_play(BEEP_RELEASE);
    app_sm_post(EV_PRESS_LOST);
}
static void on_setup(lv_event_t *e) { (void)e; app_sm_post(EV_ENTER_SETUP); }

/* A microphone, drawn from primitives: LVGL's symbol font has no mic glyph. */
static void build_mic(lv_obj_t *parent)
{
    lv_obj_t *capsule = lv_obj_create(parent);
    lv_obj_remove_style_all(capsule);
    lv_obj_set_size(capsule, 22, 40);
    lv_obj_align(capsule, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_radius(capsule, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_color(capsule, lv_color_hex(C_INK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, LV_PART_MAIN);

    /* The cradle: an arc with its indicator and knob stripped away. */
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style(arc, NULL, LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, 56, 56);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, -12);
    lv_arc_set_bg_angles(arc, 20, 160);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_INK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);

    lv_obj_t *stem = lv_obj_create(parent);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, 5, 10);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_radius(stem, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stem, lv_color_hex(C_INK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, LV_PART_MAIN);
}

static lv_obj_t *build_icon(lv_obj_t *parent, const char *sym, int32_t dx, lv_event_cb_t cb)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_OFF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, dx, -26);

    if (cb != NULL) {
        /* A 24px glyph is not a touch target. Give it an invisible 56px one. */
        lv_obj_t *hit = lv_obj_create(parent);
        lv_obj_remove_style_all(hit);
        lv_obj_set_size(hit, ICON_HIT, ICON_HIT);
        lv_obj_align(hit, LV_ALIGN_BOTTOM_MID, dx, -26 + ICON_HIT / 2 - 8);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, NULL);
    }
    return lbl;
}

/* The badge sits at the top, alone, where nothing else competes for the eye.
 * It is only ever visible when something is wrong. */
static void build_badge(void)
{
    s_badge = lv_label_create(s_main);
    lv_label_set_text(s_badge, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(s_badge, FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_badge, lv_color_hex(C_MSG), LV_PART_MAIN);
    lv_obj_align(s_badge, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    /* A 28 px glyph is not a touch target. */
    s_badge_hit = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_badge_hit);
    lv_obj_set_size(s_badge_hit, ICON_HIT, ICON_HIT);
    lv_obj_align(s_badge_hit, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_add_flag(s_badge_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_badge_hit, sheet_open, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_badge_hit, LV_OBJ_FLAG_HIDDEN);
}

/* Tapping the badge raises this. It is the only place the device tells you,
 * in words, that the key was rejected or the account is out of credit. */
static void build_sheet(void)
{
    s_sheet = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_sheet);
    lv_obj_set_size(s_sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sheet, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE); /* tap anywhere to dismiss */
    lv_obj_add_event_cb(s_sheet, sheet_close, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_sheet);
    lv_obj_set_size(card, 300, 200);
    lv_obj_center(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16202A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(C_MSG), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);

    lv_obj_t *head = lv_label_create(card);
    lv_label_set_text(head, LV_SYMBOL_WARNING "  Something went wrong");
    lv_obj_set_style_text_color(head, lv_color_hex(C_MSG), LV_PART_MAIN);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, 0);

    s_sheet_text = lv_label_create(card);
    lv_label_set_long_mode(s_sheet_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_sheet_text, 260);
    lv_obj_set_style_text_color(s_sheet_text, lv_color_hex(0xE8EDF2), LV_PART_MAIN);
    lv_obj_align(s_sheet_text, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_label_set_text(s_sheet_text, "");

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_set_size(ok, 110, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x2B3540), LV_PART_MAIN);
    lv_obj_set_style_radius(ok, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(ok, sheet_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "Dismiss");
    lv_obj_center(okl);
}

static void build_main(void)
{
    s_main = lv_obj_create(NULL);
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);
    /* A screen from lv_obj_create(NULL) is LV_OPA_TRANSP unless a theme covers
     * it. Setting bg_color alone paints nothing and the previous frame shows
     * through -- which is exactly what the first hardware boot looked like. */
    set_grad(s_main, C_BG_TOP, C_BG_BOT);

    s_btn = lv_button_create(s_main);
    lv_obj_set_size(s_btn, BTN_D, BTN_D);
    lv_obj_align(s_btn, LV_ALIGN_CENTER, 0, -46);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);
    /* A soft glow, not a drop shadow. LVGL blurs box shadows in software and
     * the cost grows with the blur radius; 40 px around a 216 px circle is
     * millions of operations per repaint. */
    lv_obj_set_style_shadow_width(s_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(s_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_btn, LV_OPA_20, LV_PART_MAIN);
    set_grad(s_btn, C_IDLE_TOP, C_IDLE_BOT);
    /* Animate the pressed style rather than snapping to it. LVGL runs this in
     * the render task on the touch event, so it costs nothing per tick. */
    static const lv_style_prop_t press_props[] = {
        LV_STYLE_BG_COLOR, LV_STYLE_BG_GRAD_COLOR,
        LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, 0,
    };
    static lv_style_transition_dsc_t press_tr;
    lv_style_transition_dsc_init(&press_tr, press_props, lv_anim_path_ease_out, PRESS_MS, 0, NULL);
    lv_obj_set_style_transition(s_btn, &press_tr, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transition(s_btn, &press_tr, LV_PART_MAIN | LV_STATE_PRESSED);
    /* 256 is 1.0. A 3% shrink reads as "it moved" without looking like a bug. */
    lv_obj_set_style_transform_scale(s_btn, 248, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_add_event_cb(s_btn, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn, on_release, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_btn, on_press_lost, LV_EVENT_PRESS_LOST, NULL);

    build_mic(s_btn);

    s_timer = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_timer, FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_timer, lv_color_hex(C_INK), LV_PART_MAIN);
    lv_obj_align(s_timer, LV_ALIGN_CENTER, 0, 54);
    lv_obj_add_flag(s_timer, LV_OBJ_FLAG_HIDDEN);

    /* The waveform. Left-mid alignment keeps every bar centred on one axis as
     * its height changes, so it grows both ways like a real level meter. */
    lv_obj_t *wave = lv_obj_create(s_main);
    lv_obj_remove_style_all(wave);
    lv_obj_set_size(wave, WAVE_BARS * (WAVE_BAR_W + WAVE_BAR_GAP), WAVE_H);
    lv_obj_align(wave, LV_ALIGN_CENTER, 0, 126);
    lv_obj_remove_flag(wave, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < WAVE_BARS; i++) {
        lv_obj_t *b = lv_obj_create(wave);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, WAVE_BAR_W, WAVE_MIN);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, i * (WAVE_BAR_W + WAVE_BAR_GAP), 0);
        lv_obj_set_style_radius(b, WAVE_BAR_W / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ON), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_PART_MAIN);
        s_wave[i] = b;
    }

    /* Transient status: dim, small, easy to ignore. */
    s_status = lv_label_create(s_main);
    lv_obj_set_style_text_color(s_status, lv_color_hex(C_STATUS), LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

    s_ico_usb  = build_icon(s_main, LV_SYMBOL_USB, -70, NULL);
    s_ico_wifi = build_icon(s_main, LV_SYMBOL_WIFI, 0, NULL);
    (void)build_icon(s_main, LV_SYMBOL_SETTINGS, 70, on_setup);

    build_badge();
    build_sheet();
}

void ui_show_setup(const prov_info_t *info)
{
    /* A cold first paint can hold the LVGL lock for a while. Giving up here
     * leaves the user staring at a half-drawn main screen with no way in. */
    if (!bsp_display_lock(10000)) {
        ESP_LOGE(TAG, "could not lock display for setup screen");
        return;
    }

    s_setup = lv_obj_create(NULL);
    lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_SCROLLABLE);
    set_grad(s_setup, C_BG_TOP, C_BG_BOT);

    lv_obj_t *t = lv_label_create(s_setup);
    lv_label_set_text(t, "Setup");
    lv_obj_set_style_text_font(t, FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xE8EDF2), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *hint = lv_label_create(s_setup);
    lv_label_set_text(hint, "Join this Wi-Fi from your phone");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8A97A3), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 58);

#if LV_USE_QRCODE
    /* Scanning this joins the AP directly, so nobody retypes an eight-digit
     * password on a phone keyboard. */
    char wifi_uri[80];
    snprintf(wifi_uri, sizeof(wifi_uri), "WIFI:T:WPA;S:%s;P:%s;;", info->ssid, info->pass);

    lv_obj_t *qr = lv_qrcode_create(s_setup);
    lv_qrcode_set_size(qr, 190);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, wifi_uri, strlen(wifi_uri));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_border_width(qr, 8, LV_PART_MAIN);
    lv_obj_set_style_border_color(qr, lv_color_white(), LV_PART_MAIN);
#endif

    lv_obj_t *creds = lv_label_create(s_setup);
    lv_label_set_text_fmt(creds, "%s\n%s\n%s", info->ssid, info->pass, info->url);
    lv_obj_set_style_text_align(creds, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(creds, lv_color_hex(0xE8EDF2), LV_PART_MAIN);
    lv_obj_align(creds, LV_ALIGN_BOTTOM_MID, 0, -26);

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
    /* 60 ms: fast enough that the wave travels smoothly, slow enough that the
     * QSPI flush is not the busiest thing on the board. */
    lv_timer_create(ui_tick, 60, NULL);
    bsp_display_unlock();

    ESP_LOGI(TAG, "ui up");
    return ESP_OK;
}
