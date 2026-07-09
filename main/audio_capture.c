#include "audio_capture.h"

#include <string.h>

#include "app_sm.h"
#include "bsp/esp-bsp.h"
#include "core/wav.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

static const char *TAG = "audio";

#define CHUNK_SAMPLES 320 /* 20 ms at 16 kHz */
#define CHUNK_BYTES   (CHUNK_SAMPLES * sizeof(int16_t))

static esp_codec_dev_handle_t s_mic;
static uint8_t *s_clip;      /* [WAV_HEADER_SIZE][pcm...] in PSRAM */
static size_t s_cursor;      /* pcm bytes written */
static volatile bool s_recording;
static uint32_t s_clip_ms;
static int s_clip_peak;
static int s_live_peak;

static int16_t s_chunk[CHUNK_SAMPLES];

static int peak_of(const int16_t *s, size_t n)
{
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        /* -32768 negates to itself in int16; widen before abs. */
        const int v = s[i] < 0 ? -(int)s[i] : (int)s[i];
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

static void capture_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (esp_codec_dev_read(s_mic, s_chunk, CHUNK_BYTES) != 0) {
            ESP_LOGW(TAG, "codec read failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const int peak = peak_of(s_chunk, CHUNK_SAMPLES);

        /* Decay the meter so it falls smoothly rather than flickering. */
        s_live_peak = peak > s_live_peak ? peak : (s_live_peak * 7) / 8;
        ui_set_level((s_live_peak * 100) / 32767);

        if (s_recording) {
            if (peak > s_clip_peak) {
                s_clip_peak = peak;
            }
            const size_t room = AUDIO_MAX_PCM_BYTES - s_cursor;
            const size_t n    = room < CHUNK_BYTES ? room : CHUNK_BYTES;
            memcpy(s_clip + WAV_HEADER_SIZE + s_cursor, s_chunk, n);
            s_cursor += n;

            s_clip_ms = (uint32_t)(s_cursor / (AUDIO_SAMPLE_RATE * sizeof(int16_t) / 1000));
            ui_set_clip(s_clip_ms, s_clip_peak);

            if (s_cursor >= AUDIO_MAX_PCM_BYTES) {
                /* Stop appending here rather than waiting for the state
                 * machine to react, so the tail of the clip is never a
                 * partially overwritten buffer. */
                s_recording = false;
                app_sm_post(EV_REC_MAX);
            }
        }
    }
}

esp_err_t audio_capture_start(void)
{
    s_clip = heap_caps_malloc(WAV_HEADER_SIZE + AUDIO_MAX_PCM_BYTES, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_clip != NULL, ESP_ERR_NO_MEM, TAG, "no PSRAM for %u byte clip",
                        (unsigned)(WAV_HEADER_SIZE + AUDIO_MAX_PCM_BYTES));

    /* bsp_audio_init(NULL) would default to 22050 Hz. It also no-ops on a
     * second call, so this must run before bsp_audio_codec_microphone_init(),
     * which calls it with NULL if I2S is not already up. */
    const i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {false, false, false},
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "bsp_audio_init @ %d Hz", AUDIO_SAMPLE_RATE);

    s_mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_mic != NULL, ESP_FAIL, TAG, "microphone init failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel         = AUDIO_CHANNELS,
        .channel_mask    = 0,
        .sample_rate     = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic, &fs) == 0, ESP_FAIL, TAG, "codec open");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_mic, 30.0f) == 0, ESP_FAIL, TAG, "set gain");

    /* Core 1: I2S DMA has hard deadlines and core 0 carries Wi-Fi. */
    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "audio_cap", 4096, NULL, 6, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task create");

    ESP_LOGI(TAG, "capture up: %d Hz, %d-bit, mono, clip cap %u s", AUDIO_SAMPLE_RATE, AUDIO_BITS,
             AUDIO_MAX_SECONDS);
    return ESP_OK;
}

void audio_record_begin(void)
{
    s_cursor    = 0;
    s_clip_ms   = 0;
    s_clip_peak = 0;
    s_recording = true;
}

void audio_record_end(void)
{
    s_recording = false;
    /* Backfill the header now that the payload length is known. The PCM was
     * written straight into s_clip + 44, so no copy is needed. */
    wav_write_header(s_clip, WAV_HEADER_SIZE, (uint32_t)s_cursor, AUDIO_SAMPLE_RATE, AUDIO_BITS,
                     AUDIO_CHANNELS);
    ESP_LOGI(TAG, "clip: %u ms, %u bytes, peak %d", (unsigned)s_clip_ms, (unsigned)s_cursor,
             s_clip_peak);
}

bool audio_clip_usable(void)
{
    return s_clip_ms >= AUDIO_MIN_CLIP_MS && s_clip_peak >= AUDIO_SILENCE_PEAK;
}

uint32_t audio_clip_ms(void) { return s_clip_ms; }
int audio_clip_peak(void) { return s_clip_peak; }
size_t audio_clip_pcm_bytes(void) { return s_cursor; }

const uint8_t *audio_clip_wav(size_t *total_len)
{
    if (total_len != NULL) {
        *total_len = WAV_HEADER_SIZE + s_cursor;
    }
    return s_clip;
}

void audio_clip_discard(void)
{
    s_recording = false;
    s_cursor    = 0;
    s_clip_ms   = 0;
    s_clip_peak = 0;
}
