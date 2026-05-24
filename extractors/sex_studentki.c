#include "extractor.h"
#include "../http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int extract_sex_studentki(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, url, NULL, NULL);
    http_session_free(sess);

    if (!resp) { fprintf(stderr, "❌ SexStudentki: нет ответа\n"); return -1; }
    if (resp->status != 200) {
        fprintf(stderr, "❌ SexStudentki: HTTP %ld\n", resp->status);
        http_resp_free(resp); return -1;
    }

    /* Title from og:title */
    const char *tm = strstr(resp->body, "og:title");
    if (tm) {
        const char *c = strstr(tm, "content=\"");
        if (c) { c += 9; const char *e = strchr(c, '"'); if (e) out->title = strndup(c, (size_t)(e-c)); }
    }

    /* Find <source src="https://x.sex-studentki.team/link_cs/..."> */
    const char *p = resp->body;
    char *best_url = NULL;

    while ((p = strstr(p, "<source"))) {
        const char *src = strstr(p, "src=\"");
        if (!src) { p++; continue; }

        /* Make sure src= is within this <source> tag (before next '>') */
        const char *tag_end = strchr(p, '>');
        if (tag_end && src > tag_end) { p++; continue; }

        src += 5; /* skip src=" */
        const char *end = strchr(src, '"');
        if (!end) { p++; continue; }

        /* Accept any https URL that looks like a video source */
        if (strncmp(src, "https://", 8) == 0 || strncmp(src, "http://", 7) == 0) {
            free(best_url);
            best_url = strndup(src, (size_t)(end - src));
        }
        p++;
    }

    http_resp_free(resp);

    if (!best_url) {
        fprintf(stderr, "❌ SexStudentki: не нашёл <source src=...>\n");
        free(out->title);
        out->title = NULL;
        return -1;
    }

    out->url = best_url;
    out->is_hls = (strstr(best_url, ".m3u8") != NULL);
    return 0;
}
