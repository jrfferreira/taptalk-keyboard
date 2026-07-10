#include "hid_kbd.h"

#include <string.h>

#include "app_sm.h"
#include "core/keymap.h"
#include "core/typeplan.h"
#include "core/textnorm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hid";

#define TYPE_CAP 1200

static char s_text[TYPE_CAP];
static TaskHandle_t s_task;
static volatile bool s_abort;

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
    size_t i = 0, skipped = 0;

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

        if (!emit(cp, &skipped)) {
            app_sm_post(EV_TYPE_ABORT);
            return;
        }
    }

#if CONFIG_TAPTALK_TRAILING_SPACE
    /* Lets you dictate several phrases without them running together. Never
     * Enter: that would submit whatever form the cursor is sitting in. */
    (void)emit(' ', &skipped);
#endif

    if (skipped > 0) {
        ESP_LOGW(TAG, "%u characters could not be typed on the US layout", (unsigned)skipped);
    }
    ESP_LOGI(TAG, "typed %u bytes", (unsigned)len);
    app_sm_post(EV_TYPE_DONE);
}

static void type_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_abort = false;
        type_out(s_text);
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
        ESP_LOGW(TAG, "HID disabled; would have typed: \"%s\"", s_text);
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
    snprintf(s_text, sizeof(s_text), "%s", utf8);
    s_abort = false;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void hid_kbd_abort(void) { s_abort = true; }
