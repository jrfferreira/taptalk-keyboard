/* Canonical 44-byte RIFF/WAVE header for uncompressed PCM. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define WAV_HEADER_SIZE 44

/* Write the header for a PCM payload of `pcm_bytes` into `dst`.
 * The payload is expected to follow immediately at dst + WAV_HEADER_SIZE,
 * which is how we record: straight into the buffer, header backfilled on stop.
 *
 * Returns WAV_HEADER_SIZE on success, 0 if dst is too small or any argument
 * is zero. All multi-byte fields are little-endian per the RIFF spec. */
size_t wav_write_header(uint8_t *dst, size_t dst_cap, uint32_t pcm_bytes, uint32_t sample_rate,
                        uint16_t bits_per_sample, uint16_t channels);
