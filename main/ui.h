/* LVGL screen.
 *
 * The state machine never calls LVGL. It calls the ui_set_* functions below,
 * which take a mutex and update a plain struct. A single lv_timer, running
 * inside the BSP's LVGL task and therefore already holding the LVGL lock,
 * polls that struct and is the only place widgets are ever touched. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/sm.h"
#include "esp_err.h"
#include "pmic.h"
#include "provisioning.h"

/* Call after bsp_display_start(). Takes the LVGL lock internally. */
esp_err_t ui_init(void);

void ui_set_state(app_state_t state);
void ui_set_level(int percent);              /* live mic meter, 0..100 */
void ui_set_clip(uint32_t ms, int peak);
void ui_set_wifi(bool up, const char *ip);
void ui_set_time(bool ok, const char *iso);
void ui_set_pmic(const pmic_status_t *status);
void ui_set_msg(const char *msg);
void ui_set_api_key(const char *masked);

/* Swaps to the setup screen and shows how to reach the portal. */
void ui_show_setup(const prov_info_t *info);
