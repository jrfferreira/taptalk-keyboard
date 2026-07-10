#include "stt_client.h"

#include <string.h>

#include "app_sm.h"
#include "config_store.h"
#include "core/multipart.h"
#include "core/textnorm.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "stt";

/* Generous: a 30 s clip has to upload over Wi-Fi before the server even starts.
 * The esp_http_client default of 5 s would time out on nearly every request. */
#define STT_TIMEOUT_MS 60000
#define CHUNK 4096

/* One PSRAM block, STT_TRANSCRIPT_CAP bytes each: at ~4 KB apiece they no
 * longer fit this task's stack, and internal RAM is too tight for buffers
 * that sit idle between requests (sdkconfig.defaults has the history). */
static char *s_raw;        /* the response body as received */
static char *s_transcript; /* the body after textnorm_clean() */
static char s_error[64];
static volatile bool s_abort;

static const char *s_key;
static const char *s_url;
static const char *s_model;
static const char *s_language;
static const uint8_t *s_wav;
static size_t s_wav_len;

const char *stt_transcript(void) { return s_transcript ? s_transcript : ""; }
const char *stt_error(void) { return s_error; }
void stt_abort(void) { s_abort = true; }

/* esp_http_client_write() is allowed to write less than asked. */
static bool write_all(esp_http_client_handle_t c, const void *data, size_t len)
{
    const char *p = data;
    size_t sent = 0;
    while (sent < len) {
        if (s_abort) {
            return false;
        }
        const size_t want = (len - sent) > CHUNK ? CHUNK : (len - sent);
        const int n = esp_http_client_write(c, p + sent, want);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static void fail(const char *why)
{
    snprintf(s_error, sizeof(s_error), "%s", why);
    ESP_LOGE(TAG, "%s", why);
    app_sm_post(EV_STT_FAIL);
}

static void stt_task(void *arg)
{
    (void)arg;
    s_transcript[0] = '\0';
    s_error[0]      = '\0';

    /* Random per request, so a boundary can never collide with the audio. */
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "----taptalk%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random());

    multipart_t mp;
    if (!multipart_build(&mp, boundary, s_model, "text",
                         s_language[0] ? s_language : NULL, "audio.wav", s_wav_len)) {
        fail("Request too large");
        vTaskDelete(NULL);
        return;
    }

    const esp_http_client_config_t cfg = {
        .url               = s_url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = STT_TIMEOUT_MS,
        /* The full Mozilla root bundle. api.openai.com chains to GTS Root R4,
         * which I could not confirm is in the reduced filter. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 1024,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        fail("HTTP init failed");
        vTaskDelete(NULL);
        return;
    }

    if (s_key[0]) {
        char auth[CONFIG_API_KEY_CAP + 16]; /* "Bearer " + key + NUL */
        snprintf(auth, sizeof(auth), "Bearer %s", s_key);
        esp_http_client_set_header(c, "Authorization", auth);
        /* The header is copied into the client; do not leave a second copy of
         * the key sitting on this task's stack. */
        memset(auth, 0, sizeof(auth));
    }

    esp_http_client_set_header(c, "Content-Type", mp.content_type);

    ESP_LOGI(TAG, "POST %u bytes of audio to %s using %s (%u total)", (unsigned)s_wav_len,
             s_url, s_model, (unsigned)mp.content_length);

    esp_err_t err = esp_http_client_open(c, (int)mp.content_length);
    if (err != ESP_OK) {
        /* Almost always Wi-Fi, DNS, or a clock so wrong that the certificate
         * looks not-yet-valid. */
        fail("Could not reach the server");
        goto done;
    }

    if (!write_all(c, mp.preamble, mp.preamble_len) || !write_all(c, s_wav, s_wav_len) ||
        !write_all(c, mp.epilogue, mp.epilogue_len)) {
        fail(s_abort ? "Cancelled" : "Upload failed");
        goto done;
    }

    if (esp_http_client_fetch_headers(c) < 0) {
        fail("No response");
        goto done;
    }

    const int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        /* Read a little of the body: it explains the failure, and it never
         * contains the key we sent. */
        char body[192] = {0};
        const int n = esp_http_client_read_response(c, body, sizeof(body) - 1);
        ESP_LOGE(TAG, "HTTP %d: %.*s", status, n > 0 ? n : 0, body);
        switch (status) {
        case 401: fail("API key rejected"); break;
        case 429: fail("Rate limited or out of credit"); break;
        case 413: fail("Recording too long"); break;
        default:  snprintf(s_error, sizeof(s_error), "Server error %d", status);
                  app_sm_post(EV_STT_FAIL);
        }
        goto done;
    }

    /* response_format=text, so the body IS the transcript. */
    const int n = esp_http_client_read_response(c, s_raw, STT_TRANSCRIPT_CAP - 1);
    if (n < 0) {
        fail("Truncated response");
        goto done;
    }
    s_raw[n] = '\0';

    /* The cap is sized so a full-length clip cannot fill it (see
     * stt_client.h); if a body ever does, say so instead of silently typing
     * half a dictation. */
    if (n == STT_TRANSCRIPT_CAP - 1) {
        char probe;
        if (esp_http_client_read(c, &probe, 1) > 0) {
            ESP_LOGW(TAG, "transcript exceeds %d bytes; tail dropped", STT_TRANSCRIPT_CAP - 1);
        }
    }

    textnorm_clean(s_raw, (size_t)n, s_transcript, STT_TRANSCRIPT_CAP);
    memset(s_raw, 0, STT_TRANSCRIPT_CAP);

    if (s_transcript[0] == '\0') {
        ESP_LOGI(TAG, "no speech");
        app_sm_post(EV_STT_EMPTY);
    } else {
        ESP_LOGI(TAG, "transcript: \"%s\"", s_transcript);
        app_sm_post(EV_STT_OK);
    }

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    vTaskDelete(NULL);
}

esp_err_t stt_start(const app_config_t *config, const uint8_t *wav, size_t wav_len)
{
    if (config == NULL) {
        snprintf(s_error, sizeof(s_error), "No transcription settings");
        return ESP_ERR_INVALID_ARG;
    }
    if (wav == NULL || wav_len == 0) {
        snprintf(s_error, sizeof(s_error), "Empty recording");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_transcript == NULL) {
        char *buf = heap_caps_malloc(2 * STT_TRANSCRIPT_CAP, MALLOC_CAP_SPIRAM);
        if (buf == NULL) {
            snprintf(s_error, sizeof(s_error), "Out of memory");
            return ESP_ERR_NO_MEM;
        }
        buf[0]       = '\0';
        s_transcript = buf;
        s_raw        = buf + STT_TRANSCRIPT_CAP;
    }

    s_abort   = false;
    s_key     = config->api_key;
    s_url     = config_stt_url(config);
    s_model   = config_stt_model(config);
    s_language = config->stt_language;
    s_wav     = wav;
    s_wav_len = wav_len;

    /* Core 0, next to Wi-Fi and lwIP. 8 KB: the mbedTLS handshake is
     * stack-hungry. */
    return xTaskCreatePinnedToCore(stt_task, "net_stt", 8192, NULL, 5, NULL, 0) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}
