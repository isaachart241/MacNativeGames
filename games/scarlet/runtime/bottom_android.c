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
#define ZOOM_HOME 1.0   /* screen pixels per world unit: the closer map's detail (its tiles at about 1.5 texels a pixel); pinch out for all of Paldea */
static double camX, camY, zoom = ZOOM_HOME;   /* map position at the screen centre; screen pixels per map unit */
static double playerX, playerY, playerDir;   /* map units; radians, 0 = north (up), clockwise */
static double lastTouch = -100;       /* time of the last touch; the view follows the player SNAP_S after it */
static int uiShown;                   /* the last drawing showed the game's interface */
static struct { int id; float x, y; } touch[2]; static int ntouch;
static double pinchDist, pinchZoom, dragX, dragY, dragCamX, dragCamY;

/* The game's interface (menus, battles, dialogue, its map screen), drawn by the Vulkan backend into a layer of its own
   instead of the top screen's frame and handed over at full size (1920x1080), about once a frame while it has any
   (bottom_ui_frame). It goes at the top of the screen, laid out afresh (ui_layout), with the map in the strip below;
   0.2 s without a frame and the whole screen is the map again. Three buffers: the one being drawn, the newest waiting,
   and the one being filled, so neither thread waits for the other's copying. */
#define UI_MAXW 1920
#define UI_MAXH 1080
static uint8_t *uiBuf[3]; static int uiW, uiH, uiReady = -1, uiDraw = -1; static double uiTime = -100;
int bottom_ui_width(void) { return UI_MAXW; }
void bottom_ui_frame(const uint8_t *rgba, int w, int h) {
    if (w > UI_MAXW || h > UI_MAXH) return;
    pthread_mutex_lock(&viewLock);
    if (!uiBuf[0]) for (int i = 0; i < 3; i++) uiBuf[i] = malloc((size_t)UI_MAXW * UI_MAXH * 4);
    int k = 0; while (k == uiDraw || k == uiReady) k++;   /* neither drawn nor waiting */
    pthread_mutex_unlock(&viewLock);
    if (!uiBuf[k]) return;
    memcpy(uiBuf[k], rgba, (size_t)w * h * 4);
    pthread_mutex_lock(&viewLock);
    uiReady = k; uiW = w; uiH = h; uiTime = now_s(); dirty = 1; pthread_cond_broadcast(&viewCond);
    pthread_mutex_unlock(&viewLock);
}
#define UI_HOLD 0.2

void bottom_map_set_player(double x, double y, double dir) {   /* a new drawing only for half a pixel or a degree */
    pthread_mutex_lock(&viewLock);
    double d = fabs(dir - playerDir); if (d > M_PI) d = 2 * M_PI - d;
    if (hypot(x - playerX, y - playerY) * zoom >= 0.5 || d >= M_PI / 180) { playerX = x; playerY = y; playerDir = dir; dirty = 1; pthread_cond_broadcast(&viewCond); }
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
                zoom = z < 0.1 ? 0.1 : z > 6 ? 6 : z;
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
/* ---------- the map art (map-cache/, saved by the Vulkan backend from the game's map screen) ---------- */
typedef struct { uint8_t *px; int w, h; } image;
static image overview, pattern;   /* render thread only */
static _Atomic int assetsChanged = 1;
void bottom_map_assets_changed(void) { assetsChanged = 1; pthread_mutex_lock(&viewLock); dirty = 1; pthread_cond_broadcast(&viewCond); pthread_mutex_unlock(&viewLock); }
static void load_image(image *im, const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    uint32_t hd[3];
    if (fread(hd, 4, 3, f) == 3 && hd[0] == 0x3150414d && hd[1] && hd[2] && hd[1] <= 4096 && hd[2] <= 4096) {
        uint8_t *px = malloc((size_t)hd[1] * hd[2] * 4);
        if (px && fread(px, 4, (size_t)hd[1] * hd[2], f) == (size_t)hd[1] * hd[2]) { free(im->px); im->px = px; im->w = (int)hd[1]; im->h = (int)hd[2]; px = NULL; }
        free(px);
    }
    fclose(f);
}
/* World (x, z) to the overview texture: u = u0 + x * scale, v = v0 + z * scale (north up, east right). Fitted by least squares
   to map-cache/calib.txt, the game's own marker positions noted whenever the zoomed-out map is shown; the scale only once
   the samples spread over 100 units. Until then, 2026-10-26's measurement at Cabo Poco: the marker at (420.33, 826.50) for
   the player at (2323.33, -741.52), 0.1746 texels a unit (east 12 units: 2.17 texels). */
static double mapScale = 0.1746, mapU0 = 420.33 - 2323.33 * 0.1746, mapV0 = 826.50 + 741.52 * 0.1746;
static double tileScale;   /* tile units a world unit, measured from the minimap as the player walks (map-cache/scale.txt); 0: 12 x mapScale */
static void load_calibration(void) {
    FILE *g = fopen("map-cache/scale.txt", "r");
    if (g) { double p; if (fscanf(g, "%lf", &p) == 1 && p > 0.5 && p < 10) { tileScale = p; mapScale = p / 12; } fclose(g); }
    FILE *f = fopen("map-cache/calib.txt", "r"); if (!f) return;
    double x, z, u, v, sx = 0, sz = 0, su = 0, sv = 0; int n = 0;
    static double X[4096], Z[4096], U[4096], Vv[4096];
    while (n < 4096 && fscanf(f, "%lf %lf %lf %lf", &x, &z, &u, &v) == 4) { X[n] = x; Z[n] = z; U[n] = u; Vv[n] = v; sx += x; sz += z; su += u; sv += v; n++; }
    fclose(f);
    if (!n) return;
    sx /= n; sz /= n; su /= n; sv /= n;
    double num = 0, den = 0;
    for (int i = 0; i < n; i++) { num += (X[i] - sx) * (U[i] - su) + (Z[i] - sz) * (Vv[i] - sv); den += (X[i] - sx) * (X[i] - sx) + (Z[i] - sz) * (Z[i] - sz); }
    if (!tileScale && den > 100.0 * 100.0 && num / den > 0.05 && num / den < 1) mapScale = num / den;   /* the measured scale wins */
    mapU0 = su - mapScale * sx; mapV0 = sv - mapScale * sz;
    fprintf(stderr, "[totk] bottom map: %d calibration samples, %.4f texels a unit, origin %.2f %.2f\n", n, mapScale, mapU0, mapV0);
}

/* The game's marks (map-cache/marks.txt, noted by the backend from the closer map): the objective's pin (the latest
   capture's only) and place names (each kept once, by image), at the player's position then plus their offset in tile
   units (one overview texel is 12 of them). */
typedef struct { char kind; double x, z; float w, h; uint64_t hash; image img; } mark;
static mark *marks; static int nmarks;
static void load_marks(void) {
    for (int i = 0; i < nmarks; i++) free(marks[i].img.px);
    free(marks); marks = NULL; nmarks = 0;
    FILE *f = fopen("map-cache/marks.txt", "r"); if (!f) return;
    char k; double px, pz, e, n; float w, h; unsigned long long hash; int cap = 0;
    double lastOx = 1e30, lastOz = 1e30;
    while (fscanf(f, " %c %lf %lf %lf %lf %f %f %llx", &k, &px, &pz, &e, &n, &w, &h, &hash) == 8) {
        double t = tileScale ? tileScale : 12 * mapScale;   /* tile units a world unit */
        if (k == 'O' && (px != lastOx || pz != lastOz)) {   /* a newer capture: the older objective pins go */
            int j = 0; for (int i = 0; i < nmarks; i++) if (marks[i].kind != 'O') marks[j++] = marks[i]; nmarks = j;
            lastOx = px; lastOz = pz;
        }
        if (k == 'C') {   /* the minimap's view of this moment: earlier minimap icons within its reach go */
            double r = w / t; int j = 0;
            for (int i = 0; i < nmarks; i++) if (!(marks[i].kind == 'M' && hypot(marks[i].x - px, marks[i].z - pz) <= r)) marks[j++] = marks[i];
            nmarks = j; continue;
        }
        int i = 0; if (k == 'L') while (i < nmarks && !(marks[i].kind == 'L' && marks[i].hash == hash)) i++; else i = nmarks;
        if (i == nmarks) { if (nmarks == cap) { cap = cap ? 2 * cap : 32; marks = realloc(marks, sizeof *marks * (size_t)cap); } memset(&marks[nmarks++], 0, sizeof *marks); }
        marks[i].kind = k; marks[i].x = px + e / t; marks[i].z = pz - n / t; marks[i].w = w; marks[i].h = h; marks[i].hash = hash;
    }
    fclose(f);
    for (int i = 0; i < nmarks; i++) { char p[64]; snprintf(p, sizeof p, "map-cache/icon_%016llx.rgba", (unsigned long long)marks[i].hash); load_image(&marks[i].img, p); }
    fprintf(stderr, "[totk] bottom map: %d marks\n", nmarks);
}
static void blit(canvas *c, const image *im, double cx, double cy, double w, double h) {   /* scaled, alpha blended */
    if (!im->px || w < 1 || h < 1) return;
    int x0 = (int)(cx - w / 2), y0 = (int)(cy - h / 2), x1 = (int)(cx + w / 2), y1 = (int)(cy + h / 2);
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->h; y++) {
        int sy = (int)((y - (cy - h / 2)) * im->h / h); if (sy < 0 || sy >= im->h) continue;
        uint32_t *o = c->px + (size_t)y * c->stride;
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < c->w; x++) {
            int sx = (int)((x - (cx - w / 2)) * im->w / w); if (sx < 0 || sx >= im->w) continue;
            const uint8_t *t = im->px + 4 * ((size_t)sy * im->w + (size_t)sx); uint32_t a = t[3]; if (!a) continue;
            uint32_t d = o[x], na = 255 - a;
            o[x] = rgb((int)((t[0] * a + (d & 255) * na) / 255), (int)((t[1] * a + (d >> 8 & 255) * na) / 255), (int)((t[2] * a + (d >> 16 & 255) * na) / 255));
        }
    }
}
static void draw_marks(canvas *c, double cx, double cy, double z) {
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < nmarks; i++) {   /* names first, pins over them */
        const mark *m = &marks[i]; if ((m->kind != 'L') != pass) continue;
        if (m->kind == 'L' && z < 0.6) continue;   /* names only once zoomed in, as on the game's map */
        /* names at 1.4 of their size on the game's 1920-wide map screen (0.9 left them at about 0.6 of it on this 1240-wide screen) */
        double sx = c->w / 2.0 + (m->x - cx) * z, sy = c->h / 2.0 + (m->z - cy) * z, k = m->kind == 'L' ? 1.4 : 0.8;
        blit(c, &m->img, sx, sy, m->w * k, m->h * k);
    }
}

static void draw_grid(canvas *c, double cx, double cy, double z) {   /* before the art has been seen once */
    fill(c, 0, 0, c->w, c->h, rgb(22, 38, 52));
    for (int major = 0; major < 2; major++) {
        double step = major ? 1000 : 100; if (step * z < 12) continue;
        uint32_t col = major ? rgb(70, 110, 130) : rgb(40, 62, 78);
        double x0 = cx - c->w / 2.0 / z, y0 = cy - c->h / 2.0 / z;
        for (double gx = floor(x0 / step) * step; gx <= x0 + c->w / z; gx += step) { int sx = (int)((gx - x0) * z); fill(c, sx, 0, sx + (major ? 2 : 1), c->h, col); }
        for (double gy = floor(y0 / step) * step; gy <= y0 + c->h / z; gy += step) { int sy = (int)((gy - y0) * z); fill(c, 0, sy, c->w, sy + (major ? 2 : 1), col); }
    }
}
/* The map screen's look: its patterned navy background, the region map over it, and once zoomed in the closer map's
   tiles over that (bilinear, with their transparency: where the art is transparent the background shows, as in the
   game). */
static uint32_t *bgFrame; static int bgW, bgH;
/* im over the canvas within [x0, x1) x [y0, y1): screen pixel (x, y) shows texel (u0 + x du, v0 + y du) */
static void draw_image(canvas *c, const image *im, double u0, double v0, double du, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
    const int W = im->w, H = im->h; const uint8_t *T = im->px;
    int64_t d = (int64_t)(du * 65536), uu0 = (int64_t)((u0 + x0 * du - 0.5) * 65536), vv = (int64_t)((v0 + y0 * du - 0.5) * 65536);
    for (int y = y0; y < y1; y++, vv += d) {
        uint32_t *o = c->px + (size_t)y * c->stride;
        int64_t ty = vv >> 16; uint32_t fy = (uint32_t)(vv & 0xffff) >> 8;
        if (ty < -1 || ty >= H) continue;
        int64_t uu = uu0;
        for (int x = x0; x < x1; x++, uu += d) {
            int64_t tx = uu >> 16;
            if (tx < -1 || tx >= W) continue;
            uint32_t fx = (uint32_t)(uu & 0xffff) >> 8, acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < 4; j++) {   /* the four texels, weights in 1/65536 */
                int64_t sx = tx + (j & 1), sy = ty + (j >> 1);
                if (sx < 0) sx = 0; if (sy < 0) sy = 0; if (sx >= W) sx = W - 1; if (sy >= H) sy = H - 1;
                uint32_t w = ((j & 1) ? fx : 256 - fx) * ((j >> 1) ? fy : 256 - fy);
                const uint8_t *t = T + 4 * ((size_t)sy * W + (size_t)sx);
                uint32_t wa = w * t[3];
                acc[0] += t[0] * wa >> 8; acc[1] += t[1] * wa >> 8; acc[2] += t[2] * wa >> 8; acc[3] += wa >> 8;
            }
            uint32_t a = acc[3] >> 8;   /* coverage 0..255 */
            if (!a) continue;
            uint32_t bg = o[x], na = 255 - a;
            uint32_t r = (acc[0] >> 16) + ((bg & 255) * na >> 8), g = (acc[1] >> 16) + ((bg >> 8 & 255) * na >> 8), bl = (acc[2] >> 16) + ((bg >> 16 & 255) * na >> 8);
            o[x] = rgb((int)(r > 255 ? 255 : r), (int)(g > 255 ? 255 : g), (int)(bl > 255 ? 255 : bl));
        }
    }
}
/* The closer map's tiles (map-cache/tiles.txt, noted by the backend): each image once, at the place of its latest note
   (the player's position then plus the centre's offset in tile units); 1000 tile units a side. */
typedef struct { double x, z; uint64_t hash; image img; int shared; } tile;   /* shared: the tile whose image this one uses (-1: its own) */
static tile *tiles; static int ntiles;
static void load_tiles(void) {
    for (int i = 0; i < ntiles; i++) free(tiles[i].img.px);
    free(tiles); tiles = NULL; ntiles = 0;
    FILE *f = fopen("map-cache/tiles.txt", "r"); if (!f) return;
    double px, pz, e, n; unsigned long long hash; int cap = 0;
    while (fscanf(f, "%lf %lf %lf %lf %llx", &px, &pz, &e, &n, &hash) == 5) {
        double t = tileScale ? tileScale : 12 * mapScale, L = 1000 / t, x = px + e / t, z = pz - n / t; int i = 0;   /* the same image in the same place: one tile */
        while (i < ntiles && !(tiles[i].hash == hash && fabs(tiles[i].x - x) < L / 2 && fabs(tiles[i].z - z) < L / 2)) i++;
        if (i == ntiles) { if (ntiles == cap) { cap = cap ? 2 * cap : 64; tiles = realloc(tiles, sizeof *tiles * (size_t)cap); } memset(&tiles[ntiles++], 0, sizeof *tiles); }
        tiles[i].x = x; tiles[i].z = z; tiles[i].hash = hash;
    }
    fclose(f);
    for (int i = 0; i < ntiles; i++) {   /* one copy of each image: open sea repeats one tile */
        int j = 0; while (j < i && tiles[j].hash != tiles[i].hash) j++;
        if (j < i) { tiles[i].img = tiles[j].img; tiles[i].img.px = NULL; tiles[i].shared = j; continue; }
        tiles[i].shared = -1;
        char p[64]; snprintf(p, sizeof p, "map-cache/tile_%016llx.rgba", (unsigned long long)tiles[i].hash); load_image(&tiles[i].img, p);
    }
    fprintf(stderr, "[totk] bottom map: %d tiles\n", ntiles);
}
static void draw_art(canvas *c, double cx, double cy, double z) {
    if (!bgFrame || bgW != c->w || bgH != c->h) {   /* the background is screen-fixed: made once */
        free(bgFrame); bgFrame = malloc((size_t)c->w * c->h * 4); bgW = c->w; bgH = c->h;
        for (int y = 0; y < c->h; y++) for (int x = 0; x < c->w; x++) {
            const uint8_t *p = pattern.px ? pattern.px + 4 * ((size_t)(y % pattern.h) * pattern.w + (size_t)(x % pattern.w)) : NULL;
            bgFrame[(size_t)y * c->w + x] = p ? rgb(p[0], p[1], p[2]) : rgb(26, 43, 74);
        }
    }
    for (int y = 0; y < c->h; y++) memcpy(c->px + (size_t)y * c->stride, bgFrame + (size_t)y * c->w, (size_t)c->w * 4);
    double wx0 = cx - c->w / 2.0 / z, wz0 = cy - c->h / 2.0 / z;   /* the world at the screen's top left */
    double L = 1000 / (tileScale ? tileScale : 12 * mapScale);   /* a tile's side in world units */
    int covered = z >= 0.45 && ntiles;   /* the overview only where the tiles leave gaps (checked every 40 pixels) */
    for (int y = 0; covered && y <= c->h; y += 40) for (int x = 0; covered && x <= c->w; x += 40) {
        double wx = wx0 + (x < c->w ? x : c->w - 1) / z, wz = wz0 + (y < c->h ? y : c->h - 1) / z; int hit = 0;
        for (int i = 0; i < ntiles && !hit; i++) {   /* a tile whose image is not written yet covers nothing (navy holes otherwise) */
            const image *im = tiles[i].shared >= 0 ? &tiles[tiles[i].shared].img : &tiles[i].img;
            hit = im->px && fabs(wx - tiles[i].x) <= L / 2 && fabs(wz - tiles[i].z) <= L / 2;
        }
        covered = hit;
    }
    if (!covered && overview.px) draw_image(c, &overview, mapU0 + wx0 * mapScale, mapV0 + wz0 * mapScale, mapScale / z, 0, 0, c->w, c->h);
    if (z < 0.45 && overview.px) return;   /* the tiles once one of their texels spans more than about a third of a screen pixel
                                              (without the region map: always, what has been explored) */
    for (int i = 0; i < ntiles; i++) {
        const tile *t = &tiles[i]; const image *im = t->shared >= 0 ? &tiles[t->shared].img : &t->img; if (!im->px) continue;
        double k = im->w / L, tx0 = t->x - L / 2, tz0 = t->z - L / 2;   /* texels a world unit; the tile's corner */
        int sx0 = (int)floor((tx0 - wx0) * z), sy0 = (int)floor((tz0 - wz0) * z), sx1 = (int)ceil((tx0 + L - wx0) * z), sy1 = (int)ceil((tz0 + L - wz0) * z);
        if (sx1 <= 0 || sy1 <= 0 || sx0 >= c->w || sy0 >= c->h) continue;
        draw_image(c, im, (wx0 - tx0) * k, (wz0 - tz0) * k, k / z, sx0, sy0, sx1, sy1);
    }
}
static void draw_map(canvas *c, double cx, double cy, double z, double px, double py, double pdir, const float (*t)[2], int nt) {
    if (overview.px || ntiles) draw_art(c, cx, cy, z); else draw_grid(c, cx, cy, z);
    draw_marks(c, cx, cy, z);
    /* The player, in the map screen's colours: a white disc in a dark outline with a yellow pointer the way they face */
    double sx = c->w / 2.0 + (px - cx) * z, sy = c->h / 2.0 + (py - cy) * z, s = sin(pdir), k = cos(pdir);
    double ax[3] = {sx + 40 * s, sx - 16 * k + 20 * s, sx + 16 * k + 20 * s}, ay[3] = {sy - 40 * k, sy - 16 * s - 20 * k, sy + 16 * s - 20 * k};
    disc(c, sx, sy, 24, rgb(16, 32, 40)); tri(c, ax, ay, rgb(16, 32, 40));
    double bx[3] = {sx + 34 * s, sx - 11 * k + 19 * s, sx + 11 * k + 19 * s}, by[3] = {sy - 34 * k, sy - 11 * s - 19 * k, sy + 11 * s - 19 * k};
    tri(c, bx, by, rgb(255, 214, 0)); disc(c, sx, sy, 19, rgb(255, 255, 255)); disc(c, sx, sy, 7, rgb(16, 32, 40));
    for (int i = 0; i < nt; i++) disc(c, t[i][0], t[i][1], 40, rgb(250, 200, 60));   /* fingers */
}
/* The interface's layout on this screen. The game spreads it over a 16:9 TV picture, its boxes in the corners with the
   layer's navy between them; fitted to this screen's width as it is, its text came out at 0.65 of its size. So the empty
   bands are taken out: columns and rows that hold nothing that stands out from the navy (text, borders, panels; not the
   faint dark backdrop of battle messages) shrink to a gap of UI_GAP (UI_EDGE at the edges), the boxes keep their places
   relative to each other, and the result is scaled to fill the width, up to UI_MAXS of the game's size and leaving the
   map at least STRIP_MIN rows. Screens that are all interface (the X menu's screens, the map screen, the title) have no
   empty bands and fill the width as before. Recomputed for each new frame, into uiOut (the screen's width x uiOutH). */
#define UI_GAP 40
#define UI_EDGE 16
#define UI_WIDEN 8
#define UI_MAXS 1.5
#define STRIP_MIN 300
static uint32_t *uiOut; static int uiOutW, uiOutH;
static int ui_runs(const uint8_t *has, int n, int *map) {
    static uint8_t d[UI_MAXW]; int m = 0;
    for (int i = 0; i < n; i++) { d[i] = 0; for (int k = i - UI_WIDEN; k <= i + UI_WIDEN; k++) if (k >= 0 && k < n && has[k]) { d[i] = 1; break; } }
    for (int i = 0; i < n;) {
        int j = i; while (j < n && d[j] == d[i]) j++;
        if (d[i]) for (int k = i; k < j; k++) map[m++] = k;
        else { int keep = i == 0 || j == n ? UI_EDGE : UI_GAP; if (keep > j - i) keep = j - i; for (int k = 0; k < keep; k++) map[m++] = i + (j - i - keep) / 2 + k; }
        i = j;
    }
    return m;
}
static void ui_layout(const uint8_t *px, int w, int h, int cw, int ch) {
    static uint8_t colHas[UI_MAXW], rowHas[UI_MAXH]; static int mx[UI_MAXW], my[UI_MAXH];
    memset(colHas, 0, sizeof colHas); memset(rowHas, 0, sizeof rowHas);
    const int br = 30, bg = 43, bb = 72;   /* the layer's navy as it arrives (the backend clears to it) */
    long n = 0, nbg = 0;
    for (int y = 0; y < h; y += 2) for (int x = 0; x < w; x += 2) {
        const uint8_t *p = px + 4 * ((size_t)y * w + x); int d = abs(p[0] - br) + abs(p[1] - bg) + abs(p[2] - bb);
        n++; if (d <= 12) nbg++; if (d > 90) colHas[x] = rowHas[y] = 1;
    }
    int lw, lh;
    if (nbg * 10 < n * 3) { for (int i = 0; i < w; i++) mx[i] = i; for (int i = 0; i < h; i++) my[i] = i; lw = w; lh = h; }   /* all interface */
    else { lw = ui_runs(colHas, w, mx); lh = ui_runs(rowHas, h, my); }
    if (lw < 1 || lh < 1) { uiOutH = 0; return; }
    double s = (double)cw / lw; if (s > (double)(ch - STRIP_MIN) / lh) s = (double)(ch - STRIP_MIN) / lh; if (s > UI_MAXS) s = UI_MAXS;
    int ow = (int)(lw * s), oh = (int)(lh * s), ox = (cw - ow) / 2; if (ow > cw) ow = cw; if (oh > ch) oh = ch;
    if (!uiOut || uiOutW != cw) { free(uiOut); uiOut = malloc((size_t)cw * ch * 4); uiOutW = cw; }
    if (!uiOut) { uiOutH = 0; return; }
    uint32_t navy = rgb(br, bg, bb);
    static int sx0[4096], sx1[4096], wx[4096];
    for (int x = 0; x < ow && x < 4096; x++) {   /* each column's two source columns and the weight of the second (of 256) */
        double f = (x + 0.5) / s - 0.5; if (f < 0) f = 0; int k = (int)f; if (k > lw - 1) k = lw - 1;
        sx0[x] = mx[k]; sx1[x] = mx[k + 1 < lw ? k + 1 : k]; wx[x] = (int)((f - k) * 256);
    }
    for (int y = 0; y < oh; y++) {
        uint32_t *o = uiOut + (size_t)y * cw;
        for (int x = 0; x < cw; x++) o[x] = navy;
        double f = (y + 0.5) / s - 0.5; if (f < 0) f = 0; int k = (int)f; if (k > lh - 1) k = lh - 1;
        const uint8_t *r0 = px + 4 * (size_t)my[k] * w, *r1 = px + 4 * (size_t)my[k + 1 < lh ? k + 1 : k] * w; int wy = (int)((f - k) * 256);
        for (int x = 0; x < ow && x < 4096; x++) {
            const uint8_t *a = r0 + 4 * sx0[x], *b = r0 + 4 * sx1[x], *c = r1 + 4 * sx0[x], *d = r1 + 4 * sx1[x]; int u = wx[x], v[3];
            for (int i = 0; i < 3; i++) { int t = (a[i] * (256 - u) + b[i] * u) >> 8, bt = (c[i] * (256 - u) + d[i] * u) >> 8; v[i] = (t * (256 - wy) + bt * wy) >> 8; }
            o[ox + x] = rgb(v[0], v[1], v[2]);
        }
    }
    uiOutH = oh;
}
static void *render_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "bottom");
    for (;;) {
        pthread_mutex_lock(&viewLock);
        for (;;) {   /* wait for something to draw: a change, or the snap-back */
            double idle = now_s() - lastTouch;
            int following = !ntouch && idle >= SNAP_S;
            int off = following && (fabs(camX - playerX) > 0.5 / zoom || fabs(camY - playerY) > 0.5 / zoom || fabs(zoom - ZOOM_HOME) > 0.002);
            int uiNow = now_s() - uiTime < UI_HOLD;
            if (win && (dirty || off)) break;
            if (win && (uiNow || (uiShown && !uiNow))) {   /* the interface may stop: look again shortly */
                if (uiShown && !uiNow) break;
                struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_nsec += 100000000; if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
                pthread_cond_timedwait(&viewCond, &viewLock, &t); continue;
            }
            if (win && !ntouch && idle < SNAP_S) {   /* wake when the snap-back is due */
                struct timespec t; clock_gettime(CLOCK_REALTIME, &t); double w = SNAP_S - idle + 0.01;
                t.tv_sec += (time_t)w; t.tv_nsec += (long)((w - floor(w)) * 1e9); if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
                pthread_cond_timedwait(&viewCond, &viewLock, &t);
            } else pthread_cond_wait(&viewCond, &viewLock);
        }
        if (!ntouch && now_s() - lastTouch >= SNAP_S) {   /* glide back to the player, a fifth of the way per frame */
            camX += (playerX - camX) * 0.2; camY += (playerY - camY) * 0.2; zoom += (ZOOM_HOME - zoom) * 0.2;
        }
        if (assetsChanged) {   /* the backend saved new art or a calibration sample */
            assetsChanged = 0; pthread_mutex_unlock(&viewLock);
            load_image(&overview, "map-cache/overview.rgba"); load_image(&pattern, "map-cache/background.rgba"); load_calibration(); load_marks(); load_tiles();
            free(bgFrame); bgFrame = NULL;
            pthread_mutex_lock(&viewLock);
        }
        double cx = camX, cy = camY, z = zoom, px = playerX, py = playerY, pd = playerDir;
        float t[2][2]; int nt = ntouch; for (int i = 0; i < nt; i++) { t[i][0] = touch[i].x; t[i][1] = touch[i].y; }
        ANativeWindow *w = win; winBusy = 1; dirty = 0;
        int fresh = uiReady >= 0; if (fresh) { uiDraw = uiReady; uiReady = -1; }   /* uiBuf[uiDraw] is ours until the next one */
        int ui = now_s() - uiTime < UI_HOLD && uiDraw >= 0; uiShown = ui;
        const uint8_t *upx = uiDraw >= 0 ? uiBuf[uiDraw] : NULL; int uw = uiW, uh = uiH;
        pthread_mutex_unlock(&viewLock);
        ANativeWindow_Buffer b;
        if (ANativeWindow_lock(w, &b, NULL) == 0) {
            canvas c = {b.bits, b.width, b.height, b.stride};
            double t0 = now_s();
            if (ui && (fresh || uiOutW != c.w)) ui_layout(upx, uw, uh, c.w, c.h);
            if (ui && uiOutH > 0) {   /* the interface at the top, the map below it */
                int rows = uiOutH < c.h ? uiOutH : c.h;
                for (int y = 0; y < rows; y++) memcpy(c.px + (size_t)y * c.stride, uiOut + (size_t)y * uiOutW, (size_t)c.w * 4);
                canvas strip = {c.px + (size_t)rows * c.stride, c.w, c.h - rows, c.stride};
                if (strip.h > 0) draw_map(&strip, cx, cy, z, px, py, pd, t, 0);
            } else
            draw_map(&c, cx, cy, z, px, py, pd, t, nt);
            ANativeWindow_unlockAndPost(w);
            static double sum; static int n; sum += now_s() - t0;
            if (++n == 200) { fprintf(stderr, "[totk] bottom map: %.1f ms a drawing (last 200)\n", sum / n * 1000); sum = 0; n = 0; }
        }
        pthread_mutex_lock(&viewLock); winBusy = 0; pthread_cond_broadcast(&viewCond); pthread_mutex_unlock(&viewLock);
        usleep(nt || ui ? 16000 : 100000);   /* ~60 drawings a second under a finger or with the interface, 10 when only following */
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
