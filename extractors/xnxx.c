#include "extractor.h"
#include "../http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Extract first argument from setXxx('value') or setXxx("value").
 * Handles protocol-relative URLs starting with // by prepending https:. */
static char *js_arg(const char *html, const char *func) {
    const char *pos = strstr(html, func);
    if (!pos) return NULL;
    pos = strchr(pos + strlen(func), '(');
    if (!pos) return NULL;
    pos++;
    char q = *pos;
    if (q != '\'' && q != '"') return NULL;
    pos++;
    const char *end = strchr(pos, q);
    if (!end) return NULL;
    size_t len = (size_t)(end - pos);
    char *s = malloc(len + 1);
    memcpy(s, pos, len);
    s[len] = '\0';
    return s;
}

static char *make_url(const char *url) {
    if (strncmp(url, "//", 2) == 0) {
        char *out = malloc(6 + strlen(url) + 1);
        sprintf(out, "https:%s", url);
        return out;
    }
    return strdup(url);
}

int extract_xnxx(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, url, NULL, NULL);
    http_session_free(sess);

    if (!resp) { fprintf(stderr, "❌ XNXX: нет ответа\n"); return -1; }
    if (resp->status != 200) {
        fprintf(stderr, "❌ XNXX: HTTP %ld\n", resp->status);
        http_resp_free(resp); return -1;
    }

    out->title = js_arg(resp->body, "setVideoTitle");
    char *high  = js_arg(resp->body, "setVideoUrlHigh");
    char *low   = js_arg(resp->body, "setVideoUrlLow");
    char *hls   = js_arg(resp->body, "setVideoHLS");
    http_resp_free(resp);

    if (high) {
        out->url = make_url(high); out->quality = 720; out->is_hls = 0;
        free(high); free(low); free(hls);
        return 0;
    }
    if (low) {
        out->url = make_url(low); out->quality = 480; out->is_hls = 0;
        free(low); free(hls);
        return 0;
    }
    if (hls) {
        out->url = make_url(hls); out->is_hls = 1;
        free(hls);
        return 0;
    }

    fprintf(stderr, "❌ XNXX: не нашёл URL видео\n");
    return -1;
}
