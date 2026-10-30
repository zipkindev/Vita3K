"""Public packaging must preserve runtime bytes and symbol identity, never raw logs."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
import uuid
import zipfile

from PackageDownloads import package_downloads
from PublicPackage import prepare_public_package, public_text, validate_symbol_privacy
from verify_pe_pdb import pdb_identity, pe_identity


def symbols():
    guid = uuid.UUID('156dded6-f0c1-4c88-b6cd-d0211ca1daed').bytes_le
    pdb = bytearray(2048)
    pdb[:32] = b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0'
    struct.pack_into('<6I', pdb, 32, 512, 0, 4, 16, 0, 1)
    struct.pack_into('<I', pdb, 512, 2)
    struct.pack_into('<4I', pdb, 1024, 2, 0xffffffff, 28, 3)
    struct.pack_into('<III', pdb, 1536, 20000404, 0, 1)
    pdb[1548:1564] = guid
    exe = bytearray(1024)
    struct.pack_into('<I', exe, 0x3c, 128)
    exe[128:132] = b'PE\0\0'
    struct.pack_into('<H12xH', exe, 134, 1, 240)
    struct.pack_into('<H', exe, 152, 0x20b)
    struct.pack_into('<II', exe, 312, 4096, 28)
    struct.pack_into('<IIII', exe, 400, 512, 4096, 512, 512)
    struct.pack_into('<IIII', exe, 524, 2, 40, 0, 560)
    cv = b'RSDS' + guid + struct.pack('<I', 1) + b'Vita3K.pdb\0'
    exe[560:560+len(cv)] = cv
    return bytes(exe), bytes(pdb)


class PublicPackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.package = self.root / 'private'
        self.package.mkdir()
        exe, pdb = symbols()
        self.assertEqual(pdb_identity(pdb)['guid'], pe_identity(exe)['guid'])
        self.write('Vita3K.exe', exe)
        self.write('Vita3K.pdb', pdb)
        self.write('Qt6Core.dll', b'unchanged released library')
        self.write('build-evidence/CMakeCache.txt', b'SITE=private-machine\nC:\\Users\\Private\\source')
        self.write('build-evidence/tests.log', b'private-machine\n100% tests passed, 0 tests failed out of 38')
        self.write('build-evidence/source.patch', 'diff --git a/test b/test\n'.encode('utf-16'))
        self.write('build-evidence/unknown.txt', b'private-machine')
        self.write('configure.log', b'private-machine')
        self.manifest = {'buildLabel': 'test.1', 'sourceCommit': 'a'*40,
                         'recipeCommit': 'b'*40, 'sourceMatch': 'Separately sourced; match unverified',
                         'exeIdentity': {'pdbPath': r'C:\Users\Private\Vita3K.pdb'},
                         'machine': 'private-machine'}
        self.save_manifest()

    def write(self, name, data):
        path = self.package / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_bytes(data)

    def save_manifest(self):
        self.manifest['files'] = [{'path': p.relative_to(self.package).as_posix(),
                                  'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                                 for p in self.package.rglob('*') if p.is_file() and p.name != 'provenance.json']
        (self.package / 'provenance.json').write_text(json.dumps(self.manifest))

    def test_public_archives_preserve_runtime_and_have_matching_symbols(self):
        self.write('unlisted.txt', b'not in original manifest')
        output = self.root / 'downloads'
        downloads = package_downloads(self.package, output, 'test.1')
        combined = {}
        for record in downloads:
            archive = output / record['file']
            self.assertEqual(record['sha256'], hashlib.sha256(archive.read_bytes()).hexdigest())
            with zipfile.ZipFile(archive) as zipped:
                combined.update({name: zipped.read(name) for name in zipped.namelist()})
        for name in ('Vita3K.exe', 'Vita3K.pdb', 'Qt6Core.dll'):
            self.assertEqual(combined[name], (self.package / name).read_bytes())
        self.assertEqual(pdb_identity(combined['Vita3K.pdb'])['guid'], pe_identity(combined['Vita3K.exe'])['guid'])
        self.assertNotIn('build-evidence/CMakeCache.txt', combined)
        self.assertNotIn('build-evidence/unknown.txt', combined)
        self.assertNotIn('build-evidence/tests.log', combined)
        self.assertNotIn('configure.log', combined)
        self.assertNotIn('unlisted.txt', combined)
        self.assertEqual(json.loads(combined['build-evidence/public-summary.json'])['testsPassed'], 38)
        self.assertTrue(combined['build-evidence/source.patch'].startswith(b'diff'))
        manifest = json.loads(combined['provenance.json'])
        self.assertNotIn('pdbPath', manifest['exeIdentity'])
        self.assertNotIn('machine', manifest)
        self.assertEqual(manifest['sourceMatch'], self.manifest['sourceMatch'])
        for record in manifest['files']:
            self.assertEqual(record['sha256'], hashlib.sha256(combined[record['path']]).hexdigest())

    def test_sensitive_allowlisted_text_fails_closed(self):
        self.write('build-evidence/source.patch', r'C:\Users\Private\file'.encode('utf-16'))
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'Private metadata'):
            prepare_public_package(self.package, self.root / 'public')

    def test_manifest_tampering_is_rejected(self):
        self.write('Qt6Core.dll', b'corrupted')
        with self.assertRaisesRegex(ValueError, 'manifest mismatch'):
            prepare_public_package(self.package, self.root / 'public')

    def test_parent_traversal_is_rejected(self):
        self.manifest['files'][0]['path'] = '../outside'
        (self.package / 'provenance.json').write_text(json.dumps(self.manifest))
        with self.assertRaisesRegex(ValueError, 'relative path'):
            prepare_public_package(self.package, self.root / 'public')

    def test_public_text_rejects_network_and_home_details(self):
        for text in (r'\\private-host\share', '/home/private/source', '192.168.12.34', '10.1.2.3', r'C:\Users\Private', json.dumps({'path': r'C:\Users\Private'})):
            with self.subTest(text=text), self.assertRaises(ValueError):
                public_text(text, 'test')

    def test_escaped_local_paths_are_not_unc_shares(self):
        # Clang/FFmpeg stores quoted command lines with doubled local separators.
        command = r'"-fdebug-compilation-dir=C:\\vcpkg\\buildtrees\\ffmpeg" "-I" "C:\\Program Files (x86)\\SDK"'
        public_text(command, 'compiler record')
        public_text(json.dumps({'command': command}), 'JSON compiler record')
        path = self.root / 'compiler-record.pdb'
        path.write_bytes(symbols()[1] + command.encode() + b'\0')
        validate_symbol_privacy(path)

    def test_actual_unc_roots_still_fail_in_compiler_and_pdb_records(self):
        for value in (r'"-I" "\\private-host\share"', r'-I\\private-host\share',
                      r'/I\\private-host\share', r'-L\\private-host\share',
                      r'/LIBPATH:\\private-host\share', '\0' + r'\\private-host\share',
                      json.dumps({'path': r'\\private-host\share'})):
            with self.subTest(value=value), self.assertRaises(ValueError):
                public_text(value, 'compiler record')

    def test_new_full_symbols_reject_personal_paths(self):
        self.write('Vita3K.pdb', symbols()[1] + b'C:\\Users\\Private\\temp.obj')
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'Private metadata'):
            prepare_public_package(self.package, self.root / 'public')

    def test_legacy_exception_preserves_full_pdb_bytes(self):
        self.write('Vita3K.pdb', symbols()[1] + b'C:\\Users\\Private\\temp.obj')
        self.save_manifest()
        output = self.root / 'public'
        prepare_public_package(self.package, output, allow_legacy_symbol_paths=True)
        self.assertEqual((output / 'Vita3K.pdb').read_bytes(), (self.package / 'Vita3K.pdb').read_bytes())

    def test_qt_vendor_debug_prefix_is_permitted(self):
        path = self.root / 'vendor.pdb'
        path.write_bytes(b'C:\\Users\\qt\\work\\qtbase\\src\\vendor.cpp')
        validate_symbol_privacy(path)


if __name__ == '__main__':
    unittest.main()
