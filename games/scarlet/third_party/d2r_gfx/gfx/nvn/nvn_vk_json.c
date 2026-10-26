// Minimal JSON reader (see nvn_vk_json.h): objects, arrays, strings (\uXXXX kept as '?' for non-ASCII), numbers,
// true / false / null. Nodes and decoded strings come from one arena freed together.
#include "nvn_vk_json.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p, *e; char *arena; size_t used, cap; int err; } P;
typedef struct { js root; char mem[]; } doc;
static void *take(P *s, size_t n) { n = (n + 7) & ~(size_t)7; if (s->used + n > s->cap) { s->err = 1; return NULL; } void *r = s->arena + s->used; s->used += n; return r; }
static void ws(P *s) { while (s->p < s->e && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) s->p++; }
static const char *pstr(P *s) {
    if (s->p >= s->e || *s->p != '"') { s->err = 1; return NULL; }
    s->p++;
    const char *q = s->p; size_t n = 0;
    while (q < s->e && *q != '"') { if (*q == '\\') q++; q++; n++; }
    char *o = take(s, n + 1); if (!o) return NULL;
    char *w = o;
    while (s->p < s->e && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\' && s->p < s->e) {
            c = *s->p++;
            switch (c) { case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                case 'u': { unsigned v = 0; for (int i = 0; i < 4 && s->p < s->e; i++, s->p++) { char h = *s->p; v = v * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0'); } c = v < 128 ? (char)v : '?'; break; }
                default: break; }
        }
        *w++ = c;
    }
    *w = 0; if (s->p < s->e) s->p++; else s->err = 1;
    return o;
}
static js *val(P *s, int depth) {
    if (depth > 64) { s->err = 1; return NULL; }
    ws(s);
    js *v = take(s, sizeof *v); if (!v) return NULL;
    memset(v, 0, sizeof *v);
    if (s->p >= s->e) { s->err = 1; return NULL; }
    char c = *s->p;
    if (c == '{' || c == '[') {
        int obj = c == '{'; v->t = obj ? JS_OBJ : JS_ARR; s->p++;
        js **tail = &v->child;
        ws(s);
        if (s->p < s->e && *s->p == (obj ? '}' : ']')) { s->p++; return v; }
        for (;;) {
            const char *k = NULL;
            if (obj) { ws(s); k = pstr(s); ws(s); if (s->err || s->p >= s->e || *s->p != ':') { s->err = 1; return NULL; } s->p++; }
            js *m = val(s, depth + 1); if (!m) return NULL;
            m->key = k; *tail = m; tail = &m->next; v->n++;
            ws(s);
            if (s->p < s->e && *s->p == ',') { s->p++; continue; }
            if (s->p < s->e && *s->p == (obj ? '}' : ']')) { s->p++; return v; }
            s->err = 1; return NULL;
        }
    }
    if (c == '"') { v->t = JS_STR; v->str = pstr(s); return s->err ? NULL : v; }
    if (!strncmp(s->p, "true", 4)) { v->t = JS_TRUE; s->p += 4; return v; }
    if (!strncmp(s->p, "false", 5)) { v->t = JS_FALSE; s->p += 5; return v; }
    if (!strncmp(s->p, "null", 4)) { v->t = JS_NULL; s->p += 4; return v; }
    char *end; v->t = JS_NUM; v->num = strtod(s->p, &end);
    if (end == s->p) { s->err = 1; return NULL; }
    s->p = end; return v;
}
js *js_parse(const char *text, size_t len) {
    P s = {text, text + len, NULL, 0, len * 24 + 65536, 0};
    s.arena = malloc(s.cap); if (!s.arena) return NULL;
    js *r = val(&s, 0);
    if (!r || s.err) { free(s.arena); return NULL; }
    return r;   // r is the first allocation of the arena
}
void js_free(js *root) { free(root); }
js *js_get(const js *o, const char *key) { if (!o || o->t != JS_OBJ) return NULL; for (js *m = o->child; m; m = m->next) if (m->key && !strcmp(m->key, key)) return m; return NULL; }
js *js_at(const js *a, int i) { if (!a || (a->t != JS_ARR && a->t != JS_OBJ)) return NULL; js *m = a->child; while (m && i--) m = m->next; return m; }
int js_int(const js *v, int d) { return v && v->t == JS_NUM ? (int)v->num : d; }
const char *js_str(const js *v, const char *d) { return v && v->t == JS_STR ? v->str : d; }
int js_bool(const js *v, int d) { return v ? (v->t == JS_TRUE ? 1 : v->t == JS_FALSE ? 0 : v->t == JS_NUM ? v->num != 0 : d) : d; }
