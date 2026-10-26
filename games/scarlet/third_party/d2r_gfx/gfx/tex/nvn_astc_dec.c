// ASTC LDR block decoder (see nvn_astc_dec.h). Written from the Khronos Data Format Specification 1.3, chapter
// "ASTC Compressed Texture Image Formats" (block mode table, integer sequence encoding, colour endpoint modes,
// partition hash, weight infill).
#include "nvn_astc_dec.h"
#include <string.h>

typedef struct { uint64_t lo, hi; } u128;
static uint32_t bits(const u128 *b, int at, int n) {   // n <= 32
    if (n <= 0) return 0;
    uint64_t v;
    if (at >= 64) v = b->hi >> (at - 64);
    else if (at + n <= 64) v = b->lo >> at;
    else v = (b->lo >> at) | (b->hi << (64 - at));
    return (uint32_t)(v & ((1ull << n) - 1));
}
static u128 reverse128(u128 b) {
    u128 r = {0, 0};
    for (int i = 0; i < 128; i++) { int bit = i < 64 ? (int)((b.lo >> i) & 1) : (int)((b.hi >> (i - 64)) & 1);
        int j = 127 - i; if (bit) { if (j < 64) r.lo |= 1ull << j; else r.hi |= 1ull << (j - 64); } }
    return r;
}

// ---------- integer sequence encoding ----------
typedef struct { uint8_t bits, trits, quints; } ise_t;
static const ise_t ISE[21] = {   // by number of levels: 2,3,4,5,6,8,10,12,16,20,24,32,40,48,64,80,96,128,160,192,256
    {1,0,0},{0,1,0},{2,0,0},{0,0,1},{1,1,0},{3,0,0},{1,0,1},{2,1,0},{4,0,0},{2,0,1},{3,1,0},{5,0,0},{3,0,1},{4,1,0},{6,0,0},{4,0,1},{5,1,0},{7,0,0},{5,0,1},{6,1,0},{8,0,0}};
static const int ISE_LEVELS[21] = {2,3,4,5,6,8,10,12,16,20,24,32,40,48,64,80,96,128,160,192,256};
static int ise_bitcount(int q, int n) {
    const ise_t *e = &ISE[q];
    return e->bits * n + (e->trits ? (8 * n + 4) / 5 : 0) + (e->quints ? (7 * n + 2) / 3 : 0);
}
static void decode_trits(uint32_t T, int t[5]) {
    uint32_t C;
    if (((T >> 2) & 7) == 7) { C = (((T >> 5) & 7) << 2) | (T & 3); t[4] = 2; t[3] = 2; }
    else { C = T & 0x1F; if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = (T >> 7) & 1; } else { t[4] = (T >> 7) & 1; t[3] = (T >> 5) & 3; } }
    if ((C & 3) == 3) { t[2] = 2; t[1] = (C >> 4) & 1; t[0] = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1)); }
    else if (((C >> 2) & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
    else { t[2] = (C >> 4) & 1; t[1] = (C >> 2) & 3; t[0] = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1)); }
}
static void decode_quints(uint32_t Q, int q[3]) {
    if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
        q[2] = (int)(((Q & 1) << 2) | ((((Q >> 4) & 1) & ~(Q & 1)) << 1) | (((Q >> 3) & 1) & ~(Q & 1))); q[1] = 4; q[0] = 4;
    } else {
        uint32_t C;
        if (((Q >> 1) & 3) == 3) { q[2] = 4; C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1); }
        else { q[2] = (Q >> 5) & 3; C = Q & 0x1F; }
        if ((C & 7) == 5) { q[1] = 4; q[0] = (C >> 3) & 3; }
        else { q[1] = (C >> 3) & 3; q[0] = C & 7; }
    }
}
// Reads n values of quant level q from b starting at bit 'at' (upwards); out[i] = {D (trit / quint or 0), m (bits)}.
/* bits of the sequence past its encoded length (ceil(8n/5) / ceil(7n/3) for a partial last group) read as zero */
static _Thread_local int g_ise_end;
static uint32_t ise_bits(const u128 *b, int at, int n) {
    if (at >= g_ise_end || n <= 0) return 0;
    if (at + n > g_ise_end) n = g_ise_end - at;
    return bits(b, at, n);
}
static void ise_decode(const u128 *b, int at, int q, int n, int *outD, int *outM) {
    const ise_t *e = &ISE[q]; int nb = e->bits;
    g_ise_end = at + ise_bitcount(q, n);
    if (e->trits) {
        for (int i = 0; i < n; i += 5) {
            int m[5] = {0}; uint32_t T = 0; int p = at;
            m[0] = (int)ise_bits(b, p, nb); p += nb; T |= ise_bits(b, p, 2); p += 2;
            m[1] = (int)ise_bits(b, p, nb); p += nb; T |= ise_bits(b, p, 2) << 2; p += 2;
            m[2] = (int)ise_bits(b, p, nb); p += nb; T |= ise_bits(b, p, 1) << 4; p += 1;
            m[3] = (int)ise_bits(b, p, nb); p += nb; T |= ise_bits(b, p, 2) << 5; p += 2;
            m[4] = (int)ise_bits(b, p, nb); p += nb; T |= ise_bits(b, p, 1) << 7; p += 1;
            int t[5]; decode_trits(T, t);
            for (int k = 0; k < 5 && i + k < n; k++) { outD[i + k] = t[k]; outM[i + k] = m[k]; }
            at = p;
        }
    } else if (e->quints) {
        for (int i = 0; i < n; i += 3) {
            int m[3]; uint32_t Q = 0; int p = at;
            m[0] = (int)ise_bits(b, p, nb); p += nb; Q |= ise_bits(b, p, 3); p += 3;
            m[1] = (int)ise_bits(b, p, nb); p += nb; Q |= ise_bits(b, p, 2) << 3; p += 2;
            m[2] = (int)ise_bits(b, p, nb); p += nb; Q |= ise_bits(b, p, 2) << 5; p += 2;
            int qq[3]; decode_quints(Q, qq);
            for (int k = 0; k < 3 && i + k < n; k++) { outD[i + k] = qq[k]; outM[i + k] = m[k]; }
            at = p;
        }
    } else {
        for (int i = 0; i < n; i++) { outD[i] = 0; outM[i] = (int)ise_bits(b, at, nb); at += nb; }
    }
}
static int replicate(int v, int from, int to) {   // bit replication of a from-bit value to to bits
    int r = 0, have = 0;
    while (have < to) { int sh = to - have - from; r |= sh >= 0 ? v << sh : v >> -sh; have += from; }
    return r & ((1 << to) - 1);
}
static int unq_color(int q, int D, int m) {
    const ise_t *e = &ISE[q]; int nb = e->bits;
    if (!e->trits && !e->quints) return replicate(m, nb, 8);
    int A = (m & 1) ? 0x1FF : 0, B = 0, C = 0;
    int b1 = (m >> 1) & 1, b2 = (m >> 2) & 1, b3 = (m >> 3) & 1, b4 = (m >> 4) & 1, b5 = (m >> 5) & 1;
    if (e->trits) {
        switch (nb) {
            case 1: C = 204; break;
            case 2: C = 93; B = (b1 << 8) | (b1 << 4) | (b1 << 2) | (b1 << 1); break;                         // b000b0bb0
            case 3: C = 44; B = (b2 << 8) | (b1 << 7) | (b2 << 3) | (b1 << 2) | (b2 << 1) | b1; break;         // cb000cbcb
            case 4: C = 22; B = (b3 << 8) | (b2 << 7) | (b1 << 6) | (b3 << 2) | (b2 << 1) | b1; break;         // dcb000dcb
            case 5: C = 11; B = (b4 << 8) | (b3 << 7) | (b2 << 6) | (b1 << 5) | (b4 << 1) | b3; break;         // edcb000ed
            case 6: C = 5; B = (b5 << 8) | (b4 << 7) | (b3 << 6) | (b2 << 5) | (b1 << 4) | b5; break;          // fedcb000f
        }
    } else {
        switch (nb) {
            case 1: C = 113; break;
            case 2: C = 54; B = (b1 << 8) | (b1 << 3) | (b1 << 2); break;                                     // b0000bb00
            case 3: C = 26; B = (b2 << 8) | (b1 << 7) | (b2 << 2) | (b1 << 1) | b2; break;                    // cb0000cbc
            case 4: C = 13; B = (b3 << 8) | (b2 << 7) | (b1 << 6) | (b3 << 1) | b2; break;                    // dcb0000dc
            case 5: C = 6; B = (b4 << 8) | (b3 << 7) | (b2 << 6) | (b1 << 5) | b4; break;                     // edcb0000e
        }
    }
    int T = D * C + B; T ^= A; T = (A & 0x80) | (T >> 2);
    return T & 0xFF;
}
static int unq_weight(int q, int D, int m) {
    const ise_t *e = &ISE[q]; int nb = e->bits, T;
    if (!e->trits && !e->quints) T = replicate(m, nb, 6);
    else if (!nb) { static const int t3[3] = {0, 32, 63}, q5[5] = {0, 16, 32, 47, 63}; T = e->trits ? t3[D] : q5[D]; }
    else {
        int A = (m & 1) ? 0x7F : 0, B = 0, C = 0, b1 = (m >> 1) & 1, b2 = (m >> 2) & 1;
        if (e->trits) { if (nb == 1) C = 50; else if (nb == 2) { C = 23; B = (b1 << 6) | (b1 << 2) | b1; } else { C = 11; B = (b2 << 6) | (b1 << 5) | (b2 << 1) | b1; } }
        else { if (nb == 1) C = 28; else { C = 13; B = (b1 << 6) | (b1 << 1); } }
        T = D * C + B; T ^= A; T = (A & 0x20) | (T >> 2);
    }
    if (T > 32) T++;
    return T;
}

// ---------- partitions ----------
static uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4; p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3; p ^= p << 6; p ^= p >> 17; return p;
}
static int select_partition(int seed, int x, int y, int z, int count, int small) {
    if (small) { x <<= 1; y <<= 1; z <<= 1; }
    seed += (count - 1) * 1024;
    uint32_t r = hash52((uint32_t)seed);
    int s1 = r & 0xF, s2 = (r >> 4) & 0xF, s3 = (r >> 8) & 0xF, s4 = (r >> 12) & 0xF, s5 = (r >> 16) & 0xF, s6 = (r >> 20) & 0xF,
        s7 = (r >> 24) & 0xF, s8 = (r >> 28) & 0xF, s9 = (r >> 18) & 0xF, s10 = (r >> 22) & 0xF, s11 = (r >> 26) & 0xF, s12 = ((r >> 30) | (r << 2)) & 0xF;
    s1 *= s1; s2 *= s2; s3 *= s3; s4 *= s4; s5 *= s5; s6 *= s6; s7 *= s7; s8 *= s8; s9 *= s9; s10 *= s10; s11 *= s11; s12 *= s12;
    int sh1, sh2, sh3;
    if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = count == 3 ? 6 : 5; }
    else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    sh3 = (seed & 0x10) ? sh1 : sh2;
    s1 >>= sh1; s2 >>= sh2; s3 >>= sh1; s4 >>= sh2; s5 >>= sh1; s6 >>= sh2; s7 >>= sh1; s8 >>= sh2; s9 >>= sh3; s10 >>= sh3; s11 >>= sh3; s12 >>= sh3;
    int a = (int)((s1 * x + s2 * y + s11 * z + (int)(r >> 14)) & 0x3F), b = (int)((s3 * x + s4 * y + s12 * z + (int)(r >> 10)) & 0x3F),
        c = (int)((s5 * x + s6 * y + s9 * z + (int)(r >> 6)) & 0x3F), d = (int)((s7 * x + s8 * y + s10 * z + (int)(r >> 2)) & 0x3F);
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

// ---------- colour endpoints ----------
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void bts(int *a, int *b) { *b = (*b >> 1) | (*a & 0x80); *a = (*a >> 1) & 0x3F; if (*a & 0x20) *a -= 0x40; }
static void blue_contract(int c[4]) { c[0] = (c[0] + c[2]) >> 1; c[1] = (c[1] + c[2]) >> 1; }
// returns 0, or 1 for HDR / reserved modes
static int endpoints(int cem, const int *v, int e0[4], int e1[4]) {
    int a, b, c, d;
    switch (cem) {
        case 0: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = e1[3] = 255; return 0;
        case 1: { int l0 = (v[0] >> 2) | (v[1] & 0xC0), l1 = l0 + (v[1] & 0x3F); if (l1 > 255) l1 = 255;
                  e0[0] = e0[1] = e0[2] = l0; e1[0] = e1[1] = e1[2] = l1; e0[3] = e1[3] = 255; return 0; }
        case 4: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = v[2]; e1[3] = v[3]; return 0;
        case 5: a = v[1]; b = v[0]; bts(&a, &b); c = v[3]; d = v[2]; bts(&c, &d);
                e0[0] = e0[1] = e0[2] = b; e0[3] = d; e1[0] = e1[1] = e1[2] = clamp255(b + a); e1[3] = clamp255(d + c); return 0;
        case 6: e0[0] = (v[0] * v[3]) >> 8; e0[1] = (v[1] * v[3]) >> 8; e0[2] = (v[2] * v[3]) >> 8; e0[3] = 255;
                e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = 255; return 0;
        case 8: case 12: {
            int A0 = cem == 12 ? v[6] : 255, A1 = cem == 12 ? v[7] : 255;
            if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) { e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e0[3] = A0; e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; e1[3] = A1; }
            else { e0[0] = v[1]; e0[1] = v[3]; e0[2] = v[5]; e0[3] = A1; blue_contract(e0); e1[0] = v[0]; e1[1] = v[2]; e1[2] = v[4]; e1[3] = A0; blue_contract(e1); }
            return 0; }
        case 9: case 13: {
            int w[8]; for (int i = 0; i < 8; i++) w[i] = i < (cem == 13 ? 8 : 6) ? v[i] : 0;
            bts(&w[1], &w[0]); bts(&w[3], &w[2]); bts(&w[5], &w[4]); if (cem == 13) bts(&w[7], &w[6]);
            int A0 = cem == 13 ? w[6] : 255, A1 = cem == 13 ? w[6] + w[7] : 255;
            if (w[1] + w[3] + w[5] >= 0) {
                e0[0] = w[0]; e0[1] = w[2]; e0[2] = w[4]; e0[3] = A0;
                e1[0] = w[0] + w[1]; e1[1] = w[2] + w[3]; e1[2] = w[4] + w[5]; e1[3] = A1;
            } else {
                e0[0] = w[0] + w[1]; e0[1] = w[2] + w[3]; e0[2] = w[4] + w[5]; e0[3] = A1; blue_contract(e0);
                e1[0] = w[0]; e1[1] = w[2]; e1[2] = w[4]; e1[3] = A0; blue_contract(e1);
            }
            for (int i = 0; i < 4; i++) { e0[i] = clamp255(e0[i]); e1[i] = clamp255(e1[i]); }
            return 0; }
        case 10: e0[0] = (v[0] * v[3]) >> 8; e0[1] = (v[1] * v[3]) >> 8; e0[2] = (v[2] * v[3]) >> 8; e0[3] = v[4];
                 e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = v[5]; return 0;
    }
    return 1;   // 2, 3, 7, 11, 14, 15: HDR
}

static void fill_error(int bw, int bh, uint8_t *dst, size_t pitch) {
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) { uint8_t *p = dst + y * pitch + x * 4; p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255; }
}

int nvn_astc_decode_block(const uint8_t blk[16], int bw, int bh, int srgb, uint8_t *dst, size_t pitch) {
    u128 b; memcpy(&b.lo, blk, 8); memcpy(&b.hi, blk + 8, 8);
    uint32_t mode = bits(&b, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {   // void extent
        if (mode & 0x200) { fill_error(bw, bh, dst, pitch); return 1; }   // HDR
        uint8_t c[4]; for (int i = 0; i < 4; i++) c[i] = (uint8_t)(bits(&b, 64 + 16 * i, 16) >> 8);
        for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) memcpy(dst + y * pitch + x * 4, c, 4);
        return 0;
    }
    // block mode
    int W, H, R, hp, dual;
    if (mode & 3) {
        R = (int)(((mode & 3) << 1) | ((mode >> 4) & 1)); hp = (mode >> 9) & 1; dual = (mode >> 10) & 1;
        int A = (mode >> 5) & 3, B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
            case 0: W = B + 4; H = A + 2; break;
            case 1: W = B + 8; H = A + 2; break;
            case 2: W = A + 2; H = B + 8; break;
            default: if (!((mode >> 8) & 1)) { W = A + 2; H = (B & 1) + 6; } else { W = (B & 1) + 2; H = A + 2; } break;
        }
    } else {
        R = (int)((((mode >> 2) & 3) << 1) | ((mode >> 4) & 1)); hp = (mode >> 9) & 1; dual = (mode >> 10) & 1;
        if ((mode & 0xF) == 0) { fill_error(bw, bh, dst, pitch); return 1; }
        int A = (mode >> 5) & 3, B = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
            case 0: W = 12; H = A + 2; break;
            case 1: W = A + 2; H = 12; break;
            case 3: if (!((mode >> 5) & 1)) { W = 6; H = 10; } else { W = 10; H = 6; } break;
            default: W = A + 6; H = B + 6; hp = 0; dual = 0; break;
        }
    }
    if (R < 2) { fill_error(bw, bh, dst, pitch); return 1; }
    static const int wq_idx[2][8] = {{0, 0, 0, 1, 2, 3, 4, 5}, {0, 0, 6, 7, 8, 9, 10, 11}};   // index into ISE levels
    int wq = wq_idx[hp][R];   // levels 2,3,4,5,6,8 / 10,12,16,20,24,32
    int nw = W * H * (dual ? 2 : 1);
    if (W > bw || H > bh || nw > 64) { fill_error(bw, bh, dst, pitch); return 1; }
    int wbits = ise_bitcount(wq, nw);
    if (wbits < 24 || wbits > 96) { fill_error(bw, bh, dst, pitch); return 1; }
    int parts = (int)bits(&b, 11, 2) + 1;
    if (parts == 4 && dual) { fill_error(bw, bh, dst, pitch); return 1; }
    int cem[4], seed = 0, color_at, below = 128 - wbits;   // 'below': first bit of the weight area (weights are at the top)
    if (parts == 1) { cem[0] = (int)bits(&b, 13, 4); color_at = 17; }
    else {
        seed = (int)bits(&b, 13, 10); color_at = 29;
        uint32_t cfg = bits(&b, 23, 6);
        if ((cfg & 3) == 0) { for (int i = 0; i < parts; i++) cem[i] = (int)(cfg >> 2); }
        else {
            int extra = 3 * parts - 4; below -= extra;
            uint32_t all = cfg | (bits(&b, below, extra) << 6);
            int base = (int)(all & 3) - 1;
            for (int i = 0; i < parts; i++) {
                int ci = (all >> (2 + i)) & 1, mi = (all >> (2 + parts + 2 * i)) & 3;
                cem[i] = ((base + ci) << 2) | mi;
            }
        }
    }
    int ccs = 0;
    if (dual) { below -= 2; ccs = (int)bits(&b, below, 2); }
    int nvals = 0; for (int i = 0; i < parts; i++) nvals += ((cem[i] >> 2) + 1) * 2;
    if (nvals > 18) { fill_error(bw, bh, dst, pitch); return 1; }
    int cbits = below - color_at;
    if (cbits < (13 * nvals + 4) / 5) { fill_error(bw, bh, dst, pitch); return 1; }
    int cq = -1; for (int q = 20; q >= 0; q--) if (ise_bitcount(q, nvals) <= cbits) { cq = q; break; }
    if (cq < 4) { fill_error(bw, bh, dst, pitch); return 1; }   // fewer than 6 levels is invalid for colours
    int D[18], M[18], cv[18];
    ise_decode(&b, color_at, cq, nvals, D, M);
    for (int i = 0; i < nvals; i++) cv[i] = unq_color(cq, D[i], M[i]);
    int e0[4][4], e1[4][4], k = 0, hdr = 0;
    for (int i = 0; i < parts; i++) { hdr |= endpoints(cem[i], cv + k, e0[i], e1[i]); k += ((cem[i] >> 2) + 1) * 2; }
    if (hdr) { fill_error(bw, bh, dst, pitch); return 1; }
    // weights: bit-reversed at the top of the block
    u128 rb = reverse128(b);
    int WD[64], WM[64], wv[64];
    ise_decode(&rb, 0, wq, nw, WD, WM);
    for (int i = 0; i < nw; i++) wv[i] = unq_weight(wq, WD[i], WM[i]);
    (void)ISE_LEVELS;
    int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1), small = bw * bh < 31, planes = dual ? 2 : 1;
    for (int t = 0; t < bh; t++) for (int s = 0; s < bw; s++) {
        int cs = Ds * s, ct = Dt * t, gs = (cs * (W - 1) + 32) >> 6, gt = (ct * (H - 1) + 32) >> 6;
        int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
        int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
        int pw[2];
        for (int pl = 0; pl < planes; pl++) {
            int v0 = js + jt * W;
            int p00 = wv[v0 * planes + pl];
            int p01 = js + 1 < W ? wv[(v0 + 1) * planes + pl] : 0;
            int p10 = jt + 1 < H ? wv[(v0 + W) * planes + pl] : 0;
            int p11 = js + 1 < W && jt + 1 < H ? wv[(v0 + W + 1) * planes + pl] : 0;
            pw[pl] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
        }
        int part = parts > 1 ? select_partition(seed, s, t, 0, parts, small) : 0;
        uint8_t *o = dst + t * pitch + s * 4;
        for (int c = 0; c < 4; c++) {
            int w = dual && c == ccs ? pw[1] : pw[0];
            int c0 = (e0[part][c] << 8) | (srgb ? 0x80 : e0[part][c]);   /* sRGB: all four channels, alpha included (matches astcenc) */
            int c1 = (e1[part][c] << 8) | (srgb ? 0x80 : e1[part][c]);
            int v = (c0 * (64 - w) + c1 * w + 32) >> 6;
            o[c] = (uint8_t)(v >> 8);
        }
    }
    return 0;
}

void nvn_astc_decode_image(const uint8_t *src, size_t src_pitch, int bw, int bh, uint32_t w, uint32_t h, int srgb, uint8_t *dst, size_t dst_pitch) {
    uint8_t tile[12 * 12 * 4];
    uint32_t nbx = (w + bw - 1) / bw, nby = (h + bh - 1) / bh;
    for (uint32_t by = 0; by < nby; by++) for (uint32_t bx = 0; bx < nbx; bx++) {
        nvn_astc_decode_block(src + by * src_pitch + bx * 16, bw, bh, srgb, tile, (size_t)bw * 4);
        for (int y = 0; y < bh && by * bh + y < h; y++) {
            uint32_t x0 = bx * bw, n = (uint32_t)bw; if (x0 + n > w) n = w - x0;
            memcpy(dst + (size_t)(by * bh + y) * dst_pitch + (size_t)x0 * 4, tile + (size_t)y * bw * 4, (size_t)n * 4);
        }
    }
}
