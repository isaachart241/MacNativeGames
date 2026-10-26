#!/usr/bin/env python3
"""Thor port: check the translator's Vulkan service (totk_xlate --service-vk, VkService.cs) against the shader bridge
(android/tools/shader_bridge.py) it was ported from: every variant in the Mac port's shader cache goes through both,
and so does a transform feedback variant of each vertex shader (its first output captured), and the two outputs
(<variant>_v.spv and _v.json), or the two errors, must be the same bytes.

    android/xlate/build.sh mac && python3 android/xlate/check_parity.py
"""
import argparse, glob, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCARLET = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(SCARLET, 'android', 'tools'))
import shader_bridge  # noqa: E402


def requests(cache):
    for p in sorted(glob.glob(os.path.join(cache, '*.variants'))):
        for v in json.load(open(p)):
            mv = v['variant']
            b = json.load(open(os.path.join(cache, mv + '_b.json')))
            rq = {'variant': mv, 'stage': b['stage'], 'code_hex': open(os.path.join(cache, mv + '.code'), 'rb').read().hex(),
                  'c1': b.get('c1', ''), 'depth_minus_w': False,
                  'caps': {'float64': False, 'sbuf_align': 64},
                  **{k: v['deps'].get(k, {}) for k in ('tex', 'img', 'attr', 'fout')}}
            if b['stage'] == 'cs' and b.get('threadgroup_size'):
                rq['local'] = b['threadgroup_size']
            yield rq
            if b['stage'] == 'vs':
                locs = sorted({vi['location'] for vi in b.get('vertex_outputs', [])}) or [0]
                yield dict(rq, variant=mv + '-tf', tf={'slots': [locs[0]], 'comps': [3], 'strides': [12]})


def main():
    work = os.path.join(SCARLET, 'SCARLET_build')
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cache', default=os.path.join(work, 'build', 'shader-cache', 'rt-totk-native-v4-unverified'))
    ap.add_argument('--service', default=os.path.join(work, 'build-android-tools', 'xlate-mac', 'totk_xlate'))
    ap.add_argument('--out', default=os.path.join(work, 'xlate-parity'))
    a = ap.parse_args()
    sc = os.path.join(work, 'build', 'spirv-cross', 'spirv-cross')
    bridge = shader_bridge.Translator(os.path.join(work, 'build', 'shader-service', 'totk_xlate'), sc, os.path.join(a.out, 'bridge'), 'thor-vk1')
    results = os.path.join(a.out, 'results')
    svc = subprocess.Popen([a.service, '--service-vk', '--spirv-cross', sc, '--out', os.path.join(a.out, 'svc-work'), '--results', results,
                            '--chain', 'thor-vk1'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    same = diff = both_failed = 0
    for rq in requests(a.cache):
        line = json.dumps(rq)
        try:
            spv, js = shader_bridge.handle(bridge, json.loads(line)); want = ('ok', spv, js)
        except Exception as e:  # noqa: BLE001 - the error text is compared
            want = ('error', str(e))
        svc.stdin.write(line + '\n'); svc.stdin.flush()
        reply = json.loads(svc.stdout.readline())
        if reply.get('ok'):
            got = ('ok', open(os.path.join(results, rq['variant'] + '_v.spv'), 'rb').read(), open(os.path.join(results, rq['variant'] + '_v.json'), 'rb').read())
        else:
            got = ('error', reply.get('error'))
        if want[0] == 'error' and got[0] == 'error':
            both_failed += 1
            if both_failed <= 3: print('both refused %s: %s | %s' % (rq['variant'][-20:], want[1][:80], got[1][:80]))
        elif want == got:
            same += 1
        else:
            diff += 1
            if diff <= 5:
                print('DIFFERENT %s: bridge %s, service %s' % (rq['variant'], want[0], got[0]))
                if want[0] == got[0] == 'ok': print('   spv same: %s, json same: %s' % (want[1] == got[1], want[2] == got[2]))
                elif got[0] == 'error': print('   service: %s' % got[1][:200])
                else: print('   bridge: %s' % want[1][:200])
    svc.stdin.close(); svc.wait()
    print('identical %d, both refused %d, different %d' % (same, both_failed, diff))
    sys.exit(1 if diff else 0)


if __name__ == '__main__':
    main()
