#include "extractor.h"
#include "../http.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Encode n as base-36 string (0-9a-z). out must be at least 20 bytes. */
static void base36(unsigned long n, char *out) {
    static const char T[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (n == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[32]; int len = 0;
    while (n) { tmp[len++] = T[n % 36]; n /= 36; }
    for (int i = 0; i < len; i++) out[i] = tmp[len - 1 - i];
    out[len] = '\0';
}

/* Ported from yt-dlp eporner.py calc_hash():
 * Split 32-char hex hash into 4×8, base36-encode each uint32, concatenate. */
static void calc_hash(const char *h32, char *out128) {
    out128[0] = '\0';
    for (int i = 0; i < 4; i++) {
        char chunk[9] = {0};
        memcpy(chunk, h32 + i * 8, 8);
        unsigned long v = strtoul(chunk, NULL, 16);
        char b[20];
        base36(v, b);
        strcat(out128, b);
    }
}

/* Extract video ID from URL path segment after /hd-porn/, /embed/, or /video- */
static char *extract_id(const char *url) {
    const char *pfx[] = {"/hd-porn/", "/embed/", "/video-", NULL};
    for (int i = 0; pfx[i]; i++) {
        const char *p = strstr(url, pfx[i]);
        if (!p) continue;
        p += strlen(pfx[i]);
        const char *e = p;
        while (*e && *e != '/' && *e != '?') e++;
        if (e > p) return strndup(p, (size_t)(e - p));
    }
    return NULL;
}

int extract_eporner(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    HttpSession *sess = http_session_new(proxy);
    HttpResp *page = http_get(sess, url, NULL, NULL);
    http_session_free(sess);

    if (!page) { fprintf(stderr, "❌ EPorner: нет ответа\n"); return -1; }
    if (page->status != 200) {
        fprintf(stderr, "❌ EPorner: HTTP %ld\n", page->status);
        http_resp_free(page); return -1;
    }

    /* Video ID: prefer final URL after possible redirect */
    char *video_id = NULL;
    if (page->effective_url)
        video_id = extract_id(page->effective_url);
    if (!video_id)
        video_id = extract_id(url);
    if (!video_id) {
        fprintf(stderr, "❌ EPorner: не смог извлечь video ID\n");
        http_resp_free(page); return -1;
    }

    /* Find hash: "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" (32 hex chars) */
    char vid_hash[33] = {0};
    const char *p = page->body;
    while ((p = strstr(p, "hash"))) {
        const char *c = p + 4;
        while (*c == ' ' || *c == '\t') c++;
        if (*c != ':' && *c != '=') { p++; continue; }
        c++;
        while (*c == ' ' || *c == '\t') c++;
        if (*c != '"' && *c != '\'') { p++; continue; }
        const char *hs = c + 1;
        const char *he = hs;
        while (*he && isxdigit((unsigned char)*he)) he++;
        if ((he - hs) == 32) { memcpy(vid_hash, hs, 32); break; }
        p++;
    }
    if (!vid_hash[0]) {
        fprintf(stderr, "❌ EPorner: hash не найден\n");
        http_resp_free(page); free(video_id); return -1;
    }

    /* Title from og:title */
    const char *tm = strstr(page->body, "og:title");
    if (tm) {
        const char *c = strstr(tm, "content=\"");
        if (c) { c += 9; const char *e = strchr(c, '"'); if (e) out->title = strndup(c, (size_t)(e-c)); }
    }
    http_resp_free(page);

    /* Compute API hash and call */
    char api_hash[128] = {0};
    calc_hash(vid_hash, api_hash);

    char api_url[512];
    snprintf(api_url, sizeof(api_url),
             "https://www.eporner.com/xhr/video/%s"
             "?hash=%s&device=generic&domain=www.eporner.com&fallback=false",
             video_id, api_hash);
    free(video_id);

    printf("🔑 EPorner: API...\n");
    sess = http_session_new(proxy);
    HttpResp *api = http_get(sess, api_url, NULL, NULL);
    http_session_free(sess);

    if (!api) { fprintf(stderr, "❌ EPorner: API не ответил\n"); return -1; }

    cJSON *root = cJSON_Parse(api->body);
    http_resp_free(api);
    if (!root) { fprintf(stderr, "❌ EPorner: API JSON не разобрался\n"); return -1; }

    cJSON *avail = cJSON_GetObjectItem(root, "available");
    if (cJSON_IsBool(avail) && !cJSON_IsTrue(avail)) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        fprintf(stderr, "❌ EPorner: %s\n",
                cJSON_IsString(msg) ? msg->valuestring : "недоступно");
        cJSON_Delete(root); return -1;
    }

    cJSON *sources = cJSON_GetObjectItem(root, "sources");
    if (!cJSON_IsObject(sources)) {
        fprintf(stderr, "❌ EPorner: sources не найден\n");
        cJSON_Delete(root); return -1;
    }

    /* sources: {"mp4": {"720p30": {"src": "url"}, ...}, "hls": {...}} */
    int   best_h   = -1;
    char *best_url = NULL;
    char *hls_url  = NULL;

    for (cJSON *kind = sources->child; kind; kind = kind->next) {
        if (!cJSON_IsObject(kind)) continue;
        int is_hls = (strcmp(kind->string, "hls") == 0);

        for (cJSON *fmt = kind->child; fmt; fmt = fmt->next) {
            if (!cJSON_IsObject(fmt)) continue;
            cJSON *src_j = cJSON_GetObjectItem(fmt, "src");
            if (!cJSON_IsString(src_j) || !src_j->valuestring[0]) continue;
            if (strncmp(src_j->valuestring, "http", 4) != 0) continue;

            if (is_hls) {
                if (!hls_url) hls_url = strdup(src_j->valuestring);
            } else {
                int h = atoi(fmt->string); /* "720p30" → 720 */
                if (quality_better(h, best_h)) {
                    best_h = h;
                    free(best_url);
                    best_url = strdup(src_j->valuestring);
                }
            }
        }
    }
    cJSON_Delete(root);

    if (best_url) {
        out->url = best_url; out->quality = best_h > 0 ? best_h : 0; out->is_hls = 0;
        free(hls_url);
        return 0;
    }
    if (hls_url) {
        out->url = hls_url; out->is_hls = 1;
        return 0;
    }

    fprintf(stderr, "❌ EPorner: не нашёл источника\n");
    return -1;
}
