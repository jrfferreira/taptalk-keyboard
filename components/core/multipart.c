#include "core/multipart.h"

#include <stdio.h>
#include <string.h>

/* RFC 7578: every line break in the framing is CRLF, delimiter lines carry a
 * "--" prefix, and the final delimiter carries a "--" suffix as well. */
bool multipart_build(multipart_t *mp, const char *boundary, const char *model,
                     const char *response_format, const char *language, const char *filename,
                     size_t file_len)
{
    if (mp == NULL || boundary == NULL || model == NULL || response_format == NULL ||
        filename == NULL || file_len == 0) {
        return false;
    }

    memset(mp, 0, sizeof(*mp));

    int n = snprintf(mp->content_type, sizeof(mp->content_type),
                     "multipart/form-data; boundary=%s", boundary);
    if (n < 0 || (size_t)n >= sizeof(mp->content_type)) {
        return false;
    }

    size_t w = 0;
    char *p  = mp->preamble;
    const size_t cap = sizeof(mp->preamble);

#define APPEND(...)                                              \
    do {                                                         \
        int _n = snprintf(p + w, cap - w, __VA_ARGS__);           \
        if (_n < 0 || (size_t)_n >= cap - w) {                    \
            return false;                                        \
        }                                                        \
        w += (size_t)_n;                                         \
    } while (0)

    APPEND("--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n", boundary, model);
    APPEND("--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\n%s\r\n",
           boundary, response_format);
    if (language != NULL && language[0] != '\0') {
        APPEND("--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n%s\r\n", boundary,
               language);
    }
    /* The server sniffs the container from the filename extension, so the
     * ".wav" here is load-bearing, not decorative. */
    APPEND("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
           "Content-Type: audio/wav\r\n\r\n",
           boundary, filename);

#undef APPEND

    mp->preamble_len = w;

    n = snprintf(mp->epilogue, sizeof(mp->epilogue), "\r\n--%s--\r\n", boundary);
    if (n < 0 || (size_t)n >= sizeof(mp->epilogue)) {
        return false;
    }
    mp->epilogue_len = (size_t)n;

    mp->content_length = mp->preamble_len + file_len + mp->epilogue_len;
    return true;
}
