#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app_sm.h"
#include "audio_capture.h"
#include "assets/bg_main.h"
#include "assets/btn_mic.h"
#include "assets/ic_halo.h"
#include "assets/ic_mic.h"
#include "beeper.h"
#include "diagnostics.h"
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ui";

#if LV_FONT_MONTSERRAT_28
#define FONT_BIG &lv_font_montserrat_28
#else
#define FONT_BIG LV_FONT_DEFAULT
#endif

/* Panel is 368x448. */
#define BTN_D 228
/* The spectrum analyser: full-width bars rising from the bottom edge, low
 * opacity, behind the button -- the sound "coming up from the floor" of the
 * screen. One bar per FFT band. Sized for the 448 px landscape width. */
#define SPEC_BARS  AUDIO_SPECTRUM_BANDS
#define SPEC_BAR_W 10
#define SPEC_STEP  16   /* bar pitch; SPEC_BARS * SPEC_STEP ~ screen width */
#define SPEC_H     230  /* max bar height -- tall, so speech is unmistakable */
#define SPEC_MIN   0    /* zero at silence */
/* Push the whole bar field a few px below the screen edge, so a short (or zero)
 * bar is clipped off-screen entirely -- no stub or shadow lingering along the
 * bottom when the room is quiet. A bar has to be taller than this to show. */
#define SPEC_SINK  14
#define ICON_HIT 72   /* transparent touch target; the glyph is smaller than the tap */
#define EDGE 16       /* inset from the rounded corners of the panel */

/* AMOLED: a true-black top costs no power and gives the gradient somewhere to
 * come from. Four stops rather than two, because two across 448 px bands
 * visibly on a panel this contrasty. */
#define C_BG_0 0x000000
#define C_BG_1 0x060B12
#define C_BG_2 0x0D1826
#define C_BG_3 0x16283A
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
    uint8_t spectrum[SPEC_BARS];
    uint32_t clip_ms;
    bool wifi;
    bool usb;
    char msg[80];
    bool msg_is_error;
    bool pending; /* a dictation is typed and can be sent or undone */
} ui_model_t;

static ui_model_t s_model;
static SemaphoreHandle_t s_lock;

static lv_obj_t *s_main, *s_setup;
static lv_obj_t *s_btn, *s_mic, *s_timer, *s_spinner, *s_status, *s_halo;
static lv_obj_t *s_ico_usb, *s_ico_wifi, *s_badge, *s_badge_hit;
static lv_obj_t *s_send_hit, *s_undo_hit; /* bottom-corner Send / Undo actions */
static lv_obj_t *s_sheet, *s_sheet_text;
static lv_obj_t *s_spec[SPEC_BARS];

/* Smoothed on-screen bar heights (fast attack, slow decay). Touched only in the
 * LVGL tick, so no lock; the raw FFT targets live in the model. */
static uint8_t s_spec_shown[SPEC_BARS];

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

void ui_set_spectrum(const uint8_t *bands)
{
    model_lock();
    memcpy(s_model.spectrum, bands, SPEC_BARS);
    model_unlock();
}
void ui_set_wifi(bool connected) { MODEL_SET(wifi, connected); }
void ui_set_usb(bool connected) { MODEL_SET(usb, connected); }
void ui_set_pending(bool pending) { MODEL_SET(pending, pending); }

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

/* ------------------------------------------------------------ screen power
 *
 * An always-on OLED both burns power and risks burn-in, so the panel fades to
 * black after a stretch with no touch, and the first touch afterwards only
 * wakes it -- it does not also start a recording. All of this state lives in the
 * LVGL task (touch callbacks and ui_tick both run there), so no lock. */
#define SCREEN_DIM_AFTER_MS 45000 /* idle time before the screen fades out */
#define SCREEN_STEP_SLEEP   5     /* brightness % per 60 ms tick, fading out */
#define SCREEN_STEP_WAKE    20    /* faster coming back, so a tap feels instant */

static int64_t s_last_touch_us;
static int     s_bright_cur = 100;
static int     s_bright_tgt = 100;
static bool    s_asleep;         /* dimming or dark: the next tap only wakes */
static bool    s_swallow_release;

/* Returns true if this touch was consumed purely to wake the screen. */
static bool woke_from_touch(void)
{
    s_last_touch_us = esp_timer_get_time();
    if (s_asleep) {
        s_bright_tgt = 100;
        s_asleep = false;
        return true;
    }
    return false;
}

/* Called every tick with the current state. Fades the panel toward its target
 * and, once idle long enough, sets that target to black. */
static void screen_power_tick(app_state_t state)
{
    const bool active = (state == ST_RECORDING || state == ST_UPLOADING || state == ST_TYPING);
    if (active) {
        s_last_touch_us = esp_timer_get_time(); /* work in progress keeps it lit */
        s_bright_tgt = 100;
        s_asleep = false;
    } else if (!s_asleep) {
        const int64_t idle_ms = (esp_timer_get_time() - s_last_touch_us) / 1000;
        if (idle_ms >= SCREEN_DIM_AFTER_MS) {
            s_bright_tgt = 0;
            s_asleep = true; /* set as soon as the fade starts, so a tap mid-fade wakes */
        }
    }

    if (s_bright_cur != s_bright_tgt) {
        const int step = (s_bright_tgt > s_bright_cur) ? SCREEN_STEP_WAKE : -SCREEN_STEP_SLEEP;
        s_bright_cur += step;
        if ((step > 0) == (s_bright_cur > s_bright_tgt)) {
            s_bright_cur = s_bright_tgt; /* overshoot -> clamp to target */
        }
        bsp_display_brightness_set(s_bright_cur);
    }
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
    diag_beat_ui(); /* the only proof this task is still alive */

    ui_model_t m;
    model_lock();
    m = s_model;
    model_unlock();

    if (m.state == ST_PROVISIONING) {
        return; /* the setup screen is static, and must stay lit to be scanned */
    }

    /* Fade the panel in/out for idle power saving. When it has gone fully dark
     * there is nothing to draw, so skip the rest of the tick -- the animations
     * would only burn CPU rendering to a black screen. The heartbeat above
     * already ran, so the watchdog still sees the task alive. */
    screen_power_tick(m.state);
    if (s_asleep && s_bright_cur == 0) {
        return;
    }

    static int last_rec = -1; /* neither true nor false, so the first tick paints */
    static int last_wifi = -1, last_usb = -1;
    static uint32_t last_timer_tenths = UINT32_MAX;
    static char last_msg[sizeof(m.msg)] = {1};

    const bool rec = (m.state == ST_RECORDING);
    const bool busy =
        (m.state == ST_UPLOADING || m.state == ST_TYPING || m.state == ST_SENDING);

    if ((int)rec != last_rec) {
        last_rec = rec;
        /* Swap the whole depth-shaded disc: green idle, red recording. */
        lv_obj_set_style_bg_image_src(s_btn, rec ? &btn_rec : &btn_idle, LV_PART_MAIN);
    }

    /* The centre of the button shows exactly one thing at a time: the mic says
     * "press me", the running clock says "I am listening", the spinner says
     * "working on it". Showing two at once was the old bug -- the mic's foot
     * bar sat exactly where the timer drew, so the timer looked absent. */
    enum { CENTRE_MIC, CENTRE_TIMER, CENTRE_SPINNER };
    static int last_centre = -1;
    const int centre = rec ? CENTRE_TIMER : (busy ? CENTRE_SPINNER : CENTRE_MIC);
    if (centre != last_centre) {
        last_centre = centre;
        if (centre == CENTRE_MIC) {
            lv_obj_remove_flag(s_mic, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_mic, LV_OBJ_FLAG_HIDDEN);
        }
        if (centre == CENTRE_TIMER) {
            lv_obj_remove_flag(s_timer, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_timer, LV_OBJ_FLAG_HIDDEN);
            last_timer_tenths = UINT32_MAX;
        }
        if (centre == CENTRE_SPINNER) {
            lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* While a clip is uploading, being typed, or being sent, the button is
     * inert: the state machine ignores a press in those states, but a bright
     * button invites one. Dim it and drop LV_OBJ_FLAG_CLICKABLE so it neither
     * reacts nor lights up until the work has landed. */
    static int last_busy = -1;
    if ((int)busy != last_busy) {
        last_busy = busy;
        lv_obj_set_style_opa(s_btn, busy ? LV_OPA_40 : LV_OPA_COVER, LV_PART_MAIN);
        if (busy) {
            lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    if (rec) {
        /* Tenths, so the readout visibly moves every 100 ms. A seconds-only
         * counter sits still for a whole second at a time, which reads as "no
         * feedback" -- the complaint this fixes. */
        const uint32_t tenths = m.clip_ms / 100;
        if (tenths != last_timer_tenths) {
            last_timer_tenths = tenths;
            lv_label_set_text_fmt(s_timer, "%u.%us", (unsigned)(tenths / 10),
                                  (unsigned)(tenths % 10));
        }
    }

    /* Halo: a slow triangle-wave breath in opacity. Brighter and quicker while
     * recording so the glow swells with the red button. The halo sits behind the
     * opaque button, so only the ring spilling past its edge is redrawn. */
    static int halo_v = 60, halo_dir = 1;
    const int halo_hi = rec ? 210 : 150; /* peak opacity, of 255 */
    const int halo_lo = rec ? 90 : 55;
    const int halo_step = rec ? 8 : 4;
    halo_v += halo_dir * halo_step;
    if (halo_v >= halo_hi) { halo_v = halo_hi; halo_dir = -1; }
    if (halo_v <= halo_lo) { halo_v = halo_lo; halo_dir = 1; }
    lv_obj_set_style_opa(s_halo, (lv_opa_t)halo_v, LV_PART_MAIN);
    lv_obj_set_style_image_recolor(s_halo, lv_color_hex(rec ? C_REC_TOP : C_ON), LV_PART_MAIN);

    /* Spectrum: each bar chases its FFT band. Fast attack, slow decay -- it
     * snaps up to a transient and eases back down, which is what makes an
     * analyser read as one. m.spectrum holds the latest FFT frame. */
    for (int i = 0; i < SPEC_BARS; i++) {
        const uint8_t tgt = m.spectrum[i];
        if (tgt >= s_spec_shown[i]) {
            s_spec_shown[i] = tgt;                       /* attack: jump up */
        } else {
            s_spec_shown[i] -= (s_spec_shown[i] - tgt) / 3 + 1; /* decay: ease down */
        }
        const int h = SPEC_MIN + (s_spec_shown[i] * (SPEC_H - SPEC_MIN)) / 100;
        if (lv_obj_get_height(s_spec[i]) != h) {
            lv_obj_set_height(s_spec[i], h);
        }
    }

    /* Send and Undo only mean anything on a dictation that has landed and not
     * yet been acted on. Outside that window they are dimmed and drop their
     * clickable flag, so a stray tap on the corner does nothing. */
    static int last_live = -1;
    const bool actions_live = (m.state == ST_IDLE_READY && m.pending);
    if ((int)actions_live != last_live) {
        last_live = actions_live;
        const lv_opa_t opa = actions_live ? LV_OPA_COVER : LV_OPA_30;
        lv_obj_set_style_opa(s_send_hit, opa, LV_PART_MAIN);
        lv_obj_set_style_opa(s_undo_hit, opa, LV_PART_MAIN);
        if (actions_live) {
            lv_obj_add_flag(s_send_hit, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(s_undo_hit, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_remove_flag(s_send_hit, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(s_undo_hit, LV_OBJ_FLAG_CLICKABLE);
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
    if (woke_from_touch()) {
        s_swallow_release = true; /* the matching release must not act either */
        return;                   /* a tap on a sleeping screen only wakes it */
    }
    ESP_LOGD(TAG, "touch: press");
    beeper_play(BEEP_PRESS);
    app_sm_post(EV_BTN_PRESS);
}

static void on_release(lv_event_t *e)
{
    (void)e;
    s_last_touch_us = esp_timer_get_time();
    if (s_swallow_release) {
        s_swallow_release = false;
        return;
    }
    ESP_LOGD(TAG, "touch: release");
    beeper_play(BEEP_RELEASE);
    app_sm_post(EV_BTN_RELEASE);
}

static void on_press_lost(lv_event_t *e)
{
    (void)e;
    if (s_swallow_release) {
        s_swallow_release = false;
        return;
    }
    ESP_LOGW(TAG, "touch: press LOST (finger slid off, or the panel stopped reporting)");
    beeper_play(BEEP_RELEASE);
    app_sm_post(EV_PRESS_LOST);
}
static void on_setup(lv_event_t *e) { (void)e; if (woke_from_touch()) return; app_sm_post(EV_ENTER_SETUP); }
static void on_setup_exit(lv_event_t *e) { (void)e; if (woke_from_touch()) return; app_sm_post(EV_SETUP_EXIT); }

/* Diagnostic: logs where a tap landed in LOGICAL (post-rotation) coordinates,
 * but only when it hit no control -- exactly the case where the cog is being
 * missed. If a tap meant for the bottom-right cog logs somewhere else, the touch
 * transform is wrong; if it logs at the cog's position, the hit-test is. */
static void screen_tap_log(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    ESP_LOGI(TAG, "tap (%d,%d) hit no control  [screen is %dx%d]", (int)p.x, (int)p.y,
             (int)lv_display_get_horizontal_resolution(NULL),
             (int)lv_display_get_vertical_resolution(NULL));
}

/* Send and Undo click (not press/release): they are one-shot taps, not a
 * hold. The beep confirms the finger; the state machine decides whether there
 * is anything to act on. */
/* Like the button, a tap on a sleeping screen only wakes it -- it must not fire
 * Send or Undo. */
static void on_send(lv_event_t *e) { (void)e; if (woke_from_touch()) return; beeper_play(BEEP_PRESS); app_sm_post(EV_SEND); }
static void on_undo(lv_event_t *e) { (void)e; if (woke_from_touch()) return; beeper_play(BEEP_PRESS); app_sm_post(EV_UNDO); }




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

/* The microphone is a baked, anti-aliased A8 mask (tools/bake_mic.py), recoloured
 * to INK so the one image reads on both the green idle and red recording discs.
 * The four-primitive version it replaces looked hand-drawn; this has smooth
 * edges and no per-part alignment to drift. */
static void build_mic(lv_obj_t *btn)
{
    s_mic = lv_image_create(btn);
    lv_image_set_src(s_mic, &ic_mic);
    lv_obj_set_style_image_recolor(s_mic, lv_color_hex(C_INK), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(s_mic, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(s_mic, LV_ALIGN_CENTER, 0, -6);
    lv_obj_remove_flag(s_mic, LV_OBJ_FLAG_CLICKABLE); /* the button handles touch */
}

/* A bottom-corner action: a 72 px transparent tap target holding a caption over
 * a glyph, tinted to say what it does. Returns the target so the tick can dim
 * it and drop its clickable flag while there is nothing to act on. */
static lv_obj_t *build_action(lv_align_t align, int dx, int dy, const char *icon,
                              const char *caption, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *hit = lv_obj_create(s_main);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, ICON_HIT, ICON_HIT);
    lv_obj_align(hit, align, dx, dy);
    lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cap = lv_label_create(hit);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *ico = lv_label_create(hit);
    lv_label_set_text(ico, icon);
    lv_obj_set_style_text_font(ico, FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_color(ico, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_align(ico, LV_ALIGN_CENTER, 0, 12);

    return hit;
}

static void build_main(void)
{
    s_main = lv_obj_create(NULL);
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);

    /* A dithered background baked to flash, not a runtime gradient.
     *
     * A live lv_grad in RGB565 bands hard: ~6 distinct blue levels across 448 px,
     * and LVGL 9 dropped the gradient dithering v8 had. tools/bake_bg.py does the
     * ramp at full precision and Floyd-Steinberg dithers it down, so the steps
     * become imperceptible pixel noise. LVGL draws it straight from .rodata --
     * no gradient math, no per-chunk recompute. The top stays true black, so those
     * OLED pixels are simply off. */
    lv_obj_set_style_bg_image_src(s_main, &bg_main, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_main, LV_OPA_COVER, LV_PART_MAIN);

    /* A breathing halo behind the button: a soft mint glow, recoloured from an
     * A8 mask and pulsed in opacity by ui_tick. The button is created next and
     * therefore sits on top, so only the glow spilling past the button's edge
     * shows -- a halo ringing it. */
    s_halo = lv_image_create(s_main);
    lv_image_set_src(s_halo, &ic_halo);
    lv_obj_set_style_image_recolor(s_halo, lv_color_hex(C_ON), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(s_halo, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(s_halo, LV_ALIGN_CENTER, 0, -8);   /* on the button centre */
    lv_obj_remove_flag(s_halo, LV_OBJ_FLAG_CLICKABLE);

    /* ---- the button owns the middle ---- */
    s_btn = lv_button_create(s_main);
    lv_obj_set_size(s_btn, BTN_D, BTN_D);
    lv_obj_align(s_btn, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_border_width(s_btn, 0, LV_PART_MAIN);

    /* The button IS an image now: a depth-shaded disc baked by
     * tools/bake_button.py -- radial sheen, specular highlight, rim, and a soft
     * glow, none of which a runtime shadow could give us (a box-shadow is what
     * froze the device; see build history). The image carries its own circular
     * alpha, so:
     *   - radius stays 0. A CIRCLE radius would clip the bg_image to a hard
     *     circle and shear off the soft glow.
     *   - the object's own fill is transparent; only the image shows.
     *   - no shadow_width, so lv_draw_sw_box_shadow() is never reached. */
    lv_obj_set_style_radius(s_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(s_btn, &btn_idle, LV_PART_MAIN);

    /* Press feedback without a transform or a shadow: darken the image slightly
     * while held. LVGL applies this in the render task on the touch event, so
     * the tick stays free. */
    lv_obj_set_style_bg_image_recolor(s_btn, lv_color_black(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_image_recolor_opa(s_btn, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_add_event_cb(s_btn, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn, on_release, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_btn, on_press_lost, LV_EVENT_PRESS_LOST, NULL);

    /* A real microphone, not LV_SYMBOL_AUDIO -- the built-in font's nearest
     * glyph is a musical note. Drawn from three primitives, the same shapes the
     * web mockup uses: a capsule body, an arc cradle, a stem. No shadow, no
     * large-radius fill, so nothing here can repeat the box-shadow OOM. */
    build_mic(s_btn);

    /* The recording readout replaces the mic in the centre of the button while
     * a clip is being captured. Big and near-white so it reads as the loud,
     * unmistakable "you are recording" signal -- INK-on-red was too quiet. */
    s_timer = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_timer, FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_timer, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_opa(s_timer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_center(s_timer);
    lv_obj_add_flag(s_timer, LV_OBJ_FLAG_HIDDEN);

    /* The spinner takes the mic's place while the device is busy -- a clip
     * transcribing or being typed, or a Send/Undo chord going out over HID.
     * It is a child of the screen, not the button, on purpose: busy
     * also dims the whole button to 40%, and a child would be dragged down
     * with it. Kept outside, the disc reads "inert" while the arc spins at
     * full strength and reads "working". White, like the timer -- the two
     * "something is happening" signals share a colour. */
    s_spinner = lv_spinner_create(s_main);
    lv_spinner_set_anim_params(s_spinner, 1000, 200);
    lv_obj_set_size(s_spinner, 96, 96);
    lv_obj_align(s_spinner, LV_ALIGN_CENTER, 0, -8); /* concentric with the button */
    lv_obj_set_style_arc_width(s_spinner, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_spinner, LV_OPA_20, LV_PART_MAIN); /* faint track */
    lv_obj_set_style_arc_width(s_spinner, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_spinner, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_spinner, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    /* ---- four corners, Apple Watch style ----
     * The panel's corners are rounded, so these are inset diagonally rather
     * than pinned to the literal corner where the glass curves away. */

    s_ico_usb = lv_label_create(s_main);
    lv_label_set_text(s_ico_usb, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(s_ico_usb, FONT_BIG, LV_PART_MAIN);
    lv_obj_align(s_ico_usb, LV_ALIGN_TOP_LEFT, EDGE + 20, EDGE + 12);

    /* Top-right: the Wi-Fi status glyph doubles as the door to setup. Its
     * colour still reports the connection (green up, grey down); wrapping it in
     * a 72 px transparent target makes tapping it open the portal, which is
     * where you go when the connection is the thing that is wrong. This is why
     * the bottom-right cog is gone -- setup lives here now. */
    lv_obj_t *wifi_hit = lv_obj_create(s_main);
    lv_obj_remove_style_all(wifi_hit);
    lv_obj_set_size(wifi_hit, ICON_HIT, ICON_HIT);
    lv_obj_align(wifi_hit, LV_ALIGN_TOP_RIGHT, -EDGE, EDGE);
    lv_obj_remove_flag(wifi_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_hit, on_setup, LV_EVENT_CLICKED, NULL);

    s_ico_wifi = lv_label_create(wifi_hit);
    lv_label_set_text(s_ico_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_ico_wifi, FONT_BIG, LV_PART_MAIN);
    lv_obj_center(s_ico_wifi);

    /* The spectrum analyser: one bar per FFT band, spanning the full width and
     * anchored to the bottom edge so the bars rise from the floor of the screen.
     * Low opacity, and moved behind the button so it reads as ambient rather
     * than a widget competing with the mic. */
    const int total = SPEC_BARS * SPEC_STEP;
    lv_obj_t *spec = lv_obj_create(s_main);
    lv_obj_remove_style_all(spec);
    lv_obj_set_size(spec, total, SPEC_H);
    /* Offset down by SPEC_SINK so the baseline sits just off-screen. */
    lv_obj_align(spec, LV_ALIGN_BOTTOM_MID, 0, SPEC_SINK);
    lv_obj_remove_flag(spec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spec, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < SPEC_BARS; i++) {
        lv_obj_t *b = lv_obj_create(spec);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, SPEC_BAR_W, SPEC_MIN);
        /* Anchored bottom so growing the height makes the bar climb upward. */
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * SPEC_STEP, 0);
        lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ON), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_PART_MAIN); /* ambient, not loud */
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE); /* never intercept the cog */
        s_spec[i] = b;
    }
    lv_obj_move_background(spec); /* behind the button and every control */

    /* The bottom corners are the two things you do with a dictation once it has
     * landed: Undo it (backspace the whole sentence away) on the left, Send it
     * (strike the configured chord -- Enter, or a modifier + Enter) on the
     * right, where the affirmative action of a dialog sits. Both start dimmed
     * and inert; the tick lights them the moment a transcript finishes typing,
     * and dims them again once it is acted on. */
    s_undo_hit = build_action(LV_ALIGN_BOTTOM_LEFT, EDGE, -EDGE, LV_SYMBOL_BACKSPACE, "Undo",
                              C_MSG, on_undo);
    s_send_hit = build_action(LV_ALIGN_BOTTOM_RIGHT, -EDGE, -EDGE, LV_SYMBOL_NEW_LINE, "Send",
                              C_ON, on_send);
    lv_obj_set_style_opa(s_send_hit, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_opa(s_undo_hit, LV_OPA_30, LV_PART_MAIN);

    /* ---- transient status, and the error badge ----
     * Status ("Transcribing...", "Typing...") sits along the bottom on the same
     * row as the cog, left side -- a log line beneath the analyser, not floating
     * beside the button. */
    s_status = lv_label_create(s_main);
    lv_obj_set_style_text_color(s_status, lv_color_hex(C_STATUS), LV_PART_MAIN);
    lv_obj_set_width(s_status, 220);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_LEFT, EDGE, -(EDGE + 26));
    lv_obj_move_foreground(s_status);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

    s_badge_hit = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_badge_hit);
    lv_obj_set_size(s_badge_hit, ICON_HIT, ICON_HIT);
    lv_obj_align(s_badge_hit, LV_ALIGN_TOP_MID, 0, EDGE - 4);
    lv_obj_add_flag(s_badge_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_badge_hit, sheet_open, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_badge_hit, LV_OBJ_FLAG_HIDDEN);

    s_badge = lv_label_create(s_main);
    lv_label_set_text(s_badge, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(s_badge, FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_badge, lv_color_hex(C_MSG), LV_PART_MAIN);
    lv_obj_align(s_badge, LV_ALIGN_TOP_MID, 0, EDGE + 10);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    build_sheet();
}

void ui_show_setup(const prov_info_t *info, bool can_exit)
{
    /* A cold first paint can hold the LVGL lock for a while. Giving up here
     * leaves the user staring at a half-drawn main screen with no way in. */
    if (!bsp_display_lock(10000)) {
        ESP_LOGE(TAG, "could not lock display for setup screen");
        return;
    }

    s_setup = lv_obj_create(NULL);
    lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_SCROLLABLE);
    /* The same dithered background as the main screen, so setup looks like part
     * of the same device rather than a bare fallback. */
    lv_obj_set_style_bg_image_src(s_setup, &bg_main, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_setup, LV_OPA_COVER, LV_PART_MAIN);

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

    /* A way out -- but only when setup was opened from the cog on a working
     * device. At first boot there is nowhere to go back to, so no button: the
     * only exit then is finishing setup. Back reboots, which reconnects and
     * lands on the main screen. */
    if (can_exit) {
        lv_obj_t *back = lv_button_create(s_setup);
        lv_obj_set_size(back, 84, 40);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, EDGE, EDGE);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x1B232B), LV_PART_MAIN);
        lv_obj_set_style_radius(back, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(back, on_setup_exit, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(back);
        lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
        lv_obj_set_style_text_color(bl, lv_color_hex(0xE8EDF2), LV_PART_MAIN);
        lv_obj_center(bl);
    }

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
    lv_obj_add_event_cb(s_main, screen_tap_log, LV_EVENT_PRESSED, NULL);
    lv_screen_load(s_main);
    /* 60 ms: fast enough that the wave travels smoothly, slow enough that the
     * QSPI flush is not the busiest thing on the board. */
    lv_timer_create(ui_tick, 60, NULL);
    bsp_display_unlock();

    ESP_LOGI(TAG, "ui up");
    return ESP_OK;
}
