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
#include "provisioning.h"

/* Call after bsp_display_start(). Takes the LVGL lock internally. */
esp_err_t ui_init(void);

void ui_set_state(app_state_t state);
void ui_set_level(int percent);     /* live mic level, 0..100; drives the wave */
void ui_set_clip(uint32_t ms, int peak);
void ui_set_wifi(bool connected);
void ui_set_usb(bool connected);

/* Transient, low-stakes: "Transcribing…", "Typing…". Dim text, no badge. */
void ui_set_status(const char *text);

/* Something went wrong and the user needs to be able to read it. Raises a
 * warning badge; tapping the badge shows the full text, which is where a
 * message like "Rate limited or out of credit" actually earns its keep. The
 * badge stays until the next recording starts, so an error cannot flash past
 * while you are looking away. */
void ui_set_error(const char *text);

void ui_clear_msg(void);

/* Swaps to the setup screen and shows how to reach the portal. */
void ui_show_setup(const prov_info_t *info, bool can_exit);
