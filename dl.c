#include "dl.h"
#include "http.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif

#define BAR_W 50

static int progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ult, curl_off_t uln) {
    (void)ud; (void)ult; (void)uln;
    if (dltotal <= 0) return 0;

    double pct = (double)dlnow / (double)dltotal * 100.0;
    int filled = (int)(pct / 100.0 * BAR_W);

    char bar[BAR_W + 1];
    for (int i = 0; i < BAR_W; i++) bar[i] = i < filled ? '#' : '.';
    bar[BAR_W] = '\0';

    double dl  = (double)dlnow   / (1024.0 * 1024.0);
    double tot = (double)dltotal / (1024.0 * 1024.0);
    printf("\r📥 [%s] %5.1f%% | %.1f/%.1f MB   ", bar, pct, dl, tot);
    fflush(stdout);
    return 0;
}

int dl_direct(const char *url, const char *proxy, const char *out_path,
              const char *cookies, const char *referer) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    FILE *fp = fopen(out_path, "wb");
    if (!fp) { curl_easy_cleanup(curl); return -1; }

    curl_easy_setopt(curl, CURLOPT_URL,              url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,        g_user_agent ? g_user_agent : HTTP_UA);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,   0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        fp);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    if (proxy && proxy[0])  curl_easy_setopt(curl, CURLOPT_PROXY,   proxy);
    else if (proxy && !proxy[0]) curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    if (cookies) curl_easy_setopt(curl, CURLOPT_COOKIE,  cookies);
    if (referer) curl_easy_setopt(curl, CURLOPT_REFERER, referer);

    CURLcode rc = curl_easy_perform(curl);
    printf("\n");
    fclose(fp);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : -1;
}

/* ── Built-in HLS downloader ─────────────────────────────────────────────── */

/* Resolve ref against base URL. */
static void url_resolve(const char *base, const char *ref, char *out, size_t sz) {
    if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8)) {
        snprintf(out, sz, "%s", ref);
        return;
    }
    if (!strncmp(ref, "//", 2)) {
        const char *c = strchr(base, ':');
        if (c) snprintf(out, sz, "%.*s:%s", (int)(c - base), base, ref);
        else   snprintf(out, sz, "https:%s", ref);
        return;
    }
    if (ref[0] == '/') {
        const char *s = strstr(base, "://");
        if (s) {
            s += 3;
            const char *slash = strchr(s, '/');
            size_t plen = slash ? (size_t)(slash - base) : strlen(base);
            snprintf(out, sz, "%.*s%s", (int)plen, base, ref);
        } else {
            snprintf(out, sz, "%s", ref);
        }
        return;
    }
    const char *last = strrchr(base, '/');
    if (last) snprintf(out, sz, "%.*s%s", (int)(last - base + 1), base, ref);
    else      snprintf(out, sz, "%s/%s", base, ref);
}

static int hls_is_master(const char *body) {
    return !!strstr(body, "#EXT-X-STREAM-INF:");
}

/* Pick highest-BANDWIDTH stream from master playlist. */
static int hls_best_stream(const char *body, const char *base,
                            char *out, size_t sz) {
    long best_bw = -1;
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-STREAM-INF:"))) {
        long bw = 0;
        const char *bwp = strstr(p, "BANDWIDTH=");
        if (bwp) bw = atol(bwp + 10);

        const char *nl = strchr(p, '\n');
        if (!nl) break;
        nl++;
        while (*nl == '\r') nl++;
        if (*nl == '#' || *nl == '\0') { p = nl; continue; }

        const char *end = nl;
        while (*end && *end != '\n' && *end != '\r') end++;

        if (bw > best_bw) {
            best_bw = bw;
            char tmp[2048];
            size_t ulen = (size_t)(end - nl);
            if (ulen >= sizeof(tmp)) ulen = sizeof(tmp) - 1;
            memcpy(tmp, nl, ulen); tmp[ulen] = '\0';
            url_resolve(base, tmp, out, sz);
        }
        p = nl;
    }
    return best_bw >= 0 ? 0 : -1;
}

/* Collect segment URLs from media playlist into a malloc'd array. */
static char **hls_collect_segs(const char *body, const char *base, int *out_n) {
    int cap = 256, n = 0;
    char **segs = malloc((size_t)cap * sizeof(char *));
    if (!segs) return nullptr;

    const char *p = body;
    while (*p) {
        const char *inf = strstr(p, "#EXTINF:");
        if (!inf) break;
        const char *nl = strchr(inf, '\n');
        if (!nl) break;
        nl++;
        while (*nl == '\r') nl++;
        if (*nl == '#' || *nl == '\0') { p = nl; continue; }

        const char *end = nl;
        while (*end && *end != '\n' && *end != '\r') end++;

        char tmp[2048];
        size_t ulen = (size_t)(end - nl);
        if (ulen >= sizeof(tmp)) ulen = sizeof(tmp) - 1;
        memcpy(tmp, nl, ulen); tmp[ulen] = '\0';

        char resolved[2048];
        url_resolve(base, tmp, resolved, sizeof(resolved));

        if (n >= cap) {
            cap *= 2;
            char **t = realloc(segs, (size_t)cap * sizeof(char *));
            if (!t) break;
            segs = t;
        }
        segs[n++] = strdup(resolved);
        p = end;
    }
    *out_n = n;
    return segs;
}

static size_t seg_write_cb(char *ptr, size_t sz, size_t n, void *ud) {
    return fwrite(ptr, sz, n, (FILE *)ud);
}

int dl_hls(const char *url, const char *proxy, const char *out_path) {
    /* ── 1. Fetch playlist ── */
    HttpSession *sess = http_session_new(proxy);
    HttpResp *resp = http_get(sess, url, NULL, NULL);
    http_session_free(sess);
    if (!resp) { fprintf(stderr, "❌ HLS: не смог скачать плейлист\n"); return -1; }

    char media_url[2048];
    snprintf(media_url, sizeof(media_url), "%s", url);

    /* ── 2. Resolve master → media playlist ── */
    if (hls_is_master(resp->body)) {
        char best[2048];
        if (hls_best_stream(resp->body, url, best, sizeof(best)) != 0) {
            fprintf(stderr, "❌ HLS: мастер-плейлист без потоков\n");
            http_resp_free(resp); return -1;
        }
        snprintf(media_url, sizeof(media_url), "%s", best);
        http_resp_free(resp);

        sess = http_session_new(proxy);
        resp = http_get(sess, media_url, NULL, NULL);
        http_session_free(sess);
        if (!resp) { fprintf(stderr, "❌ HLS: не смог скачать медиа-плейлист\n"); return -1; }
    }

    /* ── 3. Collect segments ── */
    int seg_count = 0;
    char **segs = hls_collect_segs(resp->body, media_url, &seg_count);
    http_resp_free(resp);

    if (!segs || seg_count == 0) {
        fprintf(stderr, "❌ HLS: сегменты не найдены\n");
        free(segs); return -1;
    }
    printf("📋 HLS: %d сегментов\n", seg_count);

    /* ── 4. Download segments → out_path ── */
    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        perror("fopen");
        for (int i = 0; i < seg_count; i++) free(segs[i]);
        free(segs); return -1;
    }

    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      g_user_agent ? g_user_agent : HTTP_UA);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  seg_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      fp);
    if (proxy && proxy[0])       curl_easy_setopt(curl, CURLOPT_PROXY,   proxy);
    else if (proxy && !proxy[0]) curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");

    int ok = 0;
    for (int i = 0; i < seg_count; i++) {
        if (ok == 0) {
            printf("\r📥 HLS [%d/%d] %.0f%%   ",
                   i + 1, seg_count, (double)(i + 1) / seg_count * 100.0);
            fflush(stdout);
            curl_easy_setopt(curl, CURLOPT_URL, segs[i]);
            if (curl_easy_perform(curl) != CURLE_OK) {
                fprintf(stderr, "\n❌ HLS: ошибка сегмента %d\n", i + 1);
                ok = -1;
            }
        }
        free(segs[i]);
    }
    printf("\n");

    curl_easy_cleanup(curl);
    fclose(fp);
    free(segs);
    return ok;
}

int dl_video(const VideoInfo *v, const char *proxy, const char *out_path) {
    if (v->is_hls) {
        printf("📡 HLS → встроенный загрузчик...\n");
        return dl_hls(v->url, proxy, out_path);
    }
    printf("📥 Прямой файл → libcurl...\n");
    return dl_direct(v->url, proxy, out_path, v->cookies, v->referer);
}

int ytdlp_download(const char *url, const char *proxy, const char *temp_dir) {
    char outtmpl[1024];
    snprintf(outtmpl, sizeof(outtmpl), "%s/%%(title)s.%%(ext)s", temp_dir);

    const char *args[32];
    int i = 0;
    args[i++] = "yt-dlp";
    args[i++] = "--format";              args[i++] = "best";
    args[i++] = "-o";                    args[i++] = outtmpl;
    args[i++] = "--restrict-filenames";
    args[i++] = "--no-playlist";
    args[i++] = "--retries";             args[i++] = "10";
    args[i++] = "--fragment-retries";    args[i++] = "10";
    args[i++] = "--merge-output-format"; args[i++] = "mp4";
    if (proxy) { args[i++] = "--proxy"; args[i++] = proxy; }
    args[i++] = url;
    args[i++] = NULL;

#ifdef _WIN32
    int rc = (int)_spawnvp(_P_WAIT, "yt-dlp", (const char *const *)args);
    return rc == 0 ? 0 : -1;
#else
    pid_t pid = fork();
    if (pid == 0) { execvp("yt-dlp", (char *const *)args); exit(127); }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
#endif
}
