#include "beeper.h"

#include <math.h>
#include <string.h>

#include "audio_capture.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "beep";

#define RATE AUDIO_SAMPLE_RATE
#define MAX_MS 60
#define MAX_SAMPLES ((RATE * MAX_MS) / 1000)

/* Loud enough to hear over a room, quiet enough not to clip the amplifier or
 * dominate the microphone that is about to start recording. */
#define AMPLITUDE 7000
#define VOLUME_PCT 65

typedef struct {
    uint16_t freq_a, freq_b; /* freq_b == 0 for a single tone */
    uint16_t ms;
} tone_spec_t;

/* A press that starts a recording should sound like a start: brief and bright.
 * The release falls, so the pair reads as open-then-close without either being
 * long enough to intrude. */
static const tone_spec_t TONES[] = {
    [BEEP_PRESS]   = {.freq_a = 1320, .freq_b = 0, .ms = BEEP_PRESS_MS},
    [BEEP_RELEASE] = {.freq_a = 880, .freq_b = 0, .ms = 40},
    [BEEP_ERROR]   = {.freq_a = 440, .freq_b = 330, .ms = 55},
};
#define TONE_COUNT (sizeof(TONES) / sizeof(TONES[0]))

static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_queue;
static int16_t s_pcm[TONE_COUNT][MAX_SAMPLES];
static size_t s_len[TONE_COUNT];

bool beeper_available(void) { return s_spk != NULL; }

/* A raised-cosine (Hann) envelope over the whole tone. Without it, the tone
 * starts and ends on a discontinuity, and a discontinuity is a click -- which
 * is precisely the artefact a confirmation sound must not have. */
static void render(size_t idx)
{
    const tone_spec_t *t = &TONES[idx];
    size_t n = ((size_t)RATE * t->ms) / 1000;
    if (n > MAX_SAMPLES) {
        n = MAX_SAMPLES;
    }
    if (n < 2) {
        s_len[idx] = 0; /* the envelope below divides by n-1 */
        return;
    }
    s_len[idx] = n;

    for (size_t i = 0; i < n; i++) {
        const double phase = (2.0 * M_PI * t->freq_a * i) / RATE;
        double s = sin(phase);

        /* A second voice a fifth below turns the error tone into a blip you
         * notice rather than a beep you ignore. */
        if (t->freq_b != 0) {
            s = 0.6 * s + 0.4 * sin((2.0 * M_PI * t->freq_b * i) / RATE);
        }

        const double env = 0.5 * (1.0 - cos((2.0 * M_PI * i) / (double)(n - 1)));
        s_pcm[idx][i] = (int16_t)(AMPLITUDE * s * env);
    }
}

static void beep_task(void *arg)
{
    (void)arg;
    for (;;) {
        beep_t tone;
        if (xQueueReceive(s_queue, &tone, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_spk == NULL || (size_t)tone >= TONE_COUNT || s_len[tone] == 0) {
            continue;
        }
        const int bytes = (int)(s_len[tone] * sizeof(int16_t));
        const int written = esp_codec_dev_write(s_spk, s_pcm[tone], bytes);
        if (written != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "write failed (%d)", written);
        }
    }
}

void beeper_play(beep_t tone)
{
    if (s_queue == NULL || s_spk == NULL) {
        return;
    }
    /* Never block. The caller is the LVGL render task on a touch-down; a
     * dropped beep is nothing, a stalled render task is a watchdog reset. */
    (void)xQueueSend(s_queue, &tone, 0);
}

esp_err_t beeper_init(void)
{
#if !CONFIG_TAPTALK_BEEP
    ESP_LOGI(TAG, "beeps disabled by Kconfig");
    return ESP_OK;
#else
    /* bsp_audio_init() no-ops if I2S is already up, which is exactly what we
     * want: audio_capture_start() already pinned it to 16 kHz, and this call
     * would otherwise default it to 22050. */
    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL) {
        ESP_LOGW(TAG, "no speaker; the device will be silent");
        return ESP_OK; /* never fatal */
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = RATE,
        .channel         = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
    };
    const int err = esp_codec_dev_open(s_spk, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "speaker open failed (%d); staying silent", err);
        s_spk = NULL;
        return ESP_OK;
    }
    esp_codec_dev_set_out_vol(s_spk, VOLUME_PCT);

    for (size_t i = 0; i < TONE_COUNT; i++) {
        render(i);
    }

    s_queue = xQueueCreate(4, sizeof(beep_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Core 1, with audio and LVGL. esp_codec_dev_write blocks for the duration
     * of the tone, which is why it does not run on a caller's task. */
    xTaskCreatePinnedToCore(beep_task, "beeper", 3072, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "speaker up: %d Hz, %d tones", RATE, (int)TONE_COUNT);
    return ESP_OK;
#endif
}
