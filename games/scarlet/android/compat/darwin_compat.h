/* Thor port (THOR_SCOPE.md T1): the subset of Mach / Darwin the runtime and graphics C code use, on Android (Bionic).
   Address space: mmap / munmap / mprotect and /proc/self/maps; time: CLOCK_MONOTONIC in nanoseconds (timebase 1/1);
   thread and task figures from clock_gettime and /proc. What Linux cannot do the Mach way (remapping one range onto
   another, suspending another thread, hardware counters) returns KERN_FAILURE, and every caller already handles a
   failure. Included by the shim headers under android/compat/ (mach/mach.h and friends); never on Apple platforms. */
#pragma once
#ifndef __ANDROID__
#error "darwin_compat.h is for the Android build only"
#endif
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>
#include <malloc.h>

typedef int kern_return_t;
typedef unsigned int mach_port_t, task_t, thread_t, host_t, vm_map_t, thread_act_t;
typedef thread_act_t *thread_act_array_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t, mach_vm_offset_t;
typedef uintptr_t vm_address_t, vm_offset_t;
typedef size_t vm_size_t;
typedef int vm_prot_t, vm_inherit_t, boolean_t, integer_t;
typedef unsigned int natural_t, mach_msg_type_number_t;
typedef int *vm_region_info_t, *vm_region_recurse_info_t, *thread_info_t, *task_info_t, *thread_policy_t, *processor_info_array_t;
typedef int thread_flavor_t, task_flavor_t, vm_region_flavor_t, thread_policy_flavor_t, processor_flavor_t;
typedef uintptr_t memory_object_t;

enum { KERN_SUCCESS = 0, KERN_INVALID_ADDRESS = 1, KERN_PROTECTION_FAILURE = 2, KERN_NO_SPACE = 3, KERN_INVALID_ARGUMENT = 4,
       KERN_FAILURE = 5, KERN_NOT_SUPPORTED = 46 };
#define MACH_PORT_NULL 0u
#define MEMORY_OBJECT_NULL ((memory_object_t)0)
#define TRUE 1
#define FALSE 0
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_DEFAULT (VM_PROT_READ | VM_PROT_WRITE)
#define VM_PROT_ALL (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)
#define VM_FLAGS_FIXED 0x0
#define VM_FLAGS_ANYWHERE 0x1
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_SHARE 0
#define VM_INHERIT_COPY 1
#define VM_INHERIT_NONE 2
#define VM_INHERIT_DEFAULT VM_INHERIT_COPY
extern vm_size_t vm_page_size;

static inline mach_port_t mach_task_self(void) { return 1; }
static inline mach_port_t mach_host_self(void) { return 1; }
mach_port_t mach_thread_self(void);
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) { (void)task; (void)name; return KERN_SUCCESS; }

kern_return_t mach_vm_allocate(vm_map_t task, mach_vm_address_t *addr, mach_vm_size_t size, int flags);
kern_return_t mach_vm_deallocate(vm_map_t task, mach_vm_address_t addr, mach_vm_size_t size);
kern_return_t mach_vm_protect(vm_map_t task, mach_vm_address_t addr, mach_vm_size_t size, boolean_t set_max, vm_prot_t prot);
kern_return_t mach_vm_map(vm_map_t task, mach_vm_address_t *addr, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                          memory_object_t object, uint64_t offset, boolean_t copy, vm_prot_t cur, vm_prot_t max, vm_inherit_t inherit);
kern_return_t mach_vm_remap(vm_map_t task, mach_vm_address_t *target, mach_vm_size_t size, mach_vm_offset_t mask, int flags, vm_map_t src_task,
                            mach_vm_address_t src, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit);
kern_return_t vm_remap(vm_map_t task, vm_address_t *target, vm_size_t size, vm_address_t mask, int flags, vm_map_t src_task,
                       vm_address_t src, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit);
kern_return_t vm_deallocate(vm_map_t task, vm_address_t addr, vm_size_t size);
/* Copies size bytes from addr unless part of the range is unreadable (process_vm_readv on this process). */
kern_return_t mach_vm_read_overwrite(vm_map_t task, mach_vm_address_t addr, mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *out);
kern_return_t vm_read_overwrite(vm_map_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out);

/* mach_vm_region: the first mapping that ends above *addr (from /proc/self/maps); *addr and *size describe it. */
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_EXTENDED_INFO 13
typedef struct { vm_prot_t protection, max_protection; vm_inherit_t inheritance; boolean_t shared, reserved; uint64_t offset; int behavior;
                 unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
typedef struct { vm_prot_t protection; unsigned user_tag, pages_resident, pages_shared_now_private, pages_swapped_out, pages_dirtied,
                 ref_count, shadow_depth; unsigned char external_pager, share_mode; unsigned pages_reusable; } vm_region_extended_info_data_t;
typedef struct { vm_prot_t protection, max_protection; vm_inherit_t inheritance; uint64_t offset; unsigned user_tag, pages_resident,
                 pages_shared_now_private, pages_swapped_out, pages_dirtied, ref_count; unsigned short shadow_depth; unsigned char external_pager,
                 share_mode; boolean_t is_submap; int behavior; uint32_t object_id; unsigned short user_wired_count; unsigned pages_reusable;
                 uint64_t object_id_full; } vm_region_submap_info_data_64_t;
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
#define VM_REGION_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(vm_region_extended_info_data_t) / sizeof(int)))
#define VM_REGION_SUBMAP_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_submap_info_data_64_t) / sizeof(int)))
kern_return_t mach_vm_region(vm_map_t task, mach_vm_address_t *addr, mach_vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info,
                             mach_msg_type_number_t *count, mach_port_t *object);
kern_return_t vm_region_recurse_64(vm_map_t task, vm_address_t *addr, vm_size_t *size, natural_t *depth, vm_region_recurse_info_t info,
                                   mach_msg_type_number_t *count);

/* time: nanoseconds, timebase 1/1 */
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t, *mach_timebase_info_t;
static inline uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline uint64_t mach_continuous_time(void) { struct timespec t; clock_gettime(CLOCK_BOOTTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline kern_return_t mach_timebase_info(mach_timebase_info_t i) { i->numer = 1; i->denom = 1; return KERN_SUCCESS; }
kern_return_t mach_wait_until(uint64_t deadline);

/* threads and tasks */
typedef struct { int seconds, microseconds; } time_value_t;
#define THREAD_BASIC_INFO 3
#define THREAD_IDENTIFIER_INFO 4
#define THREAD_EXTENDED_INFO 5
typedef struct { time_value_t user_time, system_time; integer_t cpu_usage; int policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef struct { uint64_t thread_id, thread_handle, dispatch_qaddr; } thread_identifier_info_data_t;
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time,
                 pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(natural_t)))
#define THREAD_IDENTIFIER_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_identifier_info_data_t) / sizeof(natural_t)))
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(natural_t)))
kern_return_t thread_info(thread_act_t thread, thread_flavor_t flavor, thread_info_t info, mach_msg_type_number_t *count);
#define THREAD_TIME_CONSTRAINT_POLICY 2
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(integer_t)))
static inline kern_return_t thread_policy_set(thread_act_t t, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)f; (void)p; (void)n; return KERN_SUCCESS; }
static inline kern_return_t thread_policy_get(thread_act_t t, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *d) { (void)t; (void)f; (void)p; (void)n; if (d) *d = TRUE; return KERN_FAILURE; }
static inline kern_return_t thread_suspend(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_resume(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_get_state(thread_act_t t, int f, void *s, mach_msg_type_number_t *n) { (void)t; (void)f; (void)s; (void)n; return KERN_FAILURE; }
static inline kern_return_t task_threads(task_t t, thread_act_array_t *a, mach_msg_type_number_t *n) { (void)t; (void)a; if (n) *n = 0; return KERN_FAILURE; }
#define TASK_VM_INFO 22
typedef struct { mach_vm_size_t virtual_size; integer_t region_count; integer_t page_size; mach_vm_size_t resident_size, resident_size_peak, device,
                 device_peak, internal, internal_peak, external, external_peak, reusable, reusable_peak, purgeable_volatile_pmap,
                 purgeable_volatile_resident, purgeable_volatile_virtual, compressed, compressed_peak, compressed_lifetime, phys_footprint,
                 min_address, max_address; } task_vm_info_data_t;
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(natural_t)))
kern_return_t task_info(task_t task, task_flavor_t flavor, task_info_t info, mach_msg_type_number_t *count);
static inline kern_return_t host_processor_info(host_t h, processor_flavor_t f, natural_t *n, processor_info_array_t *a, mach_msg_type_number_t *c) { (void)h; (void)f; (void)n; (void)a; (void)c; return KERN_FAILURE; }
/* hardware instruction / cycle counters of the calling thread: not available to apps here */
struct thsc_cpi { uint64_t tcpi_instructions, tcpi_cycles; };
static inline int thread_selfcounts(int kind, void *buf, size_t size) { (void)kind; (void)buf; (void)size; return -1; }

/* pthread extensions */
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return mach_thread_self(); }
size_t pthread_get_stacksize_np(pthread_t t);
void *pthread_get_stackaddr_np(pthread_t t);

/* malloc, caches, Mach-O sections, processes */
static inline size_t malloc_size(const void *p) { return p ? malloc_usable_size((void *)p) : 0; }
static inline void sys_icache_invalidate(void *start, size_t len) { __builtin___clear_cache((char *)start, (char *)start + len); }
static inline void sys_dcache_flush(void *start, size_t len) { (void)start; (void)len; }
