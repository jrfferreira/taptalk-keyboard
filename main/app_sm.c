#include "app_sm.h"

#include "audio_capture.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "pmic.h"
#include "provisioning.h"
#include "ui.h"

static const char *TAG = "sm";

#define EVENT_QUEUE_DEPTH 16
#define TICK_MS 500
#define ERROR_AUTO_CLEAR_TICKS (4000 / TICK_MS)

static QueueHandle_t s_queue;
static app_state_t s_state = ST_BOOT; /* written only by the sm task */
static app_config_t s_cfg;

/* Latched from the events themselves, so a guard reflects exactly what the
 * state machine has been told rather than whatever a driver happens to report
 * at the moment it is asked. Written only by the sm task. */
static bool s_wifi_up, s_time_ok, s_usb;

app_state_t app_sm_state(void) { return s_state; }

void app_sm_post(app_event_t event)
{
    if (s_queue == NULL) {
        return;
    }
    /* Never block: the caller may be the LVGL render task, a Wi-Fi event
     * handler, or an HTTP worker, none of which may stall. A dropped event is
     * recoverable; a stalled render task is not. */
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped event %d", (int)event);
    }
}

static sm_guards_t current_guards(void)
{
    sm_guards_t g = {
        .provisioned = config_is_provisioned(&s_cfg),
        .wifi_up     = s_wifi_up,
        .time_ok     = s_time_ok,
        /* Chunk 1 ships no USB HID, so there is no tud_mounted() to ask. VBUS
         * presence from the PMU is the honest stand-in: no cable, no host to
         * type into. hid_kbd.c replaces this in chunk 2. */
        .usb_mounted  = s_usb,
        .clip_usable  = audio_clip_usable(),
        .wifi_retries = net_wifi_retries(),
    };
    return g;
}

static void run_actions(uint32_t actions)
{
    if (actions & ACT_PMIC_INIT) {
        pmic_status_t st;
        const esp_err_t err = pmic_init(&st);
        if (err == ESP_OK) {
            if (!st.aldo1_on) {
                ESP_LOGE(TAG, "ALDO1 still off after enable; the microphone will be silent");
            }
            /* Seed the cable state before anything can consult the guard. */
            s_usb = pmic_vbus_present();
            ui_set_usb(s_usb);
            app_sm_post(EV_PMIC_OK);
        } else {
            ui_set_msg("PMIC not found");
            app_sm_post(EV_PMIC_FAIL);
        }
    }
    if (actions & ACT_PROV_START) {
        prov_info_t info;
        if (provisioning_start(&info) == ESP_OK) {
            ui_show_setup(&info);
        } else {
            ui_set_msg("Setup AP failed");
        }
    }
    if (actions & (ACT_WIFI_START | ACT_WIFI_RETRY)) {
        if (net_wifi_sta_connect(&s_cfg) != ESP_OK) {
            app_sm_post(EV_WIFI_DOWN);
        }
    }
    if (actions & ACT_SNTP_START) {
        net_sntp_start();
    }
    if (actions & ACT_REC_START) {
        ui_set_msg("");
        audio_record_begin();
    }
    if (actions & ACT_REC_STOP) {
        audio_record_end();
    }
    if (actions & ACT_UPLOAD_START) {
        /* Chunk 2 replaces this with the multipart HTTPS client. Reporting the
         * clip and returning to idle keeps the flow honest in the meantime
         * rather than faking a transcript. */
        ESP_LOGI(TAG, "clip ready: %u ms, peak %d (upload lands in chunk 2)",
                 (unsigned)audio_clip_ms(), audio_clip_peak());
        char msg[64];
        snprintf(msg, sizeof(msg), "Recorded %.1fs, peak %d", audio_clip_ms() / 1000.0f,
                 audio_clip_peak());
        ui_set_msg(msg);
        app_sm_post(EV_STT_EMPTY);
    }
    if (actions & ACT_CLIP_DISCARD) {
        audio_clip_discard();
    }
    if (actions & ACT_HINT_NOT_READY) {
        ui_set_msg(!s_usb      ? "No USB cable"
                   : !s_wifi_up ? "Wi-Fi not connected"
                                : "Clock not synced");
    }
    if (actions & ACT_SHOW_ERROR) {
        ui_set_msg(config_is_provisioned(&s_cfg) ? "Error - tap Setup or wait"
                                                 : "Not configured - tap Setup");
    }
    if (actions & ACT_REBOOT) {
        ESP_LOGI(TAG, "restarting to apply configuration");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    /* ACT_UPLOAD_ABORT / ACT_TYPE_START / ACT_TYPE_ABORT are unreachable in
     * chunk 1: there is no network client and no HID. Deliberately not
     * stubbed, so the compiler cannot hide their absence later. */
}

static void sm_task(void *arg)
{
    (void)arg;

    /* Kick the machine out of ST_BOOT. */
    app_sm_post(EV_NONE);

    int error_ticks = 0;

    for (;;) {
        app_event_t ev;
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(TICK_MS)) != pdTRUE) {
            /* Idle tick. Poll the cable: no VBUS means no host to type into,
             * so recording is pointless and the USB icon must say so. */
            const bool usb_now = pmic_vbus_present();
            if (usb_now != s_usb) {
                ui_set_usb(usb_now);
                app_sm_post(usb_now ? EV_USB_MOUNT : EV_USB_UNMOUNT);
            }

            /* An error state clears itself so the device does not strand the
             * user on a dead screen — unless it is unprovisioned, in which
             * case sm_step() keeps it there on purpose. */
            if (s_state == ST_ERROR && ++error_ticks >= ERROR_AUTO_CLEAR_TICKS) {
                error_ticks = 0;
                ev = EV_TIMEOUT;
            } else {
                continue;
            }
        }
        if (s_state != ST_ERROR) {
            error_ticks = 0;
        }

        /* Latch the preconditions before consulting the guards. */
        if (ev == EV_WIFI_UP) {
            s_wifi_up = true;
        } else if (ev == EV_WIFI_DOWN) {
            s_wifi_up = false;
            s_time_ok = false; /* the clock will drift; resync on reconnect */
        }
        if (ev == EV_TIME_OK) s_time_ok = true;
        if (ev == EV_TIME_FAIL) s_time_ok = false;
        if (ev == EV_USB_MOUNT) s_usb = true;
        if (ev == EV_USB_UNMOUNT) s_usb = false;

        const sm_guards_t g = current_guards();
        const sm_out_t out  = sm_step(s_state, ev, &g);

        if (out.next != s_state || out.actions != ACT_NONE) {
            ESP_LOGI(TAG, "%s --%d--> %s (actions 0x%04x)", sm_state_name(s_state), (int)ev,
                     sm_state_name(out.next), (unsigned)out.actions);
        }

        s_state = out.next;
        ui_set_state(s_state);
        run_actions(out.actions);
    }
}

void app_sm_start(const app_config_t *cfg)
{
    s_cfg = *cfg;

    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(app_event_t));
    configASSERT(s_queue != NULL);
    /* Core 1, alongside LVGL and audio; core 0 belongs to the radio. */
    xTaskCreatePinnedToCore(sm_task, "app_sm", 5120, NULL, 5, NULL, 1);
}
