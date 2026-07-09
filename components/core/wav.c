#include "core/wav.h"

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

size_t wav_write_header(uint8_t *dst, size_t dst_cap, uint32_t pcm_bytes, uint32_t sample_rate,
                        uint16_t bits_per_sample, uint16_t channels)
{
    if (dst == NULL || dst_cap < WAV_HEADER_SIZE) {
        return 0;
    }
    if (sample_rate == 0 || bits_per_sample == 0 || channels == 0) {
        return 0;
    }

    const uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8));
    const uint32_t byte_rate   = sample_rate * block_align;

    dst[0] = 'R'; dst[1] = 'I'; dst[2] = 'F'; dst[3] = 'F';
    put_u32le(dst + 4, 36 + pcm_bytes); /* size of everything after this field */
    dst[8] = 'W'; dst[9] = 'A'; dst[10] = 'V'; dst[11] = 'E';

    dst[12] = 'f'; dst[13] = 'm'; dst[14] = 't'; dst[15] = ' ';
    put_u32le(dst + 16, 16); /* PCM fmt chunk length */
    put_u16le(dst + 20, 1);  /* WAVE_FORMAT_PCM */
    put_u16le(dst + 22, channels);
    put_u32le(dst + 24, sample_rate);
    put_u32le(dst + 28, byte_rate);
    put_u16le(dst + 32, block_align);
    put_u16le(dst + 34, bits_per_sample);

    dst[36] = 'd'; dst[37] = 'a'; dst[38] = 't'; dst[39] = 'a';
    put_u32le(dst + 40, pcm_bytes);

    return WAV_HEADER_SIZE;
}
