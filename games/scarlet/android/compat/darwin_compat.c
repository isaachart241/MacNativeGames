/* Thor port: implementations behind darwin_compat.h (see there). */
#define _GNU_SOURCE
#include "darwin_compat.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

vm_size_t vm_page_size = 4096;
__attribute__((constructor)) static void page_size_init(void) { vm_page_size = (vm_size_t)sysconf(_SC_PAGESIZE); }

mach_port_t mach_thread_self(void) { return (mach_port_t)gettid(); }

static int prot_of(vm_prot_t p) { return (p & VM_PROT_READ ? PROT_READ : 0) | (p & VM_PROT_WRITE ? PROT_WRITE : 0) | (p & VM_PROT_EXECUTE ? PROT_EXEC : 0); }

/* VM_FLAGS_ANYWHERE: the kernel picks; FIXED: exactly there, failing if anything is mapped (Mach semantics) unless
   OVERWRITE, which replaces what is there. Fresh memory reads as zero, as on Mach. */
static kern_return_t map_anon(mach_vm_address_t *addr, mach_vm_size_t size, int flags, int prot) {
    if (!addr || !size) return KERN_INVALID_ARGUMENT;
    int mf = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE; void *want = NULL;
    if (!(flags & VM_FLAGS_ANYWHERE)) { want = (void *)(uintptr_t)*addr; mf |= flags & VM_FLAGS_OVERWRITE ? MAP_FIXED : MAP_FIXED_NOREPLACE; }
    void *p = mmap(want, (size_t)size, prot, mf, -1, 0);
    if (p == MAP_FAILED) return errno == EEXIST ? KERN_NO_SPACE : KERN_NO_SPACE;
    if (want && p != want) { munmap(p, (size_t)size); return KERN_NO_SPACE; }   /* kernels without MAP_FIXED_NOREPLACE treat it as a hint */
    *addr = (mach_vm_address_t)(uintptr_t)p; return KERN_SUCCESS;
}
kern_return_t mach_vm_allocate(vm_map_t task, mach_vm_address_t *addr, mach_vm_size_t size, int flags) {
    (void)task; return map_anon(addr, size, flags, PROT_READ | PROT_WRITE);
}
kern_return_t mach_vm_deallocate(vm_map_t task, mach_vm_address_t addr, mach_vm_size_t size) {
    (void)task; return size && munmap((void *)(uintptr_t)addr, (size_t)size) ? KERN_INVALID_ADDRESS : KERN_SUCCESS;
}
kern_return_t vm_deallocate(vm_map_t task, vm_address_t addr, vm_size_t size) { return mach_vm_deallocate(task, addr, size); }
kern_return_t mach_vm_protect(vm_map_t task, mach_vm_address_t addr, mach_vm_size_t size, boolean_t set_max, vm_prot_t prot) {
    (void)task; if (set_max) return KERN_SUCCESS;
    return mprotect((void *)(uintptr_t)addr, (size_t)size, prot_of(prot)) ? KERN_PROTECTION_FAILURE : KERN_SUCCESS;
}
kern_return_t mach_vm_map(vm_map_t task, mach_vm_address_t *addr, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                          memory_object_t object, uint64_t offset, boolean_t copy, vm_prot_t cur, vm_prot_t max, vm_inherit_t inherit) {
    (void)task; (void)mask; (void)offset; (void)copy; (void)max; (void)inherit;
    if (object != MEMORY_OBJECT_NULL) return KERN_NOT_SUPPORTED;
    return map_anon(addr, size, flags, prot_of(cur));
}
/* One range onto another without copying needs shared (memfd) backing chosen when the source was made; anonymous
   private memory cannot be aliased afterwards. The callers (virtual pools, iOS embedding) handle the failure. */
kern_return_t mach_vm_remap(vm_map_t task, mach_vm_address_t *target, mach_vm_size_t size, mach_vm_offset_t mask, int flags, vm_map_t src_task,
                            mach_vm_address_t src, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit) {
    (void)task; (void)target; (void)size; (void)mask; (void)flags; (void)src_task; (void)src; (void)copy; (void)cur; (void)max; (void)inherit;
    return KERN_NOT_SUPPORTED;
}
kern_return_t vm_remap(vm_map_t task, vm_address_t *target, vm_size_t size, vm_address_t mask, int flags, vm_map_t src_task,
                       vm_address_t src, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit) {
    (void)task; (void)target; (void)size; (void)mask; (void)flags; (void)src_task; (void)src; (void)copy; (void)cur; (void)max; (void)inherit;
    return KERN_NOT_SUPPORTED;
}
kern_return_t mach_vm_read_overwrite(vm_map_t task, mach_vm_address_t addr, mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *out) {
    (void)task;
    struct iovec local = { (void *)(uintptr_t)data, (size_t)size }, remote = { (void *)(uintptr_t)addr, (size_t)size };
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (n != (ssize_t)size) { if (out) *out = 0; return KERN_INVALID_ADDRESS; }
    if (out) *out = size; return KERN_SUCCESS;
}
kern_return_t vm_read_overwrite(vm_map_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out) {
    mach_vm_size_t n = 0; kern_return_t r = mach_vm_read_overwrite(task, addr, size, data, &n); if (out) *out = (vm_size_t)n; return r;
}

/* /proc/self/maps: the first mapping ending above *addr */
static int region_at(uint64_t at, uint64_t *lo, uint64_t *hi, vm_prot_t *prot, int *shared) {
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return 0;
    char line[512]; int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, b; char perm[8] = "";
        if (sscanf(line, "%llx-%llx %7s", &a, &b, perm) != 3 || b <= at) continue;
        *lo = a; *hi = b; *prot = (perm[0] == 'r' ? VM_PROT_READ : 0) | (perm[1] == 'w' ? VM_PROT_WRITE : 0) | (perm[2] == 'x' ? VM_PROT_EXECUTE : 0);
        *shared = perm[3] == 's'; found = 1; break;
    }
    fclose(f); return found;
}
kern_return_t mach_vm_region(vm_map_t task, mach_vm_address_t *addr, mach_vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info,
                             mach_msg_type_number_t *count, mach_port_t *object) {
    (void)task; if (object) *object = MACH_PORT_NULL;
    uint64_t lo, hi; vm_prot_t prot; int shared;
    if (!addr || !size || !info || !region_at(*addr, &lo, &hi, &prot, &shared)) return KERN_INVALID_ADDRESS;
    *addr = lo; *size = hi - lo;
    if (flavor == VM_REGION_BASIC_INFO_64) {
        vm_region_basic_info_data_64_t *b = (void *)info; memset(b, 0, sizeof *b); b->protection = prot; b->max_protection = VM_PROT_ALL; b->shared = shared;
        if (count) *count = VM_REGION_BASIC_INFO_COUNT_64;
    } else if (flavor == VM_REGION_EXTENDED_INFO) {
        vm_region_extended_info_data_t *x = (void *)info; memset(x, 0, sizeof *x); x->protection = prot; x->share_mode = shared ? 3 : 1;
        if (count) *count = VM_REGION_EXTENDED_INFO_COUNT;
    } else return KERN_INVALID_ARGUMENT;
    return KERN_SUCCESS;
}
kern_return_t vm_region_recurse_64(vm_map_t task, vm_address_t *addr, vm_size_t *size, natural_t *depth, vm_region_recurse_info_t info,
                                   mach_msg_type_number_t *count) {
    (void)task; (void)depth;
    uint64_t lo, hi; vm_prot_t prot; int shared;
    if (!addr || !size || !info || !region_at(*addr, &lo, &hi, &prot, &shared)) return KERN_INVALID_ADDRESS;
    *addr = (vm_address_t)lo; *size = (vm_size_t)(hi - lo);
    vm_region_submap_info_data_64_t *s = (void *)info; memset(s, 0, sizeof *s); s->protection = prot; s->max_protection = VM_PROT_ALL; s->share_mode = shared ? 3 : 1;
    if (count) *count = VM_REGION_SUBMAP_INFO_COUNT_64;
    return KERN_SUCCESS;
}

kern_return_t mach_wait_until(uint64_t deadline) {
    struct timespec t = { (time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
    return KERN_SUCCESS;
}

/* THREAD_BASIC_INFO for the calling thread only (the runtime asks about itself); others fail. */
kern_return_t thread_info(thread_act_t thread, thread_flavor_t flavor, thread_info_t info, mach_msg_type_number_t *count) {
    if (thread != mach_thread_self() || !info) return KERN_FAILURE;
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    if (flavor == THREAD_BASIC_INFO) {
        thread_basic_info_data_t *b = (void *)info; memset(b, 0, sizeof *b);
        b->user_time.seconds = (int)t.tv_sec; b->user_time.microseconds = (int)(t.tv_nsec / 1000);
        if (count) *count = THREAD_BASIC_INFO_COUNT; return KERN_SUCCESS;
    }
    if (flavor == THREAD_IDENTIFIER_INFO) {
        thread_identifier_info_data_t *i = (void *)info; memset(i, 0, sizeof *i); i->thread_id = (uint64_t)gettid();
        if (count) *count = THREAD_IDENTIFIER_INFO_COUNT; return KERN_SUCCESS;
    }
    if (flavor == THREAD_EXTENDED_INFO) {
        thread_extended_info_data_t *x = (void *)info; memset(x, 0, sizeof *x);
        x->pth_user_time = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; pthread_getname_np(pthread_self(), x->pth_name, sizeof x->pth_name);
        if (count) *count = THREAD_EXTENDED_INFO_COUNT; return KERN_SUCCESS;
    }
    return KERN_FAILURE;
}

/* TASK_VM_INFO: resident size from /proc/self/statm stands in for the physical footprint */
kern_return_t task_info(task_t task, task_flavor_t flavor, task_info_t info, mach_msg_type_number_t *count) {
    (void)task; if (flavor != TASK_VM_INFO || !info) return KERN_FAILURE;
    task_vm_info_data_t *v = (void *)info; memset(v, 0, sizeof *v);
    FILE *f = fopen("/proc/self/statm", "re"); unsigned long long size = 0, res = 0;
    if (f) { if (fscanf(f, "%llu %llu", &size, &res) != 2) size = res = 0; fclose(f); }
    v->virtual_size = size * vm_page_size; v->resident_size = v->phys_footprint = v->internal = res * vm_page_size; v->page_size = (integer_t)vm_page_size;
    if (count) *count = TASK_VM_INFO_COUNT; return KERN_SUCCESS;
}

int dm_pthread_threadid_np(pthread_t t, uint64_t *id) {   /* android_prelude.h: pthread_threadid_np */
    if (!id) return EINVAL;
    *id = (uint64_t)(!t || pthread_equal(t, pthread_self()) ? gettid() : pthread_gettid_np(t)); return 0;
}
static void stack_of(pthread_t t, void **lo, size_t *size) {
    pthread_attr_t a; *lo = NULL; *size = 0;
    if (pthread_getattr_np(t, &a)) return;
    pthread_attr_getstack(&a, lo, size); pthread_attr_destroy(&a);
}
size_t pthread_get_stacksize_np(pthread_t t) { void *lo; size_t s; stack_of(t, &lo, &s); return s; }
void *pthread_get_stackaddr_np(pthread_t t) { void *lo; size_t s; stack_of(t, &lo, &s); return lo ? (char *)lo + s : NULL; }   /* Darwin: the top */
