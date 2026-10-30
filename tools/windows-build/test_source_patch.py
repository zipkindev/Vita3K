import subprocess
from pathlib import Path
import tempfile
import unittest

from ApplySourcePatch import apply


class SourcePatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name)
        subprocess.run(['git', 'init', '-q', str(self.source)], check=True)
        repository = Path(__file__).resolve().parents[2]
        self.patch = repository / 'tools/windows-build/patches/atrac9-input-span.patch'
        self.file = self.source / 'vita3k/codec/src/atrac9.cpp'
        self.file.parent.mkdir(parents=True)
        self.current = (repository / 'vita3k/codec/src/atrac9.cpp').read_text()
        self.fixed = ('    const int available = superframe_data_left > 0 ? '
                      'static_cast<int>(std::min<uint32_t>(size, superframe_data_left)) : 0;\n'
                      '    const int res = Atrac9DecodeBounded(decoder_handle, data, available, '
                      'reinterpret_cast<short *>(result.data()), &decode_used);')
        self.original = ('    const int res = Atrac9DecodeBounded(decoder_handle, data, '
                         'superframe_data_left, reinterpret_cast<short *>(result.data()), &decode_used);')
        self.assertIn(self.fixed, self.current)

    def test_original_source_gets_bounds_fix(self):
        self.file.write_text(self.current.replace(self.fixed, self.original))
        self.assertEqual(apply(self.source, self.patch, True), 'applied')
        self.assertEqual(self.file.read_text(), self.current)

    def test_merged_source_is_accepted_without_changes(self):
        self.file.write_text(self.current)
        self.assertEqual(apply(self.source, self.patch, True), 'already-applied')
        self.assertEqual(self.file.read_text(), self.current)

    def test_already_applied_requires_explicit_opt_in(self):
        self.file.write_text(self.current)
        with self.assertRaises(RuntimeError):
            apply(self.source, self.patch)
        self.assertEqual(self.file.read_text(), self.current)

    def test_unexpected_bounds_code_fails_without_modification(self):
        modified = self.current.replace('std::min<uint32_t>(size, superframe_data_left)', 'size')
        self.file.write_text(modified)
        with self.assertRaises(RuntimeError):
            apply(self.source, self.patch, True)
        self.assertEqual(self.file.read_text(), modified)

    def test_partial_patch_fails_without_modification(self):
        modified = self.current.replace('data, available,', 'data, superframe_data_left,')
        self.file.write_text(modified)
        with self.assertRaises(RuntimeError):
            apply(self.source, self.patch, True)
        self.assertEqual(self.file.read_text(), modified)


if __name__ == '__main__':
    unittest.main()
