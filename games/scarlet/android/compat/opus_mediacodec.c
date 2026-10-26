/* Thor port: Opus decoding by Android's own decoder (c2.android.opus.decoder, the platform's libopus in the media codec
   process) through the NDK's AMediaCodec, behind the libopus calls runtime/opus_codec.c makes (opus_mediacodec.h).
   One codec per game decoder, made when the game makes its decoder (about 20 ms each; Scarlet makes 65 at start).
   Each packet is queued and its output waited for: about 1.5 ms per 20 ms packet on the Thor, a round trip to the codec
   process. The codec gets an OpusHead with no pre-skip, so it returns every sample; the count returned is the packet's
   own (from its TOC byte, RFC 6716 section 3.1), as libopus returns it. The platform decoder always outputs 48 kHz; a
   decoder the game opens at a lower rate gets every (48000 / rate)-th sample, as libopus's own CELT decoder lowers its
   rate (the packet's coded bandwidth is already below the lower rate's limit). RESET_STATE flushes the codec. A
   codec that cannot be made or stops answering leaves that stream silent (with the right length) rather than stopping
   the game. Checked on the Thor against the platform's Opus encoder: a 440 Hz tone decodes at its own level (rms 5659
   for 5657) through 70 decoders at once, and after a flush. */
#include "opus_mediacodec.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct OpusDecoder { AMediaCodec *mc; int rate, channels, last, step; uint64_t pts; uint32_t id; };

/* SCARLET_OPUS_DUMP=<file> (checks): the first 4000 packets' events, so the same packets can be decoded with libopus on
   the Mac and compared (android/tools/opus_compare.py). Records: 'C' id rate channels, 'R' id, 'P' id len packet n pcm. */
static FILE *dump; static int dump_left = -1; static uint32_t next_id;
static pthread_mutex_t dump_lock = PTHREAD_MUTEX_INITIALIZER;
static void dump_rec(char type, uint32_t id, const void *a, uint32_t an, const void *b, uint32_t bn) {
    pthread_mutex_lock(&dump_lock);
    if (dump_left < 0) { const char *p = getenv("SCARLET_OPUS_DUMP"); dump = p && *p ? fopen(p, "wb") : NULL; dump_left = dump ? 4000 : 0; }
    if (dump && (type != 'P' || dump_left > 0)) {
        fputc(type, dump); fwrite(&id, 4, 1, dump);
        if (type == 'P') { fwrite(&an, 4, 1, dump); dump_left--; }
        if (a) fwrite(a, 1, an, dump);
        if (b) { fwrite(&bn, 4, 1, dump); fwrite(b, 1, bn, dump); }
        if (!dump_left) { fclose(dump); dump = NULL; fprintf(stderr, "[totk-audio] SCARLET_OPUS_DUMP written\n"); }
    }
    pthread_mutex_unlock(&dump_lock);
}

static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t stat_n, stat_ns, stat_worst;
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static void note(double ns) {
    pthread_mutex_lock(&stats_lock);
    stat_n++; stat_ns += (uint64_t)ns; if ((uint64_t)ns > stat_worst) stat_worst = (uint64_t)ns;
    if (stat_n == 1) fprintf(stderr, "[totk-audio] platform Opus decoder: first packet decoded in %.2f ms\n", ns / 1e6);
    if (stat_n % 5000 == 0) {
        fprintf(stderr, "[totk-audio] platform Opus decoder: %llu packets, %.2f ms each on average, worst %.2f ms\n",
                (unsigned long long)stat_n, stat_ns / 1e6 / 5000, stat_worst / 1e6);
        stat_ns = 0; stat_worst = 0;
    }
    pthread_mutex_unlock(&stats_lock);
}

static AMediaCodec *codec_new(int channels) {
    AMediaCodec *mc = AMediaCodec_createDecoderByType("audio/opus"); if (!mc) return NULL;
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/opus");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, 48000);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
    /* OpusHead (RFC 7845 section 5.1): version 1, channels, pre-skip 0, input rate 48000, gain 0, mapping family 0 */
    uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, (uint8_t)channels, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0};
    int64_t zero = 0;   /* csd-1: codec delay, csd-2: seek pre-roll (ns) */
    AMediaFormat_setBuffer(f, "csd-0", head, sizeof head);
    AMediaFormat_setBuffer(f, "csd-1", &zero, sizeof zero);
    AMediaFormat_setBuffer(f, "csd-2", &zero, sizeof zero);
    int ok = AMediaCodec_configure(mc, f, NULL, NULL, 0) == AMEDIA_OK && AMediaCodec_start(mc) == AMEDIA_OK;
    AMediaFormat_delete(f);
    if (!ok) { AMediaCodec_delete(mc); return NULL; }
    return mc;
}
static void codec_free(AMediaCodec *mc) { if (mc) { AMediaCodec_stop(mc); AMediaCodec_delete(mc); } }

int opus_decoder_get_size(int channels) { return channels == 1 || channels == 2 ? (int)sizeof(OpusDecoder) : 0; }
OpusDecoder *opus_decoder_create(opus_int32 rate, int channels, int *err) {
    if ((channels != 1 && channels != 2) || (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000)) {
        if (err) *err = OPUS_BAD_ARG; return NULL;
    }
    OpusDecoder *d = calloc(1, sizeof *d); if (!d) { if (err) *err = OPUS_INTERNAL_ERROR; return NULL; }
    d->rate = rate; d->channels = channels; d->last = rate / 50; d->step = 48000 / rate;
    pthread_mutex_lock(&dump_lock); d->id = next_id++; pthread_mutex_unlock(&dump_lock);
    { int32_t rc[2] = {rate, channels}; dump_rec('C', d->id, rc, sizeof rc, NULL, 0); }
    d->mc = codec_new(channels);
    if (!d->mc) fprintf(stderr, "[totk-audio] platform Opus decoder unavailable: this stream (%d Hz, %d channels) plays silent\n", rate, channels);
    if (err) *err = OPUS_OK;
    return d;
}
void opus_decoder_destroy(OpusDecoder *d) { if (!d) return; codec_free(d->mc); free(d); }
int opus_decoder_ctl(OpusDecoder *d, int request, ...) {
    if (request == OPUS_RESET_STATE) {
        d->last = d->rate / 50;
        dump_rec('R', d->id, NULL, 0, NULL, 0);
        if (d->mc && AMediaCodec_flush(d->mc) != AMEDIA_OK) { codec_free(d->mc); d->mc = codec_new(d->channels); }
        return OPUS_OK;
    }
    if (request == OPUS_GET_FINAL_RANGE_REQUEST) {   /* not known from the platform codec: 0 (not checked, OPUS_NO_RANGE) */
        va_list ap; va_start(ap, request); opus_uint32 *p = va_arg(ap, opus_uint32 *); va_end(ap);
        if (p) *p = 0; return OPUS_OK;
    }
    return OPUS_BAD_ARG;
}

/* Samples in a packet at the decoder's rate, from its TOC byte; per_frame gets one frame's. -1: not a valid packet. */
static int packet_samples(const unsigned char *data, opus_int32 len, int rate, int *per_frame) {
    int config = data[0] >> 3, code = data[0] & 3, f48;
    if (config < 12) f48 = (int[]){480, 960, 1920, 2880}[config & 3];          /* SILK: 10, 20, 40, 60 ms */
    else if (config < 16) f48 = config & 1 ? 960 : 480;                        /* hybrid: 10, 20 ms */
    else f48 = (int[]){120, 240, 480, 960}[config & 3];                        /* CELT: 2.5, 5, 10, 20 ms */
    int frames = code == 0 ? 1 : code < 3 ? 2 : len >= 2 ? data[1] & 0x3f : 0;
    if (!frames || f48 * frames > 5760) return -1;                             /* more than 120 ms */
    *per_frame = (int)((int64_t)f48 * rate / 48000);
    return (int)((int64_t)f48 * frames * rate / 48000);
}

int opus_decode(OpusDecoder *d, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int max, int fec) {
    (void)fec; int n, per = d->last;
    if (!data || len <= 0) n = d->last;   /* a lost packet: one frame of silence at the previous length */
    else if ((n = packet_samples(data, len, d->rate, &per)) < 0) return OPUS_INVALID_PACKET;
    if (n > max) return OPUS_BUFFER_TOO_SMALL;
    size_t bytes = (size_t)n * (size_t)d->channels * sizeof *pcm;
    memset(pcm, 0, bytes);
    if (!data || len <= 0 || !d->mc) return n;
    d->last = per;
    double t0 = now_ns();
    ssize_t in = AMediaCodec_dequeueInputBuffer(d->mc, 200000);
    size_t cap = 0; uint8_t *b = in >= 0 ? AMediaCodec_getInputBuffer(d->mc, (size_t)in, &cap) : NULL;
    if (!b || (size_t)len > cap) { fprintf(stderr, "[totk-audio] platform Opus decoder: no input buffer (%zd): silence\n", in); return n; }
    memcpy(b, data, (size_t)len);
    AMediaCodec_queueInputBuffer(d->mc, (size_t)in, 0, (size_t)len, d->pts, 0);
    d->pts += (uint64_t)n * 1000000 / (uint64_t)d->rate;
    for (int waits = 0; waits < 8;) {
        AMediaCodecBufferInfo info; ssize_t o = AMediaCodec_dequeueOutputBuffer(d->mc, &info, 50000);
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *f = AMediaCodec_getOutputFormat(d->mc); int32_t r = 0;
            if (f && AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &r) && r >= d->rate && r % d->rate == 0) d->step = r / d->rate;
            if (f) AMediaFormat_delete(f);
            continue;
        }
        if (o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (o < 0) { waits++; continue; }
        const int16_t *src = (const int16_t *)(AMediaCodec_getOutputBuffer(d->mc, (size_t)o, &cap) + info.offset);
        int got = info.size / (int)(sizeof *pcm * (size_t)d->channels) / d->step; if (got > n) got = n;
        for (int k = 0; k < got; k++) for (int c = 0; c < d->channels; c++) pcm[k * d->channels + c] = src[k * d->step * d->channels + c];
        AMediaCodec_releaseOutputBuffer(d->mc, (size_t)o, false);
        note(now_ns() - t0);
        dump_rec('P', d->id, data, (uint32_t)len, pcm, (uint32_t)bytes);
        return n;
    }
    fprintf(stderr, "[totk-audio] platform Opus decoder: no output after 400 ms: silence\n");
    return n;
}
const char *opus_strerror(int e) {
    return e == OPUS_INVALID_PACKET ? "invalid packet" : e == OPUS_BUFFER_TOO_SMALL ? "buffer too small" : e == OPUS_BAD_ARG ? "invalid argument" : "error";
}
