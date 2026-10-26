/* Thor port: the libopus calls runtime/opus_codec.c makes, done by Android's own Opus decoder through the NDK's
   AMediaCodec (android/compat/opus_mediacodec.c). Names and values match libopus for the calls used. The platform codec
   does not report the decoder's final range, so opus_codec.c skips that integrity check (OPUS_NO_RANGE). */
#pragma once
#include <stdint.h>
typedef int16_t opus_int16; typedef int32_t opus_int32; typedef uint32_t opus_uint32;
typedef struct OpusDecoder OpusDecoder;
enum { OPUS_OK = 0, OPUS_BAD_ARG = -1, OPUS_BUFFER_TOO_SMALL = -2, OPUS_INTERNAL_ERROR = -3, OPUS_INVALID_PACKET = -4,
       OPUS_RESET_STATE = 4028, OPUS_GET_FINAL_RANGE_REQUEST = 4031 };
#define OPUS_GET_FINAL_RANGE(x) OPUS_GET_FINAL_RANGE_REQUEST, (x)
int opus_decoder_get_size(int channels);
OpusDecoder *opus_decoder_create(opus_int32 rate, int channels, int *err);
void opus_decoder_destroy(OpusDecoder *d);
int opus_decoder_ctl(OpusDecoder *d, int request, ...);
int opus_decode(OpusDecoder *d, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int max, int fec);
const char *opus_strerror(int e);
