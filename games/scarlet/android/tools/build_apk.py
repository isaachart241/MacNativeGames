#!/usr/bin/env python3
"""Thor port, stage T3: pack and sign the APK without the Android SDK (no aapt2, apksigner, Java or Gradle).

    python3 android/tools/build_apk.py --manifest android/app/AndroidManifest.xml \\
        --lib SCARLET_build/build-android-apk/libscarlet.so --key SCARLET_build/apk-key -o scarlet.apk

(android/Makefile APK=1 GFX=vk apk runs this.) The APK holds:
  - AndroidManifest.xml, compiled here to Android's binary XML. Only the android: attributes in ATTRS are known; their
    resource IDs are the framework's public ones (checked against the Thor's /system/framework/framework-res.apk).
  - lib/arm64-v8a/libscarlet.so, and the programs it runs (--extra-lib: the shader translator and spirv-cross).
  - an APK Signature Scheme v2 block (minSdkVersion 33 needs no v1 JAR signature), signed with openssl by a debug key
    made in --key the first time (key.pem, cert.der, pub.der). Keep the folder: Android refuses an update signed by
    another key (uninstall first, which deletes the app's files folder).
No game code or data go into the APK: the app reads them from its files folder (runtime/activity_android.c).
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ANDROID_NS = 'http://schemas.android.com/apk/res/android'
NONE = 0xffffffff

# android:<name>: (resource ID, kind)
ATTRS = {
    'theme': (0x01010000, 'ref'), 'label': (0x01010001, 'string'), 'name': (0x01010003, 'string'),
    'hasCode': (0x0101000c, 'bool'), 'debuggable': (0x0101000f, 'bool'), 'exported': (0x01010010, 'bool'),
    'launchMode': (0x0101001d, 'enum'), 'screenOrientation': (0x0101001e, 'enum'), 'configChanges': (0x0101001f, 'flags'),
    'value': (0x01010024, 'string'), 'minSdkVersion': (0x0101020c, 'int'), 'versionCode': (0x0101021b, 'int'),
    'versionName': (0x0101021c, 'string'), 'targetSdkVersion': (0x01010270, 'int'), 'allowBackup': (0x01010280, 'bool'),
    'isGame': (0x010103f4, 'bool'), 'extractNativeLibs': (0x010104ea, 'bool'), 'resizeableActivity': (0x010104f6, 'bool'),
}
ENUMS = {
    'launchMode': {'standard': 0, 'singleTop': 1, 'singleTask': 2, 'singleInstance': 3},
    'screenOrientation': {'unspecified': -1, 'landscape': 0, 'portrait': 1, 'user': 2, 'behind': 3, 'sensor': 4,
                          'nosensor': 5, 'sensorLandscape': 6, 'sensorPortrait': 7, 'reverseLandscape': 8,
                          'reversePortrait': 9, 'fullSensor': 10, 'userLandscape': 11, 'userPortrait': 12,
                          'fullUser': 13, 'locked': 14},
}
FLAGS = {
    'configChanges': {'mcc': 0x1, 'mnc': 0x2, 'locale': 0x4, 'touchscreen': 0x8, 'keyboard': 0x10,
                      'keyboardHidden': 0x20, 'navigation': 0x40, 'orientation': 0x80, 'screenLayout': 0x100,
                      'uiMode': 0x200, 'screenSize': 0x400, 'smallestScreenSize': 0x800, 'density': 0x1000,
                      'layoutDirection': 0x2000, 'colorMode': 0x4000, 'fontScale': 0x40000000},
}
STYLES = {'Theme.Material.NoActionBar.Fullscreen': 0x0103022f, 'Theme.DeviceDefault.NoActionBar.Fullscreen': 0x0103012a,
          'Theme.NoTitleBar.Fullscreen': 0x01030007}
TYPE_REFERENCE, TYPE_STRING, TYPE_INT_DEC, TYPE_INT_HEX, TYPE_INT_BOOLEAN = 0x01, 0x03, 0x10, 0x11, 0x12


def split(key):
    return tuple(key[1:].split('}', 1)) if key.startswith('{') else (None, key)


class Pool:
    def __init__(self):
        self.strings, self.index = [], {}

    def add(self, s):
        if s not in self.index:
            self.index[s] = len(self.strings)
            self.strings.append(s)
        return self.index[s]

    def chunk(self):
        offsets, data = [], b''
        for s in self.strings:
            offsets.append(len(data))
            u = s.encode('utf-16le')
            if len(u) // 2 >= 0x8000:
                sys.exit('string too long for the manifest: %r' % s[:40])
            data += struct.pack('<H', len(u) // 2) + u + b'\0\0'
        data += b'\0' * (-len(data) % 4)
        body = struct.pack('<%dI' % len(offsets), *offsets) + data
        return struct.pack('<HHIIIIII', 0x0001, 28, 28 + len(body), len(self.strings), 0, 0, 28 + 4 * len(offsets), 0) + body


def typed(name, kind, text, pool):
    """(rawValue, dataType, data) of android:name="text"."""
    if kind == 'string':
        i = pool.add(text)
        return i, TYPE_STRING, i
    if kind == 'bool':
        if text not in ('true', 'false'):
            sys.exit('android:%s="%s": expected true or false' % (name, text))
        return NONE, TYPE_INT_BOOLEAN, NONE if text == 'true' else 0
    if kind == 'int':
        return NONE, TYPE_INT_DEC, int(text, 0) & NONE
    if kind == 'enum':
        return NONE, TYPE_INT_DEC, ENUMS[name][text] & NONE
    if kind == 'flags':
        v = 0
        for f in text.split('|'):
            v |= FLAGS[name][f.strip()]
        return NONE, TYPE_INT_HEX, v
    if kind == 'ref':
        prefix = '@android:style/'
        if not text.startswith(prefix) or text[len(prefix):] not in STYLES:
            sys.exit('android:%s="%s": known styles are %s' % (name, text, ', '.join(STYLES)))
        return NONE, TYPE_REFERENCE, STYLES[text[len(prefix):]]
    raise ValueError(kind)


def binary_xml(path):
    root = ET.parse(path).getroot()
    used = set()
    for el in root.iter():
        for k in el.attrib:
            ns, local = split(k)
            if ns == ANDROID_NS:
                if local not in ATTRS:
                    sys.exit('%s: android:%s is not known to build_apk.py (add it to ATTRS)' % (path, local))
                used.add(local)
            elif ns is not None:
                sys.exit('%s: attribute namespace %s is not supported' % (path, ns))
    pool = Pool()
    # The attribute names that have resource IDs come first: the resource map gives the ID of string i.
    names = sorted(used, key=lambda n: ATTRS[n][0])
    for n in names:
        pool.add(n)
    resmap = [ATTRS[n][0] for n in names]
    prefix, uri = pool.add('android'), pool.add(ANDROID_NS)
    nodes = [struct.pack('<HHIIIII', 0x0100, 16, 24, 1, NONE, prefix, uri)]
    line = [1]

    def element(el):
        line[0] += 1
        name = pool.add(el.tag)
        attrs = []
        for k, v in el.attrib.items():
            ns, local = split(k)
            if ns == ANDROID_NS:
                rid, kind = ATTRS[local]
                raw, t, data = typed(local, kind, v, pool)
                attrs.append((rid, uri, pool.index[local], raw, t, data))
            else:
                i = pool.add(v)
                attrs.append((0, NONE, pool.add(local), i, TYPE_STRING, i))
        attrs.sort(key=lambda a: a[0])   # the framework walks them in resource ID order
        body = struct.pack('<IIHHHHHH', NONE, name, 20, 20, len(attrs), 0, 0, 0)
        for _rid, ns, n, raw, t, data in attrs:
            body += struct.pack('<IIIHBBI', ns, n, raw, 8, 0, t, data)
        nodes.append(struct.pack('<HHIII', 0x0102, 16, 16 + len(body), line[0], NONE) + body)
        for child in el:
            element(child)
        nodes.append(struct.pack('<HHIIIII', 0x0103, 16, 24, line[0], NONE, NONE, name))

    element(root)
    nodes.append(struct.pack('<HHIIIII', 0x0101, 16, 24, line[0], NONE, prefix, uri))
    rest = pool.chunk() + struct.pack('<HHI', 0x0180, 8, 8 + 4 * len(resmap)) + struct.pack('<%dI' % len(resmap), *resmap) + b''.join(nodes)
    return struct.pack('<HHI', 0x0003, 8, 8 + len(rest)) + rest


def openssl(*args):
    subprocess.run(['openssl'] + list(args), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def debug_key(folder):
    os.makedirs(folder, exist_ok=True)
    key, cert, pub = (os.path.join(folder, n) for n in ('key.pem', 'cert.der', 'pub.der'))
    if not os.path.exists(key):
        pem = os.path.join(folder, 'cert.pem')
        openssl('req', '-x509', '-newkey', 'rsa:2048', '-noenc', '-keyout', key, '-out', pem, '-days', '10000',
                '-subj', '/CN=Scarlet for the Thor (debug)')
        openssl('x509', '-in', pem, '-outform', 'DER', '-out', cert)
        openssl('pkey', '-in', key, '-pubout', '-outform', 'DER', '-out', pub)
        print('made a debug signing key in %s' % folder)
    return key, open(cert, 'rb').read(), open(pub, 'rb').read()


def lp(b):
    return struct.pack('<I', len(b)) + b


def sign_v2(apk, key_folder):
    """APK Signature Scheme v2 (source.android.com/docs/security/features/apksigning/v2): RSA PKCS#1 v1.5 with SHA-256
    over 1 MiB chunks of the entries, the central directory and the end record; the block goes before the directory."""
    eocd = apk.rfind(b'PK\x05\x06')
    cd_size, cd_off = struct.unpack_from('<II', apk, eocd + 12)
    if cd_off + cd_size != eocd:
        sys.exit('unexpected zip layout')
    sections = (apk[:cd_off], apk[cd_off:eocd], apk[eocd:])
    chunks = []
    for s in sections:
        for i in range(0, len(s), 1 << 20):
            c = s[i:i + (1 << 20)]
            chunks.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    digest = hashlib.sha256(b'\x5a' + struct.pack('<I', len(chunks)) + b''.join(chunks)).digest()
    alg = struct.pack('<I', 0x0103)   # RSASSA-PKCS1-v1_5 with SHA2-256
    key, cert, pub = debug_key(key_folder)
    signed = lp(lp(alg + lp(digest))) + lp(lp(cert)) + lp(b'')
    with tempfile.TemporaryDirectory() as tmp:
        data, sig = os.path.join(tmp, 'signed'), os.path.join(tmp, 'sig')
        open(data, 'wb').write(signed)
        openssl('dgst', '-sha256', '-sign', key, '-out', sig, data)
        signature = open(sig, 'rb').read()
    signer = lp(signed) + lp(lp(alg + lp(signature))) + lp(pub)
    value = lp(lp(signer))
    pair = struct.pack('<QI', 4 + len(value), 0x7109871a) + value
    size = len(pair) + 8 + 16
    block = struct.pack('<Q', size) + pair + struct.pack('<Q', size) + b'APK Sig Block 42'
    end = bytearray(sections[2])
    struct.pack_into('<I', end, 16, cd_off + len(block))
    return sections[0] + block + sections[1] + bytes(end)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--lib', required=True, help='libscarlet.so (android/Makefile APK=1 GFX=vk)')
    ap.add_argument('--key', required=True, help='folder of the debug signing key (made if missing)')
    ap.add_argument('--extra-lib', action='append', default=[], help='another lib*.so for lib/arm64-v8a (the shader translator, spirv-cross)')
    ap.add_argument('-o', '--output', required=True)
    args = ap.parse_args()
    manifest = binary_xml(args.manifest)
    fixed = (2008, 1, 1, 0, 0, 0)
    with tempfile.TemporaryDirectory() as tmp:
        unsigned = os.path.join(tmp, 'unsigned.apk')
        with zipfile.ZipFile(unsigned, 'w') as z:
            z.writestr(zipfile.ZipInfo('AndroidManifest.xml', fixed), manifest, compress_type=zipfile.ZIP_DEFLATED)
            z.writestr(zipfile.ZipInfo('lib/arm64-v8a/libscarlet.so', fixed), open(args.lib, 'rb').read(),
                       compress_type=zipfile.ZIP_DEFLATED)
            for extra in args.extra_lib:   # programs the app runs: extracted with execute permission (extractNativeLibs)
                info = zipfile.ZipInfo('lib/arm64-v8a/' + os.path.basename(extra), fixed)
                info.external_attr = 0o100755 << 16
                z.writestr(info, open(extra, 'rb').read(), compress_type=zipfile.ZIP_DEFLATED)
        apk = sign_v2(open(unsigned, 'rb').read(), args.key)
    with open(args.output, 'wb') as f:
        f.write(apk)
    print('built %s (%.1f MB)' % (args.output, len(apk) / 1e6))


if __name__ == '__main__':
    main()
