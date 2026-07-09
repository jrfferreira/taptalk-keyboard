/* The impure half of the state machine: owns the state variable, drains the
 * event queue, and executes the actions that core/sm.c hands back. Every
 * other task posts events here and never reads the state directly. */
#pragma once

#include "core/sm.h"

void app_sm_start(void);

/* Safe from any task, and from LVGL callbacks. Drops the event if the queue
 * is full rather than blocking a caller that may be the LVGL render task. */
void app_sm_post(app_event_t event);

/* Snapshot for the UI. Cheap; no lock needed, single writer, word-sized. */
app_state_t app_sm_state(void);
