#!/usr/bin/env python3
"""Stage 0 survey: what a Switch game's code asks of the host, read from its extracted ExeFS.

Reports, without running anything: each module's build ID and the SDK version it names, middleware linked into it, the
NVN entry points the game names compared with the ones this tree's graphics layer implements, the services its
main.npdm may open compared with the ones the host mentions, and which nn:: libraries main imports from the SDK.

usage: survey.py <exefs folder> [--json out.json]

The folder is scripts/extract_code.py's output (main, sdk, subsdk*, main.npdm). Nothing from the game is written
unless --json is given; keep that file in the work folder (it names the game's symbols).
"""
import argparse
import json
import re
import struct
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / 'third_party/d2r'))   # this game's own copy of the rewriter
import nsoimg

MIDDLEWARE = {   # a marker string in the image -> what it means for the host
    'Wwise': rb'AkSoundEngine|Wwise',
    'CRIWARE': rb'CRIWARE|CRI Middleware',
    'FMOD': rb'FMOD',
    'Havok': rb'Havok|hkBase',
    'PhysX': rb'PhysX|PxFoundation',
    'Nintendo online (NPLN)': rb'npln',
    'Nintendo online (NEX)': rb'nn::nex|NEX_',
    'gRPC': rb'grpc_',
    'OpenSSL': rb'OpenSSL',
    'Pia (P2P)': rb'nn::pia|pia::',
    'Movie: VP9': rb'vp9|libvpx',
    'Movie: H.264': rb'avc|h264|H264',
    'Unreal': rb'UnrealEngine|UE4',
    'Unity': rb'UnityEngine|il2cpp',
}


def build_id(path):
    return Path(path).read_bytes()[0x40:0x60].rstrip(b'\0').hex().upper()


def strings(img, pattern):
    return sorted({m.group(1).decode('latin1') for m in re.finditer(pattern, img)})


def sdk_imports(img):
    """nn::<library> of every undefined symbol main imports, counted."""
    libs = Counter()
    try:
        entries = nsoimg.relocs(img)
    except (AssertionError, KeyError, struct.error, ValueError):
        return None   # no MOD0 / dynamic section (not a dynamic module)
    for _, _, name, value, _ in entries:
        m = name and value is None and re.match(r'_ZN[KV]*2nn(\d+)', name)
        if m:
            n = int(m.group(1)); rest = name[m.end():]
            libs[rest[:n]] += 1
    return libs


def npdm_services(data):
    """(title name, program id, [(service, is_server)]) from main.npdm's ACI0 service access control."""
    if data[:4] != b'META':
        raise ValueError('main.npdm: no META header')
    name = data[0x20:0x30].rstrip(b'\0').decode('latin1')
    aci0, aci0_size = struct.unpack_from('<II', data, 0x70)
    aci = data[aci0:aci0 + aci0_size]
    if aci[:4] != b'ACI0':
        raise ValueError('main.npdm: no ACI0')
    program_id = struct.unpack_from('<Q', aci, 0x10)[0]
    off, size = struct.unpack_from('<II', aci, 0x28)
    out, p = [], off
    while p < off + size:
        ctrl = aci[p]; n = (ctrl & 7) + 1
        out.append((aci[p + 1:p + 1 + n].decode('latin1'), bool(ctrl & 0x80)))
        p += 1 + n
    return name, program_id, out


def implemented_nvn():
    names = set()
    for f in (HERE / 'third_party/d2r_gfx/gfx/nvn').glob('*.[cm]'):
        names.update(re.findall(r'\bNVN(?:_NOPROTO|_EXTRA)?\((nvn\w+)', f.read_text(errors='replace')))   # nvn_impl.h
    return names


def host_text():
    return '\n'.join(f.read_text(errors='replace') for f in (HERE / 'runtime').glob('*.[cm]'))


def survey(exefs):
    exefs = Path(exefs)
    modules = [m for m in ('rtld', 'main', 'sdk') + tuple(f'subsdk{i}' for i in range(10)) if (exefs / m).is_file()]
    if 'main' not in modules:
        raise SystemExit(f'{exefs}: no main module (expected scripts/extract_code.py output)')
    report = {'modules': {}, 'middleware': {}, 'nvn': {}, 'services': None, 'sdk_imports': None}
    images = {m: nsoimg.load(exefs / m) for m in modules}
    for m, img in images.items():
        report['modules'][m] = {
            'build_id': build_id(exefs / m),
            'size': len(img),
            'sdk': strings(img, rb'(SDK MW\+[ -~]+)'),
        }
        for label, pat in MIDDLEWARE.items():
            if re.search(pat, img):
                report['middleware'].setdefault(label, []).append(m)
    named = set()
    for img in images.values():
        named.update(strings(img, rb'(?<=\0)(nvn[A-Z][A-Za-z0-9]+)(?=\0)'))   # names packed one NUL apart
    have = implemented_nvn()
    report['nvn'] = {'named': len(named), 'implemented': sorted(named & have), 'missing': sorted(named - have)}
    if (exefs / 'main.npdm').is_file():
        name, program_id, services = npdm_services((exefs / 'main.npdm').read_bytes())
        text = host_text()
        report['services'] = {
            'title_name': name, 'program_id': f'{program_id:016X}',
            'list': [{'name': s, 'server': srv, 'host_mentions': f'"{s}' in text} for s, srv in services],
        }
    libs = sdk_imports(images['main'])
    report['sdk_imports'] = dict(libs.most_common()) if libs is not None else None
    return report


def print_report(r):
    print('## Modules')
    for m, info in r['modules'].items():
        print(f"- {m}: build {info['build_id']}, {info['size'] / 2**20:.1f} MiB" + (f", {'; '.join(info['sdk'])}" if info['sdk'] else ''))
    print('\n## Middleware markers (string matches; confirm by reading)')
    for label, mods in sorted(r['middleware'].items()) or [('none found', [])]:
        print(f"- {label}" + (f": {', '.join(mods)}" if mods else ''))
    n = r['nvn']
    print(f"\n## NVN: {n['named']} names, {len(n['implemented'])} implemented here, {len(n['missing'])} not")
    for name in n['missing']:
        print(f'- {name}')
    s = r['services']
    if s:
        missing = [e['name'] for e in s['list'] if not e['host_mentions']]
        print(f"\n## Services: {len(s['list'])} in main.npdm ({s['title_name']}, program {s['program_id']}); "
              f"{len(missing)} never mentioned by the host")
        for e in s['list']:
            print(f"- {e['name']}{' (server)' if e['server'] else ''}{'' if e['host_mentions'] else '  <- not in runtime/'}")
    else:
        print('\n## Services: no main.npdm in the folder')
    print('\n## nn:: libraries main imports')
    if r['sdk_imports'] is None:
        print('- (main has no dynamic section)')
    for lib, count in (r['sdk_imports'] or {}).items():
        print(f'- nn::{lib}: {count}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exefs', type=Path)
    ap.add_argument('--json', type=Path, help='also write the report as JSON (keep it in the work folder)')
    args = ap.parse_args()
    r = survey(args.exefs)
    print_report(r)
    if args.json:
        args.json.write_text(json.dumps(r, indent=2) + '\n')


if __name__ == '__main__':
    main()
