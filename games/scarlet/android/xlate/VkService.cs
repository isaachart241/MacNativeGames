// Thor port: the shader translation service for the Vulkan backend, run on the device (totk_xlate --service-vk).
// A C# port of android/tools/shader_bridge.py, which did this on the Mac: the request goes through the MSL chain exactly
// as the Mac port translates it (ShaderService.Handle in games/totk/tools/totk_xlate/Service.cs, with spirv-cross for the
// attribute inference and the FP64 check it makes from the MSL), and its SPIR-V then gets the Vulkan fixes:
//   - a pixel shader's resources move to descriptor set 1 (the support buffer stays set 0 binding 0);
//   - unwritten float4 vertex outputs start as (0, 0, 0, 1), as on the GPU;
//   - the reflection the backend reads (<variant>_v.json), from the SPIR-V itself;
//   - storage buffer accesses add their binding's offset from a 64-byte aligned start (the Adreno's alignment);
//   - a transform feedback variant writes its captured outputs to storage buffers.
// The results, <variant>_v.spv and <variant>_v.json, are byte for byte what the bridge wrote (checked over the Mac's
// shader cache: android/xlate/check_parity.py). One JSON request per line on stdin, one reply line on stdout.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static partial class ShaderService
{
    static string resultsDir;

    // ---------- SPIR-V reading (shader_bridge.py Spirv) ----------
    sealed class Var
    {
        public uint Id, TypePtr, Pointee, Storage; public string Name = ""; public long Set = -1, Binding = -1, Location = -1, Builtin = -1;
        public bool Stored, NonWritable;
    }
    sealed class Insn { public int P; public uint Op; public uint[] W; }

    static string SpvStr(uint[] w, int at)
    {
        var b = new List<byte>();
        for (int i = at; i < w.Length; i++)
        {
            uint x = w[i]; bool end = false;
            for (int k = 0; k < 4; k++) { byte c = (byte)(x >> (8 * k)); if (c == 0) { end = true; break; } b.Add(c); }
            if (end) break;
        }
        return Encoding.UTF8.GetString(b.ToArray());
    }

    sealed class Spirv
    {
        public readonly uint[] Words;
        public readonly Dictionary<uint, Var> Vars = new();   // insertion order is the module's order
        public readonly List<Var> VarOrder = new();
        public readonly Dictionary<uint, (uint op, uint[] w)> Types = new();
        public readonly Dictionary<uint, uint> Consts = new();
        public readonly Dictionary<uint, string> Names = new();
        public readonly Dictionary<uint, List<(uint dec, uint[] a)>> Decos = new();
        public readonly Dictionary<uint, uint> ChainRoot = new();
        public readonly List<(uint root, uint[] idx)> Chains = new();
        public readonly List<uint> Caps = new();
        public readonly List<string> Exts = new();
        public string EntryName = "main"; public uint[] LocalSize; public bool UsesFp64; public uint EntryId;
        public readonly List<Insn> Insns = new();
        public readonly List<(int s, int e)> Functions = new();

        public Spirv(byte[] data)
        {
            if (data.Length < 20 || data.Length % 4 != 0) throw new RuntimeError("SPIR-V: bad length");
            Words = new uint[data.Length / 4]; Buffer.BlockCopy(data, 0, Words, 0, data.Length);
            if (Words[0] != 0x07230203) throw new RuntimeError("SPIR-V: bad magic");
            int p = 5, fstart = -1;
            while (p < Words.Length)
            {
                int n = (int)(Words[p] >> 16); uint op = Words[p] & 0xffff;
                if (n == 0 || p + n > Words.Length) throw new RuntimeError($"SPIR-V: bad instruction at word {p}");
                uint[] w = Words[(p + 1)..(p + n)];
                Insns.Add(new Insn { P = p, Op = op, W = w });
                if (op == 54) fstart = p;
                else if (op == 56 && fstart >= 0) { Functions.Add((fstart, p + n)); fstart = -1; }
                if (op == 17) Caps.Add(w[0]);
                else if (op == 10) Exts.Add(SpvStr(w, 0));
                else if (op == 15) { EntryName = SpvStr(w, 2); EntryId = w[1]; }
                else if (op == 16) { if (w[1] == 17 && w.Length >= 5) LocalSize = new[] { w[2], w[3], w[4] }; }
                else if (op == 5) Names[w[0]] = SpvStr(w, 1);
                else if (op == 71) { if (!Decos.TryGetValue(w[0], out var l)) Decos[w[0]] = l = new(); l.Add((w[1], w[2..])); }
                else if ((op >= 19 && op <= 30) || op == 32)
                {
                    if (op == 22 && w[1] == 64) UsesFp64 = true;
                    Types[w[0]] = (op, w);
                }
                else if (op == 43) { if (w.Length >= 3) Consts[w[1]] = w[2]; }
                else if (op == 59)
                {
                    var v = new Var { Id = w[1], TypePtr = w[0], Storage = w[2] };
                    if (!Vars.ContainsKey(w[1])) VarOrder.Add(v); else VarOrder[VarOrder.FindIndex(x => x.Id == w[1])] = v;
                    Vars[w[1]] = v; ChainRoot[w[1]] = w[1];
                }
                else if (op == 65 || op == 66 || op == 67 || op == 70)
                {
                    if (ChainRoot.TryGetValue(w[2], out uint root))
                    {
                        ChainRoot[w[1]] = root;
                        if (w[2] == root) Chains.Add((root, w[3..]));
                    }
                }
                else if (op == 83) { if (ChainRoot.TryGetValue(w[2], out uint r)) ChainRoot[w[1]] = r; }
                else if (op == 62 || op == 63 || op == 228) Store(w[0]);
                else if ((op >= 227 && op <= 242 && op != 228 && w.Length >= 3) || ((op == 318 || op == 319 || op == 6035) && w.Length >= 3)) Store(w[2]);
                p += n;
            }
            foreach (Var v in VarOrder)
            {
                v.Name = Names.TryGetValue(v.Id, out string nm) ? nm : "";
                if (Types.TryGetValue(v.TypePtr, out var t) && t.op == 32) v.Pointee = t.w[2];
                if (Decos.TryGetValue(v.Id, out var ds))
                    foreach (var (dec, a) in ds)
                    {
                        if (dec == 34) v.Set = a[0];
                        else if (dec == 33) v.Binding = a[0];
                        else if (dec == 30) v.Location = a[0];
                        else if (dec == 11) v.Builtin = a[0];
                        else if (dec == 24) v.NonWritable = true;
                    }
            }
        }
        void Store(uint ptr) { if (ChainRoot.TryGetValue(ptr, out uint r) && Vars.TryGetValue(r, out Var v)) v.Stored = true; }
        public bool HasDeco(uint id, uint dec) => Decos.TryGetValue(id, out var l) && l.Any(d => d.dec == dec);
        public (uint ty, long count) Elem(uint ty)
        {
            long count = 1;
            while (Types.TryGetValue(ty, out var t) && (t.op == 28 || t.op == 29))
            {
                if (t.op == 29) count = 0; else count *= Consts.TryGetValue(t.w[2], out uint c) ? c : 1;
                ty = t.w[1];
            }
            return (ty, count);
        }
        public string ScalarClass(uint ty)
        {
            if (!Types.TryGetValue(ty, out var t)) return "?";
            while (t.op == 23) if (!Types.TryGetValue(t.w[1], out t)) return "?";
            return t.op == 22 ? "float" : t.op == 21 ? (t.w[2] != 0 ? "int" : "uint") : "?";
        }
        public bool InFunction(int p) => Functions.Any(f => f.s <= p && p < f.e);
        public byte[] SetDescriptorSets(Func<Var, long?> fn)
        {
            uint[] w = (uint[])Words.Clone();
            foreach (Insn i in Insns)
                if (i.Op == 71 && i.W[1] == 34 && Vars.TryGetValue(i.W[0], out Var v)) { long? s = fn(v); if (s != null) w[i.P + 3] = (uint)s.Value; }
            return ToBytes(w);
        }
    }

    static byte[] ToBytes(IList<uint> w) { var b = new byte[w.Count * 4]; for (int i = 0; i < w.Count; i++) BitConverter.TryWriteBytes(b.AsSpan(4 * i), w[i]); return b; }
    static uint[] Ins(uint op, params uint[] ops) { var r = new uint[ops.Length + 1]; r[0] = (uint)(ops.Length + 1) << 16 | op; ops.CopyTo(r, 1); return r; }
    static readonly HashSet<uint> GlobalOps = new() { 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 39, 41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 59 };
    static int LastGlobal(Spirv m) { int last = -1; for (int i = 0; i < m.Insns.Count; i++) if (GlobalOps.Contains(m.Insns[i].Op) && !m.InFunction(m.Insns[i].P)) last = i; return last; }
    static int LastAnno(Spirv m) { int last = -1; for (int i = 0; i < m.Insns.Count; i++) { uint op = m.Insns[i].Op; if (op == 71 || op == 72 || op == 332 || op == 333) last = i; } return last; }
    static uint[] InsnWords(Spirv m, Insn i) => m.Words[i.P..(i.P + (int)(m.Words[i.P] >> 16))];

    // ---------- transform feedback (shader_bridge.py tf_patch) ----------
    const uint TfInfoBinding = 98, TfBufBinding = 100, SbufOffBinding = 97;
    static (byte[] spv, List<JsonObject> extra) TfPatch(Spirv m, JsonObject tf)
    {
        uint[] words = m.Words; uint bound = words[3];
        uint NewId() => bound++;
        var types = new Dictionary<string, uint>();
        string Key(uint op, IEnumerable<uint> ops) => op + ":" + string.Join(",", ops);
        foreach (Insn i in m.Insns) if (i.Op == 19 || i.Op == 21 || i.Op == 22 || i.Op == 23 || i.Op == 32) types.TryAdd(Key(i.Op, i.W[1..]), i.W[0]);
        var decos = new List<uint[]>(); var tdefs = new List<uint[]>(); var iface = new List<uint>();
        uint Ty(uint op, params uint[] ops) { string k = Key(op, ops); if (!types.TryGetValue(k, out uint id)) { id = NewId(); types[k] = id; tdefs.Add(Ins(op, new[] { id }.Concat(ops).ToArray())); } return id; }
        uint Const(uint t, uint v) { uint i = NewId(); tdefs.Add(Ins(43, t, i, v)); return i; }
        uint tFloat = Ty(22, 32), tUint = Ty(21, 32, 0), tInt = Ty(21, 32, 1);
        bool v13 = words[1] >= 0x10300, v14 = words[1] >= 0x10400;
        uint sbClass = v13 ? 12u : 2u, sbDeco = v13 ? 2u : 3u;
        uint rarr = NewId(); tdefs.Add(Ins(29, rarr, tFloat)); decos.Add(Ins(71, rarr, 6, 4));
        uint sbs = NewId(); tdefs.Add(Ins(30, sbs, rarr)); decos.Add(Ins(71, sbs, sbDeco)); decos.Add(Ins(72, sbs, 0, 35, 0));
        uint pSbs = NewId(); tdefs.Add(Ins(32, pSbs, sbClass, sbs));
        uint pUf = NewId(); tdefs.Add(Ins(32, pUf, sbClass, tFloat));
        JsonArray slots = tf["slots"].AsArray(), comps = tf["comps"].AsArray();
        int nb = slots.Count; var bufs = new List<uint>();
        for (int b = 0; b < nb; b++)
        {
            uint v = NewId(); tdefs.Add(Ins(59, pSbs, v, sbClass)); bufs.Add(v); if (v14) iface.Add(v);
            decos.Add(Ins(71, v, 34, 0)); decos.Add(Ins(71, v, 33, TfBufBinding + (uint)b));
        }
        uint infoT = NewId(); tdefs.Add(Ins(30, new[] { infoT }.Concat(Enumerable.Repeat(tUint, 12)).ToArray())); decos.Add(Ins(71, infoT, 2));
        for (uint k = 0; k < 12; k++) decos.Add(Ins(72, infoT, k, 35, 4 * k));
        uint pInfo = NewId(); tdefs.Add(Ins(32, pInfo, 2, infoT)); uint pUu = NewId(); tdefs.Add(Ins(32, pUu, 2, tUint));
        uint info = NewId(); tdefs.Add(Ins(59, pInfo, info, 2)); decos.Add(Ins(71, info, 34, 0)); decos.Add(Ins(71, info, 33, TfInfoBinding));
        if (v14) iface.Add(info);
        (uint id, uint ty) Builtin(uint bi)
        {
            foreach (Var v in m.VarOrder) if (v.Builtin == bi && v.Storage == 1) return (v.Id, m.ScalarClass(m.Elem(v.Pointee).ty) == "uint" ? tUint : tInt);
            uint pIn = NewId(); tdefs.Add(Ins(32, pIn, 1, tInt)); uint nv = NewId(); tdefs.Add(Ins(59, pIn, nv, 1));
            decos.Add(Ins(71, nv, 11, bi)); iface.Add(nv); return (nv, tInt);
        }
        var (vid, vidT) = Builtin(42); var (iid, iidT) = Builtin(43);
        uint[] cu = new uint[12]; for (uint k = 0; k < 12; k++) cu[k] = Const(tUint, k);
        uint ci0 = Const(tInt, 0);
        var outs = new Dictionary<long, Var>();
        foreach (Var v in m.VarOrder) if (v.Storage == 3 && v.Location >= 0 && v.Builtin < 0) outs[v.Location] = v;
        uint pOf = NewId(); tdefs.Add(Ins(32, pOf, 3, tFloat)); uint pOi = NewId(); tdefs.Add(Ins(32, pOi, 3, tInt)); uint pOu = NewId(); tdefs.Add(Ins(32, pOu, 3, tUint));
        List<uint> StoreBlock()
        {
            var c = new List<uint>();
            uint Op(uint o, uint t, params uint[] a) { uint r = NewId(); c.AddRange(Ins(o, new[] { t, r }.Concat(a).ToArray())); return r; }
            uint LoadU(int k) { uint pk = Op(65, pUu, info, cu[k]); return Op(61, tUint, pk); }
            uint first = LoadU(0), count = LoadU(1), bas = LoadU(2);
            uint vi = Op(124, tUint, Op(61, vidT, vid)); uint ii = Op(124, tUint, Op(61, iidT, iid));
            uint d1 = Op(130, tUint, vi, first); uint d2 = Op(130, tUint, ii, bas); uint mul = Op(132, tUint, d2, count);
            uint slot = Op(128, tUint, d1, mul);
            for (int b = 0; b < nb; b++)
            {
                long loc = (long)slots[b];
                if (!outs.TryGetValue(loc, out Var ov)) throw new RuntimeError($"transform feedback buffer {b} captures location {loc}, which the shader does not write");
                string cls = m.ScalarClass(m.Elem(ov.Pointee).ty);
                uint lw = LoadU(4 + b); uint words4 = Op(194, tUint, lw, cu[2]);
                uint mb = Op(132, tUint, slot, words4); uint lo = LoadU(8 + b); uint baseI = Op(128, tUint, mb, lo);
                for (int k = 0; k < (int)comps[b]; k++)
                {
                    uint val;
                    if (cls == "float") val = Op(61, tFloat, Op(65, pOf, ov.Id, cu[k]));
                    else
                    {
                        uint ch = Op(65, cls == "int" ? pOi : pOu, ov.Id, cu[k]); uint ld = Op(61, cls == "int" ? tInt : tUint, ch);
                        val = Op(124, tFloat, ld);
                    }
                    uint idx = Op(128, tUint, baseI, cu[k]); uint ptr = Op(65, pUf, bufs[b], ci0, idx);
                    c.AddRange(Ins(62, ptr, val));
                }
            }
            return c;
        }
        var outw = new List<uint>(words[..5]); int lastAnno = LastAnno(m), lastGlobal = LastGlobal(m);
        bool inMain = false;
        for (int idx = 0; idx < m.Insns.Count; idx++)
        {
            Insn i = m.Insns[idx]; uint[] ins = InsnWords(m, i); int n = ins.Length;
            if (i.Op == 15 && iface.Count > 0) ins = new[] { (uint)(n + iface.Count) << 16 | 15 }.Concat(ins[1..]).Concat(iface).ToArray();
            if (i.Op == 54) inMain = i.W[1] == m.EntryId;
            if (i.Op == 253 && inMain) outw.AddRange(StoreBlock());
            outw.AddRange(ins);
            if (i.Op == 56) inMain = false;
            if (idx == lastAnno) foreach (uint[] d in decos) outw.AddRange(d);
            if (idx == lastGlobal) foreach (uint[] t in tdefs) outw.AddRange(t);
        }
        outw[3] = bound;
        var extra = new List<JsonObject> { new() { ["name"] = "tf_info", ["set"] = 0, ["binding"] = TfInfoBinding, ["desc"] = "ubo", ["count"] = 1, ["kind"] = "tf_info" } };
        for (int b = 0; b < nb; b++)
            extra.Add(new JsonObject { ["name"] = $"tf_b{b}", ["set"] = 0, ["binding"] = TfBufBinding + (uint)b, ["desc"] = "ssbo", ["count"] = 1, ["kind"] = "tf_buf", ["reg"] = b, ["rw"] = true });
        return (ToBytes(outw), extra);
    }

    // ---------- storage buffer offsets (shader_bridge.py sbuf_offsets) ----------
    static readonly HashSet<uint> ResultTyped = new(Enumerable.Range(43, 4).Select(x => (uint)x).Concat(new uint[] { 59, 61, 65, 66, 77, 78, 79, 80, 81, 82, 83, 12, 57, 245 }).Concat(Enumerable.Range(109, 96).Select(x => (uint)x)));
    static (byte[] spv, JsonObject refl)? SbufOffsets(Spirv m, string stage)
    {
        uint[] words = m.Words; uint bound = words[3];
        bool IsSbuf(Var v) => v.Storage == 12 || (v.Storage == 2 && m.HasDeco(v.Pointee, 3));
        var sb = new Dictionary<uint, Var>(); var sbOrder = new List<Var>();
        foreach (Var v in m.VarOrder) if (v.Binding >= 0 && IsSbuf(v)) { if (!sb.ContainsKey(v.Id)) sbOrder.Add(v); sb[v.Id] = v; }
        if (sb.Count == 0) return null;
        uint NewId() => bound++;
        var rtype = new Dictionary<uint, uint>();
        foreach (Insn i in m.Insns) if (ResultTyped.Contains(i.Op) && i.W.Length >= 2) rtype[i.W[1]] = i.W[0];
        uint? tUintFound = m.Insns.Where(i => i.Op == 21 && i.W[1] == 32 && i.W[2] == 0).Select(i => (uint?)i.W[0]).FirstOrDefault();
        var tdefs = new List<uint[]>(); var decos = new List<uint[]>();
        uint tUint; if (tUintFound == null) { tUint = NewId(); tdefs.Add(Ins(21, tUint, 32, 0)); } else tUint = tUintFound.Value;
        bool v13 = words[1] >= 0x10300; uint sbClass = v13 ? 12u : 2u, sbDeco = v13 ? 2u : 3u;
        uint set = stage == "ps" ? 1u : 0u;
        uint rarr = NewId(); tdefs.Add(Ins(29, rarr, tUint)); decos.Add(Ins(71, rarr, 6, 4));
        uint st = NewId(); tdefs.Add(Ins(30, st, rarr)); decos.Add(Ins(71, st, sbDeco)); decos.Add(Ins(72, st, 0, 35, 0)); decos.Add(Ins(72, st, 0, 24));
        uint pSt = NewId(); tdefs.Add(Ins(32, pSt, sbClass, st)); uint pU = NewId(); tdefs.Add(Ins(32, pU, sbClass, tUint));
        uint offs = NewId(); tdefs.Add(Ins(59, pSt, offs, sbClass)); decos.Add(Ins(71, offs, 34, set)); decos.Add(Ins(71, offs, 33, SbufOffBinding));
        uint c0 = NewId(); tdefs.Add(Ins(43, tUint, c0, 0));
        var cb = new Dictionary<long, uint>();
        uint CBind(long b) { if (!cb.TryGetValue(b, out uint id)) { id = cb[b] = NewId(); tdefs.Add(Ins(43, tUint, id, (uint)b)); } return id; }
        foreach (Var v in sbOrder) CBind(v.Binding);
        var outw = new List<uint>(words[..5]); int lastAnno = LastAnno(m), lastGlobal = LastGlobal(m);
        for (int idx = 0; idx < m.Insns.Count; idx++)
        {
            Insn i = m.Insns[idx]; uint[] ins = InsnWords(m, i);
            if ((i.Op == 65 || i.Op == 66) && sb.TryGetValue(i.W[2], out Var v) && i.W.Length >= 5)
            {
                uint elem = i.W[4]; uint et = rtype.TryGetValue(elem, out uint t) ? t : tUint;
                uint po = NewId(), o = NewId(); var pre = new List<uint>(); pre.AddRange(Ins(65, pU, po, offs, c0, CBind(v.Binding))); pre.AddRange(Ins(61, tUint, o, po));
                if (et != tUint) { uint oc = NewId(); pre.AddRange(Ins(124, et, oc, o)); o = oc; }
                uint ne = NewId(); pre.AddRange(Ins(128, et, ne, elem, o));
                outw.AddRange(pre); ins[5] = ne;
            }
            outw.AddRange(ins);
            if (idx == lastAnno) foreach (uint[] d in decos) outw.AddRange(d);
            if (idx == lastGlobal) foreach (uint[] d in tdefs) outw.AddRange(d);
        }
        outw[3] = bound;
        if (words[1] >= 0x10400) throw new RuntimeError("SPIR-V 1.4 interface update not implemented");
        var refl = new JsonObject { ["name"] = "sbuf_off", ["set"] = set, ["binding"] = SbufOffBinding, ["desc"] = "ssbo", ["count"] = 1, ["kind"] = "sbuf_off" };
        return (ToBytes(outw), refl);
    }

    // ---------- unwritten vertex outputs (shader_bridge.py vs_out_defaults) ----------
    static byte[] VsOutDefaults(Spirv m)
    {
        uint[] words = m.Words; uint bound = words[3];
        uint? tFloat = m.Insns.Where(i => i.Op == 22 && i.W[1] == 32).Select(i => (uint?)i.W[0]).FirstOrDefault();
        uint? tVec4 = tFloat == null ? null : m.Insns.Where(i => i.Op == 23 && i.W[1] == tFloat && i.W[2] == 4).Select(i => (uint?)i.W[0]).FirstOrDefault();
        var outs = m.VarOrder.Where(v => v.Storage == 3 && v.Location >= 0 && v.Builtin < 0 && tVec4 != null && v.Pointee == tVec4).ToList();
        if (outs.Count == 0) return null;
        uint c0 = bound, c1 = bound + 1, cv = bound + 2; bound += 3;
        var consts = new List<uint>(); consts.AddRange(Ins(43, tFloat.Value, c0, 0)); consts.AddRange(Ins(43, tFloat.Value, c1, 0x3f800000)); consts.AddRange(Ins(44, tVec4.Value, cv, c0, c0, c0, c1));
        var stores = new List<uint>(); foreach (Var v in outs) stores.AddRange(Ins(62, v.Id, cv));
        var outw = new List<uint>(words[..5]); int lastGlobal = LastGlobal(m);
        int state = 0;   // 1: in the entry function before its first label, 2: after the label (function variables come first), 3: done
        for (int idx = 0; idx < m.Insns.Count; idx++)
        {
            Insn i = m.Insns[idx];
            if (state == 2 && i.Op != 59) { outw.AddRange(stores); state = 3; }
            if (i.Op == 54 && i.W[1] == m.EntryId) state = 1;
            if (i.Op == 248 && state == 1) state = 2;
            outw.AddRange(InsnWords(m, i));
            if (idx == lastGlobal) outw.AddRange(consts);
        }
        outw[3] = bound;
        return ToBytes(outw);
    }

    // ---------- reflection (shader_bridge.py reflect) ----------
    const long VkTexBase = 0x168, VkSampBase = 0x568, VkImgBase = 0x120, VkCombinedBase = 0x20, VkCombinedSlots = 32;
    static long? VkSlot(long word, long bas, long cb) => cb == 2 && word * 4 >= bas && (word * 4 - bas) % 8 == 0 ? (word * 4 - bas) / 8 : null;
    static (long? t, long? s, long ty)? VkDecodeTex(string name)
    {
        Match m = Regex.Match(name, @"^[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)");
        if (!m.Success) return null;
        long slots = m.Groups[1].Success && m.Groups[1].Value.Length > 0 ? long.Parse(m.Groups[1].Value) : -1;
        long h = Convert.ToInt64(m.Groups[2].Value, 16);
        long tb = slots >= 0 ? slots & 0xffff : 2;
        long sb = slots >= 0 && (slots >> 16) != 0 ? (slots >> 16) - 1 : tb;
        long o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = h >> 28;
        long[] ws = ty != 0 ? new[] { o0, o1 } : new[] { o0 };
        if (Regex.IsMatch(name, @"^[a-z]+_i"))
        {
            long? ti = ws.Where(x => VkImgBase <= x * 4 && x * 4 < VkTexBase && VkSlot(x, VkImgBase, tb) != null).Select(x => VkSlot(x, VkImgBase, tb)).FirstOrDefault();
            return (ti, null, ty);
        }
        long? t = ws.Where(x => VkSlot(x, VkTexBase, tb) != null && x * 4 < VkSampBase).Select(x => VkSlot(x, VkTexBase, tb)).FirstOrDefault();
        if (t == null) t = ws.Where(x => VkCombinedBase <= x * 4 && x * 4 < VkCombinedBase + 8 * VkCombinedSlots && VkSlot(x, VkCombinedBase, tb) != null).Select(x => VkSlot(x, VkCombinedBase, tb)).FirstOrDefault();
        long? s = ws.Where(x => x * 4 >= VkSampBase && VkSlot(x, VkSampBase, sb) != null).Select(x => VkSlot(x, VkSampBase, sb)).FirstOrDefault();
        return (t, s, ty);
    }
    static (string desc, long count, JsonObject img) DescOf(Spirv m, Var v)
    {
        var (ty, count) = m.Elem(v.Pointee);
        if (!m.Types.TryGetValue(ty, out var t)) return ("?", count, null);
        if (v.Storage == 12) return ("ssbo", count, null);
        if (v.Storage == 2) return (m.HasDeco(ty, 3) ? "ssbo" : "ubo", count, null);
        if (v.Storage != 0) return ("?", count, null);
        if (t.op == 26) return ("sampler", count, null);
        bool combined = t.op == 27;
        (uint op, uint[] w) it = t; bool has = true;
        if (combined) has = m.Types.TryGetValue(t.w[1], out it);
        if (!has || it.op != 25) return ("?", count, null);
        uint[] w = it.w; uint dim = w[2], sampled = w[6];
        string dimName = dim switch { 0 => "1d", 1 => "2d", 2 => "3d", 3 => "cube", 5 => "buffer", _ => dim.ToString() };
        var img = new JsonObject { ["dim"] = dimName, ["arrayed"] = w[4] != 0, ["ms"] = w[5] != 0, ["shadow"] = w[3] == 1, ["class"] = m.ScalarClass(w[1]), ["format"] = w[7] };
        if (dim == 5) return (sampled == 2 ? "stexel" : "utexel", count, img);
        if (combined) return ("combined", count, img);
        return (sampled == 2 ? "storage" : "sampled", count, img);
    }
    static readonly Dictionary<string, (long lo, long hi)[]> VkDrvFillC0 = new() { ["vs"] = new[] { (0x110L, 0x210L), (0x610L, 0x620L) }, ["ps"] = new[] { (0x510L, 0x610L) }, ["cs"] = new[] { (0x200L, 0x410L) } };
    static JsonNode LongOrNull(long? v) => v == null ? null : JsonValue.Create(v.Value);

    static JsonObject ReflectVk(Spirv m, JsonObject rq, JsonObject res, string variant)
    {
        string stage = (string)rq["stage"], key = variant + "_v";
        var sbSorted = (res["sbufs"] as JsonArray ?? new JsonArray()).Select(b => b.AsObject()).OrderBy(b => Long(b["binding"])).ToList();
        long? sbBase = stage switch { "vs" => 0x110, "cs" => 0x310, "ps" => 0x510, _ => null };
        var binds = new JsonArray(); var drv = new SortedDictionary<long, (SortedSet<long> offs, bool dyn)>(); var usedSlots = new HashSet<long>();
        foreach (Var v in m.VarOrder.OrderBy(x => x.Set).ThenBy(x => x.Binding))
        {
            if (v.Binding < 0 || (v.Storage != 0 && v.Storage != 2 && v.Storage != 12)) continue;
            string nm = v.Name; var (desc, count, img) = DescOf(m, v);
            var e = new JsonObject { ["name"] = nm, ["set"] = Math.Max(v.Set, 0), ["binding"] = v.Binding, ["desc"] = desc, ["count"] = count };
            if (img != null) foreach (var kv in img) e[kv.Key] = kv.Value.DeepClone();
            Match fm = Regex.Match(nm, @"^[a-z]+_c(\d+)\z"), fs = Regex.Match(nm, @"^[a-z]+_s(\d+)\z");
            if (nm == "support_buffer") e["kind"] = "support";
            else if (fm.Success)
            {
                long n = long.Parse(fm.Groups[1].Value);
                if (3 <= n && n <= 16) { e["kind"] = "cbuf"; e["reg"] = n - 3; }
                else
                {
                    e["kind"] = n == 1 ? "c1" : "drvcb"; e["bank"] = n;
                    if (n == 0 || n == 2)
                    {
                        var offs = new SortedSet<long>(); bool dyn = false, anyc = false;
                        foreach (var (root, idx) in m.Chains)
                        {
                            if (root != v.Id) continue;
                            anyc = true;
                            if (idx.Length >= 2 && m.Consts.TryGetValue(idx[0], out uint c0) && c0 == 0 && m.Consts.TryGetValue(idx[1], out uint c1)) offs.Add((long)c1 * 16);
                            else dyn = true;
                        }
                        if (anyc) drv[n] = (offs, dyn);
                    }
                }
            }
            else if (fs.Success && desc == "ssbo")
            {
                e["kind"] = "sbuf"; int index = int.Parse(fs.Groups[1].Value);
                JsonObject b = index < sbSorted.Count ? sbSorted[index] : null;
                long cbSlot = b != null ? Long(b["cb_slot"]) : 0, cbOff = b != null ? Long(b["cb_offset"]) : 0;
                long? off = b != null && cbSlot == 0 ? cbOff * 4 : null;
                if (stage == "cs" && off != null && 0x210 <= off && off < 0x310 && (off - 0x210) % 16 == 0) { e["kind"] = "ubo_ptr"; e["reg"] = (off - 0x210) / 16; }
                else if (b != null && cbSlot != 0 && 3 <= cbSlot && cbSlot <= 16) { e["kind"] = "sbuf_ubo"; e["reg"] = cbSlot - 3; e["off"] = cbOff * 4; }
                else e["reg"] = LongOrNull(off != null && sbBase != null && 0 <= off - sbBase && off - sbBase < 0x100 && (off - sbBase) % 16 == 0 ? (off - sbBase) / 16 : null);
                e["rw"] = (b != null && (Long(b["flags"] ?? 0) & 1) != 0) || v.Stored;
            }
            else
            {
                var d = VkDecodeTex(nm);
                if (d == null && desc == "sampler") d = VkDecodeTex(Regex.Replace(nm, @"^([a-z]+)_s", "$1_t"));
                if (d == null) e["kind"] = "?";
                else if (desc == "sampler")
                {
                    if (d.Value.ty != 0) { e["kind"] = "samp"; e["reg"] = LongOrNull(d.Value.s); }
                    else { e["kind"] = "samp_tex"; e["reg"] = LongOrNull(d.Value.t); }
                }
                else
                {
                    bool isImg = Regex.IsMatch(nm, @"^[a-z]+_i");
                    e["kind"] = isImg ? "img" : desc == "utexel" ? "tbuf" : "tex"; e["reg"] = LongOrNull(d.Value.t);
                    if (isImg) e["rw"] = v.Stored;
                    if (desc == "combined")
                    {
                        if (d.Value.ty != 0) { e["samp_kind"] = "samp"; e["samp_reg"] = LongOrNull(d.Value.s); }
                        else { e["samp_kind"] = "samp_tex"; e["samp_reg"] = LongOrNull(d.Value.t); }
                    }
                    if (!isImg && d.Value.t != null) usedSlots.Add(d.Value.t.Value);
                }
            }
            binds.Add(e);
        }
        var dr = new JsonObject();
        foreach (var kv in drv) dr[kv.Key.ToString()] = new JsonObject { ["vec4_offsets"] = new JsonArray(kv.Value.offs.Select(o => (JsonNode)o).ToArray()), ["dynamic"] = kv.Value.dyn };
        var unfillable = new JsonArray();
        foreach (var kv in dr)
        {
            JsonObject d = kv.Value.AsObject();
            if ((bool)d["dynamic"]) unfillable.Add((JsonNode)$"c{kv.Key} dynamic index");   // JsonNode, not the generic Add<string> (NativeAOT)
            (long lo, long hi)[] ranges = kv.Key == "2" ? DrvFillC2 : kv.Key == "0" ? VkDrvFillC0[stage] : Array.Empty<(long lo, long hi)>();
            foreach (JsonNode o in d["vec4_offsets"].AsArray()) { long ov = Long(o); if (!ranges.Any(r => r.lo <= ov && ov < r.hi)) unfillable.Add((JsonNode)$"c{kv.Key}+0x{ov:x}"); }
        }
        var vertexInputs = new JsonArray(); var colors = new SortedSet<long>(); bool writesDepth = false;
        foreach (Var v in m.VarOrder.OrderBy(x => x.Location))
        {
            if (stage == "vs" && v.Storage == 1 && v.Location >= 0 && v.Builtin < 0) vertexInputs.Add(new JsonObject { ["location"] = v.Location, ["type"] = m.ScalarClass(m.Elem(v.Pointee).ty) });
            if (stage == "ps" && v.Storage == 3) { if (v.Location >= 0 && v.Builtin < 0) colors.Add(v.Location); if (v.Builtin == 22) writesDepth = true; }
        }
        JsonObject Ans(string k) => rq[k] as JsonObject is { Count: > 0 } o ? o : new JsonObject();
        var deps = new JsonObject { ["tex"] = new JsonObject(), ["img"] = new JsonObject(), ["attr"] = new JsonObject(), ["fout"] = new JsonObject() };
        IEnumerable<long> Ints(string k) => res[k] is JsonArray a ? a.Select(Long) : Enumerable.Empty<long>();
        foreach (long s in new SortedSet<long>(Ints("tex_queried").Concat(usedSlots))) deps["tex"][s.ToString()] = Ans("tex")[s.ToString()]?.DeepClone() ?? "2d";
        foreach (long s in Ints("img_queried")) deps["img"][s.ToString()] = Ans("img")[s.ToString()]?.DeepClone() ?? new JsonObject();
        if (stage == "vs") foreach (JsonNode vi in vertexInputs) { string loc = Long(vi["location"]).ToString(); deps["attr"][loc] = Ans("attr")[loc]?.DeepClone() ?? "float"; }
        if (stage == "ps") foreach (long c in colors) deps["fout"][c.ToString()] = Ans("fout")[c.ToString()]?.DeepClone() ?? "float";
        JsonNode local = m.LocalSize != null ? new JsonArray(m.LocalSize.Select(x => (JsonNode)x).ToArray()) : rq["local"]?.DeepClone();
        return new JsonObject
        {
            ["key"] = key, ["route"] = "B", ["target"] = "spirv", ["chain"] = chain, ["stage"] = stage, ["entry"] = m.EntryName, ["set"] = stage == "ps" ? 1 : 0,
            ["threadgroup_size"] = local, ["c1"] = rq["c1"] is JsonNode c1n ? c1n.DeepClone() : "", ["notes"] = res["notes"]?.DeepClone() ?? new JsonArray(),
            ["drv_reads"] = dr, ["drv_unfillable"] = unfillable, ["bindings"] = binds, ["vertex_inputs"] = vertexInputs, ["frag_outputs"] = new JsonArray(colors.Select(c => (JsonNode)c).ToArray()),
            ["writes_depth"] = writesDepth, ["uses_fp64"] = m.UsesFp64, ["uses_frag_coord"] = res["uses_frag_coord"]?.DeepClone(),
            ["capabilities"] = new JsonArray(m.Caps.Select(c => (JsonNode)c).ToArray()), ["extensions"] = new JsonArray(m.Exts.Select(x => (JsonNode)x).ToArray()), ["raster"] = new JsonObject(), ["deps"] = deps,
        };
    }

    // ---------- one request (shader_bridge.py handle) ----------
    static void HandleVk(JsonObject rq)
    {
        string variant = (string)rq["variant"], stage = (string)rq["stage"];
        if (rq["code_hex"] is JsonNode hex)
        {
            string codePath = Path.Combine(outDir, variant + ".code");
            File.WriteAllBytes(codePath, Convert.FromHexString((string)hex));
            rq.Remove("code_hex"); rq["code"] = codePath;
        }
        JsonObject tf = rq["tf"] as JsonObject; rq.Remove("tf");
        var mslRq = new JsonObject(); foreach (var kv in rq) if (kv.Key != "caps") mslRq[kv.Key] = kv.Value?.DeepClone();
        Handle(mslRq);
        byte[] spv = File.ReadAllBytes(Path.Combine(outDir, variant + ".spv"));
        JsonObject res = ReadRes(variant);
        var m = new Spirv(spv);
        if (stage == "ps") { spv = m.SetDescriptorSets(v => v.Name == "support_buffer" ? null : 1); m = new Spirv(spv); }
        if (stage == "vs") { byte[] d = VsOutDefaults(m); if (d != null) { spv = d; m = new Spirv(spv); } }
        JsonObject refl = ReflectVk(m, rq, res, variant);
        var so = SbufOffsets(m, stage);
        if (so != null) { spv = so.Value.spv; m = new Spirv(spv); refl["bindings"].AsArray().Add(so.Value.refl); }
        if (tf != null)
        {
            var (tspv, extra) = TfPatch(m, tf); spv = tspv;
            foreach (JsonObject e in extra) refl["bindings"].AsArray().Add(e);
            refl["tf"] = tf.DeepClone();
        }
        string vspv = Path.Combine(resultsDir, variant + "_v.spv"), vjson = Path.Combine(resultsDir, variant + "_v.json");
        File.WriteAllBytes(vspv + ".tmp", spv); File.Move(vspv + ".tmp", vspv, true);
        File.WriteAllText(vjson + ".tmp", PyJson(refl, 1, true)); File.Move(vjson + ".tmp", vjson, true);
        foreach (string s in new[] { ".code", ".spv", ".res.json", "_b.json", "_b.metal" }) File.Delete(Path.Combine(outDir, variant + s));
    }

    // totk_xlate --service-vk --spirv-cross <path> --out <work dir> --results <dir> --chain <name> [--debug-mode 0|1]
    public static int RunVk(string[] args)
    {
        string dm = Environment.GetEnvironmentVariable("TOTK_SHADER_DEBUGMODE");
        debugMode = string.IsNullOrEmpty(dm) ? 1 : (int)ParseInt(dm.Trim());
        for (int i = 1; i < args.Length; i++)
        {
            string v = i + 1 < args.Length ? args[i + 1] : null;
            switch (args[i])
            {
                case "--spirv-cross": spirvCross = v; i++; break;
                case "--out": outDir = v; i++; break;
                case "--results": resultsDir = v; i++; break;
                case "--chain": chain = v; i++; break;
                case "--debug-mode": debugMode = (int)ParseInt(v); i++; break;
                default: Console.Error.WriteLine($"totk_xlate --service-vk: unknown argument {args[i]}"); return 2;
            }
        }
        if (spirvCross == null || outDir == null || resultsDir == null || chain == null)
        {
            Console.Error.WriteLine("usage: totk_xlate --service-vk --spirv-cross <path> --out <work dir> --results <dir> --chain <name> [--debug-mode 0|1]");
            return 2;
        }
        Directory.CreateDirectory(outDir); Directory.CreateDirectory(resultsDir);
        var stdout = Console.Out;
        string line;
        while ((line = Console.In.ReadLine()) != null)
        {
            if (line.Trim().Length == 0) continue;
            string reply;
            try
            {
                var rq = JsonNode.Parse(line).AsObject();
                HandleVk(rq);
                reply = "{\"ok\": true}";
            }
            catch (Exception e)   // reported to the backend, which skips the variant's draws
            {
                Console.Error.WriteLine($"totk_xlate --service-vk: {e}");
                string kind = e is RuntimeError ? "RuntimeError" : e.GetType().Name;
                reply = PyJson(new JsonObject { ["ok"] = false, ["error"] = Cut($"{kind}: {e.Message}", 500) }, -1, true);
            }
            stdout.Write(reply + "\n");
            stdout.Flush();
        }
        return 0;
    }
}
