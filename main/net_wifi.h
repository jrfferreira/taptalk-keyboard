/* Wi-Fi station plus SNTP.
 *
 * SNTP is not optional. mbedTLS validates a certificate's notBefore against
 * the system clock, which reads 1970 until we sync, so every HTTPS request
 * fails with a confusing cert error until the time is right. */
#pragma once

#include "esp_err.h"

esp_err_t net_wifi_init(void);

/* Associates and, on success, posts EV_WIFI_UP. A disconnect posts
 * EV_TIMEOUT so the state machine can decide whether to retry or give up. */
esp_err_t net_wifi_connect(void);

/* Blocks until the clock looks sane, then posts EV_TIME_OK, or EV_TIME_FAIL
 * on timeout. Runs on its own short-lived task; safe to call from app_sm. */
void net_sntp_start(void);

uint32_t net_wifi_retries(void);
