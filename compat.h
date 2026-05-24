#pragma once

#ifdef _WIN32
#  include <direct.h>
#  include <limits.h>
#  include <string.h>
#  include <stdlib.h>
#  ifndef PATH_MAX
#    define PATH_MAX 4096
#  endif
#  define mkdir(path, mode) _mkdir(path)
static inline char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *p = (char *)malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}
#endif
