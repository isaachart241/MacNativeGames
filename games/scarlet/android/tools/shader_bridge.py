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
        self.functions, self.entry_id, fstart = [], 0, None
        p = 5
        while p < len(words):
            n, op = words[p] >> 16, words[p] & 0xffff
            if n == 0 or p + n > len(words): raise ValueError('SPIR-V: bad instruction at word %d' % p)
            w = words[p + 1:p + n]
            self.insns.append((p, op, w))
            if op == 54: fstart = p
            elif op == 56 and fstart is not None: self.functions.append((fstart, p + n)); fstart = None
            if op == 17: self.caps.append(w[0])
            elif op == 10: self.exts.append(_str(w, 0)[0])
            elif op == 15: self.entry_name = _str(w, 2)[0]; self.entry_id = w[1]
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

# ---------- transform feedback (Scarlet): stores of the captured outputs, as the Metal backend's variant does ----------
TF_INFO_BINDING, TF_BUF_BINDING = 98, 100   # set 0; nvn_vk_exec.c binds them by kind (tf_info, tf_buf)

def _ins(op, *ops):
    return [(len(ops) + 1) << 16 | op] + list(ops)

def tf_patch(m, tf):
    """A vertex shader variant that also writes, before it returns, components 0..comps-1 of the output at location slots[b]
    into storage buffer b at element ((vertex - first) + (instance - base_instance) * count) * (stride_b / 4) + off_b + c, read
    from the info block {first, count, base_instance, pad, stride0..3, off0..3} (std140 scalars). Rasterisation is discarded by the
    pipeline. Returns the new module and the extra reflection bindings."""
    words = list(m.words); bound = words[3]
    def new_id():
        nonlocal bound; bound += 1; return bound - 1
    types = {}   # existing types by (op, operands after the result id)
    for _p, op, w in m.insns:
        if op in (19, 21, 22, 23, 32): types.setdefault((op,) + tuple(w[1:]), w[0])
    decos, tdefs, iface = [], [], []
    def ty(op, *ops):
        k = (op,) + ops
        if k not in types: i = new_id(); types[k] = i; tdefs.append(_ins(op, i, *ops))
        return types[k]
    def const(t, v):
        i = new_id(); tdefs.append(_ins(43, t, i, v)); return i
    t_float, t_uint, t_int = ty(22, 32), ty(21, 32, 0), ty(21, 32, 1)
    # storage buffers: struct { float d[]; } -- StorageBuffer + Block from SPIR-V 1.3, Uniform + BufferBlock before
    sb_class, sb_deco = (12, 2) if words[1] >= 0x10300 else (2, 3)
    rarr = new_id(); tdefs.append(_ins(29, rarr, t_float)); decos.append(_ins(71, rarr, 6, 4))           # ArrayStride 4
    sbs = new_id(); tdefs.append(_ins(30, sbs, rarr)); decos.append(_ins(71, sbs, sb_deco)); decos.append(_ins(72, sbs, 0, 35, 0))   # Offset 0
    p_sbs = new_id(); tdefs.append(_ins(32, p_sbs, sb_class, sbs))
    p_uf = new_id(); tdefs.append(_ins(32, p_uf, sb_class, t_float))
    nb = len(tf['slots']); bufs = []
    for b in range(nb):
        v = new_id(); tdefs.append(_ins(59, p_sbs, v, sb_class)); bufs.append(v); iface.append(v) if words[1] >= 0x10400 else None
        decos.append(_ins(71, v, 34, 0)); decos.append(_ins(71, v, 33, TF_BUF_BINDING + b))
    # info block: 12 uints {first, count, base_instance, pad, stride0..3, off0..3}; off_b: the buffer's start in floats past the
    # 64-byte aligned offset the descriptor has to use (storage buffer offset alignment on the Adreno)
    info_t = new_id(); tdefs.append(_ins(30, info_t, *([t_uint] * 12))); decos.append(_ins(71, info_t, 2))   # Block
    for k in range(12): decos.append(_ins(72, info_t, k, 35, 4 * k))
    p_info = new_id(); tdefs.append(_ins(32, p_info, 2, info_t)); p_uu = new_id(); tdefs.append(_ins(32, p_uu, 2, t_uint))
    info = new_id(); tdefs.append(_ins(59, p_info, info, 2)); decos.append(_ins(71, info, 34, 0)); decos.append(_ins(71, info, 33, TF_INFO_BINDING))
    if words[1] >= 0x10400: iface.append(info)   # SPIR-V 1.4: every global the entry point uses is in its interface
    # built-ins: vertex and instance index (an existing declaration is used with its own integer type)
    def builtin(bi):
        for v in m.vars.values():
            if v.builtin == bi and v.storage == 1: return v.id, (t_uint if m.scalar_class(m.elem(v.pointee)[0]) == 'uint' else t_int)
        p_in = new_id(); tdefs.append(_ins(32, p_in, 1, t_int)); v = new_id(); tdefs.append(_ins(59, p_in, v, 1))
        decos.append(_ins(71, v, 11, bi)); iface.append(v); return v, t_int
    vid, vid_t = builtin(42); iid, iid_t = builtin(43)
    cu = [const(t_uint, k) for k in range(12)]; ci0 = const(t_int, 0)
    # the outputs to capture
    outs = {}
    for v in m.vars.values():
        if v.storage == 3 and v.location >= 0 and v.builtin < 0: outs[v.location] = v
    p_of = new_id(); tdefs.append(_ins(32, p_of, 3, t_float)); p_oi = new_id(); tdefs.append(_ins(32, p_oi, 3, t_int)); p_ou = new_id(); tdefs.append(_ins(32, p_ou, 3, t_uint))
    def store_block():
        c = []
        def op(o, t, *a):
            r = new_id(); c.extend(_ins(o, t, r, *a)); return r
        def load_u(k):
            pk = op(65, p_uu, info, cu[k]); return op(61, t_uint, pk)
        first, count, base = load_u(0), load_u(1), load_u(2)
        vi = op(124, t_uint, op(61, vid_t, vid)); ii = op(124, t_uint, op(61, iid_t, iid))   # OpBitcast to uint
        slot = op(128, t_uint, op(130, t_uint, vi, first), op(132, t_uint, op(130, t_uint, ii, base), count))   # IAdd(ISub, IMul(ISub))
        for b in range(nb):
            ov = outs.get(tf['slots'][b])
            if ov is None: raise ValueError('transform feedback buffer %d captures location %d, which the shader does not write' % (b, tf['slots'][b]))
            cls = m.scalar_class(m.elem(ov.pointee)[0])
            words4 = op(194, t_uint, load_u(4 + b), cu[2])   # ShiftRightLogical: stride / 4
            base_i = op(128, t_uint, op(132, t_uint, slot, words4), load_u(8 + b))
            for k in range(tf['comps'][b]):
                if cls == 'float': val = op(61, t_float, op(65, p_of, ov.id, cu[k]))
                else: val = op(124, t_float, op(61, t_int if cls == 'int' else t_uint, op(65, p_oi if cls == 'int' else p_ou, ov.id, cu[k])))
                ptr = op(65, p_uf, bufs[b], ci0, op(128, t_uint, base_i, cu[k]))
                c.extend(_ins(62, ptr, val))
        return c
    # rebuild: annotations after the last annotation, types after the last global, stores before each OpReturn of main
    out = words[:5]; last_anno = last_global = -1
    for idx, (p, op, w) in enumerate(m.insns):
        if op in (71, 72, 332, 333): last_anno = idx
        if op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 39, 41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 59) and not _in_function(m, p): last_global = idx
    in_main = False
    for idx, (p, op, w) in enumerate(m.insns):
        n = words[p] >> 16; ins = words[p:p + n]
        if op == 15 and iface: ins = [((n + len(iface)) << 16) | 15] + ins[1:] + iface   # OpEntryPoint interface
        if op == 54: in_main = w[1] == m.entry_id
        if op == 253 and in_main: out.extend(store_block())
        out.extend(ins)
        if op == 56: in_main = False
        if idx == last_anno: [out.extend(d) for d in decos]
        if idx == last_global: [out.extend(t) for t in tdefs]
    out[3] = bound
    extra = [{'name': 'tf_info', 'set': 0, 'binding': TF_INFO_BINDING, 'desc': 'ubo', 'count': 1, 'kind': 'tf_info'}]
    extra += [{'name': 'tf_b%d' % b, 'set': 0, 'binding': TF_BUF_BINDING + b, 'desc': 'ssbo', 'count': 1, 'kind': 'tf_buf', 'reg': b, 'rw': True} for b in range(nb)]
    return struct.pack('<%dI' % len(out), *out), extra

SBUF_OFF_BINDING = 97   # set 0 (vertex / compute) or the fragment set; nvn_vk_exec.c fills it (kind sbuf_off)
_RESULT_TYPED = set(range(43, 47)) | {59, 61, 65, 66, 77, 78, 79, 80, 81, 82, 83, 12, 57, 245} | set(range(109, 205))

def sbuf_offsets(m, stage):
    """The translator's storage buffer accesses index from the buffer's exact address (16-byte aligned, Metal's rule),
    but a Vulkan descriptor offset must be a multiple of minStorageBufferOffsetAlignment (64 on the Adreno), so the
    backend binds each storage buffer at its address rounded down. Every access chain into a storage buffer now adds that
    binding's distance from the rounded-down start, in 4-byte elements, read from one more storage buffer: o[binding].
    Returns the new module and the reflection binding, or None when the shader has no storage buffers."""
    words = list(m.words); bound = words[3]
    def is_sbuf(v):
        if v.storage == 12: return True
        return v.storage == 2 and m.has_deco(v.pointee, 3)
    sb = {v.id: v for v in m.vars.values() if v.binding >= 0 and is_sbuf(v)}
    if not sb: return None
    def new_id():
        nonlocal bound; bound += 1; return bound - 1
    rtype = {}
    for _p, op, w in m.insns:
        if op in _RESULT_TYPED and len(w) >= 2: rtype[w[1]] = w[0]
    t_uint = next((w[0] for _p, op, w in m.insns if op == 21 and w[1] == 32 and w[2] == 0), None)
    t_int = next((w[0] for _p, op, w in m.insns if op == 21 and w[1] == 32 and w[2] == 1), None)
    tdefs, decos = [], []
    if t_uint is None: t_uint = new_id(); tdefs.append(_ins(21, t_uint, 32, 0))
    sb_class, sb_deco = (12, 2) if words[1] >= 0x10300 else (2, 3)
    set_ = 1 if stage == 'ps' else 0
    rarr = new_id(); tdefs.append(_ins(29, rarr, t_uint)); decos.append(_ins(71, rarr, 6, 4))
    st = new_id(); tdefs.append(_ins(30, st, rarr)); decos.append(_ins(71, st, sb_deco)); decos.append(_ins(72, st, 0, 35, 0)); decos.append(_ins(72, st, 0, 24))   # NonWritable
    p_st = new_id(); tdefs.append(_ins(32, p_st, sb_class, st)); p_u = new_id(); tdefs.append(_ins(32, p_u, sb_class, t_uint))
    offs = new_id(); tdefs.append(_ins(59, p_st, offs, sb_class)); decos.append(_ins(71, offs, 34, set_)); decos.append(_ins(71, offs, 33, SBUF_OFF_BINDING))
    c0 = new_id(); tdefs.append(_ins(43, t_uint, c0, 0))
    cb = {}
    def cbind(b):
        if b not in cb: cb[b] = new_id(); tdefs.append(_ins(43, t_uint, cb[b], b))
        return cb[b]
    for v in sb.values(): cbind(v.binding)
    out = words[:5]; last_anno = max(i for i, (p, op, w) in enumerate(m.insns) if op in (71, 72, 332, 333))
    last_global = max(i for i, (p, op, w) in enumerate(m.insns)
                      if op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 39, 41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 59) and not _in_function(m, p))
    patched = 0
    for idx, (p, op, w) in enumerate(m.insns):
        n = words[p] >> 16; ins = list(words[p:p + n])
        if op in (65, 66) and w[2] in sb and len(w) >= 5:   # result type, result, base, member, element[, ...]
            v = sb[w[2]]; elem = w[4]; et = rtype.get(elem, t_uint)
            po = new_id(); o = new_id(); ins_pre = _ins(65, p_u, po, offs, c0, cbind(v.binding)) + _ins(61, t_uint, o, po)
            if et != t_uint:
                oc = new_id(); ins_pre += _ins(124, et, oc, o); o = oc
            ne = new_id(); ins_pre += _ins(128, et, ne, elem, o)
            out.extend(ins_pre); ins[5] = ne; patched += 1
        out.extend(ins)
        if idx == last_anno: [out.extend(d) for d in decos]
        if idx == last_global: [out.extend(t) for t in tdefs]
    out[3] = bound
    iface_needed = words[1] >= 0x10400
    if iface_needed:   # SPIR-V 1.4: the new global joins the entry point's interface
        raise ValueError('SPIR-V 1.4 interface update not implemented')
    refl = {'name': 'sbuf_off', 'set': set_, 'binding': SBUF_OFF_BINDING, 'desc': 'ssbo', 'count': 1, 'kind': 'sbuf_off'}
    return struct.pack('<%dI' % len(out), *out), refl, patched

def vs_out_defaults(m):
    """Scarlet (as nvn_mtl.m vs_out_defaults): on the GPU a varying component the vertex shader never writes reads as
    (0, 0, 0, 1); in SPIR-V an unwritten output is undefined. Every float vec4 user output now starts as (0, 0, 0, 1) at
    the top of the entry function (a write by the shader still replaces it)."""
    words = list(m.words); bound = words[3]
    t_float = next((w[0] for _p, op, w in m.insns if op == 22 and w[1] == 32), None)
    t_vec4 = next((w[0] for _p, op, w in m.insns if op == 23 and w[1] == t_float and w[2] == 4), None) if t_float else None
    outs = [v for v in m.vars.values() if v.storage == 3 and v.location >= 0 and v.builtin < 0 and t_vec4 and v.pointee == t_vec4]
    if not outs: return None
    c0, c1, cv = bound, bound + 1, bound + 2; bound += 3
    consts = _ins(43, t_float, c0, 0) + _ins(43, t_float, c1, 0x3f800000) + _ins(44, t_vec4, cv, c0, c0, c0, c1)
    stores = sum((_ins(62, v.id, cv) for v in outs), [])
    out = words[:5]; last_global = max(i for i, (p, op, w) in enumerate(m.insns)
                                       if op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 39, 41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 59) and not _in_function(m, p))
    state = 0   # 1: in the entry function before its first label, 2: after the label (function variables come first), 3: done
    for idx, (p, op, w) in enumerate(m.insns):
        n = words[p] >> 16
        if state == 2 and op != 59: out.extend(stores); state = 3
        if op == 54 and w[1] == m.entry_id: state = 1
        if op == 248 and state == 1: state = 2   # OpLabel
        out.extend(words[p:p + n])
        if idx == last_global: out.extend(consts)
    out[3] = bound
    return struct.pack('<%dI' % len(out), *out)

def _in_function(m, p):
    return any(fs <= p < fe for fs, fe in m.functions)

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
    code = bytes.fromhex(rq.pop('code_hex')); tf = rq.pop('tf', None)
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
    if stage == 'vs':   # unwritten varying components read (0, 0, 0, 1), as on the GPU
        d = vs_out_defaults(m)
        if d: spv = d; m = Spirv(spv)
    refl = reflect(m, rq, res, variant, tr.chain)
    so = sbuf_offsets(m, stage)   # storage buffers: the descriptor's 64-byte aligned start vs the buffer's address
    if so: spv, extra, _n = so; m = Spirv(spv); refl['bindings'].append(extra)
    if tf:   # Scarlet: a transform feedback variant of a vertex shader (nvn_vk_exec.c draws it with rasterisation discarded)
        spv, extra = tf_patch(m, tf); refl['bindings'] += extra; refl['tf'] = tf
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
