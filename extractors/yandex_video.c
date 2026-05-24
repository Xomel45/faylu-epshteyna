#include "../http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Extracts the source videoUrl from a yandex.ru/video/touch/preview/... page.
 * Returns malloc'd URL string or NULL. The caller should re-dispatch to the
 * appropriate extractor for that URL. */
char *yandex_extract_source_url(const char *url, const char *proxy) {
    HttpSession *sess = http_session_new(proxy);
    const char *extra[] = { "Accept-Language: ru-RU,ru;q=0.8", NULL };
    HttpResp *resp = http_get(sess, url, "https://yandex.ru/", extra);
    http_session_free(sess);

    if (!resp || resp->status != 200) {
        if (resp) http_resp_free(resp);
        return NULL;
    }

    char *result = NULL;

    /* Look for "videoUrl":"<url>" */
    const char *p = strstr(resp->body, "\"videoUrl\":\"");
    if (p) {
        p += 12;
        const char *e = p;
        while (*e && *e != '"') {
            if (*e == '\\') e++;
            e++;
        }
        if (e > p) result = strndup(p, (size_t)(e - p));
    }

    http_resp_free(resp);
    return result;
}
