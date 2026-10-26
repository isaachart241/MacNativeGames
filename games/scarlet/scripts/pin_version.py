#!/usr/bin/env python3
"""Write assets/expected-v<version>.json from a verified work folder: the hashes prepare_runtime.py and
prepare_patches.py check before they use anything.

Reads extracted/<dir>/extraction-manifest.json (extract_code.py), rewritten/<dir>/*.patched.bin (patch_nso.py; run
structck.py and equiv.py first) and extracted/<dir>/romfs/index.json (extract_update_romfs.py, all IVFC levels
verified). Hashes and sizes only: nothing from the game goes into the file.

usage: pin_version.py 4.0.0 [--work SCARLET_build] [--title 0100A3D008C5C000]
"""
import argparse
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('version', help='display version, e.g. 4.0.0')
    ap.add_argument('--work', type=Path, default=HERE / 'SCARLET_build')
    ap.add_argument('--title', default='0100A3D008C5C000')
    a = ap.parse_args()
    folder = 'v' + a.version.replace('.', '')
    extracted, rewritten = a.work / 'extracted' / folder, a.work / 'rewritten' / folder
    manifest = json.loads((extracted / 'extraction-manifest.json').read_text())
    exefs = {f['name']: f['sha256'] for f in manifest['files']}
    for name, digest in exefs.items():   # the files on disk still are what extract_code.py verified
        assert sha(extracted / 'exefs' / name) == digest, f'ExeFS {name} changed since extraction'
    romfs = json.loads((extracted / 'romfs/index.json').read_text())
    assert romfs['status'] == 'pass' and (extracted / 'romfs/romfs.bin').stat().st_size == romfs['romfs_size']
    main_nso = (extracted / 'exefs/main').read_bytes()
    pin = {
        '_comment': f'Verified {a.version} code (ExeFS of the update), rewrite bytes and merged RomFS. '
                    'Written by scripts/pin_version.py; checks are recorded in CHANGELOG.md.',
        'title_id': a.title.upper(),
        'update_title_id': f'{int(a.title, 16) | 0x800:016X}',
        'display_version': a.version,
        'main_build_id': main_nso[0x40:0x54].hex().upper(),
        'update_program_nca': {k: manifest['program'][k] for k in ('name', 'nca_offset', 'size')},
        'exefs_sha256': dict(sorted(exefs.items())),
        'rewritten_sha256': {p.name: sha(p) for p in sorted(rewritten.glob('*.patched.bin'))},
        'romfs': {'size': romfs['romfs_size'], 'sha256': romfs['romfs_sha256'], 'files': len(romfs['files'])},
    }
    assert set(pin['rewritten_sha256']) == {'main.patched.bin', 'sdk.patched.bin', 'subsdk0.patched.bin'}
    out = HERE / f'assets/expected-v{a.version}.json'
    if out.exists():   # entries verified by hand (prepare_runtime.py module_table) are kept, never regenerated
        for key in ('relative_beyond_image', 'relative_beyond_image_reason'):
            if key in (old := json.loads(out.read_text())):
                pin[key] = old[key]
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(pin, indent=4) + '\n')
    print(out)


if __name__ == '__main__':
    main()
