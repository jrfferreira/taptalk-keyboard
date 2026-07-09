/* First-run setup over a SoftAP captive portal.
 *
 * The board raises a WPA2 access point, serves a form at any URL the phone
 * asks for, and writes what it gets to NVS. Pasting a 164-character API key
 * from a phone password manager is the only humane way to enter one; an
 * on-screen keyboard on a 368x448 panel is not. */
#pragma once

#include "esp_err.h"

#define PROV_AP_SSID_CAP 24
#define PROV_AP_PASS_CAP 16

typedef struct {
    char ssid[PROV_AP_SSID_CAP]; /* e.g. "TapTalk-A1B2" */
    char pass[PROV_AP_PASS_CAP]; /* freshly random each boot */
    char url[24];                /* "http://192.168.4.1" */
} prov_info_t;

/* Brings up the AP, the DNS redirector, and the HTTP server. Fills *info with
 * what the setup screen must display. Posts EV_PROVISIONED once credentials
 * are saved. */
esp_err_t provisioning_start(prov_info_t *info);
