#include "extractor.h"
#include "../http.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>

/* ── XHamster URL cipher ────────────────────────────────────────────────────
 * Ported from yt-dlp's _ByteGenerator class (extractors/xhamster.py).
 * Format: byte[0]=algo_id, bytes[1..4]=seed (LE int32), bytes[5..]=ciphertext.
 * Each plaintext byte = ciphertext_byte XOR next_byte_from_generator.
 * ────────────────────────────────────────────────────────────────────────── */

static int32_t i32(uint32_t x) { return (int32_t)x; }

static uint8_t algo_next(int algo, int32_t *ps) {
    uint32_t s = (uint32_t)*ps;
    uint32_t r;
    switch (algo) {
    case 1: /* LCG: a=1664525 c=1013904223 */
        s = s * 1664525u + 1013904223u;
        *ps = i32(r = s); break;
    case 2: /* xorshift32 */
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        *ps = i32(r = s); break;
    case 3: /* Weyl + MurmurHash3 fmix32 — state updated at Weyl step only */
        s += 0x9e3779b9u; *ps = i32(s);
        s ^= s >> 16; s *= 0x85ebca77u;
        s ^= s >> 13; s *= 0xc2b2ae3du;
        r = s ^ (s >> 16); break;
    case 4: /* Weyl + ROL7 scramble — state updated at Weyl step only */
        s += 0x6d2b79f5u; *ps = i32(s);
        s = (s << 7) | (s >> 25); /* ROL 7 */
        s += 0x9e3779b9u; s ^= s >> 11;
        r = s * 0x27d4eb2du; break;
    case 5: /* xorshift + Weyl */
        s ^= s << 7; s ^= s >> 9; s ^= s << 8;
        s += 0xa5a5a5a5u;
        *ps = i32(r = s); break;
    case 6: { /* LCG + variable-right-shift */
        s = s * 0x2c9277b5u + 0xac564b05u; *ps = i32(s);
        uint32_t s2 = s ^ (s >> 18);
        r = s2 >> ((s >> 27) & 31); break; }
    case 7: { /* Weyl + multiply-xor-shift — state updated at Weyl step only */
        s += 0x9e3779b9u; *ps = i32(s);
        uint32_t e = s ^ (s << 5);
        e *= 0x7feb352du; e ^= e >> 15;
        r = e * 0x846ca68bu; break; }
    default: r = 0;
    }
    return (uint8_t)(r & 0xFF);
}

static int is_all_hex(const char *s, size_t len) {
    if (len < 12 || len % 2 != 0) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static uint8_t hex_byte(const char h, const char l) {
    int hi = h >= 'a' ? h-'a'+10 : h >= 'A' ? h-'A'+10 : h-'0';
    int lo = l >= 'a' ? l-'a'+10 : l >= 'A' ? l-'A'+10 : l-'0';
    return (uint8_t)((hi << 4) | lo);
}

/* Decrypt a raw hex string. Returns malloc'd plaintext or NULL. */
static char *decipher_hex(const char *hex, size_t hex_len) {
    if (!is_all_hex(hex, hex_len)) return NULL;

    size_t byte_len = hex_len / 2;
    uint8_t *bytes = malloc(byte_len);
    for (size_t i = 0; i < byte_len; i++)
        bytes[i] = hex_byte(hex[2*i], hex[2*i+1]);

    int algo = bytes[0];
    if (algo < 1 || algo > 7) { free(bytes); return NULL; }

    /* seed: 4 bytes little-endian signed */
    int32_t seed = (int32_t)( (uint32_t)bytes[1]        |
                              ((uint32_t)bytes[2] <<  8) |
                              ((uint32_t)bytes[3] << 16) |
                              ((uint32_t)bytes[4] << 24) );

    size_t out_len = byte_len - 5;
    char *out = malloc(out_len + 1);
    for (size_t i = 0; i < out_len; i++)
        out[i] = (char)(bytes[5 + i] ^ algo_next(algo, &seed));
    out[out_len] = '\0';
    free(bytes);
    return out;
}

/* Decipher a format URL (may be raw hex, or a URL with a hex path segment).
 * Returns malloc'd real URL, or NULL if not a recognised cipher format. */
static char *decipher_format_url(const char *fmt_url) {
    size_t len = strlen(fmt_url);

    /* Case 1: entire string is hex (≥12 chars) */
    if (is_all_hex(fmt_url, len))
        return decipher_hex(fmt_url, len);

    /* Case 2: URL with hex path segment like https://host/HEXHEX.../rest */
    if (strncmp(fmt_url, "http://",  7) != 0 &&
        strncmp(fmt_url, "https://", 8) != 0) return NULL;

    /* skip scheme://host to find path */
    const char *p = strchr(fmt_url, '/');  /* first / of scheme */
    if (!p) return NULL;
    p = strchr(p + 2, '/');                /* slash after host */
    if (!p) return NULL;
    const char *hex_start = p + 1;

    const char *hex_end = hex_start;
    while (*hex_end && isxdigit((unsigned char)*hex_end)) hex_end++;

    size_t hex_len = (size_t)(hex_end - hex_start);
    if (hex_len < 12 || hex_len % 2 != 0 ||
        (*hex_end != '/' && *hex_end != ',')) return NULL;

    char *dec = decipher_hex(hex_start, hex_len);
    if (!dec) return NULL;

    size_t prefix_len = (size_t)(hex_start - fmt_url);
    size_t dec_len    = strlen(dec);
    size_t rem_len    = strlen(hex_end);
    char *out = malloc(prefix_len + dec_len + rem_len + 1);
    memcpy(out, fmt_url, prefix_len);
    memcpy(out + prefix_len, dec, dec_len);
    memcpy(out + prefix_len + dec_len, hex_end, rem_len + 1);
    free(dec);
    return out;
}

/* ── JSON helpers ────────────────────────────────────────────────────────── */

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
            if (*p == '\\') { if (*(p+1)) p++; }
            else if (*p == '"') in_str = 0;
        }
        p++;
    }
    if (depth != 0 || *p != '}') return NULL;
    size_t len = (size_t)(p - start + 1);
    char *out = malloc(len + 1);
    memcpy(out, start, len); out[len] = '\0';
    return out;
}

/* ── Quality helpers ─────────────────────────────────────────────────────── */

/* Lower rank = better. "1080p"→0, "720p"→1, ..., unknown→99 */
static const char *QPREF[] = {"1080p","720p","480p","360p","240p",NULL};

static int qrank(const char *q) {
    if (!q) return 99;
    for (int i = 0; QPREF[i]; i++)
        if (strcmp(q, QPREF[i]) == 0) return i;
    return 99;
}

static int qval(int rank) {
    return (rank >= 0 && rank < 5) ? atoi(QPREF[rank]) : 0;
}

/* ── Main extractor ──────────────────────────────────────────────────────── */

int extract_xhamster(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, url, NULL, NULL);
    http_session_free(sess);

    if (!resp) { fprintf(stderr, "❌ XHamster: нет ответа\n"); return -1; }
    if (resp->status != 200) {
        fprintf(stderr, "❌ XHamster: HTTP %ld\n", resp->status);
        http_resp_free(resp); return -1;
    }

    /* window.initials = {...}; */
    const char *marker = strstr(resp->body, "window.initials");
    if (!marker) {
        fprintf(stderr, "❌ XHamster: window.initials не найден\n");
        http_resp_free(resp); return -1;
    }
    marker = strchr(marker, '=');
    if (!marker) { http_resp_free(resp); return -1; }
    marker++;

    char *json_str = extract_json_obj(marker);
    http_resp_free(resp);

    if (!json_str) { fprintf(stderr, "❌ XHamster: не смог извлечь JSON\n"); return -1; }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) { fprintf(stderr, "❌ XHamster: cJSON не разобрал initials\n"); return -1; }

    cJSON *vm = cJSON_GetObjectItem(root, "videoModel");
    if (!cJSON_IsObject(vm)) {
        fprintf(stderr, "❌ XHamster: videoModel не найден\n");
        cJSON_Delete(root); return -1;
    }

    /* Title */
    cJSON *title_j = cJSON_GetObjectItem(vm, "title");
    if (cJSON_IsString(title_j) && title_j->valuestring[0])
        out->title = strdup(title_j->valuestring);

    /* ── 1. videoModel.sources: {"mp4": {"720p": "url", ...}, ...} ── */
    cJSON *vm_sources = cJSON_GetObjectItem(vm, "sources");
    if (cJSON_IsObject(vm_sources)) {
        int   best_q   = -1;
        char *best_url = NULL;

        for (cJSON *fmt = vm_sources->child; fmt; fmt = fmt->next) {
            if (strcmp(fmt->string, "download") == 0) continue;
            if (!cJSON_IsObject(fmt)) continue;
            for (cJSON *q = fmt->child; q; q = q->next) {
                if (!cJSON_IsString(q) || !q->valuestring[0]) continue;
                int h = qval(qrank(q->string));
                if (quality_better(h, best_q)) {
                    best_q = h;
                    free(best_url);
                    best_url = strdup(q->valuestring);
                }
            }
        }

        if (best_url) {
            out->url     = best_url;
            out->quality = best_q > 0 ? best_q : 0;
            out->is_hls  = 0;
            cJSON_Delete(root);
            return 0;
        }
    }

    /* ── 2. xplayerSettings.sources.standard (encrypted URLs) ── */
    cJSON *xplayer  = cJSON_GetObjectItem(root, "xplayerSettings");
    cJSON *xp_src   = cJSON_IsObject(xplayer) ? cJSON_GetObjectItem(xplayer, "sources") : NULL;

    if (cJSON_IsObject(xp_src)) {
        cJSON *standard = cJSON_GetObjectItem(xp_src, "standard");
        if (cJSON_IsObject(standard)) {
            int   best_q   = -1;
            char *best_url = NULL;

            for (cJSON *fmts = standard->child; fmts; fmts = fmts->next) {
                if (!cJSON_IsArray(fmts)) continue;
                int n = cJSON_GetArraySize(fmts);
                for (int i = 0; i < n; i++) {
                    cJSON *item = cJSON_GetArrayItem(fmts, i);
                    if (!cJSON_IsObject(item)) continue;

                    const char *try_keys[] = {"url", "fallback", NULL};
                    for (int k = 0; try_keys[k]; k++) {
                        cJSON *u_j = cJSON_GetObjectItem(item, try_keys[k]);
                        if (!cJSON_IsString(u_j) || !u_j->valuestring[0]) continue;

                        char *dec = decipher_format_url(u_j->valuestring);
                        if (!dec) continue;

                        if (strstr(dec, ".m3u8")) { free(dec); continue; }

                        cJSON *q_j = cJSON_GetObjectItem(item, "quality");
                        if (!q_j) q_j = cJSON_GetObjectItem(item, "label");
                        int h = cJSON_IsString(q_j) ? qval(qrank(q_j->valuestring)) : 0;

                        if (quality_better(h, best_q)) {
                            best_q = h;
                            free(best_url);
                            best_url = dec;
                        } else {
                            free(dec);
                        }
                        break; /* url found, skip fallback */
                    }
                }
            }

            if (best_url) {
                out->url     = best_url;
                out->quality = best_q > 0 ? best_q : 0;
                out->is_hls  = 0;
                cJSON_Delete(root);
                return 0;
            }
        }

        /* ── 3. xplayerSettings.sources.hls (encrypted, last resort) ── */
        cJSON *hls_src = cJSON_GetObjectItem(xp_src, "hls");
        if (cJSON_IsObject(hls_src)) {
            const char *hls_keys[] = {"url", "fallback", NULL};
            for (int k = 0; hls_keys[k]; k++) {
                cJSON *h_j = cJSON_GetObjectItem(hls_src, hls_keys[k]);
                if (!cJSON_IsString(h_j) || !h_j->valuestring[0]) continue;
                char *hls_url = decipher_format_url(h_j->valuestring);
                if (hls_url) {
                    out->url    = hls_url;
                    out->is_hls = 1;
                    cJSON_Delete(root);
                    return 0;
                }
            }
        }
    }

    fprintf(stderr, "❌ XHamster: не нашёл ни одного источника\n");
    cJSON_Delete(root);
    return -1;
}
