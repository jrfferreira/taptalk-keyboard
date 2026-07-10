#include "diagnostics.h"

#include <inttypes.h>

#include "app_sm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_kbd.h"

static const char *TAG = "diag";

#define REPORT_MS 5000

static volatile uint32_t s_ui, s_audio, s_sm;

void diag_beat_ui(void) { s_ui++; }
void diag_beat_audio(void) { s_audio++; }
void diag_beat_sm(void) { s_sm++; }

/* The LVGL timer runs at 60 ms and the audio task at 20 ms, so over a 5 s
 * window they should tick ~83 and ~250 times. Zero means wedged, and wedged is
 * the failure that hides behind a healthy-looking log. */
static void report_task(void *arg)
{
    (void)arg;

    uint32_t last_ui = 0, last_audio = 0, last_sm = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_MS));

        const uint32_t ui = s_ui, audio = s_audio, sm = s_sm;
        const uint32_t d_ui = ui - last_ui, d_audio = audio - last_audio, d_sm = sm - last_sm;
        last_ui = ui;
        last_audio = audio;
        last_sm = sm;

        const size_t in_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t in_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG, "%s | ui %" PRIu32 "/5s audio %" PRIu32 "/5s sm %" PRIu32
                      " | heap %uK (max block %uK) psram %uK | usb %s",
                 sm_state_name(app_sm_state()), d_ui, d_audio, d_sm, (unsigned)(in_free / 1024),
                 (unsigned)(in_block / 1024), (unsigned)(ps_free / 1024),
                 hid_kbd_mounted() ? "up" : "down");

        /* Name the wedged task rather than leaving it to be inferred from
         * timestamps thirty minutes later. */
        if (d_ui == 0) {
            ESP_LOGE(TAG, "LVGL HAS NOT RENDERED FOR %d ms -- touch is dead, the screen is frozen",
                     REPORT_MS);
        }
        if (d_audio == 0) {
            ESP_LOGE(TAG, "audio capture task has not run for %d ms -- the microphone is dead",
                     REPORT_MS);
        }
        if (in_block < 8 * 1024) {
            ESP_LOGW(TAG, "internal heap fragmented: largest block only %u bytes",
                     (unsigned)in_block);
        }
    }
}

esp_err_t diag_start(void)
{
    /* Priority 1: below everything it watches, so a busy system starves this
     * task rather than the work it is reporting on. */
    const BaseType_t ok = xTaskCreatePinnedToCore(report_task, "diag", 3072, NULL, 1, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
