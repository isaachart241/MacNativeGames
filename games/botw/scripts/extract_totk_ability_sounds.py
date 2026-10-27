#!/usr/bin/env python3
"""Tears of the Kingdom's own sounds for the abilities in Breath of the Wild (docs/botw-abilities.md): reads them from a
Tears of the Kingdom work folder that build_app.py made from the user's own dump, decodes them (BWAV, DSP-ADPCM) and
writes 16-bit WAV files where the BotW app's runtime looks for them. Nothing from either game is in the repository.

usage: extract_totk_ability_sounds.py [--totk-work ~/Games/TOTK_build] [--out ~/Library/Application Support/MacNativeGames/BotW/abilities/sound]"""
import argparse, json, os, struct, sys, wave

# Ascend is "Toreroof" in Tears of the Kingdom's data, Recall "Modoreco" (their Japanese names).
SOUNDS = [
    'Toreroof_Start_00', 'Toreroof_Green_Start_00', 'Toreroof_Green_End_00', 'Toreroof_BgStart_00',
    'Fol_Pl_Toreroof_Start_00', 'Fol_Pl_Toreroof_In_00', 'Fol_Pl_Toreroof_Ground_lp_00', 'Fol_Pl_Toreroof_Jump_00',
    'Fol_Pl_Toreroof_Jump_01', 'Fol_Pl_Toreroof_Out_00', 'Fol_Pl_Toreroof_Out_Eff_00', 'Pl_Toreroof_Cancel_00',
    'Pl_Toreroof_WaterIn_00', 'Pl_Toreroof_WaterIn_01', 'Pl_Toreroof_WaterOut_00', 'Pl_Toreroof_WaterOut_01',
    'Modoreco_Start_00', 'Modoreco_Start00', 'Modoreco_Start01', 'Modoreco_Search_lp_00', 'Modoreco_Reverse_01',
    'Modoreco_Reverse_lp_02', 'Modoreco_Reverse_Spread_00', 'Modoreco_Reverse_End_00', 'Modoreco_End_00',
    'Sys_ShortCut_ItemDecide_Modoreco_01',
]

def dsp_adpcm(data, coefs, samples, hist1=0, hist2=0):
    """Nintendo DSP-ADPCM: 8-byte frames, a header byte (predictor << 4 | scale) and 14 four-bit samples."""
    out = []; i = 0
    while len(out) < samples:
        header = data[i]; i += 1
        pred, scale = header >> 4, 1 << (header & 0xf)
        c1, c2 = coefs[pred * 2], coefs[pred * 2 + 1]
        for j in range(14):
            if len(out) >= samples: break
            byte = data[i + j // 2]
            nib = (byte >> 4) if j % 2 == 0 else (byte & 0xf)
            if nib >= 8: nib -= 16
            s = ((nib * scale) << 11) + 1024 + c1 * hist1 + c2 * hist2
            s >>= 11
            s = -32768 if s < -32768 else 32767 if s > 32767 else s
            out.append(s); hist2, hist1 = hist1, s
        i += 7
    return out

def bwav(data):
    """(rate, [channel samples], loop) of a BWAV: a 0x10-byte header, then 0x4c bytes per channel."""
    assert data[:4] == b'BWAV', data[:4]
    e = '<' if data[4:6] == b'\xff\xfe' else '>'
    nch, = struct.unpack_from(e + 'H', data, 0xe)
    chans, rate, loop = [], 0, None
    for c in range(nch):
        b = 0x10 + 0x4c * c
        codec, pan, rate, samples, _ = struct.unpack_from(e + 'HHIII', data, b)
        coefs = struct.unpack_from(e + '16h', data, b + 0x10)
        offset, = struct.unpack_from(e + 'I', data, b + 0x30)
        loop_end, loop_start = struct.unpack_from(e + 'II', data, b + 0x3c)   # no loop: end 0xffffffff
        if loop_end != 0xffffffff and loop is None: loop = (loop_start, loop_end)
        if codec == 0: chans.append(list(struct.unpack_from(e + f'{samples}h', data, offset)))
        elif codec == 1: chans.append(dsp_adpcm(data[offset:], coefs, samples))
        else: raise ValueError(f'codec {codec}')
    return rate, chans, loop

def main():
    a = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    a.add_argument('--totk-work', default='~/Games/TOTK_build')
    a.add_argument('--out', default='~/Library/Application Support/MacNativeGames/BotW/abilities/sound')
    args = a.parse_args()
    romfs = os.path.join(os.path.expanduser(args.totk_work), 'totk/extracted/v0/romfs')
    out = os.path.expanduser(args.out); os.makedirs(out, exist_ok=True)
    index = {f['path']: f for f in json.load(open(os.path.join(romfs, 'index.json')))['files']}
    with open(os.path.join(romfs, 'romfs.bin'), 'rb') as r:
        for name in SOUNDS:
            f = index.get(f'Sound/Resource/Stream/{name}.bwav')
            if not f: print(f'{name}: not in this dump', file=sys.stderr); continue
            r.seek(f['offset']); rate, chans, loop = bwav(r.read(f['size']))
            n = min(len(c) for c in chans)
            with wave.open(os.path.join(out, name + '.wav'), 'wb') as w:
                w.setnchannels(len(chans)); w.setsampwidth(2); w.setframerate(rate)
                w.writeframes(b''.join(struct.pack('<' + 'h' * len(chans), *(c[k] for c in chans)) for k in range(n)))
            looppath = os.path.join(out, name + '.loop')   # the runtime loops a sound between these frames
            if loop: open(looppath, 'w').write(f'{loop[0]} {loop[1]}\n')
            elif os.path.exists(looppath): os.remove(looppath)
            print(f'{name}: {len(chans)} ch, {rate} Hz, {n / rate:.2f} s' + (f', loop {loop[0]}-{loop[1]}' if loop else ''))

if __name__ == '__main__':
    main()
