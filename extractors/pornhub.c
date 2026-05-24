#include "extractor.h"
#include "../http.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void video_info_free(VideoInfo *v) {
    if (!v) return;
    free(v->url);
    free(v->title);
    free(v->cookies);
    free(v->referer);
    v->url = v->title = v->cookies = v->referer = NULL;
}

/* Walk forward from ptr, find first '{', then return malloc'd balanced JSON object. */
static char *extract_json_obj(const char *ptr) {
    const char *start = strchr(ptr, '{');
    if (!start) return NULL;

    int depth = 0, in_str = 0;
    const char *p = start;
    while (*p) {
        if (!in_str) {
            if      (*p == '{') depth++;
            else if (*p == '}') { if (--depth == 0) break; }
            else if (*p == '"') in_str = 1;
        } else {
            if      (*p == '\\') { if (*(p + 1)) p++; }
            else if (*p == '"')  in_str = 0;
        }
        p++;
    }
    if (depth != 0 || *p != '}') return NULL;

    size_t len = (size_t)(p - start + 1);
    char *out = malloc(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* Extract <meta property="og:title" content="..."> or twitter:title */
static char *extract_title(const char *html) {
    const char *tags[] = { "twitter:title", "og:title", NULL };
    for (int t = 0; tags[t]; t++) {
        const char *pos = strstr(html, tags[t]);
        if (!pos) continue;
        const char *c = strstr(pos, "content=\"");
        if (!c) continue;
        c += 9;
        const char *end = strchr(c, '"');
        if (!end) continue;
        size_t len = (size_t)(end - c);
        char *title = malloc(len + 1);
        memcpy(title, c, len);
        title[len] = '\0';
        return title;
    }
    return NULL;
}

int extract_pornhub(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    /* Detect host */
    char host[64] = "pornhub.com";
    if (strstr(url, "pornhubpremium.com"))  strcpy(host, "pornhubpremium.com");
    else if (strstr(url, "pornhub.net"))    strcpy(host, "pornhub.net");
    else if (strstr(url, "pornhub.org"))    strcpy(host, "pornhub.org");

    HttpSession *sess = http_session_new(proxy);
    http_session_cookie(sess, host, "age_verified",           "1");
    http_session_cookie(sess, host, "accessAgeDisclaimerPH",  "1");
    http_session_cookie(sess, host, "accessAgeDisclaimerUK",  "1");
    http_session_cookie(sess, host, "accessPH",               "1");
    http_session_cookie(sess, host, "platform",               "pc");

    HttpResp *resp = http_get(sess, url, NULL, NULL);
    http_session_free(sess);

    if (!resp) { fprintf(stderr, "❌ PornHub: нет ответа\n"); return -1; }
    if (resp->status != 200) {
        fprintf(stderr, "❌ PornHub: HTTP %ld\n", resp->status);
        http_resp_free(resp); return -1;
    }

    /* Find "var flashvars_<digits> =" in the page */
    const char *fv = strstr(resp->body, "var flashvars_");
    if (!fv) {
        fprintf(stderr, "❌ PornHub: flashvars не найдены\n");
        http_resp_free(resp); return -1;
    }
    const char *eq = strchr(fv, '=');
    if (!eq) { http_resp_free(resp); return -1; }

    char *json_str = extract_json_obj(eq + 1);
    if (!json_str) {
        fprintf(stderr, "❌ PornHub: не смог распарсить flashvars JSON\n");
        http_resp_free(resp); return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        fprintf(stderr, "❌ PornHub: cJSON не смог разобрать flashvars\n");
        http_resp_free(resp); return -1;
    }

    /* Extract title */
    out->title = extract_title(resp->body);

    /* mediaDefinitions: [{videoUrl, quality}, ...] — pick best */
    cJSON *media_defs = cJSON_GetObjectItem(root, "mediaDefinitions");
    if (!cJSON_IsArray(media_defs)) {
        fprintf(stderr, "❌ PornHub: mediaDefinitions не найдены\n");
        cJSON_Delete(root); http_resp_free(resp); return -1;
    }

    int   best_quality = -1;
    char *best_url     = NULL;

    int count = cJSON_GetArraySize(media_defs);
    for (int i = 0; i < count; i++) {
        cJSON *def = cJSON_GetArrayItem(media_defs, i);
        if (!cJSON_IsObject(def)) continue;

        cJSON *vurl_j = cJSON_GetObjectItem(def, "videoUrl");
        cJSON *qual_j = cJSON_GetObjectItem(def, "quality");

        if (!cJSON_IsString(vurl_j) || !vurl_j->valuestring[0]) continue;

        int quality = cJSON_IsNumber(qual_j) ? (int)qual_j->valuedouble : 0;

        /* Skip entries where videoUrl is an array (adaptive stream references) */
        if (strstr(vurl_j->valuestring, ".m3u8")) {
            /* Prefer direct MP4 over HLS; only take HLS if nothing else found */
            if (best_url == NULL) {
                free(best_url);
                best_url     = strdup(vurl_j->valuestring);
                best_quality = quality;
                out->is_hls  = 1;
            }
            continue;
        }

        if (quality_better(quality, best_quality)) {
            free(best_url);
            best_url     = strdup(vurl_j->valuestring);
            best_quality = quality;
            out->is_hls  = 0;
        }
    }

    cJSON_Delete(root);
    http_resp_free(resp);

    if (!best_url) {
        fprintf(stderr, "❌ PornHub: не нашёл videoUrl\n");
        return -1;
    }

    out->url     = best_url;
    out->quality = best_quality > 0 ? best_quality : 0;
    return 0;
}
