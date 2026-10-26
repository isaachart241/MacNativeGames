#!/usr/bin/env python3
"""Thor port: check the platform Opus decoder (android/compat/opus_mediacodec.c) against libopus on the Mac.

Run the game on the Thor with SCARLET_OPUS_DUMP=logs/opus.dump (env.txt), pull the file, then
    python3 android/tools/opus_compare.py opus.dump
Each game decoder's packets are decoded again here with libopus (Homebrew's, as the Mac port uses), from its creation
and through its resets, and the device's output is compared with it: the signal-to-difference ratio per decoder and
overall, and the number of packets whose sample count differs.
"""
import ctypes, ctypes.util, math, struct, sys

def load_opus():
    for p in (ctypes.util.find_library('opus'), '/opt/homebrew/lib/libopus.dylib', '/usr/local/lib/libopus.dylib'):
        if p:
            try:
                return ctypes.CDLL(p)
            except OSError:
                pass
    sys.exit('libopus not found (brew install opus)')

def main():
    lib = load_opus()
    lib.opus_decoder_create.restype = ctypes.c_void_p
    lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
    lib.opus_decoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int]
    data = open(sys.argv[1], 'rb').read()
    at, decs, stats = 0, {}, {}
    pcm = (ctypes.c_int16 * (5760 * 2))()
    tot_sig = tot_diff = 0.0; packets = count_diff = 0
    while at < len(data):
        t = chr(data[at]); did = struct.unpack_from('<I', data, at + 1)[0]; at += 5
        if t == 'C':
            rate, ch = struct.unpack_from('<ii', data, at); at += 8
            err = ctypes.c_int(0); decs[did] = (lib.opus_decoder_create(rate, ch, ctypes.byref(err)), ch)
        elif t == 'R':
            lib.opus_decoder_ctl(decs[did][0], 4028)
        elif t == 'P':
            n = struct.unpack_from('<I', data, at)[0]; at += 4
            pkt = data[at:at + n]; at += n
            bn = struct.unpack_from('<I', data, at)[0]; at += 4
            dev = struct.unpack_from('<%dh' % (bn // 2), data, at); at += bn
            st, ch = decs[did]
            got = lib.opus_decode(st, pkt, n, pcm, 5760, 0)
            ref = pcm[:max(got, 0) * ch]
            if len(ref) != len(dev): count_diff += 1
            m = min(len(ref), len(dev))
            sig = sum(float(x) * x for x in ref[:m]); diff = sum(float(a - b) * (a - b) for a, b in zip(ref[:m], dev[:m]))
            s = stats.setdefault(did, [0.0, 0.0, 0]); s[0] += sig; s[1] += diff; s[2] += 1
            tot_sig += sig; tot_diff += diff; packets += 1
        else:
            sys.exit('bad record %r at %d' % (t, at - 5))
    snr = lambda s, d: float('inf') if d == 0 else 10 * math.log10(s / d) if s > 0 else float('-inf')
    for did, (s, d, k) in sorted(stats.items()):
        print('decoder %3d: %5d packets, signal/difference %.1f dB' % (did, k, snr(s, d)))
    print('all: %d packets from %d decoders, signal/difference %.1f dB, %d with another sample count'
          % (packets, len(stats), snr(tot_sig, tot_diff), count_diff))

if __name__ == '__main__':
    main()
