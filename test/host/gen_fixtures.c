/* Emits artifacts from the real core code so an independent parser can judge
 * them. Field-by-field assertions cannot catch a wrong chunk offset or a
 * byte-order slip that still "looks right" to the code that wrote it. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "core/multipart.h"
#include "core/wav.h"

#define RATE 16000
#define SECONDS 1
#define SAMPLES (RATE * SECONDS)

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <out.wav> <out.multipart>\n", argv[0]);
        return 2;
    }

    /* A 440 Hz tone, written exactly the way audio_capture.c does it:
     * PCM straight into buf+44, header backfilled afterwards. */
    static uint8_t buf[WAV_HEADER_SIZE + SAMPLES * 2];
    int16_t *pcm = (int16_t *)(buf + WAV_HEADER_SIZE);
    for (int i = 0; i < SAMPLES; i++) {
        pcm[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * i / RATE));
    }
    if (wav_write_header(buf, sizeof(buf), SAMPLES * 2, RATE, 16, 1) != WAV_HEADER_SIZE) {
        return 1;
    }

    FILE *f = fopen(argv[1], "wb");
    if (!f) return 1;
    fwrite(buf, 1, sizeof(buf), f);
    fclose(f);

    /* The exact bytes net_stt.c will put on the wire: preamble, the WAV
     * above, then the epilogue. */
    multipart_t mp;
    if (!multipart_build(&mp, "----taptalkBOUNDARY", "gpt-4o-mini-transcribe", "text", "en",
                         "audio.wav", sizeof(buf))) {
        return 1;
    }
    f = fopen(argv[2], "wb");
    if (!f) return 1;
    fwrite(mp.preamble, 1, mp.preamble_len, f);
    fwrite(buf, 1, sizeof(buf), f);
    fwrite(mp.epilogue, 1, mp.epilogue_len, f);
    fclose(f);

    /* stdout carries the metadata the validator needs to check us against. */
    printf("%s\n%zu\n", mp.content_type, mp.content_length);
    return 0;
}
