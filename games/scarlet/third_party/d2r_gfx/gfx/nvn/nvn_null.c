// Null replay backend of the NVN layer: the mtl_* seam of nvn_mtl.h / nvn_obj.h with no GPU (headless bring-up,
// docs/windows-port-plan.md C14). Drop-in alternative to nvn_mtl.m (Metal) and nvn_vk*.c (Vulkan): link exactly one.
// What the GPU would make visible to the CPU still happens, at once, when a submission is replayed: event writes,
// counter reports (occlusion queries report "visible"), buffer copies and uniform updates. Every submission completes
// immediately (fences are signaled when committed). Nothing is drawn; presents are counted and time-stamped.
// Scarlet: taken from the TotK tree for the Thor port's first stage (android/, T1: the game runs on the device with no
// picture). Frame pacing stays in hle_nvn.c (its own 60 Hz clock at acquire), so presents return at once. Transform
// feedback records (OP_TF_*) are GPU-only like the other state records.
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <time.h>

static _Atomic uint64_t g_fence, g_presents;
static double g_present_time[64];   // seconds (monotonic) of the last presents, by ticket % 64

static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t gpu_ticks(void) { return mono_ns() * 12 / 625; }   // the 19.2 MHz GPU timer the game expects

void mtl_init(void) { LOG("null graphics backend: nothing is drawn (headless)"); }
void bf_want_note(const char *key) { (void)key; }

// ---------- memory pools: GPU address = CPU address, nothing to mirror ----------
void mtl_pool_add(const void *mem, uint64_t size) { (void)mem; (void)size; }
void mtl_pool_del(const void *mem) { (void)mem; }
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) { (void)p; (void)off; (void)n; }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { (void)len; if (buf) *buf = NULL; if (off) *off = addr; return 0; }

// ---------- submissions ----------
void mtl_sub_begin(void) {}
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { (void)h; (void)n; return NULL; }
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap) { (void)h; (void)n; free(cap); }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { if (n && src != dst) memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { if (mode == 1) *val += v; else *val = v; }
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    // NVNcounterData = {counter, timestamp}; samples passed reports "visible" (no occlusion culling without a GPU).
    uint64_t rec[2] = {type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1u << 20 : 0, gpu_ticks()};
    memcpy((void *)(uintptr_t)addr, rec, sizeof rec);
}
uint64_t mtl_sub_commit(void) { return atomic_fetch_add(&g_fence, 1) + 1; }
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}

// The command records hle_nvn.c writes (nvn_obj.h): only the CPU-visible side effects are carried out.
void mtl_exec(const uint8_t *p) {
    for (int guard = 0; guard < 50000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("null backend: corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)(uintptr_t)t); break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
        case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (n) memcpy((void *)(uintptr_t)d, a + 16, n); break; }
        default: break;   // state, binds, draws, clears, texture copies: GPU-only
        }
        p += h.size;
    }
    die("null backend: command list does not end");
}
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }

// ---------- present ----------
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)period_s; (void)crop;
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    g_present_time[n % 64] = mono_ns() / 1e9;
    if (n == 1 || n % 600 == 0) LOG("null backend: present %llu", (unsigned long long)n);
    return n;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_flush(void) {}
void mtl_present_test_stall(uint64_t ns) { (void)ns; }
double mtl_presented_time(uint64_t n) { return n && n <= atomic_load(&g_presents) && atomic_load(&g_presents) - n < 64 ? g_present_time[n % 64] : 0; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_display_hz(void) { return 0; }
int mtl_has_window(void) { return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_shot_at_present(const tex_t *t) { (void)t; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns, unsigned frame_ticks) { (void)timeout_ns; (void)frame_ticks; return 0; }
void mtl_note_world(void) {}

// ---------- textures, samplers, programs ----------
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }
void mtl_probe_pin_rt(const tex_t *t) { (void)t; }
void mtl_pin_refs(void (*mark)(const void *)) { (void)mark; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)r; (void)dst; (void)row; (void)img;
    static int said; if (!said++) LOG("null backend: ReadTexels returns the destination unchanged");
}
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)r; (void)src; (void)row; (void)img;
}
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy) { (void)on; (void)px; (void)py; (void)grid; (void)cx; (void)cy; }
int mtl_ui_ghost_rect(float r[4]) { (void)r; return 0; }

// ---------- statistics (all zero) ----------
void mtl_res_counts(uint32_t tick, const char *why) { (void)tick; (void)why; }
void mtl_res_snapshot(mtl_res_count *out) { memset(out, 0, sizeof *out); }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = *draws = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes) { *load = *store = *full_rw_passes = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
void mtl_upload_wait_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { *a = *b = 0; *c = 0; }
void mtl_blit_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_ren_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_layer_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
void mtl_keepalive_stats(uint64_t v[3]) { memset(v, 0, 3 * sizeof *v); }
void mtl_pool_res_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
uint64_t mtl_chunk_commits(void) { return 0; }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb) { (void)frame; (void)mb; }

// ---------- other hooks the NVN layer expects from the Metal side ----------
uint64_t g_draw_skip[16];
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
int mfx_enabled(void) { return 0; }
int mfx_latch(void) { return 0; }
void mfx_set(int mode) { (void)mode; }
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }

// ---------- Scarlet: diagnostics the Metal backend serves (screenshots, dumps, flags): nothing to capture here ----------
void dm_gfx_anomdump(void) {}
void dm_gfx_gpucapture_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_set_flag(const char *name, int v) { LOG("null backend: flag %s=%d ignored (no picture)", name ? name : "?", v); }
void dm_gfx_shot_at(long frames, const char *path) { (void)frames; LOG("null backend: no screenshot for %s (no picture)", path ? path : "?"); }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
void dm_nvn_probe_prediction(void) {}
