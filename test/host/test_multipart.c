#include "core/multipart.h"
#include "test_util.h"

TEST_MAIN("multipart", {
    multipart_t mp;

    CHECK(multipart_build(&mp, "BND", "gpt-4o-mini-transcribe", "text", NULL, "audio.wav", 1000));

    CHECK_EQ_STR(mp.content_type, "multipart/form-data; boundary=BND");

    /* Delimiter lines carry the "--" prefix; the header value does not. */
    CHECK_EQ_STR(mp.preamble,
                 "--BND\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n"
                 "gpt-4o-mini-transcribe\r\n"
                 "--BND\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\n"
                 "text\r\n"
                 "--BND\r\nContent-Disposition: form-data; name=\"file\"; "
                 "filename=\"audio.wav\"\r\n"
                 "Content-Type: audio/wav\r\n\r\n");

    /* The closing delimiter needs both the leading CRLF and the trailing "--". */
    CHECK_EQ_STR(mp.epilogue, "\r\n--BND--\r\n");
    CHECK_EQ_INT(mp.epilogue_len, 11);

    /* Content-Length must account for every framing byte, not just the audio.
     * Getting this wrong is a hang or a 400, and it is invisible on-device. */
    CHECK_EQ_INT(mp.content_length, mp.preamble_len + 1000 + mp.epilogue_len);
    CHECK_EQ_INT(mp.preamble_len, strlen(mp.preamble));

    /* Optional language field is inserted before the file part when present. */
    multipart_t ml;
    CHECK(multipart_build(&ml, "BND", "m", "text", "pt", "a.wav", 10));
    CHECK(strstr(ml.preamble, "name=\"language\"\r\n\r\npt\r\n") != NULL);
    CHECK(ml.preamble_len > mp.preamble_len - 40); /* grew, roughly */

    /* Empty language string is treated as absent. */
    multipart_t me;
    CHECK(multipart_build(&me, "BND", "m", "text", "", "a.wav", 10));
    CHECK(strstr(me.preamble, "language") == NULL);

    /* The file part must come last: the server streams it, and any field
     * after a 1 MB body would be read only after the whole upload. */
    const char *file_at = strstr(mp.preamble, "name=\"file\"");
    const char *model_at = strstr(mp.preamble, "name=\"model\"");
    CHECK(file_at != NULL && model_at != NULL && model_at < file_at);

    /* Rejects rather than truncates. */
    CHECK(!multipart_build(&mp, "BND", "m", "text", NULL, "a.wav", 0)); /* empty file */
    CHECK(!multipart_build(&mp, NULL, "m", "text", NULL, "a.wav", 10));
    CHECK(!multipart_build(NULL, "BND", "m", "text", NULL, "a.wav", 10));

    char huge[600];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CHECK(!multipart_build(&mp, huge, "m", "text", NULL, "a.wav", 10)); /* overflows preamble */

    /* A realistic 30 s clip: 44-byte header + 960000 PCM bytes. */
    multipart_t big;
    CHECK(multipart_build(&big, "----taptalk0000", "gpt-4o-mini-transcribe", "text", NULL,
                          "audio.wav", 44 + 960000));
    CHECK_EQ_INT(big.content_length, big.preamble_len + 960044 + big.epilogue_len);
    CHECK_EQ_STR(big.content_type, "multipart/form-data; boundary=----taptalk0000");
})
