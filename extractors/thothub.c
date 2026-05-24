#include "extractor.h"
#include "../http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int extract_thothub(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, url, NULL, NULL);
    http_session_free(sess);

    if (!resp) { fprintf(stderr, "❌ Thothub: нет ответа\n"); return -1; }
    if (resp->status != 200) {
        fprintf(stderr, "❌ Thothub: HTTP %ld\n", resp->status);
        http_resp_free(resp); return -1;
    }

    /* video_url: 'function/0/https://...mp4/', */
    const char *marker = strstr(resp->body, "video_url:");
    if (!marker) {
        fprintf(stderr, "❌ Thothub: video_url не найден\n");
        http_resp_free(resp); return -1;
    }
    const char *q = strchr(marker, '\'');
    if (!q) { http_resp_free(resp); return -1; }
    q++;
    const char *q_end = strchr(q, '\'');
    if (!q_end) { http_resp_free(resp); return -1; }

    size_t raw_len = (size_t)(q_end - q);
    char raw[2048];
    if (raw_len >= sizeof(raw)) { http_resp_free(resp); return -1; }
    memcpy(raw, q, raw_len);
    raw[raw_len] = '\0';

    /* strip "function/0/" prefix */
    const char *vid_url = raw;
    if (strncmp(vid_url, "function/0/", 11) == 0)
        vid_url += 11;

    /* strip trailing slash */
    size_t vlen = strlen(vid_url);
    char *final_url = strdup(vid_url);
    if (vlen > 0 && final_url[vlen - 1] == '/')
        final_url[vlen - 1] = '\0';

    out->url    = final_url;
    out->is_hls = 0;

    /* quality from player_height */
    const char *ph = strstr(resp->body, "player_height:");
    if (ph) {
        const char *pq = strchr(ph, '\'');
        if (pq) out->quality = atoi(pq + 1);
    }

    /* title from <h1> */
    const char *h1 = strstr(resp->body, "<h1>");
    if (h1) {
        h1 += 4;
        const char *h1_end = strstr(h1, "</h1>");
        if (h1_end) {
            size_t tlen = (size_t)(h1_end - h1);
            out->title = malloc(tlen + 1);
            memcpy(out->title, h1, tlen);
            out->title[tlen] = '\0';
        }
    }

    http_resp_free(resp);
    return 0;
}
