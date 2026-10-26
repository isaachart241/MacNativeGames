/* The Thor's bottom screen (SCARLET_APK): while the game is in focus, a Presentation window on the presentation display
   (display 4, 1080x1240, where the user's launcher shows otherwise) with a map the player can drag and pinch-zoom; it
   snaps back to the player a few seconds after the last touch. Leaving the game dismisses it and the launcher is back.

   The app has no Java code, so everything goes through JNI on the UI thread: the Presentation, a SurfaceView as its
   content (its surface is polled, there being no callback object to receive surfaceCreated), and the window's input
   queue, which the activity itself takes (NativeActivity is an InputQueue.Callback): its touches arrive as a second
   AInputQueue in ANativeActivity's onInputQueueCreated and are read on the input thread. The window is not focusable,
   so touching it neither moves focus off the game (which would pause it) nor takes the pad's keys.

   Drawing is on a thread of its own, on the CPU into the window's buffers, and only when something changed (a touch,
   the snap-back, the player moving). For now the map is a grid around the origin with the player's arrow at
   (bottom_map_set_player): the game's map image and the player's position come next. */
#ifdef SCARLET_APK
#include <android/input.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int env_off(const char *n) { const char *e = getenv(n); return e && !strcmp(e, "0"); }   /* SCARLET_BOTTOM=0: off */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---------- view state (input thread and render thread, under viewLock) ---------- */
static pthread_mutex_t viewLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t viewCond = PTHREAD_COND_INITIALIZER;
static ANativeWindow *win;            /* the bottom surface while shown (render thread draws into it) */
static int winBusy, dirty = 1;        /* winBusy: the render thread is drawing into win */
#define SNAP_S 4.0
#define ZOOM_HOME 3.0   /* screen pixels per map unit (a metre): the bottom screen spans about 400 m */
static double camX, camY, zoom = ZOOM_HOME;   /* map position at the screen centre; screen pixels per map unit */
static double playerX, playerY, playerDir;   /* map units; radians, 0 = north (up), clockwise */
static double lastTouch = -100;       /* time of the last touch; the view follows the player SNAP_S after it */
static struct { int id; float x, y; } touch[2]; static int ntouch;
static double pinchDist, pinchZoom, dragX, dragY, dragCamX, dragCamY;

void bottom_map_set_player(double x, double y, double dir) {
    pthread_mutex_lock(&viewLock);
    if (x != playerX || y != playerY || dir != playerDir) { playerX = x; playerY = y; playerDir = dir; dirty = 1; pthread_cond_broadcast(&viewCond); }
    pthread_mutex_unlock(&viewLock);
}

/* Touches (input thread). One finger drags the map, two pinch around their midpoint. */
static int bottom_motion(const AInputEvent *e) {
    int act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK;
    size_t n = AMotionEvent_getPointerCount(e);
    pthread_mutex_lock(&viewLock);
    lastTouch = now_s();
    if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) ntouch = 0;
    else {
        int was = ntouch; ntouch = n > 2 ? 2 : (int)n;
        if (kind == AMOTION_EVENT_ACTION_POINTER_UP) {   /* the lifted finger is still in this event: drop it */
            size_t up = (size_t)((act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            int k = 0; for (size_t i = 0; i < n && k < 2; i++) if (i != up) { touch[k].id = AMotionEvent_getPointerId(e, i); touch[k].x = AMotionEvent_getX(e, i); touch[k].y = AMotionEvent_getY(e, i); k++; }
            ntouch = k; was = -1;
        } else for (int i = 0; i < ntouch; i++) { touch[i].id = AMotionEvent_getPointerId(e, (size_t)i); touch[i].x = AMotionEvent_getX(e, (size_t)i); touch[i].y = AMotionEvent_getY(e, (size_t)i); }
        if (ntouch != was) {   /* a new gesture: remember where it started */
            if (ntouch == 2) { pinchDist = hypot(touch[0].x - touch[1].x, touch[0].y - touch[1].y); pinchZoom = zoom; }
            dragX = ntouch == 2 ? (touch[0].x + touch[1].x) / 2 : touch[0].x; dragY = ntouch == 2 ? (touch[0].y + touch[1].y) / 2 : touch[0].y;
            dragCamX = camX; dragCamY = camY;
        } else {
            double mx = ntouch == 2 ? (touch[0].x + touch[1].x) / 2 : touch[0].x, my = ntouch == 2 ? (touch[0].y + touch[1].y) / 2 : touch[0].y;
            if (ntouch == 2 && pinchDist > 20) {
                double z = pinchZoom * hypot(touch[0].x - touch[1].x, touch[0].y - touch[1].y) / pinchDist;
                zoom = z < 0.05 ? 0.05 : z > 20 ? 20 : z;
            }
            camX = dragCamX - (mx - dragX) / zoom; camY = dragCamY - (my - dragY) / zoom;
        }
    }
    dirty = 1; pthread_cond_broadcast(&viewCond);
    pthread_mutex_unlock(&viewLock);
    return 1;
}
static AInputQueue *bottomQueue; static int expectQueue;
static int on_bottom_input(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    AInputEvent *e;
    while (bottomQueue && AInputQueue_getEvent(bottomQueue, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(bottomQueue, e)) continue;
        AInputQueue_finishEvent(bottomQueue, e, AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION ? bottom_motion(e) : 0);
    }
    return 1;
}
/* From activity_android.c's onInputQueueCreated / Destroyed (UI thread, its input lock held): 1 = the bottom screen's. */
int bottom_input_created(AInputQueue *q, ALooper *looper) {
    if (!expectQueue) return 0;
    expectQueue = 0; bottomQueue = q;
    if (looper) AInputQueue_attachLooper(q, looper, 2, on_bottom_input, NULL);
    fprintf(stderr, "[totk] bottom screen: touch input connected\n");
    return 1;
}
int bottom_input_destroyed(AInputQueue *q) {
    if (!q || q != bottomQueue) return 0;
    AInputQueue_detachLooper(q); bottomQueue = NULL;
    return 1;
}

/* ---------- drawing (render thread) ---------- */
typedef struct { uint32_t *px; int w, h, stride; } canvas;
static uint32_t rgb(int r, int g, int b) { return 0xff000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r; }   /* RGBX_8888 */
static void fill(canvas *c, int x0, int y0, int x1, int y1, uint32_t col) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; y++) { uint32_t *p = c->px + (size_t)y * c->stride; for (int x = x0; x < x1; x++) p[x] = col; }
}
static void disc(canvas *c, double cx, double cy, double r, uint32_t col) {
    for (int y = (int)(cy - r); y <= (int)(cy + r); y++) {
        if (y < 0 || y >= c->h) continue;
        double dx = sqrt(fmax(0, r * r - (y - cy) * (y - cy)));
        fill(c, (int)(cx - dx), y, (int)(cx + dx) + 1, y + 1, col);
    }
}
static void tri(canvas *c, const double *xs, const double *ys, uint32_t col) {   /* filled triangle, scanlines */
    double y0 = fmin(ys[0], fmin(ys[1], ys[2])), y1 = fmax(ys[0], fmax(ys[1], ys[2]));
    for (int y = (int)ceil(y0); y <= (int)y1; y++) {
        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < 3; i++) {
            int j = (i + 1) % 3; double ya = ys[i], yb = ys[j];
            if ((y < ya) == (y < yb) || ya == yb) continue;
            double x = xs[i] + (y - ya) * (xs[j] - xs[i]) / (yb - ya);
            if (x < lo) lo = x; if (x > hi) hi = x;
        }
        if (hi >= lo) fill(c, (int)lo, y, (int)hi + 1, y + 1, col);
    }
}
static void draw_map(canvas *c, double cx, double cy, double z, double px, double py, double pdir, const float (*t)[2], int nt) {
    fill(c, 0, 0, c->w, c->h, rgb(22, 38, 52));
    /* Grid lines every 100 map units (every 1000 brighter), only where they are at least 12 pixels apart */
    for (int major = 0; major < 2; major++) {
        double step = major ? 1000 : 100; if (step * z < 12) continue;
        uint32_t col = major ? rgb(70, 110, 130) : rgb(40, 62, 78);
        double x0 = cx - c->w / 2.0 / z, y0 = cy - c->h / 2.0 / z;
        for (double gx = floor(x0 / step) * step; gx <= x0 + c->w / z; gx += step) { int sx = (int)((gx - x0) * z); fill(c, sx, 0, sx + (major ? 2 : 1), c->h, col); }
        for (double gy = floor(y0 / step) * step; gy <= y0 + c->h / z; gy += step) { int sy = (int)((gy - y0) * z); fill(c, 0, sy, c->w, sy + (major ? 2 : 1), col); }
    }
    /* The player: an arrow pointing the way they face */
    double sx = c->w / 2.0 + (px - cx) * z, sy = c->h / 2.0 + (py - cy) * z, s = sin(pdir), k = cos(pdir);
    double ax[3] = {sx + 30 * s, sx - 18 * k - 16 * s, sx + 18 * k - 16 * s}, ay[3] = {sy - 30 * k, sy - 18 * s + 16 * k, sy + 18 * s + 16 * k};
    disc(c, sx, sy, 26, rgb(255, 255, 255)); tri(c, ax, ay, rgb(230, 60, 40));
    for (int i = 0; i < nt; i++) disc(c, t[i][0], t[i][1], 40, rgb(250, 200, 60));   /* fingers */
}
static void *render_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "bottom");
    for (;;) {
        pthread_mutex_lock(&viewLock);
        for (;;) {   /* wait for something to draw: a change, or the snap-back */
            double idle = now_s() - lastTouch;
            int following = !ntouch && idle >= SNAP_S;
            int off = following && (fabs(camX - playerX) > 0.5 / zoom || fabs(camY - playerY) > 0.5 / zoom || fabs(zoom - ZOOM_HOME) > 0.002);
            if (win && (dirty || off)) break;
            if (win && !ntouch && idle < SNAP_S) {   /* wake when the snap-back is due */
                struct timespec t; clock_gettime(CLOCK_REALTIME, &t); double w = SNAP_S - idle + 0.01;
                t.tv_sec += (time_t)w; t.tv_nsec += (long)((w - floor(w)) * 1e9); if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
                pthread_cond_timedwait(&viewCond, &viewLock, &t);
            } else pthread_cond_wait(&viewCond, &viewLock);
        }
        if (!ntouch && now_s() - lastTouch >= SNAP_S) {   /* glide back to the player, a fifth of the way per frame */
            camX += (playerX - camX) * 0.2; camY += (playerY - camY) * 0.2; zoom += (ZOOM_HOME - zoom) * 0.2;
        }
        double cx = camX, cy = camY, z = zoom, px = playerX, py = playerY, pd = playerDir;
        float t[2][2]; int nt = ntouch; for (int i = 0; i < nt; i++) { t[i][0] = touch[i].x; t[i][1] = touch[i].y; }
        ANativeWindow *w = win; winBusy = 1; dirty = 0;
        pthread_mutex_unlock(&viewLock);
        ANativeWindow_Buffer b;
        if (ANativeWindow_lock(w, &b, NULL) == 0) {
            canvas c = {b.bits, b.width, b.height, b.stride};
            draw_map(&c, cx, cy, z, px, py, pd, t, nt);
            ANativeWindow_unlockAndPost(w);
        }
        pthread_mutex_lock(&viewLock); winBusy = 0; pthread_cond_broadcast(&viewCond); pthread_mutex_unlock(&viewLock);
        usleep(nt ? 16000 : 50000);   /* ~60 drawings a second under a finger, 20 when only following the player */
    }
    return NULL;
}

/* ---------- the Presentation (UI thread) ---------- */
static jobject pres, view;   /* global references while shown */
static int pollTimer = -1;
static jobject call_obj(JNIEnv *env, jobject o, const char *name, const char *sig, ...) {
    va_list ap; va_start(ap, sig);
    jobject r = (*env)->CallObjectMethodV(env, o, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, o), name, sig), ap);
    va_end(ap); return r;
}
static void call_void(JNIEnv *env, jobject o, const char *name, const char *sig, ...) {
    va_list ap; va_start(ap, sig);
    (*env)->CallVoidMethodV(env, o, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, o), name, sig), ap);
    va_end(ap);
}
static int jni_failed(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); return 1;
}
static ANativeActivity *act;
static int on_poll(int fd, int events, void *data) {   /* every 50 ms while shown: has the surface appeared? */
    (void)events; (void)data; uint64_t n; if (read(fd, &n, sizeof n) != sizeof n || !view || !act) return 1;
    JNIEnv *env = act->env;
    (*env)->PushLocalFrame(env, 8);
    jobject s = call_obj(env, call_obj(env, view, "getHolder", "()Landroid/view/SurfaceHolder;"), "getSurface", "()Landroid/view/Surface;");
    int valid = s && (*env)->CallBooleanMethod(env, s, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, s), "isValid", "()Z"));
    if (!jni_failed(env) && valid) {
        ANativeWindow *w = ANativeWindow_fromSurface(env, s);
        if (w) {
            ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBX_8888);
            fprintf(stderr, "[totk] bottom screen: surface %dx%d\n", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
            pthread_mutex_lock(&viewLock); win = w; dirty = 1; pthread_cond_broadcast(&viewCond); pthread_mutex_unlock(&viewLock);
            struct itimerspec off = {0}; timerfd_settime(pollTimer, 0, &off, NULL);
        }
    }
    (*env)->PopLocalFrame(env, NULL);
    return 1;
}
static void bottom_show(ANativeActivity *a) {
    JNIEnv *env = a->env;
    if (pres) return;
    (*env)->PushLocalFrame(env, 32);
    jobject dm = call_obj(env, a->clazz, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", (*env)->NewStringUTF(env, "display"));
    jobjectArray ds = dm ? call_obj(env, dm, "getDisplays", "(Ljava/lang/String;)[Landroid/view/Display;", (*env)->NewStringUTF(env, "android.hardware.display.category.PRESENTATION")) : NULL;
    if (jni_failed(env) || !ds || (*env)->GetArrayLength(env, ds) < 1) { fprintf(stderr, "[totk] bottom screen: no presentation display\n"); (*env)->PopLocalFrame(env, NULL); return; }
    jobject d = (*env)->GetObjectArrayElement(env, ds, 0);
    jclass pc = (*env)->FindClass(env, "android/app/Presentation");
    jobject p = (*env)->NewObject(env, pc, (*env)->GetMethodID(env, pc, "<init>", "(Landroid/content/Context;Landroid/view/Display;)V"), a->clazz, d);
    if (jni_failed(env) || !p) { fprintf(stderr, "[totk] bottom screen: no Presentation\n"); (*env)->PopLocalFrame(env, NULL); return; }
    jobject w = call_obj(env, p, "getWindow", "()Landroid/view/Window;");
    call_void(env, w, "setFlags", "(II)V", 0x8 | 0x80, 0x8 | 0x80);   /* FLAG_NOT_FOCUSABLE, FLAG_KEEP_SCREEN_ON */
    expectQueue = 1;
    call_void(env, w, "takeInputQueue", "(Landroid/view/InputQueue$Callback;)V", a->clazz);   /* touches: bottom_input_created */
    jclass svc = (*env)->FindClass(env, "android/view/SurfaceView");
    jobject sv = (*env)->NewObject(env, svc, (*env)->GetMethodID(env, svc, "<init>", "(Landroid/content/Context;)V"),
                                   call_obj(env, p, "getContext", "()Landroid/content/Context;"));
    call_void(env, p, "setContentView", "(Landroid/view/View;)V", sv);
    call_void(env, p, "show", "()V");
    if (jni_failed(env)) { expectQueue = 0; fprintf(stderr, "[totk] bottom screen: the Presentation could not be shown\n"); (*env)->PopLocalFrame(env, NULL); return; }
    pres = (*env)->NewGlobalRef(env, p); view = (*env)->NewGlobalRef(env, sv);
    if (pollTimer < 0 && (pollTimer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) >= 0) ALooper_addFd(ALooper_forThread(), pollTimer, 4, ALOOPER_EVENT_INPUT, on_poll, NULL);
    struct itimerspec ts = {{0, 50000000}, {0, 50000000}}; timerfd_settime(pollTimer, 0, &ts, NULL);
    fprintf(stderr, "[totk] bottom screen: shown\n");
    (*env)->PopLocalFrame(env, NULL);
}
static void bottom_hide(ANativeActivity *a) {
    if (!pres) return;
    JNIEnv *env = a->env;
    pthread_mutex_lock(&viewLock);   /* the render thread lets go of the surface first */
    ANativeWindow *w = win; win = NULL;
    while (winBusy) pthread_cond_wait(&viewCond, &viewLock);
    ntouch = 0;
    pthread_mutex_unlock(&viewLock);
    if (w) ANativeWindow_release(w);
    struct itimerspec off = {0}; if (pollTimer >= 0) timerfd_settime(pollTimer, 0, &off, NULL);
    call_void(env, pres, "dismiss", "()V"); jni_failed(env);
    (*env)->DeleteGlobalRef(env, pres); (*env)->DeleteGlobalRef(env, view); pres = view = NULL;
    expectQueue = 0;
    fprintf(stderr, "[totk] bottom screen: dismissed\n");
}
/* From activity_android.c (UI thread): shown while the game has focus. */
void bottom_focus(ANativeActivity *a, int on) {
    static int started;
    if (env_off("SCARLET_BOTTOM")) return;
    act = a;
    if (!started) { started = 1; pthread_t t; if (!pthread_create(&t, NULL, render_thread, NULL)) pthread_detach(t); }
    if (on) bottom_show(a); else bottom_hide(a);
}
void bottom_destroy(ANativeActivity *a) { if (act == a) { bottom_hide(a); act = NULL; } }
#endif
