/* Tears of the Kingdom abilities in Breath of the Wild 1.6.0 (docs/botw-abilities.md). Off unless TOTK_ABILITIES=1.
   So far: Ascend, Recall and a HUD line with the player, the ceiling above him, the camera and the buttons.

   Addresses (1.6.0 main, found and checked with ThorNative/botw-abilities/tools/botw160.py, 2026-10-26):
     PlayerInfo::sInstance          main+0x2ca1140   +0x60 the player actor
     Actor::mMtx                    actor+0x398      3x4 row-major, translation at [3], [7], [11]
     camera                         [[main+0x2ce1090]+0x60]+0x20 {position, look-at, up, ...} (world_survey.c)
     player's Havok body            [[main+0x2d37af0]+0xf0]+0xd28 (world_survey.c); +0x650 his linear velocity (found
                                    in a rise: y 0 -> -20.6 after 1 s -> -60, the cap, at 3 s)
     phys::System::sInstance        main+0x2d20f28   +0x30 the worlds per layer type, each +0x8 its hkpWorld
     RayCast::worldRayCast          main+0x18ffa10   bool (RayCast *, ContactLayerType), 0 = entity layers
     RayCast vtable                 main+0x2beaeb0   onRigidBodyHit is a bare ret
     Havok monitor stream TLS key   [main+0x2c81448], read with the nn::os::GetTlsValue stub at main+0x2127fb0
   The RayCast object (0xe0 bytes) is filled as the game's own water-height helper at main+0x44aa30 fills its copy on the
   stack; the hit point is (1 - f) from + f to with f at +0x40.

   Threads. hkpWorld::castRay needs Havok's per-thread monitor stream, which only threads Havok was set up on have: the
   game's main thread (0x8001 in a test, 2026-10-26), which also polls the controller, has it; the present's thread
   (0x20004) does not, and a cast from there crashed at main+0x2079fa0. So everything that casts or moves the player runs
   in the controller poll (botw_abilities_filter_buttons, once a game frame on the main thread, through the HLE gate,
   which takes the nested entry the game code's own host calls need), and the present only draws the HUD line from what
   the poll left. */
#include "botw_abilities.h"
#include "game_version.h"
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(GAME_BOTW160)
enum { PLAYER_INFO = 0x2ca1140, PI_ACTOR = 0x60, ACTOR_MTX = 0x398,
       CAMERA_ROOT = 0x2ce1090, CAMERA_STEP1 = 0x60, CAMERA_OFF = 0x20,
       HAVOK_ROOT = 0x2d37af0, HAVOK_STEP1 = 0xf0, HAVOK_STEP2 = 0xd28, HAVOK_VELOCITY = 0x650,
       PHYS_SYSTEM = 0x2d20f28, WORLD_RAY_CAST = 0x18ffa10, RAY_CAST_VTABLE = 0x2beaeb0, MATERIAL_MASK_VTABLE = 0x2bdfc58,
       HAVOK_TLS_KEY = 0x2c81448, GET_TLS_VALUE = 0x2127fb0 };
/* ContactLayer (decomp physDefines.h): EntityGroundObject 2, EntityGround 8, EntityGroundSmooth 9, EntityGroundRough 10,
   EntityWallForClimb 15: what a ceiling is made of. */
enum { CEILING_LAYERS = 1u << 2 | 1u << 8 | 1u << 9 | 1u << 10 | 1u << 15 };
enum { PAD_B = 2, PAD_L = 64, PAD_ZL = 256 };
#define HEAD 1.8f            /* rays start this far above his feet: inside his head, under any roof he stands below */
#define ASCEND_SPEED 4.0f    /* m/s */
#define ASCEND_LIFT 1.0f     /* he ends this far above the top and drops onto it */
#define ASCEND_SETTLE 0.25   /* s held there first */
static float g_reach = 12, g_thick = 64;   /* TOTK_ASCEND_REACH: the farthest a ceiling can be above his head; TOTK_ASCEND_THICK: the thickest one */
static uint8_t *g_main;
static int g_on = -1, g_ray = 1;

static int read_mem(uintptr_t address, void *out, size_t size) {
    if (!address || address > UINTPTR_MAX - size) return 0;
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}
static uintptr_t read_ptr(uintptr_t address) { uintptr_t p = 0; return read_mem(address, &p, sizeof p) ? p : 0; }
static int place(const float v[3]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) && fabsf(v[0]) <= 6000 && v[1] >= -1000 && v[1] <= 3000 && fabsf(v[2]) <= 6000;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_UPTIME_RAW, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* The player actor and his feet (the world matrix's translation), or 0. */
static uintptr_t player(float at[3]) {
    uintptr_t info = read_ptr((uintptr_t)g_main + PLAYER_INFO), actor = info ? read_ptr(info + PI_ACTOR) : 0;
    float m[12];
    if (!actor || !read_mem(actor + ACTOR_MTX, m, sizeof m)) return 0;
    at[0] = m[3]; at[1] = m[7]; at[2] = m[11];
    return place(at) ? actor : 0;
}

/* The Havok world of the entity layers, [[System + 0x30] + 0] + 0x8: 0 until a world scene is up. */
static int world_up(void) {
    uintptr_t sys = read_ptr((uintptr_t)g_main + PHYS_SYSTEM), worlds = sys ? read_ptr(sys + 0x30) : 0, entity = worlds ? read_ptr(worlds) : 0;
    return entity && read_ptr(entity + 8);
}
/* Havok's per-thread monitor stream (key at [main+0x2c81448], main+0x2127fb0 the nn::os::GetTlsValue stub): what
   hkpWorld::castRay writes its timer through first. */
static void *havok_thread(void) {
    uintptr_t keyp = read_ptr((uintptr_t)g_main + HAVOK_TLS_KEY); uint32_t key;
    if (!keyp || !read_mem(keyp, &key, sizeof key)) return NULL;
    return ((void *(*)(uint32_t))(g_main + GET_TLS_VALUE))(key);
}

/* The game's ray cast from `from` to `to` on the entity layers in `layers`: 1 and the hit point, or 0. Only on a thread
   with a Havok stream and a world up (the callers check). g_hit_body: the Havok body hit (RayCast +0x48 is the hit
   hkpCollidable, which sits at +0x20 in its hkpEntity). */
static uintptr_t g_hit_body;
static int ray_cast(const float from[3], const float to[3], uint32_t layers, float hit[3]) {
    g_hit_body = 0;
    _Alignas(16) uint8_t ray[0xf0] = {0};
    uint64_t v;
    v = (uint64_t)(g_main + RAY_CAST_VTABLE); memcpy(ray, &v, 8);
    memcpy(ray + 0x08, from, 12);
    memcpy(ray + 0x14, to, 12);
    uint32_t ground_hit = 0xf, ignored_ground_hit = 0x10; float fraction = -1.0f;
    memcpy(ray + 0x28, &ground_hit, 4);
    memcpy(ray + 0x40, &fraction, 4);
    memcpy(ray + 0x68, &layers, 4);                                     /* the entity mask; +0x6c the sensor one stays 0 */
    v = (uint64_t)(g_main + MATERIAL_MASK_VTABLE); memcpy(ray + 0x78, &v, 8);
    v = 4ull << 32; memcpy(ray + 0xa0, &v, 8);                          /* ignored groups: size 0, capacity 4 */
    v = (uint64_t)(ray + 0xb0); memcpy(ray + 0xa8, &v, 8);              /* ... in the buffer after them */
    memcpy(ray + 0xd8, &ignored_ground_hit, 4);
    bool (*world_ray_cast)(void *, int) = (bool (*)(void *, int))(g_main + WORLD_RAY_CAST);
    if (!world_ray_cast(ray, 0)) return 0;
    memcpy(&fraction, ray + 0x40, 4);
    if (!(fraction >= 0 && fraction <= 1)) return 0;
    for (int c = 0; c < 3; c++) hit[c] = (1 - fraction) * from[c] + fraction * to[c];
    uintptr_t collidable; memcpy(&collidable, ray + 0x48, 8);
    g_hit_body = collidable ? collidable - 0x20 : 0;
    return 1;
}

static void fx(uintptr_t actor, const char *effect, const char *sound);   /* the abilities' look and sound, below */

/* Ascend (A2). Ready when a ray from his head finds a ceiling within g_reach and, within g_thick above it, a top with
   room to stand (12 and 64 m; TOTK_ASCEND_REACH, TOTK_ASCEND_THICK). A ray from inside a surface does not hit it, so tops
   are found from above (ascend_plan). ZL + L while ready (L is then kept from the game, so no rune fires) rises at
   ASCEND_SPEED and stands him on top; B takes him back down.
   Moving him is world_survey.c's tested way (survey move): every float triple within 2 m of him in the player actor, his
   Havok body and its parent, the camera's position and look-at, and the world matrix's translation keep their offset
   from him and are written every game frame, so physics and the camera follow instead of pulling him back; his Havok
   velocity is zeroed with them. */
static _Atomic int g_ready, g_phase_seen;   /* for the HUD */
static _Atomic float g_ceiling = -1, g_rise, g_rise_total;
static int g_phase;   /* 0 idle, 1 rising, 2 going back */
static float g_under, g_top;
static struct { uintptr_t at; float rel[3]; } g_slot[160];
static unsigned g_nslot;
static uintptr_t g_matrix, g_velocity;
static float g_from[3], g_to_y, g_now_y, g_back_from_y;
static double g_t0, g_dur;

static const char *g_why = "";   /* why the last plan found no way up */
static float g_why_at[2];        /* and the heights it was about */
/* Up through stacked layers (a shrine's shell, rock, terrain): from an underside, the top above it is the first surface
   a ray down from 1, 2, 4 ... m over it meets; when there is no room to stand on that top, the surface that is in the
   way is the next underside, until there is room or the climb would exceed g_thick. */
static int ascend_plan(const float at[3], float *ceiling, float *under, float *top) {
    float from[3] = {at[0], at[1] + HEAD, at[2]}, to[3] = {at[0], at[1] + HEAD + g_reach, at[2]}, hit[3], down[3], room[3];
    *ceiling = -1;
    if (!ray_cast(from, to, CEILING_LAYERS, hit)) { g_why = "no ceiling within reach"; return 0; }
    *ceiling = hit[1] - at[1];
    float base = hit[1], limit = hit[1] + g_thick;
    for (int layer = 0; layer < 8; layer++) {
        int found = 0;
        for (float d = 1; !found && base + 0.05f < limit; d *= 2) {
            float a[3] = {at[0], fminf(base + d, limit), at[2]}, b[3] = {at[0], base + 0.02f, at[2]};
            found = ray_cast(a, b, CEILING_LAYERS, down) && down[1] > base + 0.05f;
            if (base + d >= limit) break;
        }
        if (!found) { g_why = "the ceiling is thicker than TOTK_ASCEND_THICK"; g_why_at[0] = base; g_why_at[1] = limit; return 0; }
        float c[3] = {at[0], down[1] + 0.1f, at[2]}, e[3] = {at[0], down[1] + 0.1f + HEAD, at[2]};
        if (!ray_cast(c, e, CEILING_LAYERS, room)) { *under = hit[1]; *top = down[1]; return 1; }
        g_why = "no room on any layer"; g_why_at[0] = down[1]; g_why_at[1] = room[1];
        base = room[1];
    }
    return 0;
}

/* Every float triple near `cur` in the player's blocks, as survey move collects them; 0 without a Havok body. */
static int collect(uintptr_t actor, const float cur[3]) {
    uintptr_t hk1 = read_ptr((uintptr_t)g_main + HAVOK_ROOT); if (hk1) hk1 = read_ptr(hk1 + HAVOK_STEP1);
    uintptr_t hk = hk1 ? read_ptr(hk1 + HAVOK_STEP2) : 0;
    uintptr_t cam = read_ptr((uintptr_t)g_main + CAMERA_ROOT); cam = cam ? read_ptr(cam + CAMERA_STEP1) : 0;
    struct { uintptr_t base; size_t size; float window; } blocks[4] = {{actor, 0x4000, 2}, {hk, 0x1000, 2}, {hk1, 0x1000, 2}, {cam ? cam + CAMERA_OFF : 0, 0x18, 12}};
    static float buf[0x1000];
    g_nslot = 0;
    for (unsigned b = 0; b < 4; b++) {
        if (!blocks[b].base || !read_mem(blocks[b].base, buf, blocks[b].size)) continue;
        for (size_t k = 0; k + 3 <= blocks[b].size / 4 && g_nslot < 150; k++)
            if (fabsf(buf[k] - cur[0]) < blocks[b].window && fabsf(buf[k + 1] - cur[1]) < blocks[b].window && fabsf(buf[k + 2] - cur[2]) < blocks[b].window) {
                g_slot[g_nslot].at = blocks[b].base + k * 4;
                for (int c = 0; c < 3; c++) g_slot[g_nslot].rel[c] = buf[k + c] - cur[c];
                g_nslot++; k += 2;
            }
    }
    g_matrix = actor + ACTOR_MTX;
    g_velocity = hk ? hk + HAVOK_VELOCITY : 0;
    return hk != 0;
}
static void put_player(float y) {
    float at[3] = {g_from[0], y, g_from[2]};
    for (unsigned k = 0; k < g_nslot; k++) {
        float v[3] = {at[0] + g_slot[k].rel[0], at[1] + g_slot[k].rel[1], at[2] + g_slot[k].rel[2]};
        memcpy((void *)g_slot[k].at, v, sizeof v);
    }
    float *m = (float *)g_matrix; m[3] = at[0]; m[7] = at[1]; m[11] = at[2];
    /* No speed: held in place, gravity still adds to his velocity every step, and the 60 m/s he had at the top took
       him through the ground he was put on (2026-10-26). */
    if (g_velocity) memset((void *)g_velocity, 0, 12);
    g_now_y = y;
}
/* TOTK_ASCEND_DUMP=<folder>: the player's blocks at moments of a rise, to find what else holds his motion. */
static void dump(const char *label) {
    const char *dir = getenv("TOTK_ASCEND_DUMP"); if (!dir || !g_nslot) return;
    uintptr_t hk1 = read_ptr((uintptr_t)g_main + HAVOK_ROOT); if (hk1) hk1 = read_ptr(hk1 + HAVOK_STEP1);
    uintptr_t hk = hk1 ? read_ptr(hk1 + HAVOK_STEP2) : 0, actor = g_matrix - ACTOR_MTX;
    struct { const char *name; uintptr_t base; size_t size; } b[3] = {{"actor", actor, 0x4000}, {"havok", hk, 0x1000}, {"havok1", hk1, 0x1000}};
    static uint8_t buf[0x4000];
    for (int i = 0; i < 3; i++) {
        if (!b[i].base || !read_mem(b[i].base, buf, b[i].size)) continue;
        char path[1024]; snprintf(path, sizeof path, "%s/%s-%s.bin", dir, label, b[i].name);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(buf, 1, b[i].size, f); fclose(f); }
    }
    fprintf(stderr, "[abilities] ascend: dumped %s (actor %#lx, havok %#lx, havok1 %#lx)\n", label, (unsigned long)actor, (unsigned long)hk, (unsigned long)hk1);
}
static float ease(float t) { return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t); }
static void stop(const char *why) {
    if (g_phase) fprintf(stderr, "[abilities] ascend: %s\n", why);
    g_phase = 0; atomic_store(&g_phase_seen, 0);
}

/* One game frame of Ascend, on the main thread; returns the buttons the game gets. */
static int g_after;   /* TOTK_ASCEND_DUMP: frames left to log after an arrival */
static uint64_t ascend_frame(uint64_t buttons, uint64_t pressed) {
    float at[3];
    uintptr_t actor = player(at);
    if (g_after > 0 && actor) {
        float v[3] = {0}; if (g_velocity) read_mem(g_velocity, v, sizeof v);
        if (g_after-- % 3 == 0) fprintf(stderr, "[abilities] ascend: after, y %.2f, velocity %.2f %.2f %.2f\n", at[1], v[0], v[1], v[2]);
    }
    if (!actor || !world_up()) { stop("the player went away"); atomic_store(&g_ready, 0); atomic_store(&g_ceiling, -1); return buttons; }
    if (g_phase == 0) {
        static unsigned frame; static int ready; float ceiling;
        if (frame++ % 3 == 0 && havok_thread()) {
            ready = ascend_plan(at, &ceiling, &g_under, &g_top);
            atomic_store(&g_ready, ready); atomic_store(&g_ceiling, ceiling);
        }
        static double asked; static int waiting;   /* ZL + L taken, his Havok body not found yet */
        if (!waiting) {
            if ((buttons & PAD_ZL) && (pressed & PAD_L) && !ready) fprintf(stderr, "[abilities] ascend: not here: %s (ceiling %+.1f m; %.2f, %.2f)\n", g_why, atomic_load(&g_ceiling), g_why_at[0], g_why_at[1]);
            if (!((buttons & PAD_ZL) && (buttons & PAD_L) && ready)) return buttons;
            buttons &= ~(uint64_t)PAD_L;
            if (!(pressed & PAD_L)) return buttons;
            asked = now_s(); waiting = 1;
        }
        buttons &= ~(uint64_t)PAD_L;
        if (!collect(actor, at)) {   /* the body's chain is empty at times: try for half a second */
            if (now_s() - asked < 0.5) return buttons;
            uintptr_t hk1 = read_ptr((uintptr_t)g_main + HAVOK_ROOT), hk1b = hk1 ? read_ptr(hk1 + HAVOK_STEP1) : 0;
            fprintf(stderr, "[abilities] ascend: no Havok body ([main+%#x] %#lx, +%#x %#lx, +%#x %#lx), not moving\n", HAVOK_ROOT, (unsigned long)hk1,
                    HAVOK_STEP1, (unsigned long)hk1b, HAVOK_STEP2, (unsigned long)(hk1b ? read_ptr(hk1b + HAVOK_STEP2) : 0));
            waiting = 0; return buttons;
        }
        waiting = 0;
        memcpy(g_from, at, sizeof g_from); g_to_y = g_top + ASCEND_LIFT; g_t0 = now_s();
        g_dur = fmax(0.8, (g_to_y - at[1]) / ASCEND_SPEED);
        g_phase = 1; atomic_store(&g_ready, 0);
        fprintf(stderr, "[abilities] ascend: %.2f -> %.2f (ceiling underside %.2f) over %.2f s, %u copies of his position\n", at[1], g_to_y, g_under, g_dur, g_nslot);
        fx(actor, "Demo005_0_warp", "Kago_WindRiseUp");   /* the Sheikah warp's shimmer, Revali's Gale's rise */
        dump("t0");
        if (getenv("TOTK_ASCEND_DUMP"))   /* which layers make up what is above him */
            for (int layer = 0; layer < 20; layer++) {
                float a[3] = {at[0], at[1] + 60, at[2]}, b[3] = {at[0], at[1] - 1, at[2]}, h[3], y[6]; int n = 0;
                while (n < 6 && a[1] > b[1] && ray_cast(a, b, 1u << layer, h)) { y[n++] = h[1]; a[1] = h[1] - 0.05f; }
                if (n) fprintf(stderr, "[abilities] ascend: layer %d from above: %d hits, %.2f %.2f %.2f %.2f %.2f %.2f\n", layer, n, y[0], n > 1 ? y[1] : 0, n > 2 ? y[2] : 0, n > 3 ? y[3] : 0, n > 4 ? y[4] : 0, n > 5 ? y[5] : 0);
            }
    }
    { static int d1, d3; double e = now_s() - g_t0;
      if (g_phase == 1 && e < 0.1) d1 = d3 = 0;
      if (g_phase == 1 && e >= 1 && !d1) { d1 = 1; dump("t1"); }
      if (g_phase == 1 && e >= 3 && !d3) { d3 = 1; dump("t3"); } }
    double t = (now_s() - g_t0) / g_dur;
    if (g_phase == 1 && (pressed & PAD_B)) {
        g_phase = 2; g_back_from_y = g_now_y; g_t0 = now_s(); g_dur = 0.35; t = 0;
        fprintf(stderr, "[abilities] ascend: cancelled at %.2f, back to %.2f\n", g_back_from_y, g_from[1]);
        fx(actor, NULL, "Demo_SheikPadTouchOff");
    }
    if (g_phase == 1) put_player(g_from[1] + (g_to_y - g_from[1]) * ease((float)t));
    else put_player(g_back_from_y + (g_from[1] - g_back_from_y) * ease((float)t));
    { static double glow; if (g_phase == 1 && t > 0.05 && t < 1 && now_s() - glow > 1.2) { glow = now_s(); fx(actor, "Demo005_0_warp", NULL); } }   /* the glow lasts the rise */
    atomic_store(&g_rise, g_now_y - g_from[1]); atomic_store(&g_rise_total, g_to_y - g_from[1]); atomic_store(&g_phase_seen, g_phase);
    /* Held at the end a moment longer: Havok kept the contacts of his pass through the ceiling, which push him back
       down through the top when he is let go on it at once (he fell from rest through the ground he stood on,
       2026-10-26); a metre above it they expire, then he drops onto it. */
    if (t >= 1 + ASCEND_SETTLE / g_dur) {
        fprintf(stderr, "[abilities] ascend: %s at %.2f (the matrix says %.2f)\n", g_phase == 1 ? "arrived" : "back", g_now_y, player(at) ? at[1] : NAN);
        dump("end"); if (getenv("TOTK_ASCEND_DUMP")) g_after = 120;
        if (g_phase == 1) fx(actor, "Demo005_1_warp_appear", "Demo005_1_warp_appear");   /* the warp's arrival */
        g_phase = 0; atomic_store(&g_phase_seen, 0);
    }
    return 0;   /* he does nothing else while he rises */
}

/* Effects and sounds (A3b), through the actor's xlink as the game's own XLinkEventCreateAction::oneShot_ does it
   (main+0x15481e0): actor +0x568 its ksys xlink, whose +0x48 is the ELink (effects) user instance and +0x50 the SLink
   (sounds) one; a 0x18-byte xlink2::Locator {vtable main+0x2a6e688, 0, 0}; xlink2::UserInstance's searchAsset(locator,
   name) at main+0x1346600 says whether the user has that event and sets the locator to it, and emit(locator, handle)
   at main+0x13462b0 starts it. Main thread only (the controller poll), as for the ray casts. */
enum { ACTOR_XLINK = 0x568, XLINK_ELINK = 0x48, XLINK_SLINK = 0x50,
       XLINK_LOCATOR_VTABLE = 0x2a6e688, XLINK_SEARCH_ASSET = 0x1346600, XLINK_EMIT = 0x13462b0 };
typedef struct { uintptr_t event; uint32_t id, pad; } XlinkHandle;
/* Event `name` on `actor`'s effects (sound 0) or sounds (1): 1 when the user has it and it started. */
static int xlink_emit(uintptr_t actor, int sound, const char *name, XlinkHandle *handle) {
    uintptr_t xl = actor ? read_ptr(actor + ACTOR_XLINK) : 0, user = xl ? read_ptr(xl + (sound ? XLINK_SLINK : XLINK_ELINK)) : 0;
    if (!user) return 0;
    _Alignas(16) uint8_t locator[0x20] = {0};
    uint64_t v = (uint64_t)(g_main + XLINK_LOCATOR_VTABLE); memcpy(locator, &v, 8);
    if (!((bool (*)(uintptr_t, void *, const char *))(g_main + XLINK_SEARCH_ASSET))(user, locator, name)) return 0;
    XlinkHandle h = {0};
    ((bool (*)(uintptr_t, void *, XlinkHandle *))(g_main + XLINK_EMIT))(user, locator, &h);
    if (handle) *handle = h;
    return h.event != 0;
}
/* The actor a Havok body belongs to, or 0: hkpEntity +0x18 (m_userData) is its ksys phys::RigidBody, which points back
   at it at +0x78 (mHkBody) and has its UserTag at +0x80; an actor's tag (act::PhysicsUserTag) has the actor at +0x8.
   Taken only when that actor stands within 20 m of the body. */
static uintptr_t body_actor(uintptr_t body) {
    uintptr_t rb = read_ptr(body + 0x18), tag = rb && read_ptr(rb + 0x78) == body ? read_ptr(rb + 0x80) : 0, actor = tag ? read_ptr(tag + 8) : 0;
    float m[12], c[3];
    if (!actor || !read_ptr(actor + ACTOR_XLINK) || !read_mem(actor + ACTOR_MTX, m, sizeof m) || !read_mem(body + 0x1c0, c, sizeof c)) return 0;
    return fabsf(m[3] - c[0]) < 20 && fabsf(m[7] - c[1]) < 20 && fabsf(m[11] - c[2]) < 20 ? actor : 0;
}
/* The abilities' look and sound go 1:1 with Tears of the Kingdom's own assets (BOTW_ABILITIES_SCOPE.md, A3b); until those
   are in, TOTK_ABILITY_FX=botw plays BotW events that stand in for them, for tests of the plumbing only: `effect` and
   `sound` on `actor`, either may be NULL; the log says which the actor had. */
static int g_fx;
static void fx(uintptr_t actor, const char *effect, const char *sound) {
    if (!g_fx || !actor) return;
    int e = effect ? xlink_emit(actor, 0, effect, NULL) : -1, so = sound ? xlink_emit(actor, 1, sound, NULL) : -1;
    fprintf(stderr, "[abilities] fx on %#lx: %s %s, %s %s\n", (unsigned long)actor, effect ? effect : "-", e < 0 ? "" : e ? "(played)" : "(not there)", sound ? sound : "-", so < 0 ? "" : so ? "(played)" : "(not there)");
}

/* TOTK_XLINK_LIST=<file of names>, tests: every name in it `actor`'s effects or sounds have, at once (some ms). */
static void xlink_list(uintptr_t actor, const char *what) {
    FILE *f = fopen(getenv("TOTK_XLINK_LIST"), "r"); if (!f) return;
    uintptr_t xl = read_ptr(actor + ACTOR_XLINK), users[2] = {xl ? read_ptr(xl + XLINK_ELINK) : 0, xl ? read_ptr(xl + XLINK_SLINK) : 0};
    char line[256]; int n = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0; n++;
        for (int u = 0; u < 2; u++) {
            _Alignas(16) uint8_t locator[0x20] = {0}; uint64_t v = (uint64_t)(g_main + XLINK_LOCATOR_VTABLE); memcpy(locator, &v, 8);
            if (users[u] && ((bool (*)(uintptr_t, void *, const char *))(g_main + XLINK_SEARCH_ASSET))(users[u], locator, line)) { found++; fprintf(stderr, "[abilities] xlink: %s has %s %s\n", what, u ? "sound" : "effect", line); }
        }
    }
    fclose(f); fprintf(stderr, "[abilities] xlink: %s: %d of %d names\n", what, found, n);
}

/* TOTK_XLINK_PROBE="name,name,...": after ZL + Down, which of the names Link's effects and sounds have (each one found
   is started, 1.5 s apart, for screenshots). */
static void xlink_probe(uintptr_t actor, uint64_t buttons, uint64_t pressed, double now) {
    static const char *list; static char buf[4096], *names[128]; static int n = -1, next, go; static double at;
    if (n < 0) {
        n = 0; if (!(list = getenv("TOTK_XLINK_PROBE"))) return;
        snprintf(buf, sizeof buf, "%s", list);
        for (char *t = strtok(buf, ","); t && n < 128; t = strtok(NULL, ",")) names[n++] = t;
    }
    if (!go && n && (buttons & 256) && (pressed & 0x8000)) { go = 1; at = now; }
    /* TOTK_XLINK_LIST=<file of names>: with the probe started, which of them Link's two users have (no emit; 1000 a frame) */
    static FILE *lf; static int lchecked;
    if (go == 1 && !lf && getenv("TOTK_XLINK_LIST")) { lf = fopen(getenv("TOTK_XLINK_LIST"), "r"); go = 2; }
    if (lf) {
        uintptr_t xl = read_ptr(actor + ACTOR_XLINK), users[2] = {xl ? read_ptr(xl + XLINK_ELINK) : 0, xl ? read_ptr(xl + XLINK_SLINK) : 0};
        char line[256];
        for (int i = 0; i < 1000 && fgets(line, sizeof line, lf); i++) {
            line[strcspn(line, "\r\n")] = 0; lchecked++;
            for (int u = 0; u < 2; u++) {
                _Alignas(16) uint8_t locator[0x20] = {0}; uint64_t v = (uint64_t)(g_main + XLINK_LOCATOR_VTABLE); memcpy(locator, &v, 8);
                if (users[u] && ((bool (*)(uintptr_t, void *, const char *))(g_main + XLINK_SEARCH_ASSET))(users[u], locator, line))
                    fprintf(stderr, "[abilities] xlink: Link has %s %s\n", u ? "sound" : "effect", line);
            }
        }
        if (feof(lf)) { fclose(lf); lf = NULL; fprintf(stderr, "[abilities] xlink: list done, %d names checked\n", lchecked); }
        return;
    }
    if (!go || next >= n || now < at) return;
    const char *name = names[next++]; at = now + 1.5;
    XlinkHandle h;
    int e = xlink_emit(actor, 0, name, &h), so = xlink_emit(actor, 1, name, &h);
    fprintf(stderr, "[abilities] xlink: %-32s effect %s, sound %s\n", name, e ? "yes" : "no", so ? "yes" : "no");
}

/* Recall (A3). Every game frame the moving Havok bodies near him are recorded: the world's active simulation islands
   (hkpWorld +0x40) and their bodies (island +0x60), each body's centre of mass and rotation (layouts from the decomp's
   Havok headers, lib/hkStubs, by offsetof: hkpEntity +0x150 its motion, motion +0x20 its motion state, whose transform's
   rotation columns are at body +0x170 and whose current centre of mass is at body +0x1c0; linear and angular velocity at
   +0x230 / +0x240; the motion type byte at +0x15c). They check out against the player's body, which world_survey.c found
   at +0x420 in its block: his centre of mass at block +0x5e0 and the velocity A2 found at +0x650.
   Holding ZL aims at the body at the middle of the screen (the camera's look ray); R recalls it: it runs its recorded
   path backwards at the speed it went, driven like a keyframed body (each frame its velocities are set to reach the next
   point), so physics carries it and anything on it, Link included. R again, or the end of its record, lets it go. */
enum { HK_WORLD_ACTIVE_ISLANDS = 0x40, HK_ISLAND_ENTITIES = 0x60, HK_ENTITY_WORLD = 0x10, HK_ENTITY_MOTION_TYPE = 0x15c,
       HK_ENTITY_ROTATION = 0x170, HK_ENTITY_CENTER = 0x1c0, HK_ENTITY_LINEAR = 0x230, HK_ENTITY_ANGULAR = 0x240,
       PLAYER_BODY_IN_BLOCK = 0x420,
       HK_WORLD_LOCK = 0x207830c, HK_WORLD_UNLOCK = 0x2078338, HK_ENTITY_ACTIVATE = 0x2087e0c };
enum { PAD_R = 128 };
/* ContactLayer EntityObject 0, EntitySmallObject 1, EntityGroundObject 2: what can be recalled. */
enum { RECALL_LAYERS = 1u << 0 | 1u << 1 | 1u << 2 };
#define RECALL_SECONDS 30.0
#define RECALL_RANGE 40.0f   /* m from the camera */
#define RECALL_NEAR 60.0f    /* bodies recorded within this of him */
#define RECALL_CONE 15.0f    /* degrees off the look direction the aim takes a body */
enum { TRACKS = 64, SAMPLES = 1024 };
typedef struct { double t; float c[3], q[4]; } Sample;
typedef struct { uintptr_t body; double seen; unsigned head, n; Sample s[SAMPLES]; } Track;
static Track *g_track;   /* TRACKS of them, allocated at init */
static _Atomic uintptr_t g_aim;           /* the body under the aim while ZL is held, for the HUD */
static _Atomic float g_aim_seconds, g_aim_distance, g_recall_left;
static _Atomic int g_recalling;
static Track *g_rt;   /* the body being recalled */
static int g_see_all;   /* TOTK_RECALL_SEE_ALL=1 (tests): the aim takes bodies out of sight too */
static int g_recall_debug;   /* TOTK_RECALL_DEBUG=1, below */
static double g_rt_play, g_rt_last;

static void quat_from_columns(const float *m, float q[4]) {   /* m: three hkVector4 columns, 4 floats apart */
    float r00 = m[0], r10 = m[1], r20 = m[2], r01 = m[4], r11 = m[5], r21 = m[6], r02 = m[8], r12 = m[9], r22 = m[10];
    float tr = r00 + r11 + r22, s;
    if (tr > 0) { s = sqrtf(tr + 1) * 2; q[3] = s / 4; q[0] = (r21 - r12) / s; q[1] = (r02 - r20) / s; q[2] = (r10 - r01) / s; }
    else if (r00 > r11 && r00 > r22) { s = sqrtf(1 + r00 - r11 - r22) * 2; q[3] = (r21 - r12) / s; q[0] = s / 4; q[1] = (r01 + r10) / s; q[2] = (r02 + r20) / s; }
    else if (r11 > r22) { s = sqrtf(1 + r11 - r00 - r22) * 2; q[3] = (r02 - r20) / s; q[0] = (r01 + r10) / s; q[1] = s / 4; q[2] = (r12 + r21) / s; }
    else { s = sqrtf(1 + r22 - r00 - r11) * 2; q[3] = (r10 - r01) / s; q[0] = (r02 + r20) / s; q[1] = (r12 + r21) / s; q[2] = s / 4; }
}
static void qmul(const float a[4], const float b[4], float o[4]) {   /* x y z w */
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static int dynamic_body(uintptr_t body) {   /* hkpMotion::MotionType: 1 dynamic, 2 sphere and 3 box inertia, 6 thin box */
    uint8_t type; return read_mem(body + HK_ENTITY_MOTION_TYPE, &type, 1) && (type == 1 || type == 2 || type == 3 || type == 6);
}
static Track *track_of(uintptr_t body) {
    for (int i = 0; i < TRACKS; i++) if (g_track[i].body == body) return &g_track[i];
    return NULL;
}
static const Sample *newest(const Track *k) { return &k->s[(k->head + SAMPLES - 1) % SAMPLES]; }
static const Sample *nth_back(const Track *k, unsigned i) { return &k->s[(k->head + SAMPLES - 1 - i) % SAMPLES]; }
/* Seconds of motion on record up to `until` and no older than RECALL_SECONDS before `now`: rests longer than 0.25 s
   between moves count as nothing (the recall skips them). */
static double motion_before(const Track *k, double until, double now) {
    double sum = 0;
    for (unsigned i = 0; i + 1 < k->n; i++) {
        const Sample *a = nth_back(k, i), *b = nth_back(k, i + 1);
        if (now - b->t > RECALL_SECONDS) break;
        if (a->t - b->t >= 0.25) continue;
        double hi = a->t < until ? a->t : until;
        if (hi > b->t) sum += hi - b->t;
    }
    return sum;
}
static double motion_seconds(const Track *k, double now) { return motion_before(k, now, now); }

static uintptr_t entity_world(void) {
    uintptr_t sys = read_ptr((uintptr_t)g_main + PHYS_SYSTEM), worlds = sys ? read_ptr(sys + 0x30) : 0, entity = worlds ? read_ptr(worlds) : 0;
    return entity ? read_ptr(entity + 8) : 0;
}
/* One sample of every moving dynamic body within RECALL_NEAR of him (not his own, not the one being recalled). */
static void record(const float at[3], double now) {
    uintptr_t world = entity_world();
    if (!world) return;
    uintptr_t hk1 = read_ptr((uintptr_t)g_main + HAVOK_ROOT); if (hk1) hk1 = read_ptr(hk1 + HAVOK_STEP1);
    uintptr_t hk = hk1 ? read_ptr(hk1 + HAVOK_STEP2) : 0, own = hk ? hk + PLAYER_BODY_IN_BLOCK : 0;
    struct { uintptr_t data; int32_t size, cap; } islands, bodies;
    if (!read_mem(world + HK_WORLD_ACTIVE_ISLANDS, &islands, sizeof islands) || islands.size <= 0 || islands.size > 4096) return;
    static uintptr_t isl[4096], ent[4096];
    if (!read_mem(islands.data, isl, (size_t)islands.size * 8)) return;
    for (int i = 0; i < islands.size; i++) {
        if (!read_mem(isl[i] + HK_ISLAND_ENTITIES, &bodies, sizeof bodies) || bodies.size <= 0 || bodies.size > 4096) continue;
        if (!read_mem(bodies.data, ent, (size_t)bodies.size * 8)) continue;
        for (int j = 0; j < bodies.size; j++) {
            uintptr_t b = ent[j]; float rc[16];   /* rotation columns 0..2 (+0x170), translation (+0x1a0) */
            if (b == own || (g_rt && g_rt->body == b) || !dynamic_body(b) || !read_mem(b + HK_ENTITY_ROTATION, rc, sizeof rc)) continue;
            float c[3]; if (!read_mem(b + HK_ENTITY_CENTER, c, sizeof c) || !place(c)) continue;
            if (fabsf(c[0] - at[0]) > RECALL_NEAR || fabsf(c[1] - at[1]) > RECALL_NEAR || fabsf(c[2] - at[2]) > RECALL_NEAR) continue;
            Track *k = track_of(b);
            if (!k) {   /* a new one takes the slot seen longest ago */
                k = &g_track[0];
                for (int t = 1; t < TRACKS; t++) if (g_track[t].seen < k->seen) k = &g_track[t];
                if (k == g_rt) continue;
                k->body = b; k->head = k->n = 0;
            }
            k->seen = now;
            float q[4]; quat_from_columns(rc, q);
            if (k->n) {   /* only when it moved: 1 cm or about half a degree */
                const Sample *l = newest(k);
                float dq = fabsf(l->q[0] * q[0] + l->q[1] * q[1] + l->q[2] * q[2] + l->q[3] * q[3]);
                if (fabsf(l->c[0] - c[0]) < 0.01f && fabsf(l->c[1] - c[1]) < 0.01f && fabsf(l->c[2] - c[2]) < 0.01f && dq > 0.99999f) continue;
            }
            Sample *s = &k->s[k->head]; s->t = now; memcpy(s->c, c, sizeof c); memcpy(s->q, q, sizeof q);
            k->head = (k->head + 1) % SAMPLES; if (k->n < SAMPLES) k->n++;
        }
    }
}

/* The recorded pose at play time `t` (between two samples), skipping rests: 0 when `t` is before the record. */
static int pose_at(const Track *k, double t, float c[3], float q[4]) {
    for (unsigned i = 0; i + 1 < k->n; i++) {
        const Sample *a = nth_back(k, i), *b = nth_back(k, i + 1);
        if (b->t > t) continue;
        float f = a->t > b->t ? (float)((t - b->t) / (a->t - b->t)) : 0; f = f < 0 ? 0 : f > 1 ? 1 : f;
        for (int x = 0; x < 3; x++) c[x] = b->c[x] + (a->c[x] - b->c[x]) * f;
        float d = b->q[0] * a->q[0] + b->q[1] * a->q[1] + b->q[2] * a->q[2] + b->q[3] * a->q[3], sg = d < 0 ? -1 : 1, n = 0;
        for (int x = 0; x < 4; x++) { q[x] = b->q[x] + (sg * a->q[x] - b->q[x]) * f; n += q[x] * q[x]; }
        n = sqrtf(n); for (int x = 0; x < 4; x++) q[x] /= n;
        return 1;
    }
    return 0;
}
/* The play time one step back from `t`: back by dt, but over a rest at once to the sample before it. */
static double step_back(const Track *k, double t, double dt) {
    double to = t - dt;
    for (unsigned i = 0; i + 1 < k->n; i++) {
        const Sample *a = nth_back(k, i), *b = nth_back(k, i + 1);
        if (a->t < to) break;
        if (a->t - b->t >= 0.25 && to < a->t && to > b->t) return b->t;   /* a rest: skip it */
    }
    return to;
}

static void world_call(uintptr_t world, uintptr_t body, uintptr_t fn) {   /* fn(body) with the world locked */
    ((void (*)(uintptr_t))(g_main + HK_WORLD_LOCK))(world);
    ((void (*)(uintptr_t))(g_main + fn))(body);
    ((void (*)(uintptr_t))(g_main + HK_WORLD_UNLOCK))(world);
}
/* Stasis's lock-on on the object and the Slate's touch on Link; Stasis's release at the end. */
static void xlink_list(uintptr_t actor, const char *what);
static void recall_fx_start(void) {
    float at[3]; uintptr_t pl = player(at), obj = g_rt ? body_actor(g_rt->body) : 0;
    if (obj && g_recall_debug && getenv("TOTK_XLINK_LIST")) xlink_list(obj, "the recalled object");
    fx(pl, NULL, "Demo_SheikPadTouch");
    if (obj) fx(obj, "BitaLocked_Start", "BitaLockStart"); else if (g_fx) fprintf(stderr, "[abilities] fx: the recalled body has no actor\n");
}
static void recall_stop(const char *why) {
    if (!g_rt) return;
    { uintptr_t obj = body_actor(g_rt->body); if (obj) fx(obj, "BitaLocked_End", "BitaLockEnd"); }
    float zero[3] = {0};
    if (dynamic_body(g_rt->body)) { memcpy((void *)(g_rt->body + HK_ENTITY_LINEAR), zero, 12); memcpy((void *)(g_rt->body + HK_ENTITY_ANGULAR), zero, 12); }
    fprintf(stderr, "[abilities] recall: %s\n", why);
    g_rt->head = g_rt->n = 0;   /* its record is spent */
    g_rt = NULL; atomic_store(&g_recalling, 0);
}

/* TOTK_RECALL_DEBUG=1, for tests without aiming: ZL + Y kicks the nearest movable body within 30 m (awake or asleep) up
   and away from him, ZL + X recalls the nearest one with motion on record, and every 2 s the log says what is on record. */
enum { PAD_X = 4, PAD_Y = 8, HK_WORLD_INACTIVE_ISLANDS = 0x50 };
static uintptr_t g_aim_logged;
static double g_record_us; static unsigned g_record_n;   /* what recording costs a frame */
static uintptr_t nearest_body(const float at[3], int with_motion, double now) {
    uintptr_t world = entity_world(), best = 0; float bd = 30;
    if (!world) return 0;
    for (int list = 0; list < 2; list++) {
        struct { uintptr_t data; int32_t size, cap; } islands, bodies;
        if (!read_mem(world + (list ? HK_WORLD_INACTIVE_ISLANDS : HK_WORLD_ACTIVE_ISLANDS), &islands, sizeof islands) || islands.size <= 0 || islands.size > 4096) continue;
        static uintptr_t isl[4096], ent[4096];
        if (!read_mem(islands.data, isl, (size_t)islands.size * 8)) continue;
        for (int i = 0; i < islands.size; i++) {
            if (!read_mem(isl[i] + HK_ISLAND_ENTITIES, &bodies, sizeof bodies) || bodies.size <= 0 || bodies.size > 4096 || !read_mem(bodies.data, ent, (size_t)bodies.size * 8)) continue;
            for (int j = 0; j < bodies.size; j++) {
                float c[3]; Track *k;
                if (!dynamic_body(ent[j]) || !read_mem(ent[j] + HK_ENTITY_CENTER, c, sizeof c) || !place(c)) continue;
                float d = sqrtf((c[0] - at[0]) * (c[0] - at[0]) + (c[1] - at[1]) * (c[1] - at[1]) + (c[2] - at[2]) * (c[2] - at[2]));
                if (d < 1.5f || d >= bd) continue;   /* not his own body */
                if (with_motion && g_recall_debug) { k = track_of(ent[j]); fprintf(stderr, "[abilities] recall: candidate %#lx in %s island, %.1f m, track %s, %.2f s of motion\n", (unsigned long)ent[j], list ? "an inactive" : "an active", d, k ? "yes" : "no", k ? motion_seconds(k, now) : 0); }
                if (with_motion && (!(k = track_of(ent[j])) || motion_seconds(k, now) < 0.2)) continue;
                bd = d; best = ent[j];
            }
        }
    }
    return best;
}
/* The movable body, awake or asleep, nearest the camera's look direction within `cone` degrees and in its sight. */
static uintptr_t aimed_body(float cone) {
    uintptr_t world = entity_world(), best = 0, cam = read_ptr((uintptr_t)g_main + CAMERA_ROOT); cam = cam ? read_ptr(cam + CAMERA_STEP1) : 0;
    float cv[6], bestc = cosf(cone * 3.14159265f / 180);
    if (!world || !cam || !read_mem(cam + CAMERA_OFF, cv, sizeof cv)) return 0;
    float d[3] = {cv[3] - cv[0], cv[4] - cv[1], cv[5] - cv[2]}, n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); if (n < 1e-3f) return 0;
    for (int list = 0; list < 2; list++) {
        struct { uintptr_t data; int32_t size, cap; } islands, bodies;
        if (!read_mem(world + (list ? HK_WORLD_INACTIVE_ISLANDS : HK_WORLD_ACTIVE_ISLANDS), &islands, sizeof islands) || islands.size <= 0 || islands.size > 4096) continue;
        static uintptr_t isl[4096], ent[4096];
        if (!read_mem(islands.data, isl, (size_t)islands.size * 8)) continue;
        for (int i = 0; i < islands.size; i++) {
            if (!read_mem(isl[i] + HK_ISLAND_ENTITIES, &bodies, sizeof bodies) || bodies.size <= 0 || bodies.size > 4096 || !read_mem(bodies.data, ent, (size_t)bodies.size * 8)) continue;
            for (int j = 0; j < bodies.size; j++) {
                float c[3], v[3], h[3];
                if (!dynamic_body(ent[j]) || !read_mem(ent[j] + HK_ENTITY_CENTER, c, sizeof c) || !place(c)) continue;
                for (int x = 0; x < 3; x++) v[x] = c[x] - cv[x];
                float vn = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (vn < 3 || vn > RECALL_RANGE) continue;
                float cosa = (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / (vn * n); if (cosa <= bestc) continue;
                if (!g_see_all && ray_cast(cv, c, RECALL_LAYERS | CEILING_LAYERS, h) && g_hit_body != ent[j] &&
                    (h[0] - c[0]) * (h[0] - c[0]) + (h[1] - c[1]) * (h[1] - c[1]) + (h[2] - c[2]) * (h[2] - c[2]) > 1) continue;
                bestc = cosa; best = ent[j];
            }
        }
    }
    return best;
}
static void recall_debug(const float at[3], uint64_t buttons, uint64_t pressed, double now) {
    static double told; static uintptr_t kicked; static int follow; static int listed;
    if (!listed && (pressed & PAD_ZL)) {   /* once: every movable body within RECALL_RANGE, as the camera sees it */
        listed = 1;
        uintptr_t world = entity_world(), cam = read_ptr((uintptr_t)g_main + CAMERA_ROOT); cam = cam ? read_ptr(cam + CAMERA_STEP1) : 0;
        float cv[6];
        if (world && cam && read_mem(cam + CAMERA_OFF, cv, sizeof cv))
            for (int list = 0; list < 2; list++) {
                struct { uintptr_t data; int32_t size, cap; } islands, bodies;
                if (!read_mem(world + (list ? HK_WORLD_INACTIVE_ISLANDS : HK_WORLD_ACTIVE_ISLANDS), &islands, sizeof islands) || islands.size <= 0 || islands.size > 4096) continue;
                static uintptr_t isl[4096], ent[4096];
                if (!read_mem(islands.data, isl, (size_t)islands.size * 8)) continue;
                for (int i = 0; i < islands.size; i++) {
                    if (!read_mem(isl[i] + HK_ISLAND_ENTITIES, &bodies, sizeof bodies) || bodies.size <= 0 || bodies.size > 4096 || !read_mem(bodies.data, ent, (size_t)bodies.size * 8)) continue;
                    for (int j = 0; j < bodies.size; j++) {
                        float c[3], h[3]; uint8_t type = 0;
                        if (!read_mem(ent[j] + HK_ENTITY_MOTION_TYPE, &type, 1) || !read_mem(ent[j] + HK_ENTITY_CENTER, c, sizeof c) || !place(c)) continue;
                        float dx = c[0] - cv[0], dy = c[1] - cv[1], dz = c[2] - cv[2], dist = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (dist > RECALL_RANGE || type == 5) continue;
                        float lx = cv[3] - cv[0], ly = cv[4] - cv[1], lz = cv[5] - cv[2], ln = sqrtf(lx * lx + ly * ly + lz * lz);
                        float ang = acosf(fmaxf(-1, fminf(1, (dx * lx + dy * ly + dz * lz) / (dist * ln)))) * 57.29578f;
                        int seen = !ray_cast(cv, c, RECALL_LAYERS | CEILING_LAYERS, h) || g_hit_body == ent[j] || (h[0] - c[0]) * (h[0] - c[0]) + (h[1] - c[1]) * (h[1] - c[1]) + (h[2] - c[2]) * (h[2] - c[2]) <= 1;
                        fprintf(stderr, "[abilities] recall: body %#lx type %u at %.2f %.2f %.2f, %.1f m, %.0f deg off the aim, %s\n", (unsigned long)ent[j], type, c[0], c[1], c[2], dist, ang, seen ? "in sight" : "hidden");
                    }
                }
            }
    }
    if (follow > 0 && follow-- % 6 == 0) {
        float c[3] = {0}, v[3] = {0}; uint8_t type = 0; Track *k = track_of(kicked);
        read_mem(kicked + HK_ENTITY_CENTER, c, sizeof c); read_mem(kicked + HK_ENTITY_LINEAR, v, sizeof v); read_mem(kicked + HK_ENTITY_MOTION_TYPE, &type, 1);
        fprintf(stderr, "[abilities] recall: kicked body at %.2f %.2f %.2f, velocity %.2f %.2f %.2f, type %u, %u samples\n", c[0], c[1], c[2], v[0], v[1], v[2], type, k ? k->n : 0);
    }
    if (now - told > 2) {
        told = now; int n = 0; double most = 0;
        for (int i = 0; i < TRACKS; i++) if (g_track[i].n > 1 && now - g_track[i].seen < 5) { n++; double m = motion_seconds(&g_track[i], now); if (m > most) most = m; }
        fprintf(stderr, "[abilities] recall: %d moving bodies on record near him, the most %.1f s of motion; recording %.0f us a frame\n", n, most, g_record_n ? g_record_us / g_record_n : 0);
        g_record_us = 0; g_record_n = 0;
    }
    if (!(buttons & PAD_ZL)) return;
    if (pressed & PAD_Y) {
        uintptr_t b = aimed_body(25);
        if (!b) { fprintf(stderr, "[abilities] recall: kick: no movable body in sight within 25 degrees of the aim\n"); return; }
        float c[3]; read_mem(b + HK_ENTITY_CENTER, c, sizeof c);
        float dx = c[0] - at[0], dz = c[2] - at[2], n = sqrtf(dx * dx + dz * dz); if (n < 0.1f) n = 1;
        float v[3] = {dx / n * 4, 7, dz / n * 4};
        uintptr_t world = read_ptr(b + HK_ENTITY_WORLD); if (world) world_call(world, b, HK_ENTITY_ACTIVATE);
        memcpy((void *)(b + HK_ENTITY_LINEAR), v, 12);
        fprintf(stderr, "[abilities] recall: kick: body %#lx at %.2f %.2f %.2f\n", (unsigned long)b, c[0], c[1], c[2]);
        kicked = b; follow = 120;
    }
}

/* One game frame of Recall, on the main thread; returns the buttons the game gets. */
static uint64_t recall_frame(uint64_t buttons, uint64_t pressed) {
    float at[3];
    double now = now_s();
    if (!player(at) || !world_up() || !havok_thread()) { recall_stop("the world went away"); atomic_store(&g_aim, 0); return buttons; }
    record(at, now);
    { uintptr_t pl = player(at); if (pl) xlink_probe(pl, buttons, pressed, now); }
    if (g_recall_debug) { g_record_us += (now_s() - now) * 1e6; g_record_n++; recall_debug(at, buttons, pressed, now); }
    if (g_recall_debug && !g_rt && (buttons & PAD_ZL) && (pressed & PAD_X)) {   /* recall the nearest without aiming */
        uintptr_t b = nearest_body(at, 1, now); Track *k = b ? track_of(b) : NULL;
        if (!k) fprintf(stderr, "[abilities] recall: nothing near with motion on record\n");
        else {
            g_rt = k; g_rt_play = newest(k)->t; g_rt_last = now; atomic_store(&g_recalling, 1);
            uintptr_t world = read_ptr(b + HK_ENTITY_WORLD); if (world) world_call(world, b, HK_ENTITY_ACTIVATE);
            fprintf(stderr, "[abilities] recall: body %#lx (nearest), %.1f s of motion over %u samples, from %.2f %.2f %.2f\n", (unsigned long)b, motion_seconds(k, now), k->n, newest(k)->c[0], newest(k)->c[1], newest(k)->c[2]);
            recall_fx_start();
        }
    }
    if (g_rt) {
        if ((buttons & PAD_ZL) && (pressed & PAD_R)) { recall_stop("let go"); return buttons & ~(uint64_t)PAD_R; }
        if (!dynamic_body(g_rt->body)) { recall_stop("the body went away"); return buttons; }
        double dt = now - g_rt_last; g_rt_last = now; if (dt <= 0 || dt > 0.1) dt = 1.0 / 30;
        double next = step_back(g_rt, g_rt_play, dt);
        float c[3], q[4], cur[3], rc[16], qc[4];
        if (!pose_at(g_rt, next, c, q) || !read_mem(g_rt->body + HK_ENTITY_CENTER, cur, sizeof cur) || !read_mem(g_rt->body + HK_ENTITY_ROTATION, rc, sizeof rc)) {
            recall_stop("the end of its record"); return buttons;
        }
        g_rt_play = next;
        /* the velocities that take it from where it is to the next pose in one step */
        float v[3], w[3] = {0}; for (int x = 0; x < 3; x++) v[x] = (c[x] - cur[x]) / (float)dt;
        quat_from_columns(rc, qc);
        float inv[4] = {-qc[0], -qc[1], -qc[2], qc[3]}, e[4]; qmul(q, inv, e);   /* the turn still to make */
        if (e[3] < 0) for (int x = 0; x < 4; x++) e[x] = -e[x];
        float sn = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (sn > 1e-6f) { float angle = 2 * atan2f(sn, e[3]); for (int x = 0; x < 3; x++) w[x] = e[x] / sn * angle / (float)dt; }
        memcpy((void *)(g_rt->body + HK_ENTITY_LINEAR), v, 12);
        memcpy((void *)(g_rt->body + HK_ENTITY_ANGULAR), w, 12);
        static unsigned frame; if (frame++ % 10 == 0) { uintptr_t world = read_ptr(g_rt->body + HK_ENTITY_WORLD); if (world) world_call(world, g_rt->body, HK_ENTITY_ACTIVATE); }
        atomic_store(&g_recall_left, (float)motion_before(g_rt, g_rt_play, now));
        if (g_recall_debug && frame % 15 == 0) fprintf(stderr, "[abilities] recall: at %.2f %.2f %.2f, to %.2f %.2f %.2f, %.1f s left\n", cur[0], cur[1], cur[2], c[0], c[1], c[2], atomic_load(&g_recall_left));
        return buttons;
    }
    if (!(buttons & PAD_ZL)) { atomic_store(&g_aim, 0); return buttons; }
    /* aiming: the camera's look ray */
    uintptr_t cam = read_ptr((uintptr_t)g_main + CAMERA_ROOT); cam = cam ? read_ptr(cam + CAMERA_STEP1) : 0;
    float c[6], hit[3];
    if (!cam || !read_mem(cam + CAMERA_OFF, c, sizeof c) || !place(c) || !place(c + 3)) return buttons;
    float d[3] = {c[3] - c[0], c[4] - c[1], c[5] - c[2]}, n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < 1e-3f) return buttons;
    float to[3] = {c[0] + d[0] / n * RECALL_RANGE, c[1] + d[1] / n * RECALL_RANGE, c[2] + d[2] / n * RECALL_RANGE};
    Track *k = NULL; float seconds = 0; int got = ray_cast(c, to, RECALL_LAYERS, hit);
    if (got && g_hit_body && dynamic_body(g_hit_body) && (k = track_of(g_hit_body)) && k->n > 1) seconds = (float)motion_seconds(k, now);
    if (seconds <= 0.1f) {   /* aim assist: the recorded body nearest the look direction, within RECALL_CONE, in sight */
        float dir[3] = {d[0] / n, d[1] / n, d[2] / n}, best = cosf(RECALL_CONE * 3.14159265f / 180);
        for (int i = 0; i < TRACKS; i++) {
            Track *t = &g_track[i]; float bc[3], v[3], hv[3];
            if (t->n < 2 || now - t->seen > RECALL_SECONDS || !dynamic_body(t->body) || !read_mem(t->body + HK_ENTITY_CENTER, bc, sizeof bc)) continue;
            for (int x = 0; x < 3; x++) v[x] = bc[x] - c[x];
            float vn = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (vn < 1 || vn > RECALL_RANGE) continue;
            float cosa = (v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2]) / vn; if (cosa <= best) continue;
            double m = motion_seconds(t, now); if (m <= 0.1) continue;
            if (!g_see_all && ray_cast(c, bc, RECALL_LAYERS | CEILING_LAYERS, hv) && g_hit_body != t->body &&
                (hv[0] - bc[0]) * (hv[0] - bc[0]) + (hv[1] - bc[1]) * (hv[1] - bc[1]) + (hv[2] - bc[2]) * (hv[2] - bc[2]) > 1) continue;   /* hidden */
            best = cosa; k = t; seconds = (float)m; memcpy(hit, bc, sizeof hit); got = 1;
        }
        g_hit_body = k ? k->body : (got ? g_hit_body : 0);
    }
    atomic_store(&g_aim, seconds > 0.1f ? g_hit_body : 0); atomic_store(&g_aim_seconds, seconds);
    if (g_recall_debug && ((pressed & (PAD_ZL | PAD_R)) || (got && g_hit_body != g_aim_logged))) {
        g_aim_logged = got ? g_hit_body : 0;
        fprintf(stderr, "[abilities] recall: aim from %.2f %.2f %.2f: %s %#lx at %.2f %.2f %.2f, %s, %.1f s of motion\n", c[0], c[1], c[2], got ? "hit" : "nothing", (unsigned long)(got ? g_hit_body : 0),
                got ? hit[0] : 0, got ? hit[1] : 0, got ? hit[2] : 0, got && g_hit_body && dynamic_body(g_hit_body) ? "movable" : "not movable", seconds);
    }
    if (got) atomic_store(&g_aim_distance, sqrtf((hit[0] - at[0]) * (hit[0] - at[0]) + (hit[1] - at[1]) * (hit[1] - at[1]) + (hit[2] - at[2]) * (hit[2] - at[2])));
    if (seconds <= 0.1f) {
        if (pressed & PAD_R) fprintf(stderr, "[abilities] recall: nothing to recall there (%s)\n", !got || !g_hit_body ? "no body" : !dynamic_body(g_hit_body) ? "not a movable body" : !k ? "never seen moving" : "no motion on record");
        return buttons;
    }
    buttons &= ~(uint64_t)PAD_R;
    if (!(pressed & PAD_R)) return buttons;
    g_rt = k; g_rt_play = newest(k)->t; g_rt_last = now; atomic_store(&g_recalling, 1);
    uintptr_t world = read_ptr(k->body + HK_ENTITY_WORLD); if (world) world_call(world, k->body, HK_ENTITY_ACTIVATE);
    fprintf(stderr, "[abilities] recall: body %#lx, %.1f s of motion over %u samples, from %.2f %.2f %.2f\n", (unsigned long)k->body, seconds, k->n, newest(k)->c[0], newest(k)->c[1], newest(k)->c[2]);
    recall_fx_start();
    return buttons;
}

/* input.c, on the game's controller poll (main thread): the Switch buttons it is about to hand to the game. */
static _Atomic uint64_t g_raw;
uint64_t botw_abilities_filter_buttons(uint64_t buttons) {
    if (g_on <= 0 || !g_ray) return buttons;
    uint64_t was = atomic_exchange(&g_raw, buttons), pressed = buttons & ~was;
    buttons = ascend_frame(buttons, pressed);
    if (g_phase) return buttons;   /* rising */
    return recall_frame(buttons, pressed);
}

void botw_abilities_init(Module *m) {
    const char *e = getenv("TOTK_ABILITIES");
    g_on = e && !strcmp(e, "1");
    if (!g_on) return;
    if (strcmp(m->name, "main") || PLAYER_INFO + 8u > m->size || CAMERA_ROOT + 8u > m->size || HAVOK_ROOT + 8u > m->size)
        runtime_fail("abilities: Breath of the Wild 1.6.0 main expected");
    g_main = m->base;
    if (!(g_track = calloc(TRACKS, sizeof *g_track))) runtime_fail("abilities: no memory for Recall's record");
    { const char *f = getenv("TOTK_ABILITY_FX"); g_fx = f && !strcmp(f, "botw"); }
    { const char *d = getenv("TOTK_RECALL_DEBUG"); g_recall_debug = d && !strcmp(d, "1"); d = getenv("TOTK_RECALL_SEE_ALL"); g_see_all = d && !strcmp(d, "1"); }
    const char *r = getenv("TOTK_ABILITY_RAY"); g_ray = !(r && !strcmp(r, "0"));
    if ((r = getenv("TOTK_ASCEND_REACH"))) g_reach = fminf(fmaxf(strtof(r, NULL), 1), 64);
    if ((r = getenv("TOTK_ASCEND_THICK"))) g_thick = fminf(fmaxf(strtof(r, NULL), 1), 256);
    fprintf(stderr, "[abilities] on: %s\n", g_ray ? "Ascend (ZL + L below a roof), Recall (hold ZL, aim, R) and the HUD line" : "the HUD line only (TOTK_ABILITY_RAY=0: no ray casts, no Ascend)");
}

/* Every present: the HUD line from the player, the camera, the buttons and what the poll found. */
void botw_abilities_tick(void) {
    if (g_on <= 0) return;
    static unsigned frame; frame++;
    if (frame % 4) return;
    char line[128];
    float at[3];
    if (!player(at)) { hud_publish(HUD_LINE_ABILITY, "Abilities: no player", 0); return; }
    if (atomic_load(&g_recalling)) {
        snprintf(line, sizeof line, "Recall: %.1f s left  (ZL+R: let go)", atomic_load(&g_recall_left));
        hud_publish(HUD_LINE_ABILITY, line, 1);
        return;
    }
    if (atomic_load(&g_aim)) {
        snprintf(line, sizeof line, "Recall: %.1f s of motion, %.0f m  (ZL+R)", atomic_load(&g_aim_seconds), atomic_load(&g_aim_distance));
        hud_publish(HUD_LINE_ABILITY, line, 1);
        return;
    }
    int phase = atomic_load(&g_phase_seen);
    if (phase) {
        snprintf(line, sizeof line, phase == 1 ? "Ascend: rising %.1f / %.1f m  (B: back)" : "Ascend: going back %.1f / %.1f m", atomic_load(&g_rise), atomic_load(&g_rise_total));
        hud_publish(HUD_LINE_ABILITY, line, 1);
        return;
    }
    uintptr_t cam = read_ptr((uintptr_t)g_main + CAMERA_ROOT); cam = cam ? read_ptr(cam + CAMERA_STEP1) : 0;
    float c[6]; int heading = -1;   /* where the camera looks, as a compass heading (0 = north, -Z) */
    if (cam && read_mem(cam + CAMERA_OFF, c, sizeof c) && place(c) && place(c + 3))
        heading = (int)lroundf(atan2f(c[3] - c[0], -(c[5] - c[2])) * 57.29578f + 360) % 360;
    float ceiling = atomic_load(&g_ceiling); int ready = atomic_load(&g_ready);
    unsigned long long pad = runtime_input_buttons_seen() & 0xffff;
    if (ceiling >= 0) snprintf(line, sizeof line, "Abilities: %.1f %.1f %.1f  ceiling +%.1f m%s  cam %d°  pad %04llx", at[0], at[1], at[2], ceiling, ready ? " (ZL+L: Ascend)" : "", heading, pad);
    else snprintf(line, sizeof line, "Abilities: %.1f %.1f %.1f  no ceiling  cam %d°  pad %04llx", at[0], at[1], at[2], heading, pad);
    hud_publish(HUD_LINE_ABILITY, line, ready);
    if (frame % 240 == 0) fprintf(stderr, "[abilities] %s\n", line + 11);
}
#else
void botw_abilities_init(Module *m) { (void)m; }
void botw_abilities_tick(void) { }
uint64_t botw_abilities_filter_buttons(uint64_t buttons) { return buttons; }
#endif
