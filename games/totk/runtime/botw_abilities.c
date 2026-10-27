/* Tears of the Kingdom abilities in Breath of the Wild 1.6.0 (docs/botw-abilities.md). Off unless TOTK_ABILITIES=1.
   So far: Ascend and a HUD line with the player, the ceiling above him, the camera and the buttons.

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
   with a Havok stream and a world up (the callers check). */
static int ray_cast(const float from[3], const float to[3], uint32_t layers, float hit[3]) {
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
    return 1;
}

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
    }
    if (g_phase == 1) put_player(g_from[1] + (g_to_y - g_from[1]) * ease((float)t));
    else put_player(g_back_from_y + (g_from[1] - g_back_from_y) * ease((float)t));
    atomic_store(&g_rise, g_now_y - g_from[1]); atomic_store(&g_rise_total, g_to_y - g_from[1]); atomic_store(&g_phase_seen, g_phase);
    /* Held at the end a moment longer: Havok kept the contacts of his pass through the ceiling, which push him back
       down through the top when he is let go on it at once (he fell from rest through the ground he stood on,
       2026-10-26); a metre above it they expire, then he drops onto it. */
    if (t >= 1 + ASCEND_SETTLE / g_dur) {
        fprintf(stderr, "[abilities] ascend: %s at %.2f (the matrix says %.2f)\n", g_phase == 1 ? "arrived" : "back", g_now_y, player(at) ? at[1] : NAN);
        dump("end"); if (getenv("TOTK_ASCEND_DUMP")) g_after = 120;
        g_phase = 0; atomic_store(&g_phase_seen, 0);
    }
    return 0;   /* he does nothing else while he rises */
}

/* input.c, on the game's controller poll (main thread): the Switch buttons it is about to hand to the game. */
static _Atomic uint64_t g_raw;
uint64_t botw_abilities_filter_buttons(uint64_t buttons) {
    if (g_on <= 0 || !g_ray) return buttons;
    uint64_t was = atomic_exchange(&g_raw, buttons);
    return ascend_frame(buttons, buttons & ~was);
}

void botw_abilities_init(Module *m) {
    const char *e = getenv("TOTK_ABILITIES");
    g_on = e && !strcmp(e, "1");
    if (!g_on) return;
    if (strcmp(m->name, "main") || PLAYER_INFO + 8u > m->size || CAMERA_ROOT + 8u > m->size || HAVOK_ROOT + 8u > m->size)
        runtime_fail("abilities: Breath of the Wild 1.6.0 main expected");
    g_main = m->base;
    const char *r = getenv("TOTK_ABILITY_RAY"); g_ray = !(r && !strcmp(r, "0"));
    if ((r = getenv("TOTK_ASCEND_REACH"))) g_reach = fminf(fmaxf(strtof(r, NULL), 1), 64);
    if ((r = getenv("TOTK_ASCEND_THICK"))) g_thick = fminf(fmaxf(strtof(r, NULL), 1), 256);
    fprintf(stderr, "[abilities] on: %s\n", g_ray ? "Ascend (ZL + L below a roof) and the HUD line" : "the HUD line only (TOTK_ABILITY_RAY=0: no ray casts, no Ascend)");
}

/* Every present: the HUD line from the player, the camera, the buttons and what the poll found. */
void botw_abilities_tick(void) {
    if (g_on <= 0) return;
    static unsigned frame; frame++;
    if (frame % 4) return;
    char line[128];
    float at[3];
    if (!player(at)) { hud_publish(HUD_LINE_ABILITY, "Abilities: no player", 0); return; }
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
