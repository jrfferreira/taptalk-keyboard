/* Speech-to-text over HTTPS.
 *
 * OpenAI's /v1/audio/transcriptions rejects a raw audio body: the WAV has to
 * be one part of a multipart/form-data form. The framing and the exact
 * Content-Length come from core/multipart.c, so the ~1 MB of audio streams
 * straight out of PSRAM and is never copied.
 *
 * response_format=text means the reply is the transcript itself. No JSON
 * parser on the device. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Uploads `wav` on a background task and posts EV_STT_OK, EV_STT_EMPTY (the
 * clip was silence) or EV_STT_FAIL. Requires a synced clock: without it
 * mbedTLS rejects every certificate. */
esp_err_t stt_start(const char *api_key, const uint8_t *wav, size_t wav_len);

/* Asks the in-flight request to give up. */
void stt_abort(void);

/* Valid after EV_STT_OK. Normalised, NUL-terminated. */
const char *stt_transcript(void);

/* A short, human-readable reason after EV_STT_FAIL. Never contains the key. */
const char *stt_error(void);
