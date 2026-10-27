/* Sounds the host plays itself (host_sound.h). The files are 16-bit WAV, mono or stereo, at their own rate; voices are
   resampled linearly to the output rate. A voice that loops plays its intro once, then the frames between the loop
   points (the whole sound when it has none). */
#include "host_sound.h"
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[64]; int16_t *pcm; uint32_t frames, rate, loop_start, loop_end; int channels, missing; } Sound;
typedef struct { Sound *s; double pos; float volume, pan, fade; int loop, id; } Voice;
enum { SOUNDS = 64, VOICES = 16 };
static Sound g_sound[SOUNDS];
static Voice g_voice[VOICES];
static int g_next_id = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *sound_dir(void) {
    static char dir[1024];
    if (!dir[0]) {
        const char *d = getenv("TOTK_ABILITY_DATA"), *home = getenv("HOME");
        if (d && *d) snprintf(dir, sizeof dir, "%s/sound", d);
        else snprintf(dir, sizeof dir, "%s/Library/Application Support/MacNativeGames/BotW/abilities/sound", home ? home : "");
    }
    return dir;
}
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* The named sound, loaded on first use; NULL when its file is not there or not a 16-bit WAV. Called locked. */
static Sound *sound(const char *name) {
    Sound *free_slot = NULL;
    for (int i = 0; i < SOUNDS; i++) {
        if (!g_sound[i].name[0]) { if (!free_slot) free_slot = &g_sound[i]; continue; }
        if (!strcmp(g_sound[i].name, name)) return g_sound[i].missing ? NULL : &g_sound[i];
    }
    if (!free_slot) return NULL;
    Sound *s = free_slot; snprintf(s->name, sizeof s->name, "%s", name); s->missing = 1;
    char path[1200]; snprintf(path, sizeof path, "%s/%s.wav", sound_dir(), name);
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "[host-sound] %s: no %s\n", name, path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 44 ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { fclose(f); free(b); return NULL; }
    fclose(f);
    const uint8_t *fmt = NULL, *data = NULL; uint32_t data_bytes = 0;
    for (long p = 12; p + 8 <= n;) {
        uint32_t size = le32(b + p + 4);
        if (!memcmp(b + p, "fmt ", 4)) fmt = b + p + 8;
        if (!memcmp(b + p, "data", 4)) { data = b + p + 8; data_bytes = size; }
        p += 8 + size + (size & 1);
    }
    if (!fmt || !data || le16(fmt) != 1 || le16(fmt + 14) != 16 || (le16(fmt + 2) != 1 && le16(fmt + 2) != 2) || data + data_bytes > b + n) { free(b); return NULL; }
    s->channels = le16(fmt + 2); s->rate = le32(fmt + 4);
    s->frames = data_bytes / (2u * (uint32_t)s->channels);
    s->pcm = malloc((size_t)s->frames * (size_t)s->channels * 2);
    if (!s->pcm) { free(b); return NULL; }
    memcpy(s->pcm, data, (size_t)s->frames * (size_t)s->channels * 2);
    free(b);
    s->loop_start = 0; s->loop_end = s->frames;
    snprintf(path, sizeof path, "%s/%s.loop", sound_dir(), name);
    if ((f = fopen(path, "r"))) {
        unsigned a, e;
        if (fscanf(f, "%u %u", &a, &e) == 2 && a < e && e <= s->frames) { s->loop_start = a; s->loop_end = e; }
        fclose(f);
    }
    s->missing = 0;
    fprintf(stderr, "[host-sound] %s: %d ch, %u Hz, %.2f s%s\n", name, s->channels, s->rate, (double)s->frames / s->rate, s->loop_start || s->loop_end != s->frames ? ", loops" : "");
    return s;
}

int host_sound_have(const char *name) { pthread_mutex_lock(&g_mu); int ok = sound(name) != NULL; pthread_mutex_unlock(&g_mu); return ok; }

int host_sound_play(const char *name, float volume, float pan, int loop) {
    pthread_mutex_lock(&g_mu);
    Sound *s = sound(name); int id = 0;
    if (s) {
        Voice *v = NULL;
        for (int i = 0; i < VOICES && !v; i++) if (!g_voice[i].s) v = &g_voice[i];
        if (!v) { v = &g_voice[0]; for (int i = 1; i < VOICES; i++) if (!g_voice[i].loop && g_voice[i].pos > v->pos) v = &g_voice[i]; }   /* all busy: the furthest one-shot */
        *v = (Voice){.s = s, .volume = volume, .pan = pan, .loop = loop, .id = id = g_next_id++};
    }
    pthread_mutex_unlock(&g_mu);
    return id;
}
void host_sound_set(int id, float volume, float pan) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < VOICES; i++) if (g_voice[i].s && g_voice[i].id == id) { g_voice[i].volume = volume; g_voice[i].pan = pan; }
    pthread_mutex_unlock(&g_mu);
}
void host_sound_stop(int id, float fade_seconds) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < VOICES; i++) if (g_voice[i].s && g_voice[i].id == id) {
        if (fade_seconds <= 0) g_voice[i].s = NULL;
        else g_voice[i].fade = fade_seconds;
    }
    pthread_mutex_unlock(&g_mu);
}

void host_sound_mix(float *lr, int frames, int rate, float gain) {
    if (rate <= 0) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < VOICES; i++) {
        Voice *v = &g_voice[i]; Sound *s = v->s;
        if (!s) continue;
        double step = (double)s->rate / rate;
        float pl = sqrtf(0.5f * (1 - v->pan)), pr = sqrtf(0.5f * (1 + v->pan));   /* equal power */
        float fade_step = v->fade > 0 ? 1.0f / (v->fade * (float)rate) : 0, level = 1;
        for (int k = 0; k < frames; k++) {
            if (v->pos >= (v->loop ? s->loop_end : s->frames) - 1) {
                if (!v->loop) { v->s = NULL; break; }
                v->pos -= s->loop_end - s->loop_start;
            }
            uint32_t a = (uint32_t)v->pos; float t = (float)(v->pos - a);
            float l, r;
            if (s->channels == 2) {
                l = (s->pcm[2 * a] * (1 - t) + s->pcm[2 * a + 2] * t) * (1.0f / 32768);
                r = (s->pcm[2 * a + 1] * (1 - t) + s->pcm[2 * a + 3] * t) * (1.0f / 32768);
                float bl = v->pan > 0 ? 1 - v->pan : 1, br = v->pan < 0 ? 1 + v->pan : 1;   /* balance */
                l *= bl; r *= br;
            } else {
                float m = (s->pcm[a] * (1 - t) + s->pcm[a + 1] * t) * (1.0f / 32768);
                l = m * pl * 1.41421356f; r = m * pr * 1.41421356f;
            }
            if (fade_step) { level -= fade_step; if (level <= 0) { v->s = NULL; break; } }
            float g = gain * v->volume * level;
            lr[2 * k] += l * g; lr[2 * k + 1] += r * g;
            v->pos += step;
        }
        if (v->s && v->fade > 0) { v->fade -= (float)frames / rate; if (v->fade <= 0) v->s = NULL; else v->volume *= level; }
    }
    pthread_mutex_unlock(&g_mu);
    for (int k = 0; k < 2 * frames; k++) lr[k] = lr[k] > 1 ? 1 : lr[k] < -1 ? -1 : lr[k];
}
