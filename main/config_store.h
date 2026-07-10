/* Persistent configuration in NVS.
 *
 * SECURITY: the transcription API key is stored in PLAINTEXT. Flash encryption is not
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
 * Provider keys and endpoint URLs can both be substantially longer. */
#define CONFIG_SSID_CAP        33
#define CONFIG_PASS_CAP        65
#define CONFIG_API_KEY_CAP     256
#define CONFIG_STT_URL_CAP     256
#define CONFIG_STT_MODEL_CAP   64
#define CONFIG_STT_LANGUAGE_CAP 16
#define CONFIG_KBD_LAYOUT_CAP  16

#define CONFIG_DEFAULT_STT_URL   "https://api.openai.com/v1/audio/transcriptions"
#define CONFIG_DEFAULT_STT_MODEL "gpt-4o-mini-transcribe"

/* What the Send button strikes. Enter is the safe default; the modified forms
 * are what chat apps (Slack, Discord, iMessage on macOS) bind to "send" so a
 * bare Enter inserts a newline instead. Chosen on the setup portal and stored
 * as a small integer, so adding a form must only ever append here. */
typedef enum {
    SEND_KEY_ENTER = 0,
    SEND_KEY_CMD_ENTER,   /* GUI (Cmd/Win) + Enter */
    SEND_KEY_CTRL_ENTER,  /* Ctrl + Enter */
    SEND_KEY_SHIFT_ENTER, /* Shift + Enter */
    SEND_KEY_COUNT,
} send_key_t;

typedef struct {
    char wifi_ssid[CONFIG_SSID_CAP];
    char wifi_pass[CONFIG_PASS_CAP];
    char api_key[CONFIG_API_KEY_CAP];
    char stt_url[CONFIG_STT_URL_CAP];
    char stt_model[CONFIG_STT_MODEL_CAP];
    char stt_language[CONFIG_STT_LANGUAGE_CAP];
    /* A keymap name from core/keymap.h — the layout the HOST is configured
     * with, not a preference of ours: the host interprets our scancodes
     * through whatever layout it has set. */
    char kbd_layout[CONFIG_KBD_LAYOUT_CAP];
    send_key_t send_key;
} app_config_t;

/* Reads NVS into *cfg. Missing keys yield empty strings, not an error.
 * On first boot, seeds NVS from main/secrets.h if that file exists. */
esp_err_t config_load(app_config_t *cfg);

esp_err_t config_save(const app_config_t *cfg);

/* True once an SSID is stored. The API key is allowed to be absent for local
 * transcription servers that do not use bearer authentication. */
bool config_is_provisioned(const app_config_t *cfg);

/* Older NVS records predate configurable backends. Empty URL/model fields map
 * to the OpenAI defaults so firmware upgrades preserve existing behaviour. */
const char *config_stt_url(const app_config_t *cfg);
const char *config_stt_model(const app_config_t *cfg);
bool config_stt_uses_tls(const app_config_t *cfg);

/* Empty maps to "us" — records written before layouts were configurable keep
 * typing exactly as they did. */
const char *config_kbd_layout(const app_config_t *cfg);

/* Wipes the whole namespace. Used by the portal's "Erase credentials". */
esp_err_t config_erase(void);

/* "sk-proj-…GH4a" — for logs and the on-screen status line. Never log the
 * key itself; ESP_LOG output goes over a serial console anyone can attach. */
void config_mask_key(const char *key, char *out, size_t out_cap);
