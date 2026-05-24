#include "files.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <openssl/evp.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/statvfs.h>
#endif

long get_free_space_gb(const char *path) {
#ifdef _WIN32
    ULARGE_INTEGER free_bytes;
    if (!GetDiskFreeSpaceExA(path, &free_bytes, NULL, NULL)) return 0;
    return (long)(free_bytes.QuadPart >> 30);
#else
    struct statvfs st;
    if (statvfs(path, &st) != 0) return 0;
    return (long)((unsigned long long)st.f_bavail * st.f_frsize >> 30);
#endif
}

char *get_domain(const char *url) {
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;

    const char *end = start;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;

    char host[256] = {0};
    size_t hlen = (size_t)(end - start);
    if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
    memcpy(host, start, hlen);

    /* strip port */
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';

    /* keep only last two labels (sub.host.com → host.com) */
    int dots = 0;
    for (char *p = host; *p; p++) if (*p == '.') dots++;
    if (dots >= 2) {
        char *first = strchr(host, '.');
        if (first) memmove(host, first + 1, strlen(first));
    }
    return strdup(host);
}

int cleanup_old_reserved(const char *dir, int timeout_hours) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    time_t cutoff = time(NULL) - (time_t)(timeout_hours * 3600);
    int count = 0;
    struct dirent *ent;
    char path[1024];

    while ((ent = readdir(d))) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 9 || strcmp(ent->d_name + nlen - 9, ".reserved") != 0) continue;

        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_mtime < cutoff) {
            remove(path);
            printf("🧹 Удалил зависший резерв: %s\n", ent->d_name);
            count++;
        }
    }
    closedir(d);
    return count;
}

int reserve_next_number(const char *dir, char *reserved_out, size_t sz) {
    char lock_path[1024];
    snprintf(lock_path, sizeof(lock_path), "%s/.reservation.lock", dir);

#ifdef _WIN32
    HANDLE lock_h = CreateFileA(lock_path, GENERIC_WRITE,
                                0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock_h == INVALID_HANDLE_VALUE) { perror("open lock"); return -1; }
#else
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock_fd < 0) { perror("open lock"); return -1; }

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(lock_fd, F_SETLKW, &fl) < 0) {
        perror("flock"); close(lock_fd); return -1;
    }
#endif

    int used[4096] = {0};
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            int n; char suffix[64];
            if (sscanf(ent->d_name, "uchebka_%d.%63s", &n, suffix) == 2
                && n >= 0 && n < 4096) {
                used[n] = 1;
            }
            /* temp dirs are pure numbers */
            char *endp;
            long val = strtol(ent->d_name, &endp, 10);
            if (*endp == '\0' && endp != ent->d_name && val >= 0 && val < 4096) {
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
                struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                    used[(int)val] = 1;
            }
        }
        closedir(d);
    }

    int num = 0;
    while (num < 4096 && used[num]) num++;

    snprintf(reserved_out, sz, "%s/uchebka_%d.reserved", dir, num);
    FILE *f = fopen(reserved_out, "w");
#ifdef _WIN32
    if (f) { fprintf(f, "pid=%lu\n", (unsigned long)GetCurrentProcessId()); fclose(f); }
    CloseHandle(lock_h);
#else
    if (f) { fprintf(f, "pid=%d\n", getpid()); fclose(f); }
    fl.l_type = F_UNLCK;
    fcntl(lock_fd, F_SETLK, &fl);
    close(lock_fd);
#endif
    return num;
}

int compute_partial_hash(const char *path, char out_hex[33]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseeko(f, 0, SEEK_END);
    off_t file_size = ftello(f);
    rewind(f);

    size_t chunk = (size_t)CHUNK_MB * 1024 * 1024;
    unsigned char *buf = malloc(chunk);
    if (!buf) { fclose(f); return -1; }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);

    size_t n = fread(buf, 1, chunk, f);
    if (n > 0) EVP_DigestUpdate(ctx, buf, n);

    if (file_size > (off_t)(chunk * 2)) {
        fseeko(f, -(off_t)chunk, SEEK_END);
        n = fread(buf, 1, chunk, f);
        if (n > 0) EVP_DigestUpdate(ctx, buf, n);
    }

    unsigned char digest[16];
    unsigned int dlen = 16;
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);
    free(buf);
    fclose(f);

    for (int i = 0; i < 16; i++)
        sprintf(out_hex + 2 * i, "%02x", digest[i]);
    out_hex[32] = '\0';
    return 0;
}

static void find_dup_recurse(const char *dir, const char *hash,
                              const char *exclude, char **result) {
    if (*result) return;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) && !*result) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (exclude && strcmp(path, exclude) == 0) continue;

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            find_dup_recurse(path, hash, exclude, result);
        } else if (S_ISREG(st.st_mode)) {
            size_t nlen = strlen(ent->d_name);
            if (nlen >= 9 && strcmp(ent->d_name + nlen - 9, ".reserved") == 0) continue;
            if (nlen >= 5 && strcmp(ent->d_name + nlen - 5, ".lock")    == 0) continue;

            char fhash[33];
            if (compute_partial_hash(path, fhash) == 0 &&
                strcmp(fhash, hash) == 0)
                *result = strdup(path);
        }
    }
    closedir(d);
}

char *find_duplicate(const char *new_file, const char *root, const char *exclude) {
    char hash[33];
    if (compute_partial_hash(new_file, hash) != 0) return nullptr;
    printf("🔍 Проверяю дубликаты (хеш: %.8s...)...\n", hash);

    char *result = NULL;
    find_dup_recurse(root, hash, exclude ? exclude : new_file, &result);
    return result;
}

static void trash_walk(const char *dir, long max_bytes, int *count) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            trash_walk(path, max_bytes, count);
        } else if (S_ISREG(st.st_mode)) {
            size_t nlen = strlen(ent->d_name);
            if (nlen >= 9 && strcmp(ent->d_name + nlen - 9, ".reserved") == 0) continue;
            if (st.st_size < max_bytes) {
                printf("🗑️  Мусор: %s (%.1f КБ)\n",
                       ent->d_name, (double)st.st_size / 1024.0);
                remove(path);
                (*count)++;
            }
        }
    }
    closedir(d);
}

int cleanup_trash(const char *root, int max_mb) {
    int count = 0;
    trash_walk(root, (long)max_mb * 1024 * 1024, &count);
    return count;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in  = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return -1; }

    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }

    fclose(in); fclose(out);
    if (!ok) remove(dst);
    return ok ? 0 : -1;
}

int safe_move(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return 0;
    if (errno != EXDEV) return -1;
    if (copy_file(src, dst) != 0) return -1;
    remove(src);
    return 0;
}
