/* Wi-Fi plus SNTP.
 *
 * SNTP is not optional. mbedTLS validates a certificate's notBefore against
 * the system clock, which reads 1970 until we sync, so every HTTPS request
 * fails with a confusing cert error until the time is right. */
#pragma once

#include <stdint.h>

#include "config_store.h"
#include "esp_err.h"

/* NVS, netif, the default event loop, and esp_wifi_init(). Must run before
 * either net_wifi_sta_connect() or provisioning_start(), which choose the
 * radio mode between them. */
esp_err_t net_init_common(void);

/* Creates the station netif once. Provisioning needs it to scan; connecting
 * needs it to associate. Idempotent. */
void net_wifi_ensure_sta_netif(void);

/* Associates using stored credentials. Posts EV_WIFI_UP on success; a
 * disconnect posts EV_WIFI_DOWN and the state machine owns the retry policy. */
esp_err_t net_wifi_sta_connect(const app_config_t *cfg);

/* Blocks on its own short-lived task until the clock looks sane, then posts
 * EV_TIME_OK, or EV_TIME_FAIL on timeout. */
void net_sntp_start(void);

uint32_t net_wifi_retries(void);
