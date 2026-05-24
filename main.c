#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif
#include <getopt.h>

#include "compat.h"
#include "files.h"
#include "dl.h"
#include "extractors/extractor.h"

static bool is_pornhub(const char *u) {
    return !!(strstr(u, "pornhub.com") || strstr(u, "pornhub.net") ||
              strstr(u, "pornhub.org") || strstr(u, "pornhubpremium"));
}

static bool is_vk(const char *u) {
    return !!(strstr(u, "vk.com/video") || strstr(u, "vkvideo.ru") ||
              strstr(u, "vk.com/clip")  || strstr(u, "vk.com/video_ext") ||
              strstr(u, "ukdevilz.com"));
}

static bool is_sex_studentki(const char *u) {
    return !!(strstr(u, "sex-studentki.team") || strstr(u, "sex-studentki.ru") ||
              strstr(u, "sex-studentki.live"));
}

static bool is_tiktok(const char *u) {
    return !!(strstr(u, "tiktok.com") || strstr(u, "vm.tiktok.com") ||
              strstr(u, "vt.tiktok.com"));
}

static bool is_bunkr(const char *u) {
    return !!(strstr(u, "bunkr.cr")   || strstr(u, "bunkr.ru")  ||
              strstr(u, "bunkr.ph")   || strstr(u, "bunkrr.su") ||
              strstr(u, "bunkr.black")|| strstr(u, "bunkr.media"));
}

static bool is_ebalko(const char *u) {
    return !!(strstr(u, "ebalko.net") || strstr(u, "ebalka.ru") ||
              strstr(u, "34ebalka.ru") || strstr(u, "ebalka1.ru"));
}

static bool is_thothub(const char *u) {
    return !!strstr(u, "thothub.tube");
}

static bool is_xvideos(const char *u) {
    return !!(strstr(u, "xvideos.com") || strstr(u, "xvideos2.com") ||
              strstr(u, "xvideos.es"));
}

static bool is_xhamster(const char *u) {
    return !!(strstr(u, "xhamster.com") || strstr(u, "xhamster.one")  ||
              strstr(u, "xhamster.desi")|| strstr(u, "xhms.pro")      ||
              strstr(u, "xhday.com")    || strstr(u, "xhvid.com"));
}

static bool is_xnxx(const char *u) {
    return !!(strstr(u, "xnxx.com") || strstr(u, "xnxx3.com"));
}

static bool is_eporner(const char *u) {
    return !!strstr(u, "eporner.com");
}

static bool is_redtube(const char *u) {
    return !!(strstr(u, "redtube.com") || strstr(u, "embed.redtube.com"));
}

/* Return largest regular file > MIN_FILE_SIZE_MB in dir, malloc'd path or NULL */
static char *find_large_file(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return NULL;

    long   best_size = 0;
    char   best[PATH_MAX] = "";
    long   min_bytes  = (long)MIN_FILE_SIZE_MB * 1024 * 1024;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen >= 9 && strcmp(ent->d_name + nlen - 9, ".reserved") == 0) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (st.st_size > min_bytes && st.st_size > best_size) {
            best_size = st.st_size;
            snprintf(best, sizeof(best), "%s", path);
        }
    }
    closedir(d);
    return best[0] ? strdup(best) : NULL;
}

static const char *file_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "mp4";
    const char *ext = dot + 1;
    if (!strcmp(ext, "unknown_video") || !strcmp(ext, "part") ||
        !strcmp(ext, "tmp")           || !strcmp(ext, "download"))
        return "mp4";
    return ext;
}

static void rmdir_contents(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, ent->d_name);
        remove(p);
    }
    closedir(d);
    rmdir(dir);
}

/* Read one trimmed line from stdin into buf[sz]. Returns 0 on success. */
static int read_line(char *buf, size_t sz) {
    if (!fgets(buf, (int)sz, stdin)) return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

int         g_max_quality = 0;
const char *g_output_path = SECRET_PATH;
const char *g_user_agent  = NULL;

int main(int argc, char *argv[]) {
    int         no_proxy     = 0;
    int         system_proxy = 0;
    const char *flag_url     = NULL;

    static const struct option lopts[] = {
        {"no-proxy",     no_argument,       NULL, 'n'},
        {"system-proxy", no_argument,       NULL, 's'},
        {"url",          required_argument, NULL, 'u'},
        {"output",       required_argument, NULL, 'o'},
        {"quality",      required_argument, NULL, 'q'},
        {"user-agent",   required_argument, NULL, 'A'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "nsu:o:q:A:", lopts, NULL)) != -1) {
        switch (opt) {
        case 'n': no_proxy     = 1; break;
        case 's': system_proxy = 1; break;
        case 'u': flag_url     = optarg; break;
        case 'o': g_output_path = optarg; break;
        case 'q': g_max_quality = atoi(optarg); break;
        case 'A': g_user_agent  = optarg; break;
        default:
            fprintf(stderr,
                    "Использование: %s [--no-proxy] [--system-proxy] [--url URL]"
                    " [--output DIR] [--quality N] [--user-agent UA]\n", argv[0]);
            return 1;
        }
    }
    if (!flag_url && optind < argc)
        flag_url = argv[optind];

    /* proxy selection:
     *   ""    → --no-proxy:     bypass all (CURLOPT_NOPROXY="*")
     *   NULL  → --system-proxy: libcurl reads HTTP_PROXY/HTTPS_PROXY from env
     *   PROXY → default:        hardcoded address from files.h               */
    const char *proxy = no_proxy ? "" : (system_proxy ? NULL : PROXY);

    printf("--- ФАЙЛЫ ЭПШТЕЙНА " APP_VERSION " (C EDITION) ---\n");
    if (no_proxy)            printf("🚫 Режим БЕЗ прокси\n");
    else if (system_proxy)   printf("🌐 Системный прокси\n");
    if (g_max_quality > 0)   printf("🎚️  Качество ≤ %dp\n", g_max_quality);

    if (access(g_output_path, F_OK) != 0) {
        printf("❌ ОШИБКА: %s не найден.\n", g_output_path);
        return 1;
    }
    if (get_free_space_gb(g_output_path) < MIN_FREE_SPACE_GB) {
        printf("⚠️  АТАС: Мало места!\n");
        return 1;
    }

    char url_buf[2048] = {0};
    if (flag_url) {
        strncpy(url_buf, flag_url, sizeof(url_buf) - 1);
    } else {
        printf("🔗 Кидай ссылку: ");
        fflush(stdout);
        if (read_line(url_buf, sizeof(url_buf)) != 0 || !url_buf[0]) return 0;
    }

    const char *url = url_buf; /* may be repointed after yandex resolution */
    char *domain = get_domain(url);
    printf("📁 Домен: %s\n", domain);

    char domain_path[PATH_MAX];
    snprintf(domain_path, sizeof(domain_path), "%s/%s", g_output_path, domain);
    mkdir(domain_path, 0755);

    int cleaned = cleanup_old_reserved(domain_path, RESERVED_TIMEOUT_H);
    if (cleaned > 0) printf("🧹 Почистил %d зависших резервов\n", cleaned);

    char reserved_path[PATH_MAX];
    int num = reserve_next_number(domain_path, reserved_path, sizeof(reserved_path));
    if (num < 0) {
        printf("❌ Не смог зарезервировать номер!\n");
        free(domain); return 1;
    }
    printf("🔒 ЗАРЕЗЕРВИРОВАЛ НОМЕР: %d\n", num);

    char temp_dir[PATH_MAX];
    snprintf(temp_dir, sizeof(temp_dir), "%s/%d", domain_path, num);
    mkdir(temp_dir, 0755);
    printf("📂 Временная папка: %s/%d/\n", domain, num);
    printf("🚀 Прокси: %s\n", (proxy && proxy[0]) ? proxy : (proxy ? "системный" : "OFF"));

    /* --- Yandex video preview: resolve to actual source URL --- */
    char *yandex_resolved = NULL;
    if (strstr(url, "yandex.ru/video") || strstr(url, "yandex.com/video")) {
        printf("🔍 Яндекс Видео → извлекаю источник...\n");
        yandex_resolved = yandex_extract_source_url(url, proxy);
        if (yandex_resolved) {
            printf("↩️  Источник: %s\n", yandex_resolved);
            url = yandex_resolved;
            free(domain);
            domain = get_domain(url);
            printf("📁 Домен (источник): %s\n", domain);
            snprintf(domain_path, sizeof(domain_path), "%s/%s", g_output_path, domain);
            mkdir(domain_path, 0755);
        } else {
            printf("⚠️  Яндекс: не смог извлечь источник, пробую yt-dlp...\n");
        }
    }

    /* --- Dispatch to extractor --- */
    VideoInfo info = {0};
    int extracted = 0;

    if (is_pornhub(url)) {
        printf("🔍 Экстрактор: PornHub\n");
        extracted = (extract_pornhub(url, proxy, &info) == 0);
    } else if (is_vk(url)) {
        printf("🔍 Экстрактор: VK\n");
        /* ukdevilz URLs look like /watch/-158673423_456239661 — rewrite to vk.com */
        if (strstr(url, "ukdevilz.com")) {
            const char *id = strstr(url, "/watch/");
            if (id) {
                char vk_url[256];
                snprintf(vk_url, sizeof(vk_url), "https://vk.com/video%s", id + 7);
                extracted = (extract_vk(vk_url, proxy, &info) == 0);
            }
        } else {
            extracted = (extract_vk(url, proxy, &info) == 0);
        }
    } else if (is_sex_studentki(url)) {
        printf("🔍 Экстрактор: SexStudentki\n");
        extracted = (extract_sex_studentki(url, proxy, &info) == 0);
    } else if (is_xvideos(url)) {
        printf("🔍 Экстрактор: XVideos\n");
        extracted = (extract_xvideos(url, proxy, &info) == 0);
    } else if (is_xhamster(url)) {
        printf("🔍 Экстрактор: XHamster\n");
        extracted = (extract_xhamster(url, proxy, &info) == 0);
    } else if (is_xnxx(url)) {
        printf("🔍 Экстрактор: XNXX\n");
        extracted = (extract_xnxx(url, proxy, &info) == 0);
    } else if (is_eporner(url)) {
        printf("🔍 Экстрактор: EPorner\n");
        extracted = (extract_eporner(url, proxy, &info) == 0);
    } else if (is_redtube(url)) {
        printf("🔍 Экстрактор: RedTube\n");
        extracted = (extract_redtube(url, proxy, &info) == 0);
    } else if (is_tiktok(url)) {
        printf("🔍 Экстрактор: TikTok\n");
        extracted = (extract_tiktok(url, proxy, &info) == 0);
    } else if (is_bunkr(url)) {
        printf("🔍 Экстрактор: Bunkr\n");
        extracted = (extract_bunkr(url, proxy, &info) == 0);
    } else if (is_ebalko(url)) {
        printf("🔍 Экстрактор: Ebalko\n");
        extracted = (extract_ebalko(url, proxy, &info) == 0);
    } else if (is_thothub(url)) {
        printf("🔍 Экстрактор: Thothub\n");
        extracted = (extract_thothub(url, proxy, &info) == 0);
    }

    if (extracted) {
        printf("✅ URL найден");
        if (info.quality) printf(" (%dp)", info.quality);
        printf("\n");
        if (info.title) printf("📝 Название: %s\n", info.title);

        const char *ext = info.is_hls ? "ts" : file_ext(info.url);
        char out_path[PATH_MAX];
        snprintf(out_path, sizeof(out_path), "%s/video.%s", temp_dir, ext);

        if (dl_video(&info, proxy, out_path) != 0) {
            printf("❌ Скачка через экстрактор не удалась, пробую yt-dlp...\n");
            extracted = 0;
        }
        video_info_free(&info);
    }

    if (!extracted) {
        printf("🔄 Fallback → yt-dlp\n");
        if (ytdlp_download(url, proxy, temp_dir) != 0)
            printf("⚠️  yt-dlp завершился с ошибкой, проверяю файлы...\n");
    }

    /* --- Find downloaded file --- */
    printf("🔍 Ищу файл в temp папке (больше %dМБ)...\n", MIN_FILE_SIZE_MB);
    char *dl_file = find_large_file(temp_dir);

    if (!dl_file) {
        printf("⚠️  Не нашёл файлов больше %dМБ!\n", MIN_FILE_SIZE_MB);
        printf("🤔 Временная папка осталась: %s/%d/\n", domain, num);
        printf("⚠️  Резерв автоочистится через %dч\n", RESERVED_TIMEOUT_H);
        free(domain);
        return 1;
    }

    struct stat dl_st;
    stat(dl_file, &dl_st);
    printf("✅ Нашёл: %s (%.1f МБ)\n",
           strrchr(dl_file, '/') + 1,
           (double)dl_st.st_size / (1024.0 * 1024.0));

    /* --- Duplicate check --- */
    char *dup = find_duplicate(dl_file, g_output_path, dl_file);
    if (dup) {
        printf("\n⚠️  ДУБЛИКАТ! Уже есть: %s\n", dup);
        printf("Что делаем? [у]далить новый / [с]охранить оба / [з]аменить старый: ");
        fflush(stdout);

        char choice[16] = {0};
        read_line(choice, sizeof(choice));

        /* Cyrillic 'у' = 0xD1 0x83, 'з' = 0xD0 0xB7 */
        int del_new     = (choice[0] == '\xd1' && choice[1] == '\x83');
        int replace_old = (choice[0] == '\xd0' && choice[1] == '\xb7');

        if (del_new) {
            remove(dl_file);
            rmdir_contents(temp_dir);
            remove(reserved_path);
            printf("🗑️  Удалил новый файл (дубликат), резерв освобождён\n");
            free(dup); free(dl_file); free(domain);
            return 0;
        } else if (replace_old) {
            remove(dup);
            printf("🗑️  Удалил старый: %s\n", strrchr(dup, '/') + 1);
        } else {
            printf("👌 Сохраняем оба...\n");
        }
        free(dup);
    } else {
        printf("✅ Дубликатов нет!\n");
    }

    /* --- Move to final location --- */
    const char *ext = file_ext(dl_file);
    char final_name[256];
    snprintf(final_name, sizeof(final_name), "uchebka_%d.%s", num, ext);
    char final_path[PATH_MAX];
    snprintf(final_path, sizeof(final_path), "%s/%s", domain_path, final_name);

    printf("🔄 Перемещаю → %s/%s\n", domain, final_name);
    if (safe_move(dl_file, final_path) != 0) {
        perror("❌ safe_move");
        free(dl_file); free(domain);
        return 1;
    }

    /* Cleanup temp dir and reserved marker */
    rmdir_contents(temp_dir);
    remove(reserved_path);
    printf("🔓 Резерв освобождён\n");

    printf("\n✅ ГОТОВО! Сохранено: %s/%s\n", domain, final_name);

    /* --- Trash cleanup --- */
    printf("\n🧹 Зачищаю мусор (файлы < %dМБ)...\n", TRASH_SIZE_MB);
    int deleted = cleanup_trash(g_output_path, TRASH_SIZE_MB);
    if (deleted > 0)
        printf("✅ Удалено мусора: %d файл(ов)\n", deleted);
    else
        printf("👌 Мусора нет!\n");

    free(dl_file);
    free(domain);
    free(yandex_resolved);
    return 0;
}
