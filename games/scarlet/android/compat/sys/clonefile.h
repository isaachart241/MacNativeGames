/* Thor port: no copy-on-write file clones; callers fall back to copying. */
#pragma once
#include <errno.h>
static inline int clonefile(const char *src, const char *dst, int flags) { (void)src; (void)dst; (void)flags; errno = ENOTSUP; return -1; }
