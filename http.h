#pragma once
#include "compat.h"
#include <stddef.h>

#define HTTP_UA \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 " \
    "(KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36"

/* NULL = use HTTP_UA; set via --user-agent flag */
extern const char *g_user_agent;

typedef struct {
    char  *body;
    size_t len;
    long   status;
    char  *effective_url; /* final URL after redirects, malloc'd */
} HttpResp;

typedef struct HttpSession HttpSession;

HttpSession *http_session_new   (const char *proxy);
void         http_session_free  (HttpSession *s);
/* inject cookie into session jar: domain like "pornhub.com" */
void         http_session_cookie(HttpSession *s, const char *domain,
                                  const char *name, const char *val);

/* extra: NULL-terminated array of "Name: Value" strings, or NULL */
HttpResp    *http_get (HttpSession *s, const char *url,
                       const char *referer, const char **extra);
HttpResp    *http_post(HttpSession *s, const char *url, const char *data,
                       const char *referer, const char *ctype,
                       const char **extra);
void         http_resp_free(HttpResp *r);
