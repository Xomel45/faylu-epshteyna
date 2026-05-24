#include "extractor.h"
#include "../http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <curl/curl.h>

/* Decode JSON unicode escapes \uXXXX in-place (handles / etc.).
 * Also unescapes \" and \\ .  Modifies the string in-place and returns it. */
static char *decode_json_str(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\\' && r[1] == 'u' &&
            isxdigit((unsigned char)r[2]) && isxdigit((unsigned char)r[3]) &&
            isxdigit((unsigned char)r[4]) && isxdigit((unsigned char)r[5])) {
            char hex[5] = { r[2], r[3], r[4], r[5], 0 };
            unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
            if (cp < 0x80) {
                *w++ = (char)cp;
            } else if (cp < 0x800) {
                *w++ = (char)(0xC0 | (cp >> 6));
                *w++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *w++ = (char)(0xE0 | (cp >> 12));
                *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *w++ = (char)(0x80 | (cp & 0x3F));
            }
            r += 6;
        } else if (r[0] == '\\' && (r[1] == '"' || r[1] == '\\' || r[1] == '/')) {
            *w++ = r[1]; r += 2;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return s;
}

/* Extract value of "key":"<value>" from JSON blob.
 * Returns malloc'd decoded string or NULL. */
static char *json_str_field(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    /* scan to closing quote, respecting \-escapes */
    const char *s = p;
    while (*p) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"')  break;
        p++;
    }
    if (*p != '"') return NULL;
    char *val = strndup(s, (size_t)(p - s));
    return decode_json_str(val);
}

/* Get tt_chain_token cookie value from a curl handle's cookie list. */
static char *get_tt_chain_token(CURL *curl) {
    struct curl_slist *cookies = NULL;
    curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies);
    char *val = NULL;
    for (struct curl_slist *c = cookies; c; c = c->next) {
        /* netscape cookie format: domain \t flag \t path \t secure \t exp \t name \t value */
        const char *p = c->data;
        /* skip 6 tab-delimited fields to reach name */
        int tabs = 0;
        while (*p && tabs < 5) { if (*p == '\t') tabs++; p++; }
        if (strncmp(p, "tt_chain_token\t", 15) == 0) {
            val = strdup(p + 15);
            break;
        }
    }
    curl_slist_free_all(cookies);
    return val;
}

int extract_tiktok(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    HttpSession *sess = http_session_new(proxy);
    /* TikTok needs Accept-Language to serve real content */
    const char *extra[] = {
        "Accept-Language: en-US,en;q=0.9",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        NULL
    };
    HttpResp *resp = http_get(sess, url, "https://www.tiktok.com/", extra);

    if (!resp || resp->status != 200) {
        fprintf(stderr, "❌ TikTok: HTTP %ld\n", resp ? resp->status : 0L);
        if (resp) http_resp_free(resp);
        http_session_free(sess);
        return -1;
    }

    /* Find __UNIVERSAL_DATA_FOR_REHYDRATION__ JSON block */
    const char *tag = "__UNIVERSAL_DATA_FOR_REHYDRATION__\"";
    const char *p = strstr(resp->body, tag);
    if (!p) {
        fprintf(stderr, "❌ TikTok: не нашёл UNIVERSAL_DATA\n");
        http_resp_free(resp); http_session_free(sess);
        return -1;
    }
    /* advance past the script tag to the JSON '{' */
    p = strchr(p, '>');
    if (!p) { http_resp_free(resp); http_session_free(sess); return -1; }
    p++;

    /* Title from desc or og:title */
    {
        const char *tm = strstr(resp->body, "og:title");
        if (tm) {
            const char *c = strstr(tm, "content=\"");
            if (c) { c += 9; const char *e = strchr(c, '"'); if (e) out->title = strndup(c, (size_t)(e-c)); }
        }
    }

    /* Extract downloadAddr (no watermark) → fallback to playAddr */
    char *dl_addr   = json_str_field(p, "downloadAddr");
    char *play_addr = json_str_field(p, "playAddr");

    http_resp_free(resp);

    char *video_url = dl_addr ? dl_addr : play_addr;
    if (dl_addr && play_addr) free(play_addr);

    if (!video_url || strncmp(video_url, "http", 4) != 0) {
        fprintf(stderr, "❌ TikTok: URL не найден\n");
        free(video_url); http_session_free(sess);
        free(out->title); out->title = NULL;
        return -1;
    }

    /* Retrieve tt_chain_token cookie that was set when fetching the page */
    /* We need access to the raw CURL handle — expose it via a small shim */
    /* Instead: re-use session for a HEAD request to the CDN to carry the cookie,
     * then extract it from the jar. */
    {
        /* Access the internal curl handle through the session struct.
         * HttpSession is { CURL *curl; } — match the layout from http.c */
        CURL **curl_ptr = (CURL **)sess;  /* first field is curl */
        char *token = get_tt_chain_token(*curl_ptr);
        if (token) {
            char cookie_str[256];
            snprintf(cookie_str, sizeof(cookie_str), "tt_chain_token=%s", token);
            out->cookies = strdup(cookie_str);
            free(token);
        }
    }
    http_session_free(sess);

    out->url     = video_url;
    out->is_hls  = 0;
    out->referer = strdup("https://www.tiktok.com/");
    return 0;
}
