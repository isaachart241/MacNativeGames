/* Save backups on the Thor (SCARLET_APK): the save lives in the app's data folder, which Android deletes with the app
   (an uninstall, or a reinstall after a signing-key change). So a copy goes to shared storage, outside the app,
   /sdcard/Documents/Scarlet saves/<date time>/, 5 s after each commit that changed save data (the game commits several
   times in a row) once the save is done, and once at start; the 10 newest copies are kept. A thread of its own does the
   copying, so the game never waits for it. SCARLET_SAVE_BACKUP=0 turns it off; SCARLET_SAVE_BACKUP_DIR=<path> moves it.
   Restoring: copy a backup's folders into the app's user-data/save/ (android/README.md). */
#ifdef SCARLET_APK
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int runtime_fs_save_busy(void);
const char *runtime_fs_save_root(void);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static double due;   /* monotonic seconds when the next copy is due; 0: none */
static char root[1024];
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static const char *dest_dir(void) { const char *d = getenv("SCARLET_SAVE_BACKUP_DIR"); return d && *d ? d : "/storage/emulated/0/Documents/Scarlet saves"; }

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (out < 0) { close(in); return -1; }
    char buf[65536]; ssize_t n; int ok = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) { ok = -1; break; }
    if (n < 0) ok = -1;
    if (fsync(out)) ok = -1;
    close(in); close(out); return ok;
}
static int copy_tree(const char *from, const char *to) {
    DIR *d = opendir(from); if (!d) return -1;
    if (mkdir(to, 0755) && errno != EEXIST) { closedir(d); return -1; }
    struct dirent *e; int ok = 0; char a[2048], b[2048];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name); snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        struct stat st; if (stat(a, &st)) { ok = -1; continue; }
        ok |= S_ISDIR(st.st_mode) ? copy_tree(a, b) : copy_file(a, b);
    }
    closedir(d); return ok;
}
static void remove_tree(const char *path) {
    DIR *d = opendir(path); if (!d) { unlink(path); return; }
    struct dirent *e; char c[2048];
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; snprintf(c, sizeof c, "%s/%s", path, e->d_name); remove_tree(c); }
    closedir(d); rmdir(path);
}
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void prune(const char *dir, int keep) {   /* the folders are named by date and time: the oldest sort first */
    DIR *d = opendir(dir); if (!d) return;
    char *names[512]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 512) if (e->d_name[0] != '.') names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, by_name);
    for (int i = 0; i < n; i++) { if (i < n - keep) { char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, names[i]); remove_tree(p); } free(names[i]); }
}
static void backup_now(void) {
    const char *dir = dest_dir(); char parent[1024]; snprintf(parent, sizeof parent, "%s", dir);
    char *slash = strrchr(parent, '/'); if (slash) { *slash = 0; mkdir(parent, 0755); }
    if (mkdir(dir, 0755) && errno != EEXIST) { fprintf(stderr, "[scarlet] save backup: cannot create %s: %s\n", dir, strerror(errno)); return; }
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char name[64]; strftime(name, sizeof name, "%Y-%m-%d %H.%M.%S", &tm);
    char tmp[1200], fin[1200]; snprintf(tmp, sizeof tmp, "%s/.%s", dir, name); snprintf(fin, sizeof fin, "%s/%s", dir, name);
    remove_tree(tmp);
    if (copy_tree(root, tmp) || rename(tmp, fin)) { fprintf(stderr, "[scarlet] save backup to %s failed: %s\n", fin, strerror(errno)); remove_tree(tmp); return; }
    prune(dir, 10);
    fprintf(stderr, "[scarlet] save backup: %s\n", fin);
}
static void *backup_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "save backup");
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!due) pthread_cond_wait(&cond, &lock);
        double wait = due - now_s();
        if (wait > 0) { pthread_mutex_unlock(&lock); usleep((useconds_t)(wait * 1e6)); continue; }
        pthread_mutex_unlock(&lock);
        if (runtime_fs_save_busy()) { usleep(500000); continue; }   /* still saving: the copy waits for the end */
        pthread_mutex_lock(&lock); due = 0; pthread_mutex_unlock(&lock);
        backup_now();
    }
    return NULL;
}
static void schedule(const char *save_root, double delay) {
    const char *e = getenv("SCARLET_SAVE_BACKUP"); if (e && !strcmp(e, "0")) return;
    static int started;
    pthread_mutex_lock(&lock);
    if (save_root && *save_root) snprintf(root, sizeof root, "%s", save_root);
    if (!started) { started = 1; pthread_t t; if (!pthread_create(&t, NULL, backup_thread, NULL)) pthread_detach(t); }
    if (root[0]) { due = now_s() + delay; pthread_cond_broadcast(&cond); }
    pthread_mutex_unlock(&lock);
}
void runtime_save_committed(const char *save_root) { schedule(save_root, 5); }   /* runtime/fs_host.c, at a commit that changed save data */
/* At start (runtime/activity_android.c): one copy of the save as it is, once the game has set up its storage. */
static void *first_copy(void *arg) {
    (void)arg;
    for (int i = 0; i < 600 && !runtime_fs_save_root()[0]; i++) usleep(100000);
    if (runtime_fs_save_root()[0]) schedule(runtime_fs_save_root(), 10);
    return NULL;
}
void runtime_save_backup_start(void) { pthread_t t; if (!pthread_create(&t, NULL, first_copy, NULL)) pthread_detach(t); }
#endif
