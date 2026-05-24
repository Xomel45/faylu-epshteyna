#include "http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct HttpSession {
    CURL *curl;
};

static size_t write_cb(char *ptr, size_t sz, size_t n, void *ud) {
    HttpResp *r = ud;
    size_t total = sz * n;
    char *tmp = realloc(r->body, r->len + total + 1);
    if (!tmp) return 0;
    r->body = tmp;
    memcpy(r->body + r->len, ptr, total);
    r->len += total;
    r->body[r->len] = '\0';
    return total;
}

HttpSession *http_session_new(const char *proxy) {
    static int inited;
    if (!inited++) curl_global_init(CURL_GLOBAL_DEFAULT);

    HttpSession *s = calloc(1, sizeof(*s));
    s->curl = curl_easy_init();

    curl_easy_setopt(s->curl, CURLOPT_USERAGENT,      g_user_agent ? g_user_agent : HTTP_UA);
    curl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s->curl, CURLOPT_MAXREDIRS,      10L);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(s->curl, CURLOPT_TIMEOUT,        60L);
    curl_easy_setopt(s->curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(s->curl, CURLOPT_COOKIEFILE, "");
    if (proxy && proxy[0])
        curl_easy_setopt(s->curl, CURLOPT_PROXY, proxy);
    else if (proxy && !proxy[0])
        curl_easy_setopt(s->curl, CURLOPT_NOPROXY, "*");
    /* proxy == NULL → libcurl auto-detects HTTP_PROXY/HTTPS_PROXY from env */
    return s;
}

void http_session_free(HttpSession *s) {
    if (!s) return;
    curl_easy_cleanup(s->curl);
    free(s);
}

void http_session_cookie(HttpSession *s, const char *domain,
                          const char *name, const char *val) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "Set-Cookie: %s=%s; domain=.%s; path=/", name, val, domain);
    curl_easy_setopt(s->curl, CURLOPT_COOKIELIST, buf);
}

static HttpResp *do_req(HttpSession *s, const char *url,
                        const char *post_data, const char *referer,
                        const char *ctype, const char **extra) {
    HttpResp *r = calloc(1, sizeof(*r));

    curl_easy_setopt(s->curl, CURLOPT_URL,           url);
    curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(s->curl, CURLOPT_WRITEDATA,     r);
    curl_easy_setopt(s->curl, CURLOPT_REFERER,       referer ? referer : "");

    struct curl_slist *hdrs = NULL;
    if (ctype) {
        char ct[256];
        snprintf(ct, sizeof(ct), "Content-Type: %s", ctype);
        hdrs = curl_slist_append(hdrs, ct);
    }
    for (int i = 0; extra && extra[i]; i++)
        hdrs = curl_slist_append(hdrs, extra[i]);
    curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, hdrs);

    if (post_data) {
        curl_easy_setopt(s->curl, CURLOPT_POST,          1L);
        curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS,    post_data);
        curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_data));
    } else {
        curl_easy_setopt(s->curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode rc = curl_easy_perform(s->curl);
    if (hdrs) curl_slist_free_all(hdrs);

    if (rc != CURLE_OK) {
        fprintf(stderr, "⚠️  HTTP: %s\n", curl_easy_strerror(rc));
        free(r->body); free(r);
        return nullptr;
    }
    curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &r->status);
    char *eff = NULL;
    curl_easy_getinfo(s->curl, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) r->effective_url = strdup(eff);
    return r;
}

HttpResp *http_get(HttpSession *s, const char *url,
                   const char *referer, const char **extra) {
    return do_req(s, url, NULL, referer, NULL, extra);
}

HttpResp *http_post(HttpSession *s, const char *url, const char *data,
                    const char *referer, const char *ctype, const char **extra) {
    return do_req(s, url, data, referer, ctype, extra);
}

void http_resp_free(HttpResp *r) {
    if (!r) return;
    free(r->body);
    free(r->effective_url);
    free(r);
}
