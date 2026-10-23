import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from ApplyDependencyPatches import apply


class Patches(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.dep = self.root / 'external/dynarmic'
        self.dep.mkdir(parents=True)
        self.git('init', '-q')
        (self.dep / 'dummy.txt').write_text('before\n')
        self.git('add', '.')
        self.git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'base')
        self.base = self.git('rev-parse', 'HEAD')
        self.directory = self.root / 'tools/dependency-patches'
        self.directory.mkdir(parents=True)
        self.patch = self.directory / 'dynarmic-jit-observer.patch'
        self.patch.write_text('diff --git a/dummy.txt b/dummy.txt\n--- a/dummy.txt\n+++ b/dummy.txt\n@@ -1 +1 @@\n-before\n+after\n')
        self.data = {'schema': 1, 'dynarmic': {'baseCommit': self.base, 'file': self.patch.name,
                                           'sha256': hashlib.sha256(self.patch.read_bytes()).hexdigest()}}
        self.save()
    def tearDown(self):
        self.temp.cleanup()
    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.dep), *args], text=True).strip()
    def save(self):
        (self.directory / 'manifest.json').write_text(json.dumps(self.data))
    def test_valid_then_repeat_refused(self):
        apply(self.root)
        self.assertEqual((self.dep / 'dummy.txt').read_text(), 'after\n')
        with self.assertRaises(ValueError): apply(self.root)
    def test_wrong_hash(self):
        self.patch.write_text('altered')
        with self.assertRaises(ValueError): apply(self.root)
        self.assertEqual((self.dep / 'dummy.txt').read_text(), 'before\n')
    def test_wrong_base(self):
        self.data['dynarmic']['baseCommit'] = '0' * 40; self.save()
        with self.assertRaises(ValueError): apply(self.root)
    def test_dirty(self):
        (self.dep / 'untracked.txt').write_text('dirty')
        with self.assertRaises(ValueError): apply(self.root)
    def test_path(self):
        self.data['dynarmic']['file'] = '../outside'; self.save()
        with self.assertRaises(ValueError): apply(self.root)
    def test_absent_is_noop(self):
        (self.directory / 'manifest.json').unlink()
        apply(self.root)
        self.assertEqual(self.git('status', '--porcelain'), '')


if __name__ == '__main__': unittest.main()
