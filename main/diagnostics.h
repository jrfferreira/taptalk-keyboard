/* A heartbeat that tells you which task died.
 *
 * The LVGL task once wedged on a 131 KB transform layer while every other task
 * kept running. The serial log looked completely healthy -- the state machine
 * logged its transitions, the upload succeeded, HID typed -- and nothing said
 * "the screen stopped rendering thirty seconds ago". Finding that took reading
 * timestamps and noticing a recording had run to its 30 s cap.
 *
 * So: every task that can plausibly hang bumps a counter, and one low-priority
 * task prints whether those counters are moving. */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Bumped from the LVGL timer, the audio capture task, and the state machine. */
void diag_beat_ui(void);
void diag_beat_audio(void);
void diag_beat_sm(void);

/* Spawns the reporter. Call last, once everything else is up. */
esp_err_t diag_start(void);
