#include "extractor.h"
#include "../http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int extract_pornonahobka(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, url, NULL, NULL);
    http_session_free(sess);

    if (!resp) { fprintf(stderr, "❌ Pornonahobka: нет ответа\n"); return -1; }
    if (resp->status != 200) {
        fprintf(stderr, "❌ Pornonahobka: HTTP %ld\n", resp->status);
        http_resp_free(resp); return -1;
    }

    const char *tm = strstr(resp->body, "og:title");
    if (tm) {
        const char *c = strstr(tm, "content=\"");
        if (c) { c += 9; const char *e = strchr(c, '"'); if (e) out->title = strndup(c, (size_t)(e - c)); }
    }

    const char *p = strstr(resp->body, "ya:ovs:content_url");
    if (p) {
        const char *c = strstr(p, "content=\"");
        if (c) {
            c += 9;
            const char *e = strchr(c, '"');
            if (e && e > c) {
                out->url    = strndup(c, (size_t)(e - c));
                out->is_hls = 0;
                http_resp_free(resp);
                return 0;
            }
        }
    }

    http_resp_free(resp);
    fprintf(stderr, "❌ Pornonahobka: не нашёл ya:ovs:content_url\n");
    free(out->title); out->title = NULL;
    return -1;
}
