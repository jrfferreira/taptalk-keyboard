#include "net_wifi.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_sm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "net";

/* Anything past this means SNTP has actually run; the epoch default is 1970. */
#define SANE_EPOCH 1735689600 /* 2025-01-01 */
#define SNTP_TIMEOUT_MS 20000

static uint32_t s_retries;
static bool s_sta_started;
static bool s_sta_netif;
/* Provisioning brings the STA interface up purely to scan. Without this guard
 * the STA_START handler would try to associate with credentials we do not have
 * yet, and hammer the retry ladder while the user is still typing them in. */
static bool s_want_connect;

uint32_t net_wifi_retries(void) { return s_retries; }

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        if (s_want_connect) {
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_want_connect) {
            return; /* a scan ended, not a lost connection */
        }
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        s_retries++;
        ui_set_wifi(false);
        app_sm_post(EV_WIFI_DOWN);
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *e = data;
    char ip[20];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "got ip %s", ip);
    s_retries = 0;
    ui_set_wifi(true);
    app_sm_post(EV_WIFI_UP);
}

esp_err_t net_init_common(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL),
        TAG, "ip handler");
    return ESP_OK;
}

void net_wifi_ensure_sta_netif(void)
{
    if (!s_sta_netif) {
        esp_netif_create_default_wifi_sta();
        s_sta_netif = true;
    }
}

esp_err_t net_wifi_sta_connect(const app_config_t *cfg)
{
    s_want_connect = true;

    if (s_sta_started) {
        /* A retry: the driver is already up, so just re-associate. */
        return esp_wifi_connect();
    }

    net_wifi_ensure_sta_netif();

    /* Not snprintf: wifi_config_t.sta.ssid is exactly 32 bytes with no room
     * for a terminator, so snprintf would silently drop the last character of
     * a legal 32-character SSID. The struct is zeroed, and esp_wifi treats a
     * NUL-padded field as the SSID. Same for the 64-byte passphrase. */
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, cfg->wifi_ssid, strnlen(cfg->wifi_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, cfg->wifi_pass, strnlen(cfg->wifi_pass, sizeof(wc.sta.password)));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "sta config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    s_sta_started = true;
    ESP_LOGI(TAG, "associating with \"%s\"", cfg->wifi_ssid);
    return ESP_OK; /* STA_START fires, whose handler calls esp_wifi_connect() */
}

static void sntp_task(void *arg)
{
    (void)arg;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        app_sm_post(EV_TIME_FAIL);
        vTaskDelete(NULL);
        return;
    }

    const bool synced = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS)) == ESP_OK;

    time_t now = 0;
    time(&now);

    /* sync_wait can report success on a bogus value; the epoch check is what
     * actually protects the TLS handshake. */
    if (synced && now > SANE_EPOCH) {
        struct tm tm;
        char iso[24];
        gmtime_r(&now, &tm);
        strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M", &tm);
        ESP_LOGI(TAG, "time synced: %s UTC", iso);
        app_sm_post(EV_TIME_OK);
    } else {
        ESP_LOGE(TAG, "SNTP failed (epoch=%lld); TLS would reject every certificate",
                 (long long)now);
        app_sm_post(EV_TIME_FAIL);
    }

    esp_netif_sntp_deinit();
    vTaskDelete(NULL);
}

void net_sntp_start(void)
{
    /* Core 0, next to the radio and lwIP. */
    xTaskCreatePinnedToCore(sntp_task, "sntp", 4096, NULL, 5, NULL, 0);
}
