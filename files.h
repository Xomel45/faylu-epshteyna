#pragma once
#include <stddef.h>

#define SECRET_PATH        "/run/media/xomel45/SECRET"
#define PROXY              "http://127.0.0.1:10809"
#define MIN_FREE_SPACE_GB  1
#define MIN_FILE_SIZE_MB   35
#define TRASH_SIZE_MB      5
#define RESERVED_TIMEOUT_H 2
#define CHUNK_MB           10

long  get_free_space_gb   (const char *path);
[[nodiscard]] char *get_domain          (const char *url);   /* malloc'd, caller frees */
int   cleanup_old_reserved(const char *dir, int timeout_hours);
/* Returns reserved number (>=0) or -1 on error.
   Writes full path to .reserved file into reserved_out. */
[[nodiscard]] int   reserve_next_number (const char *dir, char *reserved_out, size_t sz);
[[nodiscard]] int   compute_partial_hash(const char *path, char out_hex[33]);
/* Returns malloc'd path to duplicate, or NULL. Caller frees. */
[[nodiscard]] char *find_duplicate      (const char *new_file, const char *root,
                                          const char *exclude);
int   cleanup_trash       (const char *root, int max_mb);
[[nodiscard]] int   safe_move           (const char *src, const char *dst);
