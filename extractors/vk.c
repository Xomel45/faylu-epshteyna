#include "extractor.h"
#include "../http.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Parse video ID from VK URL.
   Handles:
     vk.com/video-12345_67890
     vk.com/video12345_67890
     vk.com/video_ext.php?oid=-12345&id=67890&...
   Writes "-12345_67890" style string into out. */
static int vk_parse_video_id(const char *url, char *vid_out, size_t vid_sz,
                               char *list_out, size_t list_sz) {
    list_out[0] = '\0';

    if (strstr(url, "video_ext.php")) {
        const char *oid_p = strstr(url, "oid=");
        if (!oid_p) return -1;
        long oid = strtol(oid_p + 4, NULL, 10);

        /* find standalone "id=" */
        const char *id_p = NULL;
        const char *p = url;
        while ((p = strstr(p, "id="))) {
            if (p == url || *(p - 1) == '&' || *(p - 1) == '?') {
                id_p = p; break;
            }
            p++;
        }
        if (!id_p) return -1;
        long id = strtol(id_p + 3, NULL, 10);
        snprintf(vid_out, vid_sz, "%ld_%ld", oid, id);
        return 0;
    }

    /* Regular URL: /video[-]OWNER_ID */
    const char *vp = strstr(url, "/video");
    const char *cp = strstr(url, "/clip");

    if (cp && (!vp || cp > vp)) vp = cp + 5;
    else if (vp)                 vp += 6;
    else return -1;

    char *endp;
    long owner = strtol(vp, &endp, 10);
    if (*endp != '_') return -1;
    long vid = strtol(endp + 1, &endp, 10);

    snprintf(vid_out, vid_sz, "%ld_%ld", owner, vid);

    /* Optional list= param */
    const char *list_p = strstr(url, "list=");
    if (list_p) {
        list_p += 5;
        const char *list_end = list_p;
        while (*list_end && *list_end != '&' && *list_end != '#') list_end++;
        size_t llen = (size_t)(list_end - list_p);
        if (llen < list_sz) {
            memcpy(list_out, list_p, llen);
            list_out[llen] = '\0';
        }
    }
    return 0;
}

int extract_vk(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    char video_id[64], list_id[128];
    if (vk_parse_video_id(url, video_id, sizeof(video_id),
                           list_id, sizeof(list_id)) != 0) {
        fprintf(stderr, "❌ VK: не смог распарсить video ID из URL\n");
        return -1;
    }
    printf("🎬 VK video ID: %s\n", video_id);

    /* Build POST data */
    char post_data[256];
    if (list_id[0])
        snprintf(post_data, sizeof(post_data),
                 "act=show&video=%s&list=%s&al=1", video_id, list_id);
    else
        snprintf(post_data, sizeof(post_data),
                 "act=show&video=%s&al=1", video_id);

    const char *extra[] = {
        "X-Requested-With: XMLHttpRequest",
        NULL
    };

    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_post(sess, "https://vk.com/al_video.php",
                               post_data,
                               "https://vk.com/al_video.php",
                               "application/x-www-form-urlencoded",
                               extra);
    http_session_free(sess);

    if (!resp) { fprintf(stderr, "❌ VK: нет ответа\n"); return -1; }

    /* VK sometimes prepends "<!--" to defeat content sniffing */
    const char *json_start = resp->body;
    while (*json_start && *json_start != '{') json_start++;

    cJSON *root = cJSON_Parse(json_start);
    http_resp_free(resp);

    if (!root) { fprintf(stderr, "❌ VK: JSON не распарсился\n"); return -1; }

    /* Structure: {"payload": ["code", [null, info_page_html, ..., opts_obj]]} */
    cJSON *outer_payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsArray(outer_payload) || cJSON_GetArraySize(outer_payload) < 2) {
        fprintf(stderr, "❌ VK: неожиданная структура payload\n");
        cJSON_Delete(root); return -1;
    }

    cJSON *code_item = cJSON_GetArrayItem(outer_payload, 0);
    if (cJSON_IsString(code_item)) {
        if (strcmp(code_item->valuestring, "3") == 0) {
            fprintf(stderr, "❌ VK: требуется авторизация\n");
            cJSON_Delete(root); return -1;
        }
        if (strcmp(code_item->valuestring, "8") == 0) {
            fprintf(stderr, "❌ VK: ошибка сервера\n");
            cJSON_Delete(root); return -1;
        }
    }

    cJSON *inner = cJSON_GetArrayItem(outer_payload, 1);
    if (!cJSON_IsArray(inner)) {
        fprintf(stderr, "❌ VK: inner payload не массив\n");
        cJSON_Delete(root); return -1;
    }

    int inner_sz = cJSON_GetArraySize(inner);
    if (inner_sz < 2) {
        fprintf(stderr, "❌ VK: inner payload слишком короткий\n");
        cJSON_Delete(root); return -1;
    }

    cJSON *opts = cJSON_GetArrayItem(inner, inner_sz - 1);
    if (!cJSON_IsObject(opts)) {
        fprintf(stderr, "❌ VK: opts не объект\n");
        cJSON_Delete(root); return -1;
    }

    cJSON *player = cJSON_GetObjectItem(opts, "player");
    if (!cJSON_IsObject(player)) {
        fprintf(stderr, "❌ VK: player не найден в opts\n");
        cJSON_Delete(root); return -1;
    }

    cJSON *params_arr = cJSON_GetObjectItem(player, "params");
    if (!cJSON_IsArray(params_arr) || cJSON_GetArraySize(params_arr) == 0) {
        fprintf(stderr, "❌ VK: player.params пуст\n");
        cJSON_Delete(root); return -1;
    }

    cJSON *params = cJSON_GetArrayItem(params_arr, 0);
    if (!cJSON_IsObject(params)) {
        fprintf(stderr, "❌ VK: player.params[0] не объект\n");
        cJSON_Delete(root); return -1;
    }

    /* Scan all available direct-MP4 qualities and pick best respecting cap. */
    static const char *pref[] = {
        "url2160","url1440","url1080","url720","url480","url360","url240",NULL
    };
    int   best_h   = -1;
    char *best_url = NULL;

    for (int i = 0; pref[i]; i++) {
        cJSON *item = cJSON_GetObjectItem(params, pref[i]);
        if (!cJSON_IsString(item) || !item->valuestring[0]) continue;
        int h = atoi(pref[i] + 3); /* "url720" → 720 */
        if (quality_better(h, best_h)) {
            free(best_url);
            best_url = strdup(item->valuestring);
            best_h   = h;
        }
    }

    if (best_url) {
        out->url     = best_url;
        out->is_hls  = 0;
        out->quality = best_h;
        cJSON_Delete(root);
        return 0;
    }

    /* HLS fallback */
    cJSON *hls = cJSON_GetObjectItem(params, "hls");
    if (cJSON_IsString(hls) && hls->valuestring[0]) {
        out->url    = strdup(hls->valuestring);
        out->is_hls = 1;
        cJSON_Delete(root);
        return 0;
    }

    fprintf(stderr, "❌ VK: не нашёл ни одного videoUrl\n");
    cJSON_Delete(root);
    return -1;
}
