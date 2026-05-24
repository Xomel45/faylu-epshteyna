#include "extractor.h"
#include "../http.h"
#include "../vendor/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <openssl/evp.h>

/* Base64 decode using OpenSSL EVP. Returns malloc'd bytes, sets *out_len. */
static unsigned char *b64_decode(const char *in, size_t *out_len) {
    size_t in_len = strlen(in);
    size_t max_out = (in_len / 4) * 3 + 4;
    unsigned char *buf = malloc(max_out);
    if (!buf) return NULL;

    EVP_ENCODE_CTX *ctx = EVP_ENCODE_CTX_new();
    int total = 0, written = 0;
    EVP_DecodeInit(ctx);
    EVP_DecodeUpdate(ctx, buf, &written, (const unsigned char *)in, (int)in_len);
    total = written;
    EVP_DecodeFinal(ctx, buf + total, &written);
    total += written;
    EVP_ENCODE_CTX_free(ctx);

    *out_len = (size_t)total;
    return buf;
}

/* XOR decrypt: key repeated, in-place on bytes. Returns malloc'd string. */
static char *xor_decrypt(const unsigned char *data, size_t data_len,
                          const char *key) {
    size_t key_len = strlen(key);
    char *out = malloc(data_len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < data_len; i++)
        out[i] = (char)((unsigned char)data[i] ^ (unsigned char)key[i % key_len]);
    out[data_len] = '\0';
    return out;
}

/* Extract first run of digits after needle in haystack. */
static char *extract_digits_after(const char *haystack, const char *needle) {
    const char *p = strstr(haystack, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return NULL;
    const char *e = p;
    while (isdigit((unsigned char)*e)) e++;
    return strndup(p, (size_t)(e - p));
}

/* Get numeric file ID from any bunkr URL variant:
 *   get.bunkrr.su/file/49192341  → "49192341"
 *   bunkr.cr/f/pk5NHl27OPocf    → fetch page, find data-file-id="..." */
static char *get_file_id(const char *url, const char *proxy) {
    /* Direct: get.bunkrr.su/file/{id} */
    {
        const char *p = strstr(url, "/file/");
        if (p && isdigit((unsigned char)p[6])) {
            p += 6;
            const char *e = p;
            while (isdigit((unsigned char)*e)) e++;
            if (e > p) return strndup(p, (size_t)(e - p));
        }
    }

    /* Slug URL: fetch HTML and find data-file-id="..." */
    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, url, NULL, NULL);
    http_session_free(sess);
    if (!resp || resp->status != 200) {
        if (resp) http_resp_free(resp);
        return NULL;
    }

    char *id = extract_digits_after(resp->body, "data-file-id=\"");
    if (!id) id = extract_digits_after(resp->body, "get.bunkrr.su/file/");
    http_resp_free(resp);
    return id;
}

int extract_bunkr(const char *url, const char *proxy, VideoInfo *out) {
    memset(out, 0, sizeof(*out));

    char *file_id = get_file_id(url, proxy);
    if (!file_id) {
        fprintf(stderr, "❌ Bunkr: не смог извлечь file ID\n");
        return -1;
    }
    printf("🔑 Bunkr: file ID = %s\n", file_id);

    /* POST {"id":"<file_id>"} to API */
    char body[64];
    snprintf(body, sizeof(body), "{\"id\":\"%s\"}", file_id);
    free(file_id);

    const char *headers[] = {
        "Origin: https://get.bunkrr.su",
        "Referer: https://get.bunkrr.su/",
        NULL
    };

    HttpSession *sess = http_session_new(proxy);
    HttpResp *api = http_post(sess, "https://apidl.bunkr.ru/api/_001_v2",
                              body, NULL, "application/json", headers);
    http_session_free(sess);

    if (!api) { fprintf(stderr, "❌ Bunkr: API не ответил\n"); return -1; }
    if (api->status != 200) {
        fprintf(stderr, "❌ Bunkr: API HTTP %ld\n", api->status);
        http_resp_free(api); return -1;
    }

    cJSON *root = cJSON_Parse(api->body);
    http_resp_free(api);
    if (!root) { fprintf(stderr, "❌ Bunkr: JSON не разобрался\n"); return -1; }

    cJSON *enc  = cJSON_GetObjectItem(root, "encrypted");
    cJSON *j_ts = cJSON_GetObjectItem(root, "timestamp");
    cJSON *j_url= cJSON_GetObjectItem(root, "url");

    if (!cJSON_IsTrue(enc) || !cJSON_IsNumber(j_ts) || !cJSON_IsString(j_url)) {
        fprintf(stderr, "❌ Bunkr: неожиданный ответ API\n");
        cJSON_Delete(root); return -1;
    }

    long ts = (long)j_ts->valuedouble;
    char key[64];
    snprintf(key, sizeof(key), "SECRET_KEY_%ld", ts / 3600);

    size_t dec_len = 0;
    unsigned char *dec_bytes = b64_decode(j_url->valuestring, &dec_len);
    cJSON_Delete(root);

    if (!dec_bytes) { fprintf(stderr, "❌ Bunkr: base64 decode провалился\n"); return -1; }

    char *cdn_url = xor_decrypt(dec_bytes, dec_len, key);
    free(dec_bytes);

    if (!cdn_url || strncmp(cdn_url, "http", 4) != 0) {
        fprintf(stderr, "❌ Bunkr: расшифровка дала невалидный URL: %.40s\n",
                cdn_url ? cdn_url : "(null)");
        free(cdn_url); return -1;
    }

    out->url = cdn_url;
    out->is_hls = (strstr(cdn_url, ".m3u8") != NULL);
    return 0;
}
