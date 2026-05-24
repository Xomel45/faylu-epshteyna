#include "extractor.h"
#include "../http.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Extract balanced {...} starting from the first '{' in ptr. */
static char *extract_obj(const char *ptr) {
    const char *s = strchr(ptr, '{');
    if (!s) return NULL;
    int depth = 0, in_str = 0;
    const char *p = s;
    while (*p) {
        if (!in_str) {
            if      (*p == '{') depth++;
            else if (*p == '}') { if (--depth == 0) break; }
            else if (*p == '"') in_str = 1;
        } else {
            if (*p == '\\') { if (*(p+1)) p++; }
            else if (*p == '"') in_str = 0;
        }
        p++;
    }
    if (depth != 0) return NULL;
    size_t len = (size_t)(p - s + 1);
    char *out = malloc(len + 1);
    memcpy(out, s, len); out[len] = '\0';
    return out;
}

/* Extract balanced [...] starting from the first '[' in ptr. */
static char *extract_arr(const char *ptr) {
    const char *s = strchr(ptr, '[');
    if (!s) return NULL;
    int depth = 0, in_str = 0;
    const char *p = s;
    while (*p) {
        if (!in_str) {
            if      (*p == '[') depth++;
            else if (*p == ']') { if (--depth == 0) break; }
            else if (*p == '"') in_str = 1;
        } else {
            if (*p == '\\') { if (*(p+1)) p++; }
            else if (*p == '"') in_str = 0;
        }
        p++;
    }
    if (depth != 0) return NULL;
    size_t len = (size_t)(p - s + 1);
    char *out = malloc(len + 1);
    memcpy(out, s, len); out[len] = '\0';
    return out;
}

int extract_redtube(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    /* Extract numeric video ID */
    const char *id_s = NULL;
    const char *embed_id = strstr(url, "id=");
    if (embed_id && strstr(url, "embed.redtube"))
        id_s = embed_id + 3;
    else {
        const char *sl = strrchr(url, '/');
        if (sl) id_s = sl + 1;
    }
    if (!id_s || !isdigit((unsigned char)*id_s)) {
        fprintf(stderr, "❌ RedTube: не смог извлечь video ID\n"); return -1;
    }
    const char *id_e = id_s;
    while (isdigit((unsigned char)*id_e)) id_e++;

    char canonical[128];
    snprintf(canonical, sizeof(canonical), "https://www.redtube.com/%.*s",
             (int)(id_e - id_s), id_s);

    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, canonical, NULL, NULL);
    http_session_free(sess);

    if (!resp) { fprintf(stderr, "❌ RedTube: нет ответа\n"); return -1; }
    if (resp->status != 200) {
        fprintf(stderr, "❌ RedTube: HTTP %ld\n", resp->status);
        http_resp_free(resp); return -1;
    }

    /* Title */
    const char *tm = strstr(resp->body, "og:title");
    if (tm) {
        const char *c = strstr(tm, "content=\"");
        if (c) { c += 9; const char *e = strchr(c, '"'); if (e) out->title = strndup(c, (size_t)(e-c)); }
    }

    /* ── Path A: sources: {"720": "url", ...} ── */
    {
        int best_h = -1; char *best_url = NULL;
        const char *p = resp->body;

        while ((p = strstr(p, "sources"))) {
            const char *c = p + 7;
            /* skip optional quote around key */
            while (*c == '"' || *c == '\'' || *c == ' ') c++;
            if (*c != ':') { p++; continue; }
            c++;
            while (*c == ' ' || *c == '\t') c++;
            if (*c != '{') { p++; continue; }

            char *json = extract_obj(c);
            if (!json) { p++; continue; }

            cJSON *root = cJSON_Parse(json);
            free(json);
            if (!root) { p++; continue; }

            for (cJSON *item = root->child; item; item = item->next) {
                if (!cJSON_IsString(item) || !item->valuestring[0]) continue;
                int h = atoi(item->string); /* "720" → 720 */
                if (quality_better(h, best_h)) {
                    best_h = h;
                    free(best_url);
                    best_url = strdup(item->valuestring);
                }
            }
            cJSON_Delete(root);
            break;
        }

        if (best_url) {
            out->url = best_url; out->quality = best_h > 0 ? best_h : 0; out->is_hls = 0;
            http_resp_free(resp);
            return 0;
        }
    }

    /* ── Path B: mediaDefinition: [{format, quality, videoUrl}, ...] ── */
    {
        int best_h = -1; char *best_url = NULL;
        const char *p = strstr(resp->body, "mediaDefinition");
        if (p) {
            char *arr_json = extract_arr(p);
            if (arr_json) {
                cJSON *arr = cJSON_Parse(arr_json);
                free(arr_json);
                if (arr && cJSON_IsArray(arr)) {
                    int n = cJSON_GetArraySize(arr);
                    for (int i = 0; i < n; i++) {
                        cJSON *item = cJSON_GetArrayItem(arr, i);
                        if (!cJSON_IsObject(item)) continue;
                        cJSON *fmt  = cJSON_GetObjectItem(item, "format");
                        cJSON *qual = cJSON_GetObjectItem(item, "quality");
                        cJSON *vurl = cJSON_GetObjectItem(item, "videoUrl");
                        if (!cJSON_IsString(fmt)  || strcmp(fmt->valuestring,  "mp4") != 0) continue;
                        if (!cJSON_IsString(qual) || !qual->valuestring[0]) continue;
                        if (!cJSON_IsString(vurl) || !vurl->valuestring[0]) continue;
                        int h = atoi(qual->valuestring);
                        if (quality_better(h, best_h)) {
                            best_h = h;
                            free(best_url);
                            best_url = strdup(vurl->valuestring);
                        }
                    }
                    cJSON_Delete(arr);
                }
            }
        }

        if (best_url) {
            out->url = best_url; out->quality = best_h > 0 ? best_h : 0; out->is_hls = 0;
            http_resp_free(resp);
            return 0;
        }
    }

    http_resp_free(resp);
    fprintf(stderr, "❌ RedTube: не нашёл источника\n");
    return -1;
}
