/* Thor port: the Android counterpart of frontend.m (same exported functions), for both Android builds:
   - the command-line program (stages T1, T2: run over adb, TOTK_HEADLESS=1): no window, boot progress to stderr, the
     graphics layer renders offscreen (screenshots: TOTK_VK_SHOT=<dir>);
   - the APK (stage T3, SCARLET_APK): runtime/activity_android.c hands over the activity's window, which the Vulkan
     backend presents to, and its input events (runtime/gamepad_android.c).
   In both, the VI display hands the graphics layer a placeholder window, and a 60 Hz thread signals the game's vsync
   events (CVDisplayLink on the Mac). Text the game asks for comes from SCARLET_TEXT (default "Thor"), so the
   trainer-name prompt can be answered from a test script. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"
#ifdef SCARLET_APK
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#endif

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
#ifdef SCARLET_APK
static _Atomic int focused = 1;
/* The activity's window focus (Home, recent apps, the notification shade): the game is told as when the Mac window
   loses focus (nn::oe message 15), held buttons are let go, and the sound is silent while out of focus. */
void runtime_android_focus(int on) {
    if (atomic_exchange(&focused, on) == on) return;
    if (!on) runtime_input_clear();
    runtime_oe_push_message(15);
    fprintf(stderr, "[totk] app %s\n", on ? "in focus" : "out of focus: sound off");
}
int runtime_front_focus(void) { return focused; }
#else
int runtime_front_focus(void) { return 1; }
#endif
int runtime_front_window_visible(void) { return 1; }
void runtime_shortcut_help_toggle(void) {}

#ifdef SCARLET_APK
/* exit() in an app runs the framework's static destructors while its threads still run (FORTIFY then aborts on a
   destroyed mutex, and Android reports a crash). The game's own exit work is the Vulkan pipeline cache (an atexit
   handler of the backend): saved here, then _exit. */
void vk_pcache_save(void) __attribute__((weak));
_Noreturn void runtime_android_exit(int code) { if (vk_pcache_save) vk_pcache_save(); fflush(NULL); _exit(code); }
#define front_exit runtime_android_exit
#else
#define front_exit exit
#endif

/* Closing (the game's own quit, or a script): wait up to 10 s for a save in progress, as the Mac window does. */
void runtime_front_request_close(void) {
    for (int i = 0; i < 1000 && runtime_fs_save_busy(); i++) usleep(10000);
    fprintf(stderr, "[totk] close requested: exiting\n");
    front_exit(0);
}

/* Text entry (nn::swkbd, runtime/swkbd_host.c). The app asks the player in a dialog (runtime/activity_android.c); with
   SCARLET_TEXT set (scripts), and in the command-line build, it is confirmed at once with SCARLET_TEXT, or the game's
   initial text, or "Thor". The caller checks the length and character limits again. */
#ifdef SCARLET_APK
int runtime_android_text(const RuntimeTextRequest *req, char *out, size_t out_size);
void runtime_android_text_test(const char *op, const char *text);
#endif
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    const char *t = getenv("SCARLET_TEXT");
#ifdef SCARLET_APK
    if (!t || !*t) return runtime_android_text(req, out, out_size);
#endif
    if (!t || !*t) t = req && req->initial && *req->initial ? req->initial : "Thor";
    if (req && req->digits_only && strspn(t, "0123456789") != strlen(t)) t = "0";
    snprintf(out, out_size, "%s", t);
    fprintf(stderr, "[totk] text entry \"%s\": answered \"%s\" (SCARLET_TEXT)\n", req && req->prompt ? req->prompt : "", out);
    return 1;
}
#ifdef SCARLET_APK
void runtime_front_text_test(const char *op, const char *text) { if (op) runtime_android_text_test(op, text); }
#else
void runtime_front_text_test(const char *op, const char *text) { (void)text; fprintf(stderr, "[totk] text entry test \"%s\" ignored: answered from SCARLET_TEXT here\n", op ? op : ""); }
#endif

/* The display's vsync: 60 Hz on an absolute clock, so late wakeups do not drift. */
/* In the app the ticks stop while the activity has no window (Home, recent apps, the screen off), and so do the game's
   presents (nvn_vk_present_hook, below): Scarlet does not pace on the vsync events alone, so its frame loop is held at
   its next present instead of rendering what nobody sees, and goes on when the window is back. */
#ifdef SCARLET_APK
static _Atomic int vsyncPaused = 1;   /* until the first window */
#else
static _Atomic int vsyncPaused;
#endif
static void *vsync_thread(void *arg) {
    (void)arg; pthread_setname_np("vsync");
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    for (;;) {
        t.tv_nsec += 16666667; if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
        if (atomic_load(&vsyncPaused)) continue;
        pthread_mutex_lock(&vsyncLock); for (unsigned i = 0; i < vsyncCount; i++) hle_signal_system_event(vsyncEvents[i]); pthread_mutex_unlock(&vsyncLock);
    }
    return NULL;
}
static void *guest_main(void *callback) { ((void (*)(void))callback)(); return NULL; }
void runtime_front_run(void (*callback)(void)) {
    bootStartNS = runtime_now_ns();
#ifdef SCARLET_APK
    fprintf(stderr, "[totk] Android app front end: the game is shown in the activity's window\n");
#else
    fprintf(stderr, "[totk] headless Android front end: no window, nothing is shown (Thor port stage T1)\n");
#endif
    runtime_gamepad_start();
    pthread_t v; if (pthread_create(&v, NULL, vsync_thread, NULL)) runtime_fail("vsync thread creation");
    pthread_t guest; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    int err = pthread_create(&guest, &attr, guest_main, (void *)callback); pthread_attr_destroy(&attr);
    if (err) runtime_fail("native game thread creation");
    pthread_join(guest, NULL);
    front_exit(0);
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

#ifdef SCARLET_APK
/* The activity's window (runtime/activity_android.c). The Vulkan backend makes its surface from it at a present when it
   has none; when the window goes away, the pointer is cleared first (no new surface from it), then the backend destroys
   its swapchain and surface (nvn_vk_window_lost waits for a present in progress), then the window is released. */
void nvn_vk_window_lost(void) __attribute__((weak));
static pthread_mutex_t windowLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t windowCond = PTHREAD_COND_INITIALIZER;
static ANativeWindow *window;
/* Called by the Vulkan backend at every present, before it takes its locks: waits while there is no window. */
void nvn_vk_present_hook(void) {
    if (!atomic_load(&vsyncPaused)) return;
    pthread_mutex_lock(&windowLock);
    while (atomic_load(&vsyncPaused)) pthread_cond_wait(&windowCond, &windowLock);
    pthread_mutex_unlock(&windowLock);
}
void runtime_android_window(ANativeWindow *w) {
    if (w) ANativeWindow_acquire(w);
    pthread_mutex_lock(&windowLock); ANativeWindow *old = window; window = w; pthread_mutex_unlock(&windowLock);
    if (!w) atomic_store(&vsyncPaused, 1);   /* no new frames for the old window */
    if (old) { if (nvn_vk_window_lost) nvn_vk_window_lost(); ANativeWindow_release(old); }
    if (w) {
        pthread_mutex_lock(&windowLock); atomic_store(&vsyncPaused, 0); pthread_cond_broadcast(&windowCond); pthread_mutex_unlock(&windowLock);
        fprintf(stderr, "[totk] window %dx%d\n", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
    }
    else fprintf(stderr, "[totk] window gone: the game is paused until it is back\n");
}
const char *const *front_vk_instance_extensions(uint32_t *count) {
    static const char *const ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    if (count) *count = 2; return ext;
}
uint64_t front_vk_surface(void *vk_instance) {
    VkSurfaceKHR s = VK_NULL_HANDLE;
    pthread_mutex_lock(&windowLock);
    if (window) {
        VkAndroidSurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, window};
        VkResult r = vkCreateAndroidSurfaceKHR((VkInstance)vk_instance, &ci, NULL, &s);
        if (r != VK_SUCCESS) { fprintf(stderr, "[totk] vkCreateAndroidSurfaceKHR: %d\n", r); s = VK_NULL_HANDLE; }
    }
    pthread_mutex_unlock(&windowLock);
    return (uint64_t)s;
}
#else
/* Vulkan (android/Makefile GFX=vk): no window, so the backend renders offscreen (screenshots: TOTK_VK_SHOT=<dir>). */
const char *const *front_vk_instance_extensions(uint32_t *count) { if (count) *count = 0; return NULL; }
uint64_t front_vk_surface(void *vk_instance) { (void)vk_instance; return 0; }
#endif

/* Probes that drive the Metal path (gpu_path_probe.m, vpool_probe.m): not available without a GPU backend. */
void runtime_gpu_path_probe(const char *which) { fprintf(stderr, "[totk] GPU path probe %s: needs the Metal backend\n", which ? which : ""); exit(2); }
void runtime_vpool_probe(void) { fprintf(stderr, "[totk] vpool probe: needs the Metal backend\n"); exit(2); }
