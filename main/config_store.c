#include "config_store.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

#define NS "taptalk"
#define K_SSID "ssid"
#define K_PASS "pass"
#define K_KEY  "apikey"
#define K_STT_URL "stt_url"
#define K_STT_MODEL "stt_model"
#define K_STT_LANG "stt_lang"
#define K_KBD_LAYOUT "kbd_layout"

/* Optional developer convenience: if main/secrets.h exists it seeds NVS on
 * first boot, so a bench build skips the portal. It is gitignored and must
 * never be present in a tree used to cut a release binary. */
#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_SECRETS_SEED 1
#endif

static esp_err_t get_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    const esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

esp_err_t config_load(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored configuration");
    } else if (err != ESP_OK) {
        return err;
    } else {
        ESP_RETURN_ON_ERROR(get_str(h, K_SSID, cfg->wifi_ssid, sizeof(cfg->wifi_ssid)), TAG, "ssid");
        ESP_RETURN_ON_ERROR(get_str(h, K_PASS, cfg->wifi_pass, sizeof(cfg->wifi_pass)), TAG, "pass");
        ESP_RETURN_ON_ERROR(get_str(h, K_KEY, cfg->api_key, sizeof(cfg->api_key)), TAG, "apikey");
        ESP_RETURN_ON_ERROR(get_str(h, K_STT_URL, cfg->stt_url, sizeof(cfg->stt_url)), TAG, "stt url");
        ESP_RETURN_ON_ERROR(get_str(h, K_STT_MODEL, cfg->stt_model, sizeof(cfg->stt_model)), TAG, "stt model");
        ESP_RETURN_ON_ERROR(get_str(h, K_STT_LANG, cfg->stt_language, sizeof(cfg->stt_language)), TAG, "stt language");
        ESP_RETURN_ON_ERROR(get_str(h, K_KBD_LAYOUT, cfg->kbd_layout, sizeof(cfg->kbd_layout)), TAG, "kbd layout");
        nvs_close(h);
    }

#ifdef HAVE_SECRETS_SEED
    if (!config_is_provisioned(cfg) && sizeof(TAPTALK_WIFI_SSID) > 1 &&
        strcmp(TAPTALK_WIFI_SSID, "your-ssid") != 0) {
        ESP_LOGW(TAG, "seeding NVS from main/secrets.h (development build)");
        snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", TAPTALK_WIFI_SSID);
        snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", TAPTALK_WIFI_PASSWORD);
        if (strcmp(TAPTALK_OPENAI_API_KEY, "sk-...") != 0) {
            snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", TAPTALK_OPENAI_API_KEY);
        }
        (void)config_save(cfg);
    }
#endif

    char masked[24];
    config_mask_key(cfg->api_key, masked, sizeof(masked));
    ESP_LOGI(TAG, "ssid=%s stt_url=%s stt_model=%s language=%s layout=%s api_key=%s",
             cfg->wifi_ssid[0] ? cfg->wifi_ssid : "<unset>", config_stt_url(cfg),
             config_stt_model(cfg), cfg->stt_language[0] ? cfg->stt_language : "<auto>",
             config_kbd_layout(cfg), masked);
    return ESP_OK;
}

esp_err_t config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open rw");

    esp_err_t err = nvs_set_str(h, K_SSID, cfg->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, K_PASS, cfg->wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, K_KEY, cfg->api_key);
    if (err == ESP_OK) err = nvs_set_str(h, K_STT_URL, cfg->stt_url);
    if (err == ESP_OK) err = nvs_set_str(h, K_STT_MODEL, cfg->stt_model);
    if (err == ESP_OK) err = nvs_set_str(h, K_STT_LANG, cfg->stt_language);
    if (err == ESP_OK) err = nvs_set_str(h, K_KBD_LAYOUT, cfg->kbd_layout);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "save");
    ESP_LOGI(TAG, "configuration saved");
    return ESP_OK;
}

bool config_is_provisioned(const app_config_t *cfg) { return cfg->wifi_ssid[0] != '\0'; }

const char *config_stt_url(const app_config_t *cfg)
{
    return cfg->stt_url[0] ? cfg->stt_url : CONFIG_DEFAULT_STT_URL;
}

const char *config_stt_model(const app_config_t *cfg)
{
    return cfg->stt_model[0] ? cfg->stt_model : CONFIG_DEFAULT_STT_MODEL;
}

bool config_stt_uses_tls(const app_config_t *cfg)
{
    return strncmp(config_stt_url(cfg), "https://", 8) == 0;
}

const char *config_kbd_layout(const app_config_t *cfg)
{
    return cfg->kbd_layout[0] ? cfg->kbd_layout : "us";
}

esp_err_t config_erase(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open rw");
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "credentials erased");
    return err;
}

void config_mask_key(const char *key, char *out, size_t out_cap)
{
    if (out_cap == 0) {
        return;
    }
    const size_t n = key != NULL ? strlen(key) : 0;
    if (n == 0) {
        snprintf(out, out_cap, "<unset>");
    } else if (n <= 12) {
        /* Too short to reveal a prefix and a suffix without revealing most of
         * it. Something is probably wrong with the key anyway. */
        snprintf(out, out_cap, "<set, %u chars>", (unsigned)n);
    } else {
        snprintf(out, out_cap, "%.8s...%s", key, key + n - 4);
    }
}
