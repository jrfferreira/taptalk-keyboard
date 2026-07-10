#include "audio_capture.h"

#include <math.h>
#include <string.h>

#include "app_sm.h"
#include "bsp/esp-bsp.h"
#include "core/wav.h"
#include "esp_check.h"
#include "dsps_fft2r.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "beeper.h"
#include "diagnostics.h"
#include "ui.h"

static const char *TAG = "audio";

#define CHUNK_SAMPLES 320 /* 20 ms at 16 kHz */
#define CHUNK_BYTES   (CHUNK_SAMPLES * sizeof(int16_t))

/* Peak that fills the level meter. Well below full scale (32767): loud speech
 * at 42 dB gain lands around here, so normal speech spans the bars instead of
 * pinning them near zero. */
#define METER_REF_PEAK 8000.0f

/* FFT magnitudes are much larger than raw peaks (summed over a window), so the
 * spectrum reference is far higher than the meter's. Tuned for speech. */
#define SPECTRUM_REF 25000.0f

static esp_codec_dev_handle_t s_mic;
static uint8_t *s_clip;      /* [WAV_HEADER_SIZE][pcm...] in PSRAM */
static size_t s_cursor;      /* pcm bytes written */
static volatile bool s_recording;
/* Bytes of our own press tone still to be discarded at the head of the clip.
 * Touched by capture_task and set by audio_record_begin(); the state machine
 * guarantees they never race. */
static volatile size_t s_trim_bytes;
static uint32_t s_clip_ms;
static int s_clip_peak;
static int s_live_peak;
static const char *s_reject; /* why the last clip was unusable, or "" */

static int16_t s_chunk[CHUNK_SAMPLES];

/* ------------------------------------------------------------- spectrum ---
 *
 * A 256-point FFT of each chunk, grouped into AUDIO_SPECTRUM_BANDS log-spaced
 * bands so bass and treble get their own bars -- a real analyser, not the
 * scrolling level history it replaces. esp-dsp does the transform in-place on
 * an interleaved complex array; we feed real samples with zero imaginary part
 * and read magnitudes from the lower half. */
#define FFT_SIZE 256
#define FFT_BINS (FFT_SIZE / 2)

static float s_window[FFT_SIZE];        /* Hann, cuts spectral leakage */
static float s_fft[FFT_SIZE * 2];       /* interleaved re,im */
static uint16_t s_band_lo[AUDIO_SPECTRUM_BANDS]; /* first FFT bin of each band */
static bool s_fft_ready;

static void spectrum_init(void)
{
    if (dsps_fft2r_init_fc32(NULL, FFT_SIZE) != ESP_OK) {
        ESP_LOGW(TAG, "FFT init failed; spectrum disabled");
        return;
    }
    for (int i = 0; i < FFT_SIZE; i++) {
        s_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
    }
    /* Log-spaced band edges from ~125 Hz (bin 2) to ~6 kHz (bin 96): speech
     * lives here, and log spacing matches how the ear groups pitch. */
    const float lo = 2.0f, hi = 96.0f;
    for (int b = 0; b < AUDIO_SPECTRUM_BANDS; b++) {
        s_band_lo[b] = (uint16_t)(lo * powf(hi / lo, (float)b / AUDIO_SPECTRUM_BANDS));
    }
    s_fft_ready = true;
}

static void spectrum_of(const int16_t *pcm)
{
    if (!s_fft_ready) {
        return;
    }
    for (int i = 0; i < FFT_SIZE; i++) {
        s_fft[2 * i]     = (float)pcm[i] * s_window[i];
        s_fft[2 * i + 1] = 0.0f;
    }
    dsps_fft2r_fc32(s_fft, FFT_SIZE);
    dsps_bit_rev_fc32(s_fft, FFT_SIZE);

    uint8_t bands[AUDIO_SPECTRUM_BANDS];
    for (int b = 0; b < AUDIO_SPECTRUM_BANDS; b++) {
        const int k0 = s_band_lo[b];
        const int k1 = (b + 1 < AUDIO_SPECTRUM_BANDS) ? s_band_lo[b + 1] : FFT_BINS;
        float mag = 0.0f;
        for (int k = k0; k < k1 && k < FFT_BINS; k++) {
            const float re = s_fft[2 * k], im = s_fft[2 * k + 1];
            const float m = sqrtf(re * re + im * im);
            if (m > mag) {
                mag = m; /* peak within the band reads livelier than an average */
            }
        }
        /* sqrt curve + a reference tuned so speech spans the bars, same logic as
         * the old level meter. */
        float norm = sqrtf(mag / SPECTRUM_REF);
        if (norm > 1.0f) {
            norm = 1.0f;
        }
        bands[b] = (uint8_t)(norm * 100.0f);
    }
    ui_set_spectrum(bands);
}

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

        diag_beat_audio();

        const int peak = peak_of(s_chunk, CHUNK_SAMPLES);

        /* Decay the meter so it falls smoothly rather than flickering. */
        s_live_peak = peak > s_live_peak ? peak : (s_live_peak * 7) / 8;

        /* Map the peak to a 0..100 meter. NOT against full scale (32767): speech
         * peaks around 500-3000, so that mapping pinned every bar near the floor
         * and the meter looked like a flat line. Reference a realistic loud-speech
         * level instead, and take the square root so quiet speech still lifts the
         * bars visibly rather than hugging zero. */
        float norm = (float)s_live_peak / METER_REF_PEAK;
        if (norm > 1.0f) {
            norm = 1.0f;
        }
        ui_set_level((int)(sqrtf(norm) * 100.0f));
        spectrum_of(s_chunk);

        if (s_recording) {
            /* Swallow the press tone. It plays out of a speaker two centimetres
             * from this microphone, and a 1320 Hz sine at the head of the clip
             * is something a transcriber will happily try to turn into a word.
             *
             * The tail of the beep usually lands mid-chunk, so drop bytes, not
             * whole chunks -- and drop them BEFORE measuring the clip peak, or
             * our own beep satisfies the silence guard for us. */
            size_t off = 0;
            if (s_trim_bytes > 0) {
                off = s_trim_bytes < CHUNK_BYTES ? s_trim_bytes : CHUNK_BYTES;
                s_trim_bytes -= off;
                if (off == CHUNK_BYTES) {
                    continue; /* the entire chunk was beep */
                }
            }

            /* off is a BYTE count; s_chunk is int16_t*, so indexing it directly
             * would skip twice as far. */
            const int16_t *pcm  = (const int16_t *)((const uint8_t *)s_chunk + off);
            const size_t kept   = CHUNK_BYTES - off;
            const int kept_peak = peak_of(pcm, kept / sizeof(int16_t));
            if (kept_peak > s_clip_peak) {
                s_clip_peak = kept_peak;
            }

            const size_t room = AUDIO_MAX_PCM_BYTES - s_cursor;
            const size_t n    = room < kept ? room : kept;
            memcpy(s_clip + WAV_HEADER_SIZE + s_cursor, pcm, n);
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
    /* 42 dB, not 30. At 30 dB, normal speech peaked at ~500/32767 -- below the
     * 600 silence guard -- so clips were silently dropped as TOO QUIET and never
     * uploaded. +12 dB is ~4x amplitude (peak ~2000), clear of the guard with
     * headroom below full scale for loud input. */
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_mic, 42.0f) == 0, ESP_FAIL, TAG, "set gain");

    spectrum_init();

    /* Core 1: I2S DMA has hard deadlines and core 0 carries Wi-Fi. */
    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "audio_cap", 4096, NULL, 6, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task create");

    ESP_LOGI(TAG, "capture up: %d Hz, %d-bit, mono, clip cap %u s", AUDIO_SAMPLE_RATE, AUDIO_BITS,
             AUDIO_MAX_SECONDS);
    return ESP_OK;
}

/* The tone, plus a margin for the amplifier to settle. Rounded to a whole
 * sample so the PCM never desynchronises. */
#define TRIM_MS (BEEP_PRESS_MS + 16)
#define TRIM_BYTES (((size_t)AUDIO_SAMPLE_RATE * TRIM_MS / 1000) * sizeof(int16_t))

void audio_record_begin(void)
{
    s_trim_bytes = beeper_available() ? TRIM_BYTES : 0;
    ESP_LOGI(TAG, "record: begin (discarding %u bytes = %u ms of press tone)",
             (unsigned)s_trim_bytes, (unsigned)(s_trim_bytes / 32));
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
    /* Only a too-short tap is rejected now. A quiet clip still uploads: if the
     * user held the button and spoke, that intent is honoured, and true silence
     * comes back as empty text ("No speech detected") rather than being eaten
     * here. A client-side volume gate that discards a deliberate hold is wrong. */
    s_reject = s_clip_ms < AUDIO_MIN_CLIP_MS ? "Too short - hold and speak" : "";
    ESP_LOGI(TAG, "record: end -- %u ms, %u bytes, peak %d/32767 (%s)", (unsigned)s_clip_ms,
             (unsigned)s_cursor, s_clip_peak, s_reject[0] ? s_reject : "usable");
}

bool audio_clip_usable(void)
{
    return s_clip_ms >= AUDIO_MIN_CLIP_MS;
}

const char *audio_clip_reject_reason(void) { return s_reject; }

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
