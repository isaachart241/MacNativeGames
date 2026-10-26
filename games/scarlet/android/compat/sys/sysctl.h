/* Thor port: no sysctl by name; callers fall back to defaults. */
#pragma once
#include <errno.h>
#include <stddef.h>
static inline int sysctlbyname(const char *n, void *o, size_t *ol, void *nv, size_t nl) { (void)n; (void)o; (void)ol; (void)nv; (void)nl; errno = ENOENT; return -1; }
