#pragma once
#include "extractors/extractor.h"

[[nodiscard]] int dl_video (const VideoInfo *v, const char *proxy, const char *out_path);
[[nodiscard]] int dl_direct(const char *url,    const char *proxy, const char *out_path,
                             const char *cookies, const char *referer);
[[nodiscard]] int dl_hls   (const char *url,    const char *proxy, const char *out_path);
[[nodiscard]] int ytdlp_download(const char *url, const char *proxy, const char *temp_dir);
