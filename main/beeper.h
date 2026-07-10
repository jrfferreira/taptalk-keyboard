/* Short confirmation tones through the ES8311 and the NS4150B amplifier.
 *
 * The board has no haptic motor -- no LRA, no ERM, no DRV2605 -- so a click is
 * the only way to confirm a press without the user looking at the screen,
 * which is the whole point when the thing is held up to your mouth.
 *
 * The tones share the microphone's 16 kHz I2S configuration, because
 * bsp_audio_init() no-ops on a second call and audio_capture_start() got there
 * first. A tone generated at any other rate would play back at the wrong
 * pitch. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Duration of the press tone. The recorder swallows this much of its own
 * leading audio so the click is not transcribed. */
#define BEEP_PRESS_MS 34

typedef enum {
    BEEP_PRESS,   /* rising, bright: recording started */
    BEEP_RELEASE, /* falling, softer: recording stopped */
    BEEP_ERROR,   /* low double blip */
} beep_t;

/* Opens the speaker codec. Must run AFTER audio_capture_start(), which is what
 * pins I2S to 16 kHz. Never fatal: on failure the device simply stays silent. */
esp_err_t beeper_init(void);

/* Returns immediately; the tone plays on the beeper task. Safe from the LVGL
 * render task and from the state machine. */
void beeper_play(beep_t tone);

bool beeper_available(void);
