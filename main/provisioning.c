#include "provisioning.h"

#include <stdlib.h>
#include <string.h>

#include "app_sm.h"
#include "config_store.h"
#include "core/dnsreply.h"
#include "core/formdec.h"
#include "core/jsonesc.h"
#include "core/keymap.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include <stdbool.h>
#include "esp_wifi.h"
#include "net_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "prov";

#define AP_CHANNEL 1
#define AP_MAX_CONN 2
#define AP_IP "192.168.4.1"
#define DNS_PORT 53
/* Every form value may expand to %XX. The endpoint and provider key are the
 * largest fields, so leave enough room to reject overlong values cleanly. */
#define MAX_BODY 2300

static httpd_handle_t s_httpd;

/* Portal lifecycle, shared between provisioning_start and provisioning_stop so
 * the AP->STA switch can happen in place (no reboot -- see provisioning_stop). */
static bool          s_started;    /* the portal is up */
static esp_netif_t  *s_ap_netif;   /* the SoftAP netif, destroyed on stop */
static volatile bool s_dns_stop;   /* asks the dns task to close and exit */

/* ------------------------------------------------------------------ page */

/* The SSID stays a real <input>, not a <select>: password managers look for a
 * text field marked autocomplete="username" next to a password field, and a
 * <select> breaks that detection. The picker writes into it instead. */
static const char PAGE_FORM[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>TapTalk setup</title>"
    "<style>"
    "body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:24px;background:#0a0d10;color:#e8edf2}"
    "h1{font-size:21px;margin:0 0 4px}p{color:#8a97a3;margin:0 0 20px;font-size:14px}"
    "label{display:block;margin:18px 0 5px;font-size:13px;color:#bdc1c6}"
    "input,select{width:100%;box-sizing:border-box;padding:12px;font-size:16px;border-radius:9px;"
    "border:1px solid #2b343d;background:#141a21;color:#e8edf2}"
    "input:focus,select:focus{outline:2px solid #3ddc97;outline-offset:1px;border-color:#3ddc97}"
    "button{width:100%;margin-top:26px;padding:14px;font-size:16px;font-weight:600;border:0;"
    "border-radius:9px;background:#2e7d32;color:#fff}"
    ".row{display:flex;gap:8px}.row input{flex:1}"
    ".ghost{width:auto;margin:0;padding:12px 14px;background:#1b232b;color:#8a97a3;font-weight:400}"
    "small{display:block;margin-top:18px;color:#8a97a3;font-size:12px}"
    ".danger{background:none;border:1px solid #4b555f;color:#8a97a3;margin-top:12px;font-weight:400}"
    "#hint{font-size:12px;color:#6b7885;margin:6px 0 0}"
    /* Hidden until /config says a key is stored; there is nothing to forget
     * on a fresh device. */
    "#ck{display:none}#ck input{width:auto;margin:0 8px 0 0;vertical-align:middle}"
    "</style>"
    "<h1>TapTalk setup</h1><p>Credentials are stored on the device.</p>"
    /* autocomplete=on plus a username/current-password pair is what makes a
     * password manager offer to fill and to save. */
    "<form method=POST action=/save autocomplete=on>"

    "<label for=netsel>Wi-Fi network</label>"
    "<select id=netsel><option value=''>Scanning\xe2\x80\xa6</option></select>"

    "<label for=ssid>Network name</label>"
    "<input id=ssid name=ssid autocomplete=username autocapitalize=off autocorrect=off "
    "spellcheck=false required>"
    "<p id=hint></p>"

    "<label for=pass>Wi-Fi password</label>"
    "<div class=row>"
    "<input id=pass name=pass type=password autocomplete=current-password autocapitalize=off "
    "autocorrect=off spellcheck=false>"
    "<button type=button class=ghost id=tp>Show</button></div>"

    "<label for=url>Transcription endpoint</label>"
    "<input id=url name=url type=url required autocapitalize=off autocorrect=off "
    "spellcheck=false value='https://api.openai.com/v1/audio/transcriptions'>"
    "<p class=help>Use an OpenAI-compatible <code>/v1/audio/transcriptions</code> endpoint. "
    "A local HTTP endpoint is allowed, but only use HTTP on a trusted LAN.</p>"

    "<label for=model>Model</label>"
    "<input id=model name=model required autocapitalize=off autocorrect=off "
    "spellcheck=false value='gpt-4o-mini-transcribe'>"

    "<label for=lang>Language (optional)</label>"
    "<input id=lang name=lang maxlength=15 autocapitalize=off autocorrect=off "
    "spellcheck=false placeholder='auto, en, pt, ...'>"

    /* The HOST decodes our keystrokes through its own layout, so this must
     * match the computer being typed into, not a preference. */
    "<label for=layout>Keyboard layout of the computer</label>"
    "<select id=layout name=layout>"
    "<option value=us>English (US)</option>"
    "<option value=uk>English (UK)</option>"
    "<option value=us-intl>English (US International)</option>"
    "<option value=de>Deutsch</option>"
    "<option value=es>Espa\xc3\xb1ol \xe2\x80\x94 Espa\xc3\xb1a</option>"
    "<option value=es-la>Espa\xc3\xb1ol \xe2\x80\x94 Latinoam\xc3\xa9rica</option>"
    "<option value=fr>Fran\xc3\xa7ais (AZERTY)</option>"
    "<option value=it>Italiano</option>"
    "<option value=abnt2>Portugu\xc3\xaas \xe2\x80\x94 Brasil (ABNT2)</option>"
    "<option value=pt>Portugu\xc3\xaas \xe2\x80\x94 Portugal</option>"
    "</select>"
    "<p class=help>The layout the computer you type into is set to. Accents are "
    "typed with dead keys, so this must match or they come out wrong.</p>"

    "<label for=key>API key (optional for local servers)</label>"
    "<div class=row>"
    "<input id=key name=key type=password autocomplete=off autocapitalize=off autocorrect=off "
    "spellcheck=false placeholder='sk-\xe2\x80\xa6'>"
    "<button type=button class=ghost id=tk>Show</button></div>"
    "<label id=ck><input type=checkbox name=clearkey value=1>Forget the stored API key</label>"

    /* What the on-screen Send button strikes. The value is the send_key_t index,
     * so these options must stay in the enum's order. */
    "<label for=sendkey>Send button types</label>"
    "<select id=sendkey name=sendkey>"
    "<option value=0>Enter</option>"
    "<option value=1>Cmd / Win + Enter</option>"
    "<option value=2>Ctrl + Enter</option>"
    "<option value=3>Shift + Enter</option>"
    "</select>"

    "<button type=submit>Save and restart</button></form>"
    "<form method=POST action=/erase>"
    "<button type=submit class=danger>Erase stored credentials</button></form>"
    "<small>The API key and endpoint are stored unencrypted. Anyone with this board and a USB "
    "cable can read them. Erase them before lending the device.</small>"
    /* Filled by the /config fetch below. Which commit built this firmware --
     * handy when reporting a bug or confirming a flash. */
    "<small id=fw style='color:#4b555f;margin-top:8px'></small>"

    "<script>"
    "var sel=document.getElementById('netsel'),ssid=document.getElementById('ssid'),"
    "hint=document.getElementById('hint'),pass=document.getElementById('pass'),"
    "key=document.getElementById('key'),sc=null;"
    "function toggle(b,i){b.onclick=function(){var p=i.type==='password';"
    "i.type=p?'text':'password';b.textContent=p?'Hide':'Show';};}"
    "toggle(document.getElementById('tp'),pass);"
    "toggle(document.getElementById('tk'),key);"
    /* The stored password is kept only for the stored network, so the "leave
     * blank" offer must appear and disappear as the SSID field changes. */
    "function ph(){pass.placeholder=(sc&&sc.has_pass&&ssid.value===sc.ssid)?"
    "'Leave blank to keep the stored password':'';}"
    "ssid.oninput=ph;"
    "sel.onchange=function(){if(sel.value){ssid.value=sel.value;ph();"
    "pass.focus();}};"
    "fetch('/scan').then(function(r){return r.json();}).then(function(n){"
    "sel.innerHTML='';"
    "var o=document.createElement('option');o.value='';"
    /* textContent, never innerHTML: an SSID is 32 bytes of anything. */
    "o.textContent=n.length?'Choose a network\\u2026':'No networks found';"
    "sel.appendChild(o);"
    "n.forEach(function(a){var e=document.createElement('option');"
    "e.value=a.s;e.textContent=a.s+(a.p?' \\ud83d\\udd12':'');sel.appendChild(e);});"
    "hint.textContent=n.length?'Not listed? Type it above.':"
    "'Scan found nothing. Type the name above.';"
    "}).catch(function(){sel.innerHTML='<option>Scan unavailable</option>';"
    "hint.textContent='Type the network name above.';});"
    /* Prefill from the stored configuration, so changing one field does not
     * mean retyping -- or silently resetting -- the others. A fresh device
     * returns empty strings and the compiled-in defaults above stand. */
    "fetch('/config').then(function(r){return r.json();}).then(function(c){"
    "sc=c;"
    "if(c.fw)document.getElementById('fw').textContent='Firmware '+c.fw;"
    "if(c.ssid)ssid.value=c.ssid;"
    "if(c.url)document.getElementById('url').value=c.url;"
    "if(c.model)document.getElementById('model').value=c.model;"
    "if(c.lang)document.getElementById('lang').value=c.lang;"
    /* Both selects, or saving from a reopened form silently resets them:
     * the layout to US, the send chord to Enter. */
    "if(c.layout)document.getElementById('layout').value=c.layout;"
    "if(c.sendkey)document.getElementById('sendkey').value=c.sendkey;"
    "if(c.has_key){key.placeholder='Leave blank to keep the stored key';"
    "document.getElementById('ck').style.display='block';}"
    "ph();"
    "}).catch(function(){});"
    "</script>";

static const char PAGE_SAVED[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:24px;background:#101418;"
    "color:#e8e8e8}</style>"
    "<h1>Saved</h1><p>The device is joining your network. "
    "This access point is going away.</p>";

static const char PAGE_ERASED[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Erased</title>"
    "<style>body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:24px;background:#101418;"
    "color:#e8e8e8}</style>"
    "<h1>Erased</h1><p>The stored Wi-Fi and API credentials are gone. This "
    "access point stays up so you can set the device up again.</p>";

/* ------------------------------------------------------------------ scan */

#define MAX_APS 20
#define SCAN_RECORDS 40

typedef struct {
    char ssid[33];
    bool secure;
    int8_t rssi;
} ap_entry_t;

static ap_entry_t s_aps[MAX_APS];
static size_t s_ap_count;

/* Scanned ONCE, before the access point is serving anyone. A scan makes the
 * radio hop channels, which would drop the very phone that asked for it, so
 * there is no rescan button: if a network is missing, the user types it. */
static void scan_networks(void)
{
    const wifi_scan_config_t cfg = {.show_hidden = false};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed; the user can still type a network name");
        return;
    }

    uint16_t n = SCAN_RECORDS;
    wifi_ap_record_t *recs = calloc(SCAN_RECORDS, sizeof(wifi_ap_record_t));
    if (recs == NULL || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        return;
    }

    for (uint16_t i = 0; i < n && s_ap_count < MAX_APS; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue; /* hidden */
        }
        /* The air is full of duplicates: mesh nodes, repeaters, both bands.
         * Keep the strongest sighting of each name. */
        bool dup = false;
        for (size_t j = 0; j < s_ap_count; j++) {
            if (strcmp(s_aps[j].ssid, ssid) == 0) {
                if (recs[i].rssi > s_aps[j].rssi) {
                    s_aps[j].rssi = recs[i].rssi;
                }
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        snprintf(s_aps[s_ap_count].ssid, sizeof(s_aps[s_ap_count].ssid), "%s", ssid);
        s_aps[s_ap_count].rssi   = recs[i].rssi;
        s_aps[s_ap_count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        s_ap_count++;
    }
    free(recs);

    /* Strongest first: the network you are standing next to should be at top. */
    for (size_t i = 1; i < s_ap_count; i++) {
        const ap_entry_t k = s_aps[i];
        size_t j = i;
        while (j > 0 && s_aps[j - 1].rssi < k.rssi) {
            s_aps[j] = s_aps[j - 1];
            j--;
        }
        s_aps[j] = k;
    }

    ESP_LOGI(TAG, "scan found %u networks", (unsigned)s_ap_count);
}

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
    /* A phone decides it is behind a captive portal by fetching a known URL and
     * checking it got the expected answer. Apple wants a page titled "Success",
     * Android wants a bare 204, Windows wants "Microsoft Connect Test".
     *
     * Answering all of them with our form gives the right verdict but the wrong
     * behaviour: iOS marks the network as captive and then waits to be
     * redirected. A 302 to the portal is what actually opens the sheet. */
    char host[64] = {0};
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));

    if (strcmp(host, AP_IP) != 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, NULL, 0);
        ESP_LOGD(TAG, "probe for \"%s\" -> 302", host);
        return ESP_OK;
    }
    return send_html(req, PAGE_FORM, sizeof(PAGE_FORM) - 1);
}

/* [{"s":"name","p":1},...] — `p` is protected, i.e. show a padlock.
 * Each SSID goes through json_escape(): those 32 bytes came off the air. */
static esp_err_t get_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    httpd_resp_sendstr_chunk(req, "[");
    for (size_t i = 0; i < s_ap_count; i++) {
        char esc[sizeof(s_aps[i].ssid) * 2 + 1];
        if (json_escape(s_aps[i].ssid, strlen(s_aps[i].ssid), esc, sizeof(esc)) < 0) {
            continue; /* unrepresentable; drop it rather than emit broken JSON */
        }
        char item[sizeof(esc) + 32];
        snprintf(item, sizeof(item), "%s{\"s\":\"%s\",\"p\":%d}", i ? "," : "", esc,
                 s_aps[i].secure ? 1 : 0);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* Scratch for handlers that need the stored configuration. File-scope rather
 * than a local: the struct is ~700 bytes and post_save's frame already holds
 * the 2.3 KB body on the default 4 KB httpd stack. esp_http_server runs every
 * handler on its single server task, so nothing races it. It briefly holds
 * both secrets, hence the wipe after every use. */
static app_config_t s_stored;

/* One "key":"value", pair. A value that cannot be escaped into the buffer
 * becomes "", which the page treats the same as nothing-stored. */
static void send_cfg_field(httpd_req_t *req, const char *key, const char *val)
{
    /* Sized for the worst legal case: a 255-byte URL of nothing but
     * backslashes doubles; a 32-byte SSID of control bytes grows sixfold. */
    char esc[CONFIG_STT_URL_CAP * 2 + 1];
    if (json_escape(val, strlen(val), esc, sizeof(esc)) < 0) {
        esc[0] = '\0';
    }
    char item[sizeof(esc) + 24];
    snprintf(item, sizeof(item), "\"%s\":\"%s\",", key, esc);
    httpd_resp_sendstr_chunk(req, item);
}

/* Stored values for the form to prefill, so reconfiguring one field does not
 * silently reset the rest to compiled-in defaults. Non-secrets only: the
 * Wi-Fi password and API key never leave the device. The page learns just
 * whether they exist, to offer "leave blank to keep". Every value was typed
 * by a user once, so it is escaped like the off-the-air SSIDs in /scan. */
static esp_err_t get_config(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    memset(&s_stored, 0, sizeof(s_stored));
    (void)config_load(&s_stored); /* failure leaves empty fields: "nothing stored" */

    httpd_resp_sendstr_chunk(req, "{");
    send_cfg_field(req, "fw", esp_app_get_description()->version); /* release+githash */
    send_cfg_field(req, "ssid", s_stored.wifi_ssid);
    send_cfg_field(req, "url", s_stored.stt_url);
    send_cfg_field(req, "model", s_stored.stt_model);
    send_cfg_field(req, "lang", s_stored.stt_language);
    send_cfg_field(req, "layout", s_stored.kbd_layout);
    char flags[48];
    snprintf(flags, sizeof(flags), "\"sendkey\":%d,\"has_pass\":%d,\"has_key\":%d}",
             (int)s_stored.send_key, s_stored.wifi_pass[0] ? 1 : 0,
             s_stored.api_key[0] ? 1 : 0);
    memset(&s_stored, 0, sizeof(s_stored));
    httpd_resp_sendstr_chunk(req, flags);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
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

/* Multipart fields are emitted verbatim. Keep user-provided strings to visible
 * ASCII so a CR/LF cannot add a field or header, and URLs cannot be malformed
 * into a surprising request. */
static bool printable_ascii(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x21 || (unsigned char)*s > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool valid_stt_url(const char *url)
{
    return printable_ascii(url) &&
           (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0) &&
           strchr(url, '@') == NULL;
}

static esp_err_t post_save(httpd_req_t *req)
{
    /* static, not on the stack: 2300 bytes of body plus a ~700-byte config below
     * overflowed the httpd task stack and reset the board on Save. The AP serves
     * one client and httpd handles requests serially, so a single shared buffer
     * is safe. */
    static char body[MAX_BODY];
    static app_config_t cfg;
    size_t len = 0;
    if (read_body(req, body, sizeof(body), &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }

    memset(&cfg, 0, sizeof(cfg));

    /* form_get refuses to truncate: an over-long value is an error, not a
     * silently clipped credential that fails to authenticate later. */
    const int nssid = form_get(body, len, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    const int npass = form_get(body, len, "pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    const int nkey  = form_get(body, len, "key", cfg.api_key, sizeof(cfg.api_key));
    const int nurl = form_get(body, len, "url", cfg.stt_url, sizeof(cfg.stt_url));
    const int nmodel = form_get(body, len, "model", cfg.stt_model, sizeof(cfg.stt_model));
    const int nlang = form_get(body, len, "lang", cfg.stt_language, sizeof(cfg.stt_language));
    const int nlayout = form_get(body, len, "layout", cfg.kbd_layout, sizeof(cfg.kbd_layout));
    char clearv[8];
    const int nclear = form_get(body, len, "clearkey", clearv, sizeof(clearv));

    /* One digit of send_key_t. A missing or bogus value falls back to Enter,
     * which is what cfg was zeroed to -- a bad dropdown must not block saving
     * Wi-Fi credentials. */
    char skbuf[8] = {0};
    const int nsk = form_get(body, len, "sendkey", skbuf, sizeof(skbuf));

    /* Wipe the body: it held both passwords in plaintext. */
    memset(body, 0, sizeof(body));

    if (nsk > 0) {
        const int v = atoi(skbuf);
        if (v >= 0 && v < SEND_KEY_COUNT) {
            cfg.send_key = (send_key_t)v;
        }
    }

    if (nssid <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi network is required");
        return ESP_OK;
    }
    if (npass == FORMDEC_BAD || nkey == FORMDEC_BAD || nurl == FORMDEC_BAD ||
        nmodel == FORMDEC_BAD || nlang == FORMDEC_BAD || nlayout == FORMDEC_BAD ||
        nclear == FORMDEC_BAD) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value too long or malformed");
        return ESP_OK;
    }
    if (nurl <= 0 || nmodel <= 0 || !valid_stt_url(cfg.stt_url) ||
        !printable_ascii(cfg.stt_model) || !printable_ascii(cfg.stt_language)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "enter an HTTP(S) endpoint and a valid model name");
        return ESP_OK;
    }
    /* Absent (an old cached form) or empty means the default; anything else
     * must name a layout this firmware ships. Storing an unknown token would
     * only surface as silently wrong typing much later. */
    if (cfg.kbd_layout[0] != '\0' && keymap_by_name(cfg.kbd_layout) == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown keyboard layout");
        return ESP_OK;
    }

    /* The form never echoes secrets back, so an empty secret field means
     * "keep what is stored", with two deliberate exceptions:
     *  - a Wi-Fi password belongs to its network. If the SSID changed, empty
     *    means an open network, not the old network's password;
     *  - the API key survives endpoint changes (one key often serves several
     *    OpenAI-compatible URLs). Ticking "forget the stored API key" is the
     *    explicit way to drop it. */
    memset(&s_stored, 0, sizeof(s_stored));
    (void)config_load(&s_stored);
    if (npass <= 0 && strcmp(cfg.wifi_ssid, s_stored.wifi_ssid) == 0) {
        memcpy(cfg.wifi_pass, s_stored.wifi_pass, sizeof(cfg.wifi_pass));
    }
    if (nkey <= 0 && nclear <= 0) {
        memcpy(cfg.api_key, s_stored.api_key, sizeof(cfg.api_key));
    }
    memset(&s_stored, 0, sizeof(s_stored));

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
    send_html(req, PAGE_ERASED, sizeof(PAGE_ERASED) - 1);
    /* Stay in setup: an erased device is unprovisioned, which is exactly what
     * the portal is for. Posting EV_PROVISIONED here would try to associate
     * with the credentials we just wiped and fall into the error state. The AP
     * stays up for reconfiguration; the next boot comes up unprovisioned. */
    return ESP_OK;
}

/* Not named httpd_start(): that is esp_http_server's own symbol, and shadowing
 * it here turns the call below into unbounded recursion. */
static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    /* The default 4 KB is not enough for post_save once the endpoint/model
     * fields grew MAX_BODY to 2300 and app_config_t past 700 bytes: the handler
     * frame overflowed the task stack and the board reset on Save (looked like a
     * power-off). Give it real headroom; the buffers are also moved off the
     * stack (see post_save), but a comfortable stack is the belt to that brace. */
    cfg.stack_size = 8192;
    /* The AP is ours alone; a short timeout keeps a stalled phone from
     * holding the single worker. */
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");

    const httpd_uri_t save  = {.uri = "/save", .method = HTTP_POST, .handler = post_save};
    const httpd_uri_t erase = {.uri = "/erase", .method = HTTP_POST, .handler = post_erase};
    const httpd_uri_t scan  = {.uri = "/scan", .method = HTTP_GET, .handler = get_scan};
    const httpd_uri_t conf  = {.uri = "/config", .method = HTTP_GET, .handler = get_config};
    const httpd_uri_t any   = {.uri = "/*", .method = HTTP_GET, .handler = get_any};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save), TAG, "uri save");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &erase), TAG, "uri erase");
    /* Registered before the wildcard handler: matching runs in registration
     * order, and the catch-all would happily swallow "/scan" and "/config". */
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &scan), TAG, "uri scan");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &conf), TAG, "uri config");
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

    /* A 1 s receive timeout so recvfrom returns periodically and the loop can
     * notice the stop signal. Without it the task would block forever and
     * provisioning_stop could not retire it when setup returns to STA. */
    const struct timeval rcv_to = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));

    static const uint8_t ap_ip[4] = {192, 168, 4, 1};
    uint8_t buf[256];
    for (;;) {
        if (s_dns_stop) {
            close(sock);
            vTaskDelete(NULL);
            return;
        }
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue; /* timeout (re-check the stop flag) or a spurious wakeup */
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
     * running HTTP server would each fail in its own way. (s_started lives at
     * file scope now, so provisioning_stop can clear it.) */
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
    /* Forget any prior STA run-state so the reconnect after save does a full
     * start rather than trying to re-associate on a stopped driver. */
    net_wifi_sta_forget();
    s_dns_stop = false; /* clear a stale stop from a previous portal session */

    s_ap_netif = esp_netif_create_default_wifi_ap();
    net_wifi_ensure_sta_netif();

    /* Hand the client a DNS server, namely us.
     *
     * ESP-IDF's SoftAP DHCP server does NOT offer one by default. Without it a
     * phone joins, gets an address, and has nowhere to resolve
     * captive.apple.com -- so its probe fails, no portal sheet appears, and the
     * DNS responder we so carefully fuzzed sits on port 53 with nobody asking
     * it anything. The network joins. The page never opens. */
    esp_netif_dns_info_t dns = {.ip = {.type = ESP_IPADDR_TYPE_V4}};
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(AP_IP);

    /* DHCP is not running yet, so the stop is a no-op; set_dns_info refuses
     * while it runs, hence the ordering. */
    (void)esp_netif_dhcps_stop(s_ap_netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns), TAG, "dns info");

    uint8_t offer_dns = 2; /* dhcps_offer_option: OFFER_DNS */
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                               &offer_dns, sizeof(offer_dns)),
                        TAG, "dhcps dns option");
    (void)esp_netif_dhcps_start(s_ap_netif);

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

    /* APSTA, not AP: scanning needs the station interface. The STA side never
     * associates -- net_wifi.c gates that on s_want_connect. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "ap config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    /* The setup-time brownout hit exactly here: a phone joining the AP triggers
     * a TX burst that trips the AXP2101. Cap the power before anyone connects. */
    net_wifi_limit_tx_power();

    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "http");
    xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 4, NULL, 0);

    s_started = true;
    s_info    = *info;

    /* The AP password is printed on the screen anyway, and this log only
     * reaches a serial console. The API key is never logged. */
    ESP_LOGI(TAG, "setup AP up: ssid=%s pass=%s url=%s", info->ssid, info->pass, info->url);
    return ESP_OK;
}

void provisioning_scan(void)
{
    /* Split out of provisioning_start so the caller can paint the setup screen
     * first: a full-channel scan blocks ~2 s, and running it before the screen
     * switched made entering setup feel frozen. Run right after the screen is up,
     * still before any phone has joined the AP, so the scan's channel hopping
     * disturbs no one. The cached result is what the /scan endpoint returns. */
    if (!s_started) {
        return;
    }
    s_ap_count = 0;
    scan_networks();
}

void provisioning_stop(void)
{
    if (!s_started) {
        return;
    }

    /* Tear the portal down so the radio can return to plain STA in place. We do
     * this instead of rebooting because esp_restart() powers this AXP2101 board
     * off (a software reset drops the rails) rather than restarting it. Order:
     * stop serving, retire the DNS task, stop the radio, then drop the AP netif.
     * net_wifi_sta_connect() runs next and does a clean STA start. */
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    /* Ask the DNS task to exit and give it more than its 1 s recv timeout to
     * close the socket, so its port is free if setup is reopened later. */
    s_dns_stop = true;
    vTaskDelay(pdMS_TO_TICKS(1200));

    esp_wifi_stop();

    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    s_started = false;
    ESP_LOGI(TAG, "setup portal down; returning to station mode");
}
