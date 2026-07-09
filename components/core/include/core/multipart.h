/* multipart/form-data framing for OpenAI /v1/audio/transcriptions.
 *
 * The endpoint rejects a raw audio body; the WAV must be one part of a
 * multipart form alongside a `model` field. We build only the small preamble
 * and epilogue here and compute the exact Content-Length, so the ~1 MB of
 * audio can be streamed straight out of PSRAM without a second copy. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MULTIPART_PREAMBLE_CAP 512
#define MULTIPART_EPILOGUE_CAP 64
#define MULTIPART_CTYPE_CAP    128

typedef struct {
    char preamble[MULTIPART_PREAMBLE_CAP];
    size_t preamble_len;
    char epilogue[MULTIPART_EPILOGUE_CAP];
    size_t epilogue_len;
    char content_type[MULTIPART_CTYPE_CAP]; /* value for the Content-Type header */
    size_t content_length;                  /* preamble + file_len + epilogue */
} multipart_t;

/* Build the framing around a file part of exactly `file_len` bytes.
 * `boundary` is the bare token (no leading dashes); delimiter lines get the
 * "--" prefix that RFC 7578 requires.
 * `language` may be NULL to omit the field.
 * Returns false if anything would overflow a fixed buffer. */
bool multipart_build(multipart_t *mp, const char *boundary, const char *model,
                     const char *response_format, const char *language, const char *filename,
                     size_t file_len);
