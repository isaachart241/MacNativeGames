#!/usr/bin/env python3
"""Thor port, stage T2: shader translation for the Vulkan backend, done on the Mac and reached from the device over adb.

The Vulkan backend (third_party/d2r_gfx/gfx/nvn/nvn_vk_rt.c) asks a translator service for each shader variant it has not
got: one JSON request line ({variant, stage, code_hex, c1, tex/img/attr/fout answers, local, depth_minus_w, caps}), as
the TotK tree's "totk_xlate --service --target spirv" takes it. Scarlet's translator (prebuilt/macos-arm64, the MSL
chain) has no SPIR-V target, but its SPIR-V is the shader the SPIR-V chain would emit, translated the same way
(Service.cs Handle and HandleSpirv build the same job), except that every resource is in descriptor set 0 and the
host capabilities are Metal's (no FP64: the MSL chain's softfloat lowering is kept, which the Adreno needs too). So this
bridge runs the MSL chain, then
  - moves a pixel shader's resources to set 1 (the support buffer stays set 0 binding 0, shared by every stage), and
  - writes the SPIR-V chain's reflection (<variant>_v.json) from the SPIR-V itself, a port of SpirvReflect.cs and
    Service.cs HandleSpirv,
and sends <variant>_v.spv and <variant>_v.json back: a reply line {"ok": true, "spv": n, "json": m} followed by the n + m
bytes. Errors are {"ok": false, "error": ...} with no payload.

Usage (from games/scarlet, after "make shader-tools"):
  adb reverse tcp:7841 tcp:7841
  python3 android/tools/shader_bridge.py --port 7841
and start the game on the device with TOTK_XLATE_TCP=7841.
"""
import argparse, json, os, re, socket, struct, subprocess, sys, threading

HERE = os.path.dirname(os.path.abspath(__file__))
SCARLET = os.path.dirname(os.path.dirname(HERE))

# ---------- SPIR-V reading (SpirvReflect.cs) ----------
class Var:
    __slots__ = ('id', 'type_ptr', 'pointee', 'storage', 'name', 'set', 'binding', 'location', 'builtin', 'stored', 'nonwritable')
    def __init__(self, id_, type_ptr, storage):
        self.id, self.type_ptr, self.storage = id_, type_ptr, storage
        self.pointee = 0; self.name = ''; self.set = self.binding = self.location = self.builtin = -1
        self.stored = self.nonwritable = False

def _str(w, at):
    b = bytearray(); i = at
    while i < len(w):
        x = w[i]; i += 1; end = False
        for k in range(4):
            c = (x >> (8 * k)) & 255
            if c == 0: end = True; break
            b.append(c)
        if end: break
    return b.decode('utf-8', 'replace'), i

class Spirv:
    def __init__(self, data):
        if len(data) < 20 or len(data) % 4: raise ValueError('SPIR-V: bad length')
        words = struct.unpack('<%dI' % (len(data) // 4), data)
        if words[0] != 0x07230203: raise ValueError('SPIR-V: bad magic')
        self.vars, self.types, self.consts, self.names, self.decos = {}, {}, {}, {}, {}
        self.chain_root, self.chains, self.caps, self.exts = {}, [], [], []
        self.entry_name, self.local_size, self.uses_fp64 = 'main', None, False
        self.insns = []   # (word offset, opcode, operands) for rewriting
        p = 5
        while p < len(words):
            n, op = words[p] >> 16, words[p] & 0xffff
            if n == 0 or p + n > len(words): raise ValueError('SPIR-V: bad instruction at word %d' % p)
            w = words[p + 1:p + n]
            self.insns.append((p, op, w))
            if op == 17: self.caps.append(w[0])
            elif op == 10: self.exts.append(_str(w, 0)[0])
            elif op == 15: self.entry_name = _str(w, 2)[0]
            elif op == 16:
                if w[1] == 17 and len(w) >= 5: self.local_size = [w[2], w[3], w[4]]
            elif op == 5: self.names[w[0]] = _str(w, 1)[0]
            elif op == 71: self.decos.setdefault(w[0], []).append((w[1], w[2:]))
            elif op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32):
                if op == 22 and w[1] == 64: self.uses_fp64 = True
                self.types[w[0]] = (op, w)
            elif op == 43:
                if len(w) >= 3: self.consts[w[1]] = w[2]
            elif op == 59:
                self.vars[w[1]] = Var(w[1], w[0], w[2]); self.chain_root[w[1]] = w[1]
            elif op in (65, 66, 67, 70):
                root = self.chain_root.get(w[2])
                if root is not None:
                    self.chain_root[w[1]] = root
                    if w[2] == root: self.chains.append((root, w[3:]))
            elif op == 83:
                if w[2] in self.chain_root: self.chain_root[w[1]] = self.chain_root[w[2]]
            elif op in (62, 63, 228):
                self._store(w[0])
            elif (227 <= op <= 242 and op != 228 and len(w) >= 3) or (op in (318, 319, 6035) and len(w) >= 3):
                self._store(w[2])
            p += n
        self.words = list(words)
        for v in self.vars.values():
            v.name = self.names.get(v.id, '')
            t = self.types.get(v.type_ptr)
            if t and t[0] == 32: v.pointee = t[1][2]
            for dec, a in self.decos.get(v.id, []):
                if dec == 34: v.set = a[0]
                elif dec == 33: v.binding = a[0]
                elif dec == 30: v.location = a[0]
                elif dec == 11: v.builtin = a[0]
                elif dec == 24: v.nonwritable = True
    def _store(self, ptr):
        r = self.chain_root.get(ptr)
        if r is not None and r in self.vars: self.vars[r].stored = True
    def has_deco(self, id_, dec): return any(d == dec for d, _ in self.decos.get(id_, []))
    def elem(self, ty):
        count = 1; t = self.types.get(ty)
        while t and t[0] in (28, 29):
            if t[0] == 29: count = 0
            else: count *= self.consts.get(t[1][2], 1)
            ty = t[1][1]; t = self.types.get(ty)
        return ty, count
    def scalar_class(self, ty):
        t = self.types.get(ty)
        while t and t[0] == 23: t = self.types.get(t[1][1])
        if not t: return '?'
        return 'float' if t[0] == 22 else ('int' if t[1][2] else 'uint') if t[0] == 21 else '?'
    def set_descriptor_sets(self, fn):
        """Rewrites every DescriptorSet decoration to fn(var) (None keeps it); returns the new module bytes."""
        w = self.words[:]
        for p, op, ops in self.insns:
            if op == 71 and ops[1] == 34 and ops[0] in self.vars:
                s = fn(self.vars[ops[0]])
                if s is not None: w[p + 3] = s
        return struct.pack('<%dI' % len(w), *w)

# ---------- reflection (Service.cs HandleSpirv) ----------
TEX_BASE, SAMP_BASE, IMG_BASE, COMBINED_BASE, COMBINED_SLOTS = 0x168, 0x568, 0x120, 0x20, 32
DRV_FILL_C0 = {'vs': [(0x110, 0x210), (0x610, 0x620)], 'ps': [(0x510, 0x610)], 'cs': [(0x200, 0x410)]}
DRV_FILL_C2 = [(0x20, 0x120), (0x168, 0x568), (0x568, 0x968)]

def _slot(word, base, cb):
    return (word * 4 - base) // 8 if cb == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0 else None
def decode_tex(name):
    m = re.match(r'^[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    slots = int(m.group(1)) if m.group(1) else -1
    h = int(m.group(2), 16)
    tb = slots & 0xffff if slots >= 0 else 2
    sb = (slots >> 16) - 1 if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    words = [o0, o1] if ty else [o0]
    if re.match(r'^[a-z]+_i', name):
        t = next((_slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < TEX_BASE and _slot(x, IMG_BASE, tb) is not None), None)
        return t, None, ty
    t = next((_slot(x, TEX_BASE, tb) for x in words if _slot(x, TEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    if t is None:
        t = next((_slot(x, COMBINED_BASE, tb) for x in words if COMBINED_BASE <= x * 4 < COMBINED_BASE + 8 * COMBINED_SLOTS and _slot(x, COMBINED_BASE, tb) is not None), None)
    s = next((_slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and _slot(x, SAMP_BASE, sb) is not None), None)
    return t, s, ty

def desc_of(m, v):
    ty, count = m.elem(v.pointee); t = m.types.get(ty); img = None
    if not t: return '?', count, img
    if v.storage == 12: return 'ssbo', count, img
    if v.storage == 2: return ('ssbo' if m.has_deco(ty, 3) else 'ubo'), count, img
    if v.storage != 0: return '?', count, img
    if t[0] == 26: return 'sampler', count, img
    combined = t[0] == 27
    it = m.types.get(t[1][1]) if combined else t
    if not it or it[0] != 25: return '?', count, img
    w = it[1]; dim, sampled = w[2], w[6]
    img = {'dim': {0: '1d', 1: '2d', 2: '3d', 3: 'cube', 5: 'buffer'}.get(dim, str(dim)), 'arrayed': w[4] != 0, 'ms': w[5] != 0,
           'shadow': w[3] == 1, 'class': m.scalar_class(w[1]), 'format': w[7]}
    if dim == 5: return ('stexel' if sampled == 2 else 'utexel'), count, img
    if combined: return 'combined', count, img
    return ('storage' if sampled == 2 else 'sampled'), count, img

def reflect(m, rq, res, variant, chain):
    stage = rq['stage']; key = variant + '_v'
    sb_sorted = sorted(res.get('sbufs', []), key=lambda b: b['binding'])
    sb_base = {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}.get(stage)
    binds, drv, used_slots = [], {}, set()
    for v in sorted(m.vars.values(), key=lambda x: (x.set, x.binding)):
        if v.binding < 0 or v.storage not in (0, 2, 12): continue
        nm = v.name; desc, count, img = desc_of(m, v)
        e = {'name': nm, 'set': max(v.set, 0), 'binding': v.binding, 'desc': desc, 'count': count}
        if img: e.update(img)
        fm = re.match(r'^[a-z]+_c(\d+)\Z', nm); fs = re.match(r'^[a-z]+_s(\d+)\Z', nm)
        if nm == 'support_buffer': e['kind'] = 'support'
        elif fm:
            n = int(fm.group(1))
            if 3 <= n <= 16: e['kind'] = 'cbuf'; e['reg'] = n - 3
            else:
                e['kind'] = 'c1' if n == 1 else 'drvcb'; e['bank'] = n
                if n in (0, 2):
                    offs, dyn, anyc = set(), False, False
                    for root, idx in m.chains:
                        if root != v.id: continue
                        anyc = True
                        if len(idx) >= 2 and m.consts.get(idx[0]) == 0 and idx[1] in m.consts: offs.add(m.consts[idx[1]] * 16)
                        else: dyn = True
                    if anyc: drv[n] = (offs, dyn)
        elif fs and desc == 'ssbo':
            e['kind'] = 'sbuf'; index = int(fs.group(1))
            b = sb_sorted[index] if index < len(sb_sorted) else None
            cb_slot = b['cb_slot'] if b else 0; cb_off = b['cb_offset'] if b else 0
            off = cb_off * 4 if b is not None and cb_slot == 0 else None
            if stage == 'cs' and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0: e['kind'] = 'ubo_ptr'; e['reg'] = (off - 0x210) // 16
            elif b is not None and cb_slot != 0 and 3 <= cb_slot <= 16: e['kind'] = 'sbuf_ubo'; e['reg'] = cb_slot - 3; e['off'] = cb_off * 4
            else: e['reg'] = (off - sb_base) // 16 if off is not None and sb_base is not None and 0 <= off - sb_base < 0x100 and (off - sb_base) % 16 == 0 else None
            e['rw'] = bool(b is not None and (b.get('flags', 0) & 1)) or v.stored
        else:
            d = decode_tex(nm) or (decode_tex(re.sub(r'^([a-z]+)_s', r'\1_t', nm)) if desc == 'sampler' else None)
            if d is None: e['kind'] = '?'
            elif desc == 'sampler':
                if d[2]: e['kind'] = 'samp'; e['reg'] = d[1]
                else: e['kind'] = 'samp_tex'; e['reg'] = d[0]
            else:
                is_img = re.match(r'^[a-z]+_i', nm) is not None
                e['kind'] = 'img' if is_img else ('tbuf' if desc == 'utexel' else 'tex'); e['reg'] = d[0]
                if is_img: e['rw'] = v.stored
                if desc == 'combined':
                    if d[2]: e['samp_kind'] = 'samp'; e['samp_reg'] = d[1]
                    else: e['samp_kind'] = 'samp_tex'; e['samp_reg'] = d[0]
                if not is_img and d[0] is not None: used_slots.add(d[0])
        binds.append(e)
    dr = {str(k): {'vec4_offsets': sorted(o), 'dynamic': dy} for k, (o, dy) in sorted(drv.items())}
    unfillable = []
    for k, d in dr.items():
        if d['dynamic']: unfillable.append('c%s dynamic index' % k)
        ranges = DRV_FILL_C2 if k == '2' else DRV_FILL_C0[stage] if k == '0' else []
        unfillable += ['c%s+0x%x' % (k, o) for o in d['vec4_offsets'] if not any(lo <= o < hi for lo, hi in ranges)]
    vertex_inputs, colors, writes_depth = [], set(), False
    for v in sorted(m.vars.values(), key=lambda x: x.location):
        if stage == 'vs' and v.storage == 1 and v.location >= 0 and v.builtin < 0:
            vertex_inputs.append({'location': v.location, 'type': m.scalar_class(m.elem(v.pointee)[0])})
        if stage == 'ps' and v.storage == 3:
            if v.location >= 0 and v.builtin < 0: colors.add(v.location)
            if v.builtin == 22: writes_depth = True
    ans = lambda k: rq.get(k) or {}
    deps = {'tex': {}, 'img': {}, 'attr': {}, 'fout': {}}
    for s in sorted(set(res.get('tex_queried', [])) | used_slots): deps['tex'][str(s)] = ans('tex').get(str(s), '2d')
    for s in res.get('img_queried', []): deps['img'][str(s)] = ans('img').get(str(s), {})
    if stage == 'vs':
        for vi in vertex_inputs: deps['attr'][str(vi['location'])] = ans('attr').get(str(vi['location']), 'float')
    if stage == 'ps':
        for c in sorted(colors): deps['fout'][str(c)] = ans('fout').get(str(c), 'float')
    return {
        'key': key, 'route': 'B', 'target': 'spirv', 'chain': chain, 'stage': stage, 'entry': m.entry_name, 'set': 1 if stage == 'ps' else 0,
        'threadgroup_size': m.local_size or rq.get('local'), 'c1': rq.get('c1', ''), 'notes': res.get('notes', []),
        'drv_reads': dr, 'drv_unfillable': unfillable, 'bindings': binds, 'vertex_inputs': vertex_inputs, 'frag_outputs': sorted(colors),
        'writes_depth': writes_depth, 'uses_fp64': m.uses_fp64, 'uses_frag_coord': res.get('uses_frag_coord'),
        'capabilities': m.caps, 'extensions': m.exts, 'raster': {}, 'deps': deps,
    }

# ---------- the translator (MSL chain) ----------
class Translator:
    def __init__(self, exe, spirv_cross, out, chain):
        self.out, self.chain = out, chain
        os.makedirs(out, exist_ok=True)
        self.p = subprocess.Popen([exe, '--service', '--spirv-cross', spirv_cross, '--out', out, '--chain', chain],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lock = threading.Lock()
    def call(self, rq):
        with self.lock:
            self.p.stdin.write(json.dumps(rq) + '\n'); self.p.stdin.flush()
            line = self.p.stdout.readline()
        if not line: raise RuntimeError('translator exited')
        return json.loads(line)

def handle(tr, rq):
    variant, stage = rq['variant'], rq['stage']
    code = bytes.fromhex(rq.pop('code_hex'))
    code_path = os.path.join(tr.out, variant + '.code')
    with open(code_path, 'wb') as f: f.write(code)
    msl_rq = {k: v for k, v in rq.items() if k not in ('caps',)}
    msl_rq['code'] = code_path
    reply = tr.call(msl_rq)
    if not reply.get('ok'): raise RuntimeError('translator: %s' % reply.get('error'))
    with open(os.path.join(tr.out, variant + '.spv'), 'rb') as f: spv = f.read()
    with open(os.path.join(tr.out, variant + '.res.json')) as f: res = json.load(f)
    m = Spirv(spv)
    if stage == 'ps':   # the SPIR-V chain's layout: fragment resources in set 1, the shared support buffer in set 0
        spv = m.set_descriptor_sets(lambda v: None if v.name == 'support_buffer' else 1)
        m = Spirv(spv)
    refl = reflect(m, rq, res, variant, tr.chain)
    return spv, json.dumps(refl, indent=1).encode()

def serve(tr, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', port)); srv.listen(4)
    print('[bridge] listening on 127.0.0.1:%d (adb reverse tcp:%d tcp:%d)' % (port, port, port), flush=True)
    n = 0
    while True:
        c, _ = srv.accept()
        threading.Thread(target=client, args=(tr, c), daemon=True).start()

def client(tr, c):
    f = c.makefile('rb')
    while True:
        line = f.readline()
        if not line: break
        try:
            rq = json.loads(line)
            spv, js = handle(tr, rq)
            c.sendall(json.dumps({'ok': True, 'spv': len(spv), 'json': len(js)}).encode() + b'\n' + spv + js)
            print('[bridge] %s %s: %d + %d bytes' % (rq['stage'], rq['variant'], len(spv), len(js)), flush=True)
        except Exception as e:   # noqa: BLE001 - the reply carries any failure to the device
            c.sendall(json.dumps({'ok': False, 'error': str(e)}).encode() + b'\n')
            print('[bridge] FAILED: %s' % e, flush=True)
    c.close()

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', type=int, default=7841)
    ap.add_argument('--work', default=os.path.join(SCARLET, 'SCARLET_build'))
    ap.add_argument('--chain', default='thor-vk1')
    a = ap.parse_args()
    build = os.path.join(a.work, 'build')
    tr = Translator(os.path.join(build, 'shader-service', 'totk_xlate'), os.path.join(build, 'spirv-cross', 'spirv-cross'),
                    os.path.join(a.work, 'thor-shader-work'), a.chain)
    serve(tr, a.port)

if __name__ == '__main__':
    main()
