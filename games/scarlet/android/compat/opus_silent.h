/* Thor port: stand-in for the libopus calls runtime/opus_codec.c makes, used while the Android build has no libopus.
   It decodes nothing: each packet's length in samples comes from its TOC byte (RFC 6716 section 3.1: frame duration from
   the configuration, frame count from the code), and the output is silence, so the game's audio timing stays right.
   The final-range integrity check needs a real decoder and is skipped (OPUS_SILENT in opus_codec.c). */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int16_t opus_int16; typedef int32_t opus_int32; typedef uint32_t opus_uint32;
typedef struct OpusDecoder { int rate, channels, last; } OpusDecoder;
enum { OPUS_OK = 0, OPUS_BAD_ARG = -1, OPUS_BUFFER_TOO_SMALL = -2, OPUS_INVALID_PACKET = -4, OPUS_RESET_STATE = 4028, OPUS_GET_FINAL_RANGE_REQUEST = 4031 };
#define OPUS_GET_FINAL_RANGE(x) OPUS_GET_FINAL_RANGE_REQUEST, (x)
static inline int opus_decoder_get_size(int channels) { return channels == 1 || channels == 2 ? (int)sizeof(OpusDecoder) : 0; }
static inline OpusDecoder *opus_decoder_create(opus_int32 rate, int channels, int *err) {
    OpusDecoder *d = calloc(1, sizeof *d); if (d) { d->rate = rate; d->channels = channels; d->last = rate / 50; }
    if (err) *err = d ? OPUS_OK : OPUS_BAD_ARG; return d;
}
static inline void opus_decoder_destroy(OpusDecoder *d) { free(d); }
static inline int opus_decoder_ctl(OpusDecoder *d, int request, ...) {
    if (request == OPUS_RESET_STATE) { d->last = d->rate / 50; return OPUS_OK; }
    if (request == OPUS_GET_FINAL_RANGE_REQUEST) {   /* no decoder state: report 0 (not checked when OPUS_SILENT) */
        __builtin_va_list ap; __builtin_va_start(ap, request); opus_uint32 *p = __builtin_va_arg(ap, opus_uint32 *); __builtin_va_end(ap);
        if (p) *p = 0; return OPUS_OK;
    }
    return OPUS_BAD_ARG;
}
static inline int opus_decode(OpusDecoder *d, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int max, int fec) {
    (void)fec; int n;
    if (!data || len <= 0) n = d->last;   /* a lost packet: one frame of the previous length (packet loss concealment) */
    else {
        int config = data[0] >> 3, code = data[0] & 3, f48;
        if (config < 12) f48 = (int[]){480, 960, 1920, 2880}[config & 3];          /* SILK: 10, 20, 40, 60 ms */
        else if (config < 16) f48 = config & 1 ? 960 : 480;                        /* hybrid: 10, 20 ms */
        else f48 = (int[]){120, 240, 480, 960}[config & 3];                        /* CELT: 2.5, 5, 10, 20 ms */
        int frames = code == 0 ? 1 : code < 3 ? 2 : len >= 2 ? data[1] & 0x3f : 0;
        if (!frames) return OPUS_INVALID_PACKET;
        n = f48 * frames;
        if (n > 5760) return OPUS_INVALID_PACKET;                                  /* more than 120 ms */
        n = (int)((int64_t)n * d->rate / 48000); d->last = n / frames;
    }
    if (n > max) return OPUS_BUFFER_TOO_SMALL;
    memset(pcm, 0, (size_t)n * (size_t)d->channels * sizeof *pcm);
    return n;
}
static inline const char *opus_strerror(int e) { return e == OPUS_INVALID_PACKET ? "invalid packet" : e == OPUS_BUFFER_TOO_SMALL ? "buffer too small" : "error"; }
