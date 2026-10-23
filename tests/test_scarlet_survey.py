"""games/scarlet/scripts/survey.py reads a synthetic ExeFS: build IDs, the SDK string, middleware markers, NVN names
split by what the graphics layer implements, and main.npdm's service list."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/scarlet/scripts'))
import survey  # noqa: E402

BUILD_ID = bytes(range(0x10, 0x24))


def nso_bytes(rodata):
    """Uncompressed NSO: an empty text segment, the given rodata, no data; no MOD0 (not a dynamic module)."""
    text = bytes(0x100)
    header = bytearray(0x100)
    header[0:4] = b'NSO0'
    segments = [(text, 0), (rodata, len(text)), (b'', len(text) + len(rodata))]
    offset = 0x100
    for index, (data, address) in enumerate(segments):
        struct.pack_into('<IIII', header, 0x10 + 16 * index, offset, address, len(data), address)
        offset += len(data)
    header[0x40:0x54] = BUILD_ID
    struct.pack_into('<III', header, 0x60, *(len(d) for d, _ in segments))
    return bytes(header) + b''.join(d for d, _ in segments)


def npdm_bytes(services):
    sac = b''.join(bytes([(len(s) - 1) | (0x80 if srv else 0)]) + s.encode() for s, srv in services)
    aci = bytearray(0x40)
    aci[0:4] = b'ACI0'
    struct.pack_into('<Q', aci, 0x10, 0x0100A3D008C5C000)
    struct.pack_into('<II', aci, 0x28, len(aci), len(sac))
    aci += sac
    meta = bytearray(0x80)
    meta[0:4] = b'META'
    meta[0x20:0x26] = b'Survey'
    struct.pack_into('<II', meta, 0x70, len(meta), len(aci))
    return bytes(meta + aci)


class Survey(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.exefs = Path(self.tmp.name)
        rodata = (b'\0SDK MW+Nintendo+NintendoSdk_nnSdk-99_1_0-Release\0'
                  b'\0nvnDeviceInitialize\0nvnMadeUpEntryPoint\0AkSoundEngine::Init\0')
        (self.exefs / 'main').write_bytes(nso_bytes(rodata))
        (self.exefs / 'sdk').write_bytes(nso_bytes(b'\0nothing here\0'))
        (self.exefs / 'main.npdm').write_bytes(npdm_bytes([('audren:u', False), ('zz:fake', False), ('srv:x', True)]))

    def tearDown(self):
        self.tmp.cleanup()

    def test_report(self):
        r = survey.survey(self.exefs)
        self.assertEqual(list(r['modules']), ['main', 'sdk'])
        self.assertEqual(r['modules']['main']['build_id'], BUILD_ID.hex().upper())
        self.assertEqual(r['modules']['main']['sdk'], ['SDK MW+Nintendo+NintendoSdk_nnSdk-99_1_0-Release'])
        self.assertEqual(r['middleware'], {'Wwise': ['main']})
        self.assertEqual(r['nvn'], {'named': 2, 'implemented': ['nvnDeviceInitialize'], 'missing': ['nvnMadeUpEntryPoint']})
        self.assertEqual(r['services']['program_id'], '0100A3D008C5C000')
        self.assertEqual(r['services']['title_name'], 'Survey')
        self.assertEqual([(e['name'], e['server'], e['host_mentions']) for e in r['services']['list']],
                         [('audren:u', False, True), ('zz:fake', False, False), ('srv:x', True, False)])
        self.assertIsNone(r['sdk_imports'])

    def test_no_main_is_refused(self):
        (self.exefs / 'main').unlink()
        with self.assertRaises(SystemExit):
            survey.survey(self.exefs)


if __name__ == '__main__':
    unittest.main()
