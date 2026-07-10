/* ES8311 microphone capture at 16 kHz / 16-bit / mono.
 *
 * The task runs continuously once started, so the on-screen level meter is
 * live even when idle — that is what tells us the AXP2101's ALDO1 rail is
 * actually powering the microphone. Recording merely decides whether the
 * samples are also appended to the clip. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_BITS        16
#define AUDIO_CHANNELS    1
#define AUDIO_MAX_SECONDS 30
#define AUDIO_MAX_PCM_BYTES ((size_t)AUDIO_MAX_SECONDS * AUDIO_SAMPLE_RATE * (AUDIO_BITS / 8) * AUDIO_CHANNELS)

/* A clip shorter than this is an accidental tap, not speech. There is no
 * peak/volume gate: a deliberate hold uploads even if quiet, and true silence
 * returns empty text from the backend rather than being dropped locally. */
#define AUDIO_MIN_CLIP_MS   300

/* Allocates the PSRAM clip, brings up I2S at 16 kHz, opens the codec, and
 * spawns the capture task. Must run after pmic_init(). */
esp_err_t audio_capture_start(void);

void audio_record_begin(void); /* reset cursor, start appending */
void audio_record_end(void);   /* stop appending, backfill the WAV header */

/* True once a completed clip is long enough and loud enough to be worth
 * uploading. Feeds the sm_guards_t.clip_usable guard. */
bool audio_clip_usable(void);

/* Why the last completed clip was unusable ("Too quiet ...", "Too short ..."),
 * or "" if it was fine. Valid after audio_record_end(), survives discard. */
const char *audio_clip_reject_reason(void);

uint32_t audio_clip_ms(void);
int audio_clip_peak(void);      /* 0..32767 */
size_t audio_clip_pcm_bytes(void);

/* The whole WAV, header included. Valid between audio_record_end() and the
 * next audio_record_begin(). Never freed; the state machine's ordering is
 * what guarantees the network task and the capture task never overlap. */
const uint8_t *audio_clip_wav(size_t *total_len);

void audio_clip_discard(void);
