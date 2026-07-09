/* Persistent configuration in NVS.
 *
 * SECURITY: the OpenAI key is stored in PLAINTEXT. Flash encryption is not
 * enabled (it burns eFuses irreversibly and breaks plaintext reflashing, which
 * the web-flasher story depends on). Anyone with physical access to the board
 * and a USB cable can recover the key with `esptool read_flash`. This is a
 * deliberate, documented trade-off for a personal device — see docs/security.md.
 * Erase the key with config_erase() before lending or selling the board. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* 32 bytes is the 802.11 SSID limit; 64 is the WPA2 passphrase limit.
 * OpenAI project keys run to roughly 164 characters today. */
#define CONFIG_SSID_CAP    33
#define CONFIG_PASS_CAP    65
#define CONFIG_API_KEY_CAP 256

typedef struct {
    char wifi_ssid[CONFIG_SSID_CAP];
    char wifi_pass[CONFIG_PASS_CAP];
    char api_key[CONFIG_API_KEY_CAP];
} app_config_t;

/* Reads NVS into *cfg. Missing keys yield empty strings, not an error.
 * On first boot, seeds NVS from main/secrets.h if that file exists. */
esp_err_t config_load(app_config_t *cfg);

esp_err_t config_save(const app_config_t *cfg);

/* True once an SSID is stored. The API key is allowed to be absent so the
 * network can be brought up and debugged before the key is pasted in. */
bool config_is_provisioned(const app_config_t *cfg);

bool config_has_api_key(const app_config_t *cfg);

/* Wipes the whole namespace. Used by the portal's "Erase credentials". */
esp_err_t config_erase(void);

/* "sk-proj-…GH4a" — for logs and the on-screen status line. Never log the
 * key itself; ESP_LOG output goes over a serial console anyone can attach. */
void config_mask_key(const char *key, char *out, size_t out_cap);
