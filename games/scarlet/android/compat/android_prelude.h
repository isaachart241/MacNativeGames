/* Thor port: force-included into every Android translation unit (-include). Darwin extensions to pthread and the clock
   that are used without a header of their own. Defined after <pthread.h> so Bionic's own prototypes are untouched. */
#pragma once
#ifdef __ANDROID__
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/* clock_gettime_nsec_np and the Darwin clock ids (uptime clocks map to CLOCK_MONOTONIC, which excludes suspend as they do) */
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define CLOCK_UPTIME_RAW_APPROX CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC_COARSE
#endif
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; if (clock_gettime(c, &t)) return 0; return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

/* pthread_threadid_np(NULL or a thread, &id): Darwin accepts NULL for the calling thread */
int dm_pthread_threadid_np(pthread_t t, uint64_t *id);
#define pthread_threadid_np(t, id) dm_pthread_threadid_np((pthread_t)(uintptr_t)(t), (id))
static inline pthread_t pthread_from_mach_thread_np(unsigned port) { (void)port; return 0; }

/* pthread_setname_np: Darwin names the calling thread with one argument, Linux takes the thread first */
static inline int dm_setname1(const char *n) { return pthread_setname_np(pthread_self(), n); }
static inline int dm_setname2(pthread_t t, const char *n) { return pthread_setname_np(t, n); }
#define DM_SETNAME_PICK(_1, _2, f, ...) f
#define pthread_setname_np(...) DM_SETNAME_PICK(__VA_ARGS__, dm_setname2, dm_setname1, 0)(__VA_ARGS__)

/* QoS on thread attributes: no equivalent; accepted and ignored */
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *a, int q, int rel) { (void)a; (void)q; (void)rel; return 0; }

/* struct stat time fields: Darwin st_mtimespec / st_ctimespec are Linux st_mtim / st_ctim */
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
#define st_atimespec st_atim
#endif
