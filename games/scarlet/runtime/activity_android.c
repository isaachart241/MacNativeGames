/* Thor port, stage T3: the APK's entry (built only with SCARLET_APK: android/Makefile apk). The app has no Java code:
   Android's NativeActivity loads libscarlet.so and calls ANativeActivity_onCreate on the UI thread. The game starts once
   per process, on its own thread, in the app's external files folder (/sdcard/Android/data/<package>/files), which
   holds what /data/local/tmp/scarlet holds for the command-line build: extracted/, rewritten/v400-android/, user-data/
   (the saves). Two files there are for testing: env.txt (one VAR=value per line, set before the game starts) and
   logs/run.log (the game's stderr; the previous run's is logs/run-prev.log). The window and the input queue go to
   runtime/frontend_android.c and runtime/gamepad_android.c. Leaving the app keeps the game running without a picture;
   coming back gives it a new window. */
#ifdef SCARLET_APK
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "runtime.h"

int main(int argc, char **argv);   /* runtime/loader.c */
_Noreturn void runtime_android_exit(int code);   /* runtime/frontend_android.c */
void runtime_android_window(ANativeWindow *w);
void runtime_android_focus(int on);
int runtime_android_input(const AInputEvent *event);

#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "scarlet", __VA_ARGS__)

/* Full screen without the status and navigation bars: View.setSystemUiVisibility through JNI (UI thread only). */
static void immersive(ANativeActivity *a) {
    JNIEnv *env = a->env;
    jclass ac = (*env)->GetObjectClass(env, a->clazz);
    jobject win = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getWindow", "()Landroid/view/Window;"));
    if (win) {
        jclass wc = (*env)->GetObjectClass(env, win);
        jobject decor = (*env)->CallObjectMethod(env, win, (*env)->GetMethodID(env, wc, "getDecorView", "()Landroid/view/View;"));
        if (decor) {
            jclass vc = (*env)->GetObjectClass(env, decor);
            /* LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION | LAYOUT_FULLSCREEN | HIDE_NAVIGATION | FULLSCREEN | IMMERSIVE_STICKY */
            (*env)->CallVoidMethod(env, decor, (*env)->GetMethodID(env, vc, "setSystemUiVisibility", "(I)V"), 0x100 | 0x200 | 0x400 | 0x2 | 0x4 | 0x1000);
        }
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
}

/* env.txt: VAR=value lines ('#' starts a comment line), for test switches such as SCARLET_AUDIO_MUTE=1. */
static void load_env(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0; setenv(line, eq + 1, 1);
        fprintf(stderr, "[totk] env.txt: %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

static void *game_thread(void *arg) {
    const char *dir = arg;
    if (chdir(dir)) { ALOG("cannot enter %s: %s", dir, strerror(errno)); return NULL; }
    mkdir("logs", 0777);
    rename("logs/run.log", "logs/run-prev.log");
    int fd = open("logs/run.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { dup2(fd, 2); dup2(fd, 1); close(fd); }
    setvbuf(stderr, NULL, _IOLBF, 0);
    fprintf(stderr, "[totk] Scarlet for the Thor: data folder %s\n", dir);
    load_env("env.txt");
    const char *rewritten = getenv("SCARLET_REWRITTEN"); if (!rewritten || !*rewritten) rewritten = "rewritten/v400-android";
    struct stat st;
    if (stat(rewritten, &st) || stat("extracted", &st)) {
        fprintf(stderr, "[totk] no game data here: copy extracted/, %s/ and user-data/ into %s (THOR_SCOPE.md, stage T3)\n", rewritten, dir);
        ALOG("no game data in %s", dir);
        return NULL;
    }
    char *argv[] = {"scarlet", (char *)rewritten, "boot", NULL};
    runtime_android_exit(main(3, argv));
}

/* The input queue's events, read on an input thread of their own (ALooper) so the UI thread never waits for the game. */
static pthread_mutex_t inputLock = PTHREAD_MUTEX_INITIALIZER;
static AInputQueue *inputQueue;
static ALooper *inputLooper;
static int on_input(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    pthread_mutex_lock(&inputLock);
    AInputEvent *e;
    while (inputQueue && AInputQueue_getEvent(inputQueue, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(inputQueue, e)) continue;
        AInputQueue_finishEvent(inputQueue, e, runtime_android_input(e));
    }
    pthread_mutex_unlock(&inputLock);
    return 1;
}
static void *input_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "input");
    ALooper *l = ALooper_prepare(0);
    pthread_mutex_lock(&inputLock);
    inputLooper = l;
    if (inputQueue) AInputQueue_attachLooper(inputQueue, l, 1, on_input, NULL);
    pthread_mutex_unlock(&inputLock);
    for (;;) ALooper_pollOnce(-1, NULL, NULL, NULL);
    return NULL;
}
static void input_created(ANativeActivity *a, AInputQueue *q) {
    (void)a; pthread_mutex_lock(&inputLock);
    inputQueue = q; if (inputLooper) AInputQueue_attachLooper(q, inputLooper, 1, on_input, NULL);
    pthread_mutex_unlock(&inputLock);
}
static void input_destroyed(ANativeActivity *a, AInputQueue *q) {
    (void)a; pthread_mutex_lock(&inputLock);
    AInputQueue_detachLooper(q); if (inputQueue == q) inputQueue = NULL;
    pthread_mutex_unlock(&inputLock);
    runtime_input_clear();
}

static void window_created(ANativeActivity *a, ANativeWindow *w) { (void)a; runtime_android_window(w); }
static void window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; runtime_android_window(NULL); }
static void focus_changed(ANativeActivity *a, int on) { if (on) immersive(a); runtime_android_focus(on); }
static void on_resume(ANativeActivity *a) { (void)a; ALOG("resume"); }
static void on_pause(ANativeActivity *a) { (void)a; ALOG("pause"); }
static void on_destroy(ANativeActivity *a) { (void)a; ALOG("activity destroyed (the game keeps running)"); }

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_size) {
    (void)saved; (void)saved_size;
    a->callbacks->onNativeWindowCreated = window_created;
    a->callbacks->onNativeWindowDestroyed = window_destroyed;
    a->callbacks->onInputQueueCreated = input_created;
    a->callbacks->onInputQueueDestroyed = input_destroyed;
    a->callbacks->onWindowFocusChanged = focus_changed;
    a->callbacks->onResume = on_resume;
    a->callbacks->onPause = on_pause;
    a->callbacks->onDestroy = on_destroy;
    ANativeActivity_setWindowFlags(a, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    immersive(a);
    static int started;
    if (started) { ALOG("activity created again: the game is already running"); return; }
    started = 1;
    const char *dir = a->externalDataPath && *a->externalDataPath ? a->externalDataPath : a->internalDataPath;
    ALOG("starting the game in %s", dir);
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, input_thread, NULL) || pthread_create(&t, &attr, game_thread, strdup(dir))) ALOG("thread creation failed");
    pthread_attr_destroy(&attr);
}
#endif
