#include "app_sm.h"

#include "audio_capture.h"
#include "beeper.h"
#include "diagnostics.h"
#include "hid_kbd.h"
#include "stt_client.h"
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
        /* A host that has enumerated us, not merely a cable delivering power. */
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
            app_sm_post(EV_PMIC_OK);
        } else {
            ui_set_error("PMIC not found");
            app_sm_post(EV_PMIC_FAIL);
        }
    }
    if (actions & ACT_PROV_START) {
        prov_info_t info;
        if (provisioning_start(&info) == ESP_OK) {
            ui_show_setup(&info, config_is_provisioned(&s_cfg));
        } else {
            ui_set_error("Setup AP failed");
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
        /* A new recording clears whatever went wrong last time. */
        ui_clear_msg();
        audio_record_begin();
    }
    if (actions & ACT_REC_STOP) {
        audio_record_end();
    }
    if (actions & ACT_UPLOAD_START) {
        size_t total = 0;
        const uint8_t *wav = audio_clip_wav(&total);
        ui_set_status("Transcribing...");
        if (stt_start(s_cfg.api_key, wav, total) != ESP_OK) {
            ui_set_error(stt_error());
            app_sm_post(EV_STT_FAIL);
        }
    }
    if (actions & ACT_UPLOAD_ABORT) {
        stt_abort();
    }
    if (actions & ACT_TYPE_START) {
        ui_set_status("Typing...");
        if (hid_kbd_type(stt_transcript()) != ESP_OK) {
            app_sm_post(EV_TYPE_ABORT);
        }
    }
    if (actions & ACT_TYPE_ABORT) {
        hid_kbd_abort();
    }
    if (actions & ACT_HINT_QUIET) {
        /* Before ACT_CLIP_DISCARD below wipes the reason. This is what turns
         * "button goes red, then nothing" into an explanation. */
        beeper_play(BEEP_ERROR);
        ui_set_error(audio_clip_reject_reason());
    }
    if (actions & ACT_CLIP_DISCARD) {
        audio_clip_discard();
    }
    if (actions & ACT_HINT_NOT_READY) {
        beeper_play(BEEP_ERROR);
        ui_set_error(!s_usb                      ? "Not plugged into a computer"
                   : !s_wifi_up                  ? "Wi-Fi not connected"
                   : !config_has_api_key(&s_cfg) ? "No API key - tap Setup"
                                                 : "Clock not synced");
    }
    if (actions & ACT_SHOW_ERROR) {
        beeper_play(BEEP_ERROR);
        /* Prefer the real reason over a generic one. */
        ui_set_error(!config_is_provisioned(&s_cfg) ? "Not configured - tap Setup"
                   : stt_error()[0] != '\0'      ? stt_error()
                                                 : "Error - tap Setup or wait");
    }
    if (actions & ACT_REBOOT) {
        ESP_LOGI(TAG, "restarting to apply configuration");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}

static void sm_task(void *arg)
{
    (void)arg;

    /* Kick the machine out of ST_BOOT. */
    app_sm_post(EV_NONE);

    int error_ticks = 0;

    for (;;) {
        app_event_t ev;
        diag_beat_sm();
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(TICK_MS)) != pdTRUE) {
            /* Idle tick.
             *
             * The record GUARD tracks tud_mounted(): an enumerated host is what
             * makes typing possible. The ICON tracks the PMIC's VBUS-good bit,
             * because tud_mounted() is configured bus-powered and never reports
             * a cable unplug on a board that keeps running on battery -- so the
             * icon would otherwise stay lit forever. Two signals, two meanings. */
            const bool usb_now = hid_kbd_mounted();
            if (usb_now != s_usb) {
                app_sm_post(usb_now ? EV_USB_MOUNT : EV_USB_UNMOUNT);
            }
            ui_set_usb(pmic_vbus_present());

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
            ESP_LOGI(TAG, "%s --%s--> %s (actions 0x%04x) [wifi=%d time=%d usb=%d clip=%d]",
                     sm_state_name(s_state), sm_event_name(ev), sm_state_name(out.next),
                     (unsigned)out.actions, g.wifi_up, g.time_ok, g.usb_mounted, g.clip_usable);
        }

        s_state = out.next;
        ui_set_state(s_state);

        /* Outcomes the action mask cannot express: a finished burst should
         * clear "Typing…", and a silent clip deserves to say so rather than
         * look like nothing happened. */
        if (ev == EV_TYPE_DONE) {
            ui_clear_msg();
        } else if (ev == EV_STT_EMPTY) {
            ui_set_status("No speech detected");
        }

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
