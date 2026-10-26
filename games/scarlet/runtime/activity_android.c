/* Thor port, stage T3: the APK's entry (built only with SCARLET_APK: android/Makefile apk). The app has no Java code:
   Android's NativeActivity loads libscarlet.so and calls ANativeActivity_onCreate on the UI thread. The game starts once
   per process, on its own thread, in the app's external files folder (/sdcard/Android/data/<package>/files), which
   holds what /data/local/tmp/scarlet holds for the command-line build: extracted/, rewritten/v400-android/, user-data/
   (the saves). Two files there are for testing: env.txt (one VAR=value per line, set before the game starts) and
   logs/run.log (the game's stderr; the previous run's is logs/run-prev.log). The window and the input queue go to
   runtime/frontend_android.c and runtime/gamepad_android.c. Leaving the app pauses the game (no vsync without a
   window); coming back gives it a new window and it goes on. */
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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
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
    runtime_env_input_script(); runtime_env_input_file(); runtime_env_autoload(); runtime_env_thread_dumps();
    const char *buzz = getenv("SCARLET_RUMBLE_TEST");   // 1..255: one 300 ms vibration at that strength, to check the motor
    if (buzz && atoi(buzz) > 0) { void runtime_android_rumble(int); runtime_android_rumble(atoi(buzz) > 255 ? 255 : atoi(buzz)); usleep(300000); runtime_android_rumble(0); }
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
int bottom_input_created(AInputQueue *q, ALooper *looper), bottom_input_destroyed(AInputQueue *q);   /* runtime/bottom_android.c */
void bottom_focus(ANativeActivity *a, int on), bottom_destroy(ANativeActivity *a);
static void input_created(ANativeActivity *a, AInputQueue *q) {
    (void)a; pthread_mutex_lock(&inputLock);
    if (!bottom_input_created(q, inputLooper)) { inputQueue = q; if (inputLooper) AInputQueue_attachLooper(q, inputLooper, 1, on_input, NULL); }
    pthread_mutex_unlock(&inputLock);
}
static void input_destroyed(ANativeActivity *a, AInputQueue *q) {
    (void)a; pthread_mutex_lock(&inputLock);
    int bottom = bottom_input_destroyed(q);
    if (!bottom) { AInputQueue_detachLooper(q); if (inputQueue == q) inputQueue = NULL; }
    pthread_mutex_unlock(&inputLock);
    if (!bottom) runtime_input_clear();
}

/* Text entry (the game's keyboard applet, runtime/swkbd_host.c, through runtime_front_text_input): an Android dialog
   with the game's prompt, one text field (the game's initial text, its length limit, digits or hidden when it asks) and
   an OK button, with the on-screen keyboard. It is made through JNI on the UI thread, which a pipe on the UI thread's
   looper wakes. The app has no Java code of its own, so the dialog has no listener: it cannot be cancelled, and a 100 ms
   timer on the same looper notices it has closed and reads the field. The game thread waits meanwhile. */
static ANativeActivity *activity;   /* the current activity (UI thread), NULL after onDestroy */
static int uiPipe[2] = {-1, -1}, uiTimer = -1;
static pthread_mutex_t textLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t textCond = PTHREAD_COND_INITIALIZER;
static const RuntimeTextRequest *textReq; static char *textOut; static size_t textOutSize;
static int textState;   /* 0 idle, 1 asked, 2 shown, 3 answered */
static jobject textDialog, textField;   /* global references while shown */
static char textTest[256]; static int textTestOp;   /* runtime_front_text_test: 1 type, 2 enter */

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
static void text_answer(const char *text, int ok) {   /* UI thread */
    pthread_mutex_lock(&textLock);
    if (textState == 1 || textState == 2) { snprintf(textOut, textOutSize, "%s", text); textState = ok ? 3 : 4; pthread_cond_broadcast(&textCond); }
    pthread_mutex_unlock(&textLock);
}
static void text_show(JNIEnv *env, ANativeActivity *a) {
    const RuntimeTextRequest *r = textReq;
    (*env)->PushLocalFrame(env, 32);
    jclass etc = (*env)->FindClass(env, "android/widget/EditText");
    jobject et = (*env)->NewObject(env, etc, (*env)->GetMethodID(env, etc, "<init>", "(Landroid/content/Context;)V"), a->clazz);
    call_void(env, et, "setSingleLine", "()V");
    call_void(env, et, "setText", "(Ljava/lang/CharSequence;)V", (*env)->NewStringUTF(env, r->initial ? r->initial : ""));
    call_void(env, et, "setSelectAllOnFocus", "(Z)V", JNI_TRUE);
    call_void(env, et, "setImeOptions", "(I)V", 6);   /* IME_ACTION_DONE. Landscape keyboards take the whole screen with their
                                                         own field (pinned below instead, the keyboard hid the dialog): Done
                                                         puts the keyboard away and shows the dialog's OK */
    /* InputType: TYPE_CLASS_NUMBER, or TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_PASSWORD */
    if (r->digits_only) call_void(env, et, "setInputType", "(I)V", 2); else if (r->secure) call_void(env, et, "setInputType", "(I)V", 0x81);
    if (r->max_units > 0) {
        jclass lfc = (*env)->FindClass(env, "android/text/InputFilter$LengthFilter"), ifc = (*env)->FindClass(env, "android/text/InputFilter");
        jobject lf = (*env)->NewObject(env, lfc, (*env)->GetMethodID(env, lfc, "<init>", "(I)V"), r->max_units);
        call_void(env, et, "setFilters", "([Landroid/text/InputFilter;)V", (*env)->NewObjectArray(env, 1, ifc, lf));
    }
    jclass bc = (*env)->FindClass(env, "android/app/AlertDialog$Builder");
    jobject b = (*env)->NewObject(env, bc, (*env)->GetMethodID(env, bc, "<init>", "(Landroid/content/Context;)V"), a->clazz);
    call_obj(env, b, "setTitle", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", (*env)->NewStringUTF(env, r->prompt ? r->prompt : ""));
    call_obj(env, b, "setView", "(Landroid/view/View;)Landroid/app/AlertDialog$Builder;", et);
    call_obj(env, b, "setPositiveButton", "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;",
             (*env)->NewStringUTF(env, "OK"), NULL);
    call_obj(env, b, "setCancelable", "(Z)Landroid/app/AlertDialog$Builder;", JNI_FALSE);
    jobject dlg = call_obj(env, b, "create", "()Landroid/app/AlertDialog;");
    if (!jni_failed(env) && dlg) {
        call_void(env, call_obj(env, dlg, "getWindow", "()Landroid/view/Window;"), "setSoftInputMode", "(I)V", 5);   /* SOFT_INPUT_STATE_ALWAYS_VISIBLE */
        call_void(env, dlg, "show", "()V");
        (*env)->CallBooleanMethod(env, et, (*env)->GetMethodID(env, etc, "requestFocus", "()Z"));
    }
    if (jni_failed(env) || !dlg) { fprintf(stderr, "[totk] text entry: the dialog could not be shown; the initial text is used\n"); text_answer(r->initial ? r->initial : "", 1); }
    else {
        textDialog = (*env)->NewGlobalRef(env, dlg); textField = (*env)->NewGlobalRef(env, et);
        pthread_mutex_lock(&textLock); textState = 2; pthread_mutex_unlock(&textLock);
        struct itimerspec ts = {{0, 100000000}, {0, 100000000}}; timerfd_settime(uiTimer, 0, &ts, NULL);
        fprintf(stderr, "[totk] text entry dialog shown: \"%s\"\n", r->prompt ? r->prompt : "");
    }
    (*env)->PopLocalFrame(env, NULL);
}
static int on_ui(int fd, int events, void *data) {   /* UI thread: a request to show the dialog */
    (void)events; (void)data; char c; while (read(fd, &c, 1) == 1) {}
    pthread_mutex_lock(&textLock); int show = textState == 1; pthread_mutex_unlock(&textLock);
    if (show) { if (activity) text_show(activity->env, activity); else text_answer(textReq->initial ? textReq->initial : "", 1); }
    return 1;
}
static int on_timer(int fd, int events, void *data) {   /* UI thread, every 100 ms while the dialog is shown */
    (void)events; (void)data; uint64_t n; if (read(fd, &n, sizeof n) != sizeof n || !textDialog || !activity) return 1;
    JNIEnv *env = activity->env;
    (*env)->PushLocalFrame(env, 16);
    pthread_mutex_lock(&textLock); int op = textTestOp; char test[sizeof textTest]; memcpy(test, textTest, sizeof test); textTestOp = 0; pthread_mutex_unlock(&textLock);
    if (op == 1) call_void(env, textField, "setText", "(Ljava/lang/CharSequence;)V", (*env)->NewStringUTF(env, test));
    if (op == 2) call_void(env, textDialog, "dismiss", "()V");
    jclass dc = (*env)->GetObjectClass(env, textDialog);
    if (!(*env)->CallBooleanMethod(env, textDialog, (*env)->GetMethodID(env, dc, "isShowing", "()Z"))) {
        jobject s = call_obj(env, call_obj(env, textField, "getText", "()Landroid/text/Editable;"), "toString", "()Ljava/lang/String;");
        const char *u = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
        fprintf(stderr, "[totk] text entry: \"%s\"\n", u ? u : "");
        text_answer(u ? u : "", 1);
        if (u) (*env)->ReleaseStringUTFChars(env, s, u);
        (*env)->DeleteGlobalRef(env, textDialog); (*env)->DeleteGlobalRef(env, textField); textDialog = textField = NULL;
        struct itimerspec off = {0}; timerfd_settime(uiTimer, 0, &off, NULL);
    }
    jni_failed(env);
    (*env)->PopLocalFrame(env, NULL);
    return 1;
}
/* Called by runtime/frontend_android.c on a game thread: 1 with the text (UTF-8), the dialog's only answer. */
int runtime_android_text(const RuntimeTextRequest *req, char *out, size_t out_size) {
    pthread_mutex_lock(&textLock);
    while (textState) pthread_cond_wait(&textCond, &textLock);   /* one dialog at a time */
    textReq = req; textOut = out; textOutSize = out_size; textState = 1;
    pthread_mutex_unlock(&textLock);
    char c = 1; if (uiPipe[1] < 0 || write(uiPipe[1], &c, 1) != 1) { pthread_mutex_lock(&textLock); textState = 0; pthread_mutex_unlock(&textLock); return 0; }
    pthread_mutex_lock(&textLock);
    while (textState < 3) pthread_cond_wait(&textCond, &textLock);
    int ok = textState == 3; textState = 0; pthread_cond_broadcast(&textCond);
    pthread_mutex_unlock(&textLock);
    return ok;
}
/* Scripts (TOTK_INPUT_FILE "type <text>" / "enter"): set the open dialog's text, or close it as OK would. */
void runtime_android_text_test(const char *op, const char *text) {
    pthread_mutex_lock(&textLock);
    if (!strcmp(op, "type")) { snprintf(textTest, sizeof textTest, "%s", text ? text : ""); textTestOp = 1; }
    else if (!strcmp(op, "enter")) textTestOp = 2;
    pthread_mutex_unlock(&textLock);
}

/* Rumble (runtime/gamepad_android.c): one strength, 0..255, for Android's vibrator (the Thor has one, with amplitude
   control). A thread of its own, attached to the VM, applies it, so no Java call happens on a game thread: while the
   strength is above 0 it renews a 100 ms one-shot vibration every 50 ms, and 0 cancels it. */
static JavaVM *appVM; static jobject appContext;   /* the application context (a global reference), for the vibrator */
static pthread_mutex_t rumbleLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rumbleCond = PTHREAD_COND_INITIALIZER;
static int rumbleLevel, rumbleStarted;
static void *rumble_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "rumble");
    JNIEnv *env; if ((*appVM)->AttachCurrentThread(appVM, &env, NULL) != JNI_OK) return NULL;
    jclass cc = (*env)->GetObjectClass(env, appContext);
    jobject vib = (*env)->CallObjectMethod(env, appContext, (*env)->GetMethodID(env, cc, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"),
                                           (*env)->NewStringUTF(env, "vibrator"));
    jclass vc = (*env)->FindClass(env, "android/os/Vibrator"), ec = (*env)->FindClass(env, "android/os/VibrationEffect");
    if ((*env)->ExceptionCheck(env) || !vib || !vc || !ec) { (*env)->ExceptionClear(env); fprintf(stderr, "[totk] rumble: no vibrator\n"); return NULL; }
    /* Media usage, as for games (without attributes Android files it under touch feedback, which the touch haptics
       setting turns off); the media vibration setting applies. */
    jclass ac = (*env)->FindClass(env, "android/os/VibrationAttributes");
    jobject attrs = ac ? (*env)->CallStaticObjectMethod(env, ac, (*env)->GetStaticMethodID(env, ac, "createForUsage", "(I)Landroid/os/VibrationAttributes;"),
                                                        (*env)->GetStaticIntField(env, ac, (*env)->GetStaticFieldID(env, ac, "USAGE_MEDIA", "I"))) : NULL;
    if ((*env)->ExceptionCheck(env) || !attrs) { (*env)->ExceptionClear(env); fprintf(stderr, "[totk] rumble: no vibration attributes\n"); return NULL; }
    jmethodID vibrate = (*env)->GetMethodID(env, vc, "vibrate", "(Landroid/os/VibrationEffect;Landroid/os/VibrationAttributes;)V"), cancel = (*env)->GetMethodID(env, vc, "cancel", "()V");
    jmethodID oneshot = (*env)->GetStaticMethodID(env, ec, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
    fprintf(stderr, "[totk] rumble: the game's vibration goes to Android's vibrator\n");
    int on = 0;
    for (;;) {
        pthread_mutex_lock(&rumbleLock);
        if (!rumbleLevel && !on) pthread_cond_wait(&rumbleCond, &rumbleLock);
        else { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_nsec += 50000000; if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; } pthread_cond_timedwait(&rumbleCond, &rumbleLock, &t); }
        int level = rumbleLevel;
        pthread_mutex_unlock(&rumbleLock);
        (*env)->PushLocalFrame(env, 8);
        if (level > 0) { jobject eff = (*env)->CallStaticObjectMethod(env, ec, oneshot, (jlong)100, (jint)level); if (eff) (*env)->CallVoidMethod(env, vib, vibrate, eff, attrs); on = 1; }
        else if (on) { (*env)->CallVoidMethod(env, vib, cancel); on = 0; }
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
        (*env)->PopLocalFrame(env, NULL);
    }
    return NULL;
}
void runtime_android_rumble(int level) {
    if (!appVM || !appContext) return;
    pthread_mutex_lock(&rumbleLock);
    if (level != rumbleLevel) { rumbleLevel = level; pthread_cond_broadcast(&rumbleCond); }
    if (!rumbleStarted && level > 0) { rumbleStarted = 1; pthread_t t; if (!pthread_create(&t, NULL, rumble_thread, NULL)) pthread_detach(t); }
    pthread_mutex_unlock(&rumbleLock);
}

static void window_created(ANativeActivity *a, ANativeWindow *w) { (void)a; runtime_android_window(w); }
static void window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; runtime_android_window(NULL); }
static void focus_changed(ANativeActivity *a, int on) { if (on) immersive(a); runtime_android_focus(on); bottom_focus(a, on); }
static void on_resume(ANativeActivity *a) { (void)a; ALOG("resume"); }
static void on_pause(ANativeActivity *a) { (void)a; ALOG("pause"); }
static void on_destroy(ANativeActivity *a) { bottom_destroy(a); if (activity == a) activity = NULL; ALOG("activity destroyed (the game keeps running)"); }

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
    activity = a;
    if (!appContext) {   /* for the rumble thread: the application context outlives any one activity */
        appVM = a->vm;
        jclass ac = (*a->env)->GetObjectClass(a->env, a->clazz);
        jobject app = (*a->env)->CallObjectMethod(a->env, a->clazz, (*a->env)->GetMethodID(a->env, ac, "getApplicationContext", "()Landroid/content/Context;"));
        if (app) appContext = (*a->env)->NewGlobalRef(a->env, app);
        if ((*a->env)->ExceptionCheck(a->env)) (*a->env)->ExceptionClear(a->env);
    }
    if (uiPipe[0] < 0) {   /* the text entry dialog's wake-ups on the UI thread's looper (the same looper for every activity) */
        ALooper *ui = ALooper_forThread();
        if (pipe2(uiPipe, O_NONBLOCK | O_CLOEXEC) == 0) ALooper_addFd(ui, uiPipe[0], 2, ALOOPER_EVENT_INPUT, on_ui, NULL);
        if ((uiTimer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) >= 0) ALooper_addFd(ui, uiTimer, 3, ALOOPER_EVENT_INPUT, on_timer, NULL);
    }
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
