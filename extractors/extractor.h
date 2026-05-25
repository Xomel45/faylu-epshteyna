#pragma once

typedef struct {
    char *url;     /* direct mp4/webm or m3u8 URL */
    char *title;   /* may be NULL */
    bool  is_hls;  /* true = m3u8, false = direct file */
    int   quality; /* e.g. 720, 0 = unknown */
    char *cookies; /* optional "name=val; ..."  for download (e.g. TikTok) */
    char *referer; /* optional Referer header for download */
} VideoInfo;

void video_info_free(VideoInfo *v);

/* 0 = no cap; > 0 = max quality height allowed (set via --quality flag) */
extern int g_max_quality;

/* Returns true if height h is a better pick than best_h under the cap. */
static inline bool quality_better(int h, int best_h) {
    if (g_max_quality > 0) {
        bool h_ok    = (h    <= g_max_quality);
        bool best_ok = (best_h <= g_max_quality && best_h >= 0);
        if ( h_ok && !best_ok) return true;
        if (!h_ok &&  best_ok) return false;
        if ( h_ok)             return h > best_h;
        return h < best_h;
    }
    return h > best_h;
}

[[nodiscard]] int extract_pornhub     (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_vk          (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_xvideos     (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_xhamster    (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_xnxx        (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_eporner     (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_redtube     (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_sex_studentki(const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_ebalko       (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_bunkr        (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_tiktok       (const char *url, const char *proxy, VideoInfo *out);
[[nodiscard]] int extract_thothub      (const char *url, const char *proxy, VideoInfo *out);

/* Yandex video preview: returns malloc'd source videoUrl, caller re-dispatches */
[[nodiscard]] int extract_pornonahobka  (const char *url, const char *proxy, VideoInfo *out);

/* Yandex video preview: returns malloc'd source videoUrl, caller re-dispatches */
[[nodiscard]] char *yandex_extract_source_url(const char *url, const char *proxy);
