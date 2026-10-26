/* Thor port, stage T1: the headless Android counterpart of frontend.m (same exported functions). There is no window yet:
   boot progress goes to stderr, the VI display hands the graphics layer a placeholder window, and a 60 Hz thread signals
   the game's vsync events (CVDisplayLink on the Mac). Text the game asks for comes from SCARLET_TEXT (default "Thor"),
   so the trainer-name prompt can be answered from a test script. The APK stage (T4) replaces this file with the
   SurfaceView / input side. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"

static uint64_t bootStartNS;
static pthread_mutex_t vsyncLock = PTHREAD_MUTEX_INITIALIZER;
static void *vsyncEvents[16];
static unsigned vsyncCount;

static double boot_s(void) { return bootStartNS ? (runtime_now_ns() - bootStartNS) / 1e9 : 0; }
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    static const char *last; static uint64_t last_pct = ~0ull;
    uint64_t pct = total ? done * 100 / total : 0;
    if (stage != last || (total && pct / 10 != last_pct / 10)) {
        if (total) fprintf(stderr, "[totk-boot] %.3f %s %.1f / %.1f GB\n", boot_s(), stage ? stage : "startup", done / 1e9, total / 1e9);
        else fprintf(stderr, "[totk-boot] %.3f %s\n", boot_s(), stage ? stage : "startup");
        last = stage; last_pct = pct;
    }
}
void runtime_boot_count(const char *stage, uint64_t done, uint64_t total) {
    static const char *last;
    if (stage != last || done == total) { fprintf(stderr, "[totk-boot] %.3f %s %llu / %llu\n", boot_s(), stage ? stage : "startup", (unsigned long long)done, (unsigned long long)total); last = stage; }
}
void runtime_boot_status(const char *stage, const char *detail) {
    fprintf(stderr, "[totk-boot] %.3f %s — %s\n", boot_s(), stage ? stage : "startup", detail ? detail : "");
}
void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders, uint64_t shader_total, uint64_t pipelines, uint64_t pipeline_total) {
    (void)done; (void)total; (void)shaders; (void)shader_total; (void)pipelines; (void)pipeline_total;
}
void runtime_warm_done(void) {}
uint64_t front_game_res(int docked) { return docked ? (1080ULL << 32) | 1920 : (720ULL << 32) | 1280; }
void front_frame_presented(void) {}
void *front_metal_layer(void) { return NULL; }
int runtime_front_focus(void) { return 1; }
int runtime_front_window_visible(void) { return 1; }
void runtime_shortcut_help_toggle(void) {}

/* Closing (the game's own quit, or a script): wait up to 10 s for a save in progress, as the Mac window does. */
void runtime_front_request_close(void) {
    for (int i = 0; i < 1000 && runtime_fs_save_busy(); i++) usleep(10000);
    fprintf(stderr, "[totk] close requested: exiting\n");
    exit(0);
}

/* Text entry (nn::swkbd, runtime/swkbd_host.c): confirmed at once with SCARLET_TEXT, or the game's initial text, or "Thor".
   The caller checks the length and character limits again. */
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    const char *t = getenv("SCARLET_TEXT");
    if (!t || !*t) t = req && req->initial && *req->initial ? req->initial : "Thor";
    if (req && req->digits_only && strspn(t, "0123456789") != strlen(t)) t = "0";
    snprintf(out, out_size, "%s", t);
    fprintf(stderr, "[totk] text entry \"%s\": answered \"%s\" (SCARLET_TEXT)\n", req && req->prompt ? req->prompt : "", out);
    return 1;
}
void runtime_front_text_test(const char *op, const char *text) { (void)text; fprintf(stderr, "[totk] text entry test \"%s\" ignored: answered from SCARLET_TEXT here\n", op ? op : ""); }

/* The display's vsync: 60 Hz on an absolute clock, so late wakeups do not drift. */
static void *vsync_thread(void *arg) {
    (void)arg; pthread_setname_np("vsync");
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    for (;;) {
        t.tv_nsec += 16666667; if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
        pthread_mutex_lock(&vsyncLock); for (unsigned i = 0; i < vsyncCount; i++) hle_signal_system_event(vsyncEvents[i]); pthread_mutex_unlock(&vsyncLock);
    }
    return NULL;
}
static void *guest_main(void *callback) { ((void (*)(void))callback)(); return NULL; }
void runtime_front_run(void (*callback)(void)) {
    bootStartNS = runtime_now_ns();
    fprintf(stderr, "[totk] headless Android front end: no window, nothing is shown (Thor port stage T1)\n");
    pthread_t v; if (pthread_create(&v, NULL, vsync_thread, NULL)) runtime_fail("vsync thread creation");
    pthread_t guest; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    int err = pthread_create(&guest, &attr, guest_main, (void *)callback); pthread_attr_destroy(&attr);
    if (err) runtime_fail("native game thread creation");
    pthread_join(guest, NULL);
    exit(0);
}

static int viReady;
static unsigned displayIdentity, layerIdentity, windowIdentity;
static void vi_initialize(void) { viReady = 1; fprintf(stderr, "[totk] headless VI display initialized\n"); }
static uint32_t vi_open(void **out) { if (!viReady || !out) return 0xf601; *out = &displayIdentity; return 0; }
static uint32_t vi_layer(void **out, void *display) { if (!out || display != &displayIdentity) return 0xf601; *out = &layerIdentity; return 0; }
static uint32_t vi_window(void **out, void *layer) { if (!out || layer != &layerIdentity) return 0xf601; *out = &windowIdentity; return 0; }
static uint32_t vi_vsync(void *ev, void *display) {
    if (!ev || display != &displayIdentity) return 0xf601; hle_bind_system_event(ev, 1);
    pthread_mutex_lock(&vsyncLock); if (vsyncCount >= 16) runtime_fail("VI display event limit"); vsyncEvents[vsyncCount++] = ev; pthread_mutex_unlock(&vsyncLock); return 0;
}
void *runtime_front_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2vi10InitializeEv", vi_initialize}, {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_layer}, {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_window},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

/* Vulkan (android/Makefile GFX=vk): no window yet, so the backend renders offscreen (screenshots: TOTK_VK_SHOT=<dir>). */
const char *const *front_vk_instance_extensions(uint32_t *count) { if (count) *count = 0; return NULL; }
uint64_t front_vk_surface(void *vk_instance) { (void)vk_instance; return 0; }

/* Probes that drive the Metal path (gpu_path_probe.m, vpool_probe.m): not available without a GPU backend. */
void runtime_gpu_path_probe(const char *which) { fprintf(stderr, "[totk] GPU path probe %s: needs the Metal backend\n", which ? which : ""); exit(2); }
void runtime_vpool_probe(void) { fprintf(stderr, "[totk] vpool probe: needs the Metal backend\n"); exit(2); }
