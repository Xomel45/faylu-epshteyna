#include "extractor.h"
#include "../http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ebalko.net does a JS redirect to 34ebalka.ru.actor with the same path.
 * That page has ya:ovs:content_url with the direct MP4 URL. */
static char *rewrite_to_canonical(const char *url) {
    /* Replace any ebalko hostname with 34ebalka.ru.actor */
    const char *path = NULL;

    /* Find start of path (after scheme + host) */
    const char *s = strstr(url, "://");
    if (s) {
        path = strchr(s + 3, '/');
    }
    if (!path) path = "";

    char *out = malloc(64 + strlen(path));
    if (!out) return NULL;
    sprintf(out, "https://34ebalka.ru.actor%s", path);
    return out;
}

int extract_ebalko(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    char *canon = rewrite_to_canonical(url);
    if (!canon) { fprintf(stderr, "❌ Ebalko: malloc\n"); return -1; }

    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, canon, NULL, NULL);
    http_session_free(sess);
    free(canon);

    if (!resp) { fprintf(stderr, "❌ Ebalko: нет ответа\n"); return -1; }
    if (resp->status != 200) {
        fprintf(stderr, "❌ Ebalko: HTTP %ld\n", resp->status);
        http_resp_free(resp); return -1;
    }

    /* Title */
    const char *tm = strstr(resp->body, "og:title");
    if (tm) {
        const char *c = strstr(tm, "content=\"");
        if (c) { c += 9; const char *e = strchr(c, '"'); if (e) out->title = strndup(c, (size_t)(e-c)); }
    }

    /* Path A: ya:ovs:content_url — Yandex OVS meta, always holds direct video URL */
    {
        const char *p = strstr(resp->body, "ya:ovs:content_url");
        if (p) {
            const char *c = strstr(p, "content=\"");
            if (c) {
                c += 9;
                const char *e = strchr(c, '"');
                if (e && e > c) {
                    out->url = strndup(c, (size_t)(e - c));
                    out->is_hls = 0;
                    http_resp_free(resp);
                    return 0;
                }
            }
        }
    }

    /* Path B: og:video with type video/mp4 → look for get_file URL in og:video */
    {
        const char *p = resp->body;
        while ((p = strstr(p, "og:video"))) {
            /* Look ahead for content= within ~200 chars */
            const char *c = strstr(p, "content=\"");
            if (!c || (c - p) > 200) { p++; continue; }
            c += 9;
            const char *e = strchr(c, '"');
            if (!e) { p++; continue; }
            /* Accept only URLs with /get_file/ */
            size_t seg_len = (size_t)(e - c);
            char *seg = strndup(c, seg_len);
            int has_get_file = seg && strstr(seg, "/get_file/") != NULL;
            free(seg);
            if (has_get_file) {
                out->url = strndup(c, (size_t)(e - c));
                out->is_hls = 0;
                http_resp_free(resp);
                return 0;
            }
            p++;
        }
    }

    http_resp_free(resp);
    fprintf(stderr, "❌ Ebalko: не нашёл источника\n");
    free(out->title); out->title = NULL;
    return -1;
}
