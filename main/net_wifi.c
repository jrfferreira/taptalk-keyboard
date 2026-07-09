#include "net_wifi.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

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

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy main/secrets.example.h to main/secrets.h and fill in your Wi-Fi credentials. secrets.h is gitignored."
#endif

static const char *TAG = "net";

/* Anything past this means SNTP has actually run; the epoch default is 1970. */
#define SANE_EPOCH 1735689600 /* 2025-01-01 */
#define SNTP_TIMEOUT_MS 20000

static uint32_t s_retries;

uint32_t net_wifi_retries(void) { return s_retries; }

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        s_retries++;
        ui_set_wifi(false, "");
        /* The state machine owns the retry policy, including when to stop. */
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
    ui_set_wifi(true, ip);
    app_sm_post(EV_WIFI_UP);
}

esp_err_t net_wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL),
        TAG, "ip handler");

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", TAPTALK_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", TAPTALK_WIFI_PASSWORD);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    return ESP_OK;
}

esp_err_t net_wifi_connect(void)
{
    /* esp_wifi_start() triggers STA_START, whose handler calls connect().
     * On a retry the driver is already started, so connect directly. */
    const esp_err_t err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_NOT_STOPPED || err == ESP_OK) {
        if (err == ESP_ERR_WIFI_NOT_STOPPED) {
            esp_wifi_connect();
        }
        return ESP_OK;
    }
    return err;
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
        ui_set_time(true, iso);
        app_sm_post(EV_TIME_OK);
    } else {
        ESP_LOGE(TAG, "SNTP failed (epoch=%lld); TLS would reject every certificate",
                 (long long)now);
        ui_set_time(false, "");
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
