#include "core/wav.h"
#include "test_util.h"

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

TEST_MAIN("wav", {
    uint8_t h[WAV_HEADER_SIZE];

    /* The exact shape we record: 16 kHz, 16-bit, mono, one second. */
    const uint32_t pcm = 32000;
    CHECK_EQ_INT(wav_write_header(h, sizeof(h), pcm, 16000, 16, 1), WAV_HEADER_SIZE);

    CHECK(memcmp(h + 0, "RIFF", 4) == 0);
    CHECK(memcmp(h + 8, "WAVE", 4) == 0);
    CHECK(memcmp(h + 12, "fmt ", 4) == 0);
    CHECK(memcmp(h + 36, "data", 4) == 0);

    CHECK_EQ_INT(u32(h + 4), 36 + pcm);
    CHECK_EQ_INT(u32(h + 16), 16);   /* PCM fmt chunk size */
    CHECK_EQ_INT(u16(h + 20), 1);    /* WAVE_FORMAT_PCM */
    CHECK_EQ_INT(u16(h + 22), 1);    /* mono */
    CHECK_EQ_INT(u32(h + 24), 16000);
    CHECK_EQ_INT(u32(h + 28), 32000); /* byte rate = 16000 * 1 * 2 */
    CHECK_EQ_INT(u16(h + 32), 2);     /* block align */
    CHECK_EQ_INT(u16(h + 34), 16);
    CHECK_EQ_INT(u32(h + 40), pcm);

    /* Stereo 16-bit derives a different block align and byte rate. */
    CHECK_EQ_INT(wav_write_header(h, sizeof(h), 100, 44100, 16, 2), WAV_HEADER_SIZE);
    CHECK_EQ_INT(u16(h + 32), 4);
    CHECK_EQ_INT(u32(h + 28), 44100 * 4);

    /* An empty clip still yields a structurally valid header. */
    CHECK_EQ_INT(wav_write_header(h, sizeof(h), 0, 16000, 16, 1), WAV_HEADER_SIZE);
    CHECK_EQ_INT(u32(h + 40), 0);
    CHECK_EQ_INT(u32(h + 4), 36);

    /* Refuses to scribble past a short buffer or accept degenerate params. */
    CHECK_EQ_INT(wav_write_header(h, WAV_HEADER_SIZE - 1, pcm, 16000, 16, 1), 0);
    CHECK_EQ_INT(wav_write_header(NULL, sizeof(h), pcm, 16000, 16, 1), 0);
    CHECK_EQ_INT(wav_write_header(h, sizeof(h), pcm, 0, 16, 1), 0);
    CHECK_EQ_INT(wav_write_header(h, sizeof(h), pcm, 16000, 16, 0), 0);
})
