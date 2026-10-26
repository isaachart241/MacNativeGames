#!/usr/bin/env python3
"""Thor port: translate for the Thor, ahead of play, the shaders the Mac port has met, so the game on the device does
not need the Mac's shader bridge where the Mac has already been.

The Mac's shader cache (SCARLET_build/build/shader-cache/<chain>/) keeps, for each shader variant it translated, the
guest code (<variant>.code: the code area from 0x30, the bytes the Vulkan backend sends), the constant section c1 and
the stage (<variant>_b.json), and the bind state the translation depends on (<key>.variants: tex / img / attr / fout).
Shader keys are the same on both ports (SHA-256 of the control section and the code). This runs each of those through
the bridge's translation (android/tools/shader_bridge.py handle: the MSL chain's SPIR-V with the Vulkan fixes) with the
recorded bind state as the answers, and writes what the device's cache holds (third_party/d2r_gfx/gfx/nvn/nvn_vk_rt.c):
<out>/<variant>_v.spv and _v.json, and the index lines (index-add.tsv: "<key>\\t<variant>\\t0\\t<deps>").
Shaders whose key the device already has (--have: the device's index.tsv) are skipped.

    python3 android/tools/prefill_shaders.py --have device-index.tsv --caps device-vk-caps.json \\
        --out SCARLET_build/thor-shader-prefill
then copy <out> into the app's user-data/shader-cache-vk and append index-add.tsv to its index.tsv
(android/tools/push_shaders.sh). Transform feedback variants are not made here: the Mac captures differently, and
their layout is known only when the game draws.
"""
import argparse, glob, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shader_bridge  # noqa: E402

SCARLET = os.path.dirname(os.path.dirname(HERE))


def mac_variants(cache):
    """(key, mac variant, deps) for every variant in the Mac cache."""
    for path in sorted(glob.glob(os.path.join(cache, '*.variants'))):
        key = os.path.basename(path)[:-len('.variants')]
        with open(path) as f:
            for v in json.load(f):
                yield key, v['variant'], v['deps']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--work', default=os.path.join(SCARLET, 'SCARLET_build'))
    ap.add_argument('--mac-cache', help='default: <work>/build/shader-cache/rt-totk-native-v4-unverified')
    ap.add_argument('--chain', default='thor-vk1')
    ap.add_argument('--have', help="the device's index.tsv: keys it has are skipped")
    ap.add_argument('--caps', required=True, help="the device's vk-caps.json (the translator capabilities it sends)")
    ap.add_argument('--out', required=True)
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    cache = a.mac_cache or os.path.join(a.work, 'build', 'shader-cache', 'rt-totk-native-v4-unverified')
    have = set()
    if a.have:
        with open(a.have) as f:
            have = {line.split('\t', 1)[0] for line in f if '\t' in line}
    with open(a.caps) as f:
        caps = json.load(f)
    build = os.path.join(a.work, 'build')
    work = os.path.join(a.out, 'work')
    tr = shader_bridge.Translator(os.path.join(build, 'shader-service', 'totk_xlate'), os.path.join(build, 'spirv-cross', 'spirv-cross'),
                                  work, a.chain)
    done = skipped = failed = 0
    index = open(os.path.join(a.out, 'index-add.tsv'), 'a')
    for key, mac_variant, deps in mac_variants(cache):
        if key in have:
            skipped += 1
            continue
        if a.limit and done >= a.limit:
            break
        variant = '%s-m%s' % (key, mac_variant.rsplit('-', 1)[1][:12])
        if os.path.exists(os.path.join(a.out, variant + '_v.spv')):
            skipped += 1
            continue
        with open(os.path.join(cache, mac_variant + '_b.json')) as f:
            b = json.load(f)
        with open(os.path.join(cache, mac_variant + '.code'), 'rb') as f:
            code = f.read()
        rq = {'variant': variant, 'stage': b['stage'], 'code_hex': code.hex(), 'c1': b.get('c1', ''),
              'depth_minus_w': False, 'caps': caps,
              'tex': deps.get('tex', {}), 'img': deps.get('img', {}), 'attr': deps.get('attr', {}), 'fout': deps.get('fout', {})}
        if b['stage'] == 'cs' and b.get('threadgroup_size'):
            rq['local'] = b['threadgroup_size']
        try:
            spv, js = shader_bridge.handle(tr, rq)
        except Exception as e:  # noqa: BLE001 - reported, the rest go on
            failed += 1
            print('FAILED %s %s: %s' % (b['stage'], variant, e), flush=True)
            continue
        with open(os.path.join(a.out, variant + '_v.spv'), 'wb') as f:
            f.write(spv)
        with open(os.path.join(a.out, variant + '_v.json'), 'wb') as f:
            f.write(js)
        d = json.loads(js)['deps']
        index.write('%s\t%s\t0\t%s\n' % (key, variant, json.dumps(d, sort_keys=True, separators=(',', ':'))))
        index.flush()
        done += 1
        if done % 25 == 0:
            print('%d translated' % done, flush=True)
    print('translated %d, skipped %d (the device has them), failed %d -> %s' % (done, skipped, failed, a.out))


if __name__ == '__main__':
    main()
