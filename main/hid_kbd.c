#include "hid_kbd.h"

#include <string.h>

#include "app_sm.h"
#include "core/keymap.h"
#include "core/typeplan.h"
#include "core/textnorm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stt_client.h"

static const char *TAG = "hid";

/* Sized from the transcript cap so a max-length dictation is never cut here;
 * lives in PSRAM because internal RAM is the scarce resource. */
#define TYPE_CAP STT_TRANSCRIPT_CAP

/* The typing task does one of three things per wake-up. s_op says which; it is
 * set before the notify and read once at the top of the loop, so a single
 * writer at a time is guaranteed by the state machine that drives it. */
typedef enum { OP_TYPE, OP_SEND, OP_UNDO } hid_op_t;

static char *s_text; /* TYPE_CAP bytes, allocated on first hid_kbd_type() */
static TaskHandle_t s_task;
static volatile bool s_abort;
static volatile hid_op_t s_op;
static uint8_t s_send_mod, s_send_key; /* the chord OP_SEND strikes */
static int s_last_typed_units;         /* cursor advances from the last OP_TYPE, for Undo */

#if CONFIG_TAPTALK_ENABLE_HID

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"

#define HID_RID_KBD 1
#define KEY_MS CONFIG_TAPTALK_KEY_INTERVAL_MS
/* If the endpoint never frees up, the host is gone or wedged. */
#define READY_TIMEOUT_MS 500

/* ------------------------------------------------------------ descriptors */

static const uint8_t s_hid_report_desc[] = {TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_RID_KBD))};

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_HID, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_HID       0x83

#define CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CFG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    /* bInterval 5 ms: two reports per keystroke, so this bounds typing speed.
     * Lower than the host polls and keys are dropped. */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 5, false, sizeof(s_hid_report_desc), EPNUM_HID, 16, 5),
};

static const tusb_desc_device_t s_desc_device = {
    .bLength         = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB          = 0x0200,
    /* Composite device: the host must read the IAD to find both interfaces. */
    .bDeviceClass    = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor        = 0x303A, /* Espressif */
    .idProduct       = 0x4004,
    .bcdDevice       = 0x0100,
    .iManufacturer   = 1,
    .iProduct        = 2,
    .iSerialNumber   = 3,
    .bNumConfigurations = 1,
};

static const char *s_str_desc[] = {
    (const char[]){0x09, 0x04}, /* 0: English (US) */
    "TapTalk",                  /* 1 */
    "TapTalk Keyboard",         /* 2 */
    "000001",                   /* 3 */
    "TapTalk Console",          /* 4: CDC */
    "TapTalk Keyboard",         /* 5: HID */
};

/* TinyUSB calls these from its own task. */
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    /* The host telling us about Caps Lock and friends. We do not care. */
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

bool hid_kbd_mounted(void) { return tud_mounted(); }

/* ---------------------------------------------------------------- sending */

/* The typing task is the ONLY caller of tud_hid_*. Two tasks racing into the
 * HID endpoint is a documented crash (espressif/esp-idf#9691); one writer,
 * guarded by tud_hid_ready(), removes the race by construction. */
static bool wait_ready(void)
{
    for (int i = 0; i < READY_TIMEOUT_MS; i++) {
        if (!tud_mounted()) {
            return false;
        }
        if (tud_hid_ready()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGW(TAG, "HID endpoint never became ready");
    return false;
}

static bool send_frame(const hid_step_t *f)
{
    if (!wait_ready()) {
        return false;
    }
    uint8_t kc[6] = {f->key, 0, 0, 0, 0, 0};
    tud_hid_keyboard_report(HID_RID_KBD, f->mod, kc);
    vTaskDelay(pdMS_TO_TICKS(KEY_MS));
    return true;
}

/* core/typeplan.c decides which reports a character becomes, including the
 * release frame that keeps "aa" from arriving as "a". This function only puts
 * them on the wire. */
static bool emit(uint32_t codepoint, size_t *skipped)
{
    hid_frames_t plan;
    if (typeplan_char(&keymap_us, codepoint, &plan) == KEYMAP_SKIPPED) {
        (*skipped)++;
        return true; /* dropped, not fatal */
    }
    for (uint8_t i = 0; i < plan.n; i++) {
        if (!send_frame(&plan.frames[i])) {
            return false;
        }
    }
    return true;
}

static void type_out(const char *s)
{
    const size_t len = strlen(s);
    size_t i = 0, skipped = 0, chars = 0;

    while (i < len) {
        if (s_abort || !tud_mounted()) {
            ESP_LOGW(TAG, "typing aborted at byte %u/%u", (unsigned)i, (unsigned)len);
            app_sm_post(EV_TYPE_ABORT);
            return;
        }

        uint32_t cp;
        const size_t used = utf8_next(s + i, len - i, &cp);
        if (used == 0) {
            break;
        }
        i += used;
        chars++;

        if (!emit(cp, &skipped)) {
            app_sm_post(EV_TYPE_ABORT);
            return;
        }
    }

    /* One backspace undoes one grapheme, so Undo counts characters that landed
     * on screen, not bytes or keystrokes: a skipped codepoint typed nothing,
     * and a dead-key accent is two strikes but a single character. */
    size_t units = chars - skipped;

#if CONFIG_TAPTALK_TRAILING_SPACE
    /* Lets you dictate several phrases without them running together. Never
     * Enter: that would submit whatever form the cursor is sitting in -- Send
     * is the deliberate way to do that now. The space is part of what was
     * typed, so Undo must delete it too. */
    (void)emit(' ', &skipped);
    units++;
#endif

    s_last_typed_units = (int)units;

    if (skipped > 0) {
        ESP_LOGW(TAG, "%u characters could not be typed on the US layout", (unsigned)skipped);
    }
    ESP_LOGI(TAG, "typed %u bytes (%u undoable characters)", (unsigned)len, (unsigned)units);
    app_sm_post(EV_TYPE_DONE);
}

/* Send: one chord, held and released. Reports back with the typing events so
 * the state machine leaves ST_SENDING the same way it leaves ST_TYPING. */
static void send_chord(uint8_t mod, uint8_t key)
{
    const hid_step_t press   = {.mod = mod, .key = key};
    const hid_step_t release = {.mod = 0, .key = 0};
    if (s_abort || !send_frame(&press) || !send_frame(&release)) {
        ESP_LOGW(TAG, "send chord mod=0x%02x key=0x%02x aborted", mod, key);
        app_sm_post(EV_TYPE_ABORT);
        return;
    }
    ESP_LOGI(TAG, "sent chord mod=0x%02x key=0x%02x", mod, key);
    app_sm_post(EV_TYPE_DONE);
}

/* Undo: one Backspace per character the last dictation put on screen. */
static void undo_backspaces(void)
{
    const int n = s_last_typed_units;
    const hid_step_t press   = {.mod = 0, .key = HID_KEY_BACKSPACE};
    const hid_step_t release = {.mod = 0, .key = 0};

    for (int k = 0; k < n; k++) {
        if (s_abort || !tud_mounted()) {
            ESP_LOGW(TAG, "undo aborted at %d/%d", k, n);
            app_sm_post(EV_TYPE_ABORT);
            return;
        }
        if (!send_frame(&press) || !send_frame(&release)) {
            app_sm_post(EV_TYPE_ABORT);
            return;
        }
    }

    /* A dictation can be undone once. Clearing the count keeps a second Undo
     * from eating into whatever the user typed by hand afterwards. */
    s_last_typed_units = 0;
    ESP_LOGI(TAG, "undid %d characters", n);
    app_sm_post(EV_TYPE_DONE);
}

static void type_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_abort = false;
        switch (s_op) {
        case OP_SEND: send_chord(s_send_mod, s_send_key); break;
        case OP_UNDO: undo_backspaces(); break;
        case OP_TYPE:
        default:      type_out(s_text); break;
        }
    }
}

esp_err_t hid_kbd_start(void)
{
    /* esp_tinyusb 2.x: descriptors live in a nested struct, and the port and
     * task settings are explicit. The 1.x flat config does not compile here. */
    const tinyusb_config_t cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy  = {.skip_setup = false, .self_powered = false},
        .task = {.size = 4096, .priority = 5, .xCoreID = 0},
        .descriptor =
            {
                .device            = &s_desc_device,
                .string            = s_str_desc,
                .string_count      = sizeof(s_str_desc) / sizeof(s_str_desc[0]),
                .full_speed_config = s_cfg_desc,
            },
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG, "tinyusb install");

    const tinyusb_config_cdcacm_t acm = {.cdc_port = TINYUSB_CDC_ACM_0};
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm), TAG, "cdc init");

    /* From here the log comes out of the composite CDC interface, not the
     * USB-Serial-JTAG port that just disappeared. Reattach your monitor. */
    ESP_RETURN_ON_ERROR(tinyusb_console_init(TINYUSB_CDC_ACM_0), TAG, "cdc console");

    /* Core 0, alongside the TinyUSB task itself. */
    ESP_RETURN_ON_FALSE(
        xTaskCreatePinnedToCore(type_task, "hid_type", 4096, NULL, 5, &s_task, 0) == pdPASS,
        ESP_FAIL, TAG, "task");

    ESP_LOGI(TAG, "USB composite up: HID keyboard + CDC console");
    return ESP_OK;
}

#else /* !CONFIG_TAPTALK_ENABLE_HID */

/* Bring-up build: no TinyUSB, so the USB-Serial-JTAG console survives. */
bool hid_kbd_mounted(void) { return true; }

static void type_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        switch (s_op) {
        case OP_SEND:
            ESP_LOGW(TAG, "HID disabled; would have sent chord mod=0x%02x key=0x%02x", s_send_mod,
                     s_send_key);
            break;
        case OP_UNDO:
            ESP_LOGW(TAG, "HID disabled; would have undone %d characters", s_last_typed_units);
            s_last_typed_units = 0;
            break;
        case OP_TYPE:
        default:
            ESP_LOGW(TAG, "HID disabled; would have typed: \"%s\"", s_text);
            break;
        }
        app_sm_post(EV_TYPE_DONE);
    }
}

esp_err_t hid_kbd_start(void)
{
    ESP_LOGW(TAG, "HID disabled (CONFIG_TAPTALK_ENABLE_HID=n); transcripts are logged only");
    return xTaskCreatePinnedToCore(type_task, "hid_type", 4096, NULL, 5, &s_task, 0) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

#endif

esp_err_t hid_kbd_type(const char *utf8)
{
    if (s_task == NULL || utf8 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_text == NULL) {
        s_text = heap_caps_malloc(TYPE_CAP, MALLOC_CAP_SPIRAM);
        if (s_text == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    snprintf(s_text, TYPE_CAP, "%s", utf8);
    s_op    = OP_TYPE;
    s_abort = false;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t hid_kbd_send(uint8_t mod, uint8_t key)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_send_mod = mod;
    s_send_key = key;
    s_op       = OP_SEND;
    s_abort    = false;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t hid_kbd_undo(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_op    = OP_UNDO;
    s_abort = false;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void hid_kbd_abort(void) { s_abort = true; }
