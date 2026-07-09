#include "provisioning.h"

#include <string.h>

#include "app_sm.h"
#include "config_store.h"
#include "core/dnsreply.h"
#include "core/formdec.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include <stdbool.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "prov";

#define AP_CHANNEL 1
#define AP_MAX_CONN 2
#define AP_IP "192.168.4.1"
#define DNS_PORT 53
/* ssid + pass + key, url-encoded worst case, plus field names. */
#define MAX_BODY 1400

static httpd_handle_t s_httpd;

/* ------------------------------------------------------------------ page */

static const char PAGE_FORM[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>TapTalk setup</title>"
    "<style>"
    "body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:24px;background:#101418;color:#e8e8e8}"
    "h1{font-size:20px;margin:0 0 4px}p{color:#9aa0a6;margin:0 0 20px;font-size:14px}"
    "label{display:block;margin:16px 0 4px;font-size:13px;color:#bdc1c6}"
    "input{width:100%;box-sizing:border-box;padding:12px;font-size:16px;border-radius:8px;"
    "border:1px solid #3c4043;background:#1c2126;color:#e8e8e8}"
    "button{width:100%;margin-top:24px;padding:14px;font-size:16px;font-weight:600;border:0;"
    "border-radius:8px;background:#2e7d32;color:#fff}"
    "small{display:block;margin-top:16px;color:#9aa0a6;font-size:12px}"
    ".danger{background:none;border:1px solid #5f6368;color:#9aa0a6;margin-top:12px;font-weight:400}"
    "</style>"
    "<h1>TapTalk setup</h1><p>Credentials are stored on the device.</p>"
    "<form method=POST action=/save>"
    "<label for=ssid>Wi-Fi network</label>"
    "<input id=ssid name=ssid autocapitalize=off autocorrect=off required>"
    "<label for=pass>Wi-Fi password</label>"
    "<input id=pass name=pass type=password autocapitalize=off autocorrect=off>"
    "<label for=key>OpenAI API key</label>"
    "<input id=key name=key type=password autocapitalize=off autocorrect=off "
    "placeholder='sk-... (optional for now)'>"
    "<button type=submit>Save and restart</button></form>"
    "<form method=POST action=/erase>"
    "<button type=submit class=danger>Erase stored credentials</button></form>"
    "<small>The API key is stored unencrypted. Anyone with this board and a USB "
    "cable can read it. Erase it before lending the device.</small>";

static const char PAGE_SAVED[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:24px;background:#101418;"
    "color:#e8e8e8}</style>"
    "<h1>Saved</h1><p>The device is restarting and will join your network. "
    "This access point is going away.</p>";

/* ------------------------------------------------------------------ http */

static esp_err_t send_html(httpd_req_t *req, const char *html, size_t len)
{
    httpd_resp_set_type(req, "text/html");
    /* The page carries a Wi-Fi password field; keep it out of any cache. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, len);
}

/* Serves the form for every GET, whatever the path. Android probes
 * /generate_204, Apple probes /hotspot-detect.html, Windows /connecttest.txt;
 * answering all of them with HTML rather than a 204 is what makes the phone
 * pop the "sign in to network" sheet. */
static esp_err_t get_any(httpd_req_t *req)
{
    return send_html(req, PAGE_FORM, sizeof(PAGE_FORM) - 1);
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap, size_t *out_len)
{
    const size_t total = req->content_len;
    if (total == 0 || total >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t got = 0;
    while (got < total) {
        const int n = httpd_req_recv(req, buf + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    *out_len = got;
    return ESP_OK;
}

static esp_err_t post_save(httpd_req_t *req)
{
    char body[MAX_BODY];
    size_t len = 0;
    if (read_body(req, body, sizeof(body), &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }

    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* form_get refuses to truncate: an over-long value is an error, not a
     * silently clipped credential that fails to authenticate later. */
    const int nssid = form_get(body, len, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    const int npass = form_get(body, len, "pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    const int nkey  = form_get(body, len, "key", cfg.api_key, sizeof(cfg.api_key));

    /* Wipe the body: it held both passwords in plaintext. */
    memset(body, 0, sizeof(body));

    if (nssid <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi network is required");
        return ESP_OK;
    }
    if (npass == FORMDEC_BAD || nkey == FORMDEC_BAD) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value too long or malformed");
        return ESP_OK;
    }

    if (config_save(&cfg) != ESP_OK) {
        memset(&cfg, 0, sizeof(cfg));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not save");
        return ESP_OK;
    }
    memset(&cfg, 0, sizeof(cfg));

    send_html(req, PAGE_SAVED, sizeof(PAGE_SAVED) - 1);

    /* Let the response drain before the radio goes away. */
    vTaskDelay(pdMS_TO_TICKS(500));
    app_sm_post(EV_PROVISIONED);
    return ESP_OK;
}

static esp_err_t post_erase(httpd_req_t *req)
{
    config_erase();
    send_html(req, PAGE_SAVED, sizeof(PAGE_SAVED) - 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    app_sm_post(EV_PROVISIONED); /* reboots; comes back unprovisioned */
    return ESP_OK;
}

/* Not named httpd_start(): that is esp_http_server's own symbol, and shadowing
 * it here turns the call below into unbounded recursion. */
static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    /* The AP is ours alone; a short timeout keeps a stalled phone from
     * holding the single worker. */
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");

    const httpd_uri_t save  = {.uri = "/save", .method = HTTP_POST, .handler = post_save};
    const httpd_uri_t erase = {.uri = "/erase", .method = HTTP_POST, .handler = post_erase};
    const httpd_uri_t any   = {.uri = "/*", .method = HTTP_GET, .handler = get_any};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save), TAG, "uri save");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &erase), TAG, "uri erase");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &any), TAG, "uri any");
    return ESP_OK;
}

/* ------------------------------------------------------------------- dns */

/* Answers every A query with our own address, so whatever hostname the phone
 * probes resolves here and the captive-portal sheet opens by itself. The
 * packet parsing lives in core/dnsreply.c, where it is fuzzed on the host —
 * this is untrusted input from anything that joins the setup AP. */
static void dns_task(void *arg)
{
    (void)arg;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket");
        vTaskDelete(NULL);
        return;
    }
    const struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(DNS_PORT),
        .sin_addr   = {.s_addr = htonl(INADDR_ANY)},
    };
    if (bind(sock, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "dns bind");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    static const uint8_t ap_ip[4] = {192, 168, 4, 1};
    uint8_t buf[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        const size_t reply = dns_build_reply(buf, (size_t)n, sizeof(buf), ap_ip);
        if (reply > 0) {
            sendto(sock, buf, reply, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

/* -------------------------------------------------------------------- ap */

static void make_identity(prov_info_t *info)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(info->ssid, sizeof(info->ssid), "TapTalk-%02X%02X", mac[4], mac[5]);

    /* A fresh password every boot, from the hardware RNG. It is displayed on
     * the screen, so there is no reason for it to be guessable or reused. */
    static const char digits[] = "0123456789";
    for (int i = 0; i < 8; i++) {
        info->pass[i] = digits[esp_random() % 10];
    }
    info->pass[8] = '\0';

    snprintf(info->url, sizeof(info->url), "http://%s", AP_IP);
}

esp_err_t provisioning_start(prov_info_t *info)
{
    /* ACT_PROV_START can fire more than once: the user taps Setup from the
     * idle screen, or from the error screen after a wrong password. Creating
     * the AP netif twice, re-binding the DNS socket, or starting an already
     * running HTTP server would each fail in its own way. */
    static bool s_started;
    static prov_info_t s_info;
    if (s_started) {
        *info = s_info;
        ESP_LOGI(TAG, "setup already running");
        return ESP_OK;
    }

    make_identity(info);

    /* We may be associated as a station already. Stop the radio before
     * switching modes; a running STA holds the netif we are about to replace. */
    esp_wifi_stop();

    esp_netif_create_default_wifi_ap();

    wifi_config_t wc = {0};
    /* wc.ap.ssid is 32 bytes with no room for a terminator, so copy by length
     * rather than let snprintf drop the last character. Bound strnlen by the
     * SOURCE size: bounding it by the 32-byte destination would read past the
     * end of the 24-byte prov_info_t field. */
    const size_t ssid_len = strnlen(info->ssid, sizeof(info->ssid));
    const size_t pass_len = strnlen(info->pass, sizeof(info->pass));
    memcpy(wc.ap.ssid, info->ssid, ssid_len);
    memcpy(wc.ap.password, info->pass, pass_len);
    wc.ap.ssid_len       = (uint8_t)ssid_len;
    wc.ap.channel        = AP_CHANNEL;
    wc.ap.max_connection = AP_MAX_CONN;
    /* WPA2 rather than an open network: the form carries the Wi-Fi password
     * and the API key in the clear over plain HTTP. */
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "ap mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "ap config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "http");
    xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 4, NULL, 0);

    s_started = true;
    s_info    = *info;

    /* The AP password is printed on the screen anyway, and this log only
     * reaches a serial console. The API key is never logged. */
    ESP_LOGI(TAG, "setup AP up: ssid=%s pass=%s url=%s", info->ssid, info->pass, info->url);
    return ESP_OK;
}
