import argparse
from contextlib import redirect_stdout
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import RetainArchives as retention


class ArchiveRetentionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.state = self.root / 'state'
        self.state.mkdir()
        self.protection = {'labels': ['released'], 'sources': ['a' * 40], 'assets': []}
        self.args = argparse.Namespace(root=self.root, state=self.state, days=30, keep=1, apply=True)

    def archive(self, label, age, source='b' * 40):
        folder = self.root / 'Vita3K-Go' / f'{label}-123'
        package = folder / 'windows-package'
        package.mkdir(parents=True)
        files = []
        for name in ('Vita3K.exe', 'Vita3K.pdb'):
            payload = (label + name).encode()
            (package / name).write_bytes(payload)
            files.append({'path': name, 'sha256': hashlib.sha256(payload).hexdigest()})
        identity = {'guid': 'test-pair', 'age': 1}
        manifest = {'buildLabel': label, 'runId': '123', 'sourceCommit': source,
                    'sourceRepository': 'https://github.com/zipkindev/Vita3K-Go',
                    'exeIdentity': identity, 'pdbIdentity': identity, 'files': files}
        (package / 'provenance.json').write_text(json.dumps(manifest))
        stamp = dt.datetime.now(retention.UTC) - dt.timedelta(days=age)
        (folder / 'archive-verified.json').write_text(json.dumps(
            {'Verified': True, 'Run': '123', 'Source': source, 'Utc': stamp.isoformat()}))
        return folder

    def apply(self):
        with redirect_stdout(io.StringIO()):
            retention.apply_policy(self.args, self.protection)

    def test_delete_old_unreleased_whole_folder_keep_release_symbols_recent_and_newest(self):
        released = self.archive('released', 100)
        same_source = self.archive('renamed-release', 100, 'a' * 40)
        old = self.archive('expired', 60)
        newest = self.archive('newest', 40)
        self.apply()
        self.assertFalse(old.exists())
        for folder in (released, same_source, newest):
            self.assertTrue((folder / 'windows-package/Vita3K.exe').exists())
            self.assertTrue((folder / 'windows-package/Vita3K.pdb').exists())

    def test_recent_and_manual_pin_survive(self):
        recent = self.archive('recent', 20)
        pinned = self.archive('pinned', 100)
        (pinned / 'KEEP').touch()
        self.archive('newest', 1)
        self.apply()
        self.assertTrue(recent.exists())
        self.assertTrue(pinned.exists())

    def test_dry_run_does_not_delete(self):
        old = self.archive('expired', 60)
        self.archive('newest', 1)
        self.args.apply = False
        self.apply()
        self.assertTrue(old.exists())

    def test_corrupt_symbols_abort_deletion(self):
        old = self.archive('expired', 60)
        self.archive('newest', 1)
        (old / 'windows-package/Vita3K.pdb').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.apply()
        self.assertTrue(old.exists())

    def test_bad_provenance_and_reparse_tree_are_preserved(self):
        old = self.archive('bad-identity', 60)
        linked = self.archive('linked', 60)
        (old / 'archive-verified.json').write_text('{}')
        (linked / 'link').symlink_to(self.state, target_is_directory=True)
        self.archive('newest', 1)
        self.apply()
        self.assertTrue(old.exists())
        self.assertTrue(linked.exists())

    def test_github_failure_never_applies_cleanup(self):
        old = self.archive('expired', 60)
        (self.state / 'protected-builds.json').write_text(json.dumps(self.protection))
        with patch.object(retention, 'listing', side_effect=OSError('API unavailable')):
            with self.assertRaises(OSError):
                retention.run(self.args)
        self.assertTrue(old.exists())

    def test_incomplete_or_malformed_protection_aborts_before_github_or_deletion(self):
        old = self.archive('expired', 60)
        invalid = [{}, {'labels': [], 'sources': []},
                   {'labels': 'released', 'sources': [], 'assets': []},
                   {'labels': [], 'sources': ['bad-source'], 'assets': []},
                   {'labels': [], 'sources': [], 'assets': [None]}]
        for ledger in invalid:
            with self.subTest(ledger=ledger):
                (self.state / 'protected-builds.json').write_text(json.dumps(ledger))
                with patch.object(retention, 'listing') as listing:
                    with self.assertRaises(ValueError):
                        retention.run(self.args)
                    listing.assert_not_called()
                self.assertTrue((old / 'windows-package/Vita3K.pdb').exists())

    def test_unreadable_subdirectory_retains_whole_archive(self):
        old = self.archive('expired', 60)
        self.archive('newest', 1)
        unreadable = old / 'unreadable'
        unreadable.mkdir()
        original = os.scandir

        def scandir(path):
            if Path(path) == unreadable:
                raise PermissionError('Unreadable archive directory')
            return original(path)

        with patch.object(retention.os, 'scandir', side_effect=scandir):
            self.apply()
        self.assertTrue((old / 'windows-package/Vita3K.exe').exists())
        self.assertTrue((old / 'windows-package/Vita3K.pdb').exists())

    def test_draft_asset_alias_and_removed_release_protection_are_sticky(self):
        protection = retention.refresh_protection(
            self.protection,
            [{'tag_name': 'draft', 'draft': True, 'assets': [{'name': 'Vita3K-compiled-label-diagnostics.zip'}]}],
            [{'name': 'draft', 'commit': {'sha': 'c' * 40}},
             {'name': 'unreleased-tag', 'commit': {'sha': 'd' * 40}}])
        self.assertIn('released', protection['labels'])
        self.assertIn('draft', protection['labels'])
        self.assertIn('c' * 40, protection['sources'])
        self.assertNotIn('unreleased-tag', protection['labels'])
        self.assertNotIn('d' * 40, protection['sources'])
        self.assertTrue(retention.protected(
            retention.inspect_archive(self.archive('compiled-label', 100)), protection))

    def test_missing_release_tag_aborts_protection_refresh(self):
        with self.assertRaisesRegex(ValueError, 'missing from tag'):
            retention.refresh_protection(self.protection,
                [{'tag_name': 'published', 'draft': False, 'assets': []}], [])

    def test_asset_release_labels_do_not_pin_unrelated_version_prefixes(self):
        protection = {'labels': [], 'sources': [],
                      'assets': ['Vita3K-v1.3.3-win11merge.14-diagnostics.zip']}
        for label, expected in [('v1.3.3-win11merge.1', False),
                                ('v1.3.3-win11merge.14', True)]:
            with self.subTest(label=label):
                archive = retention.inspect_archive(self.archive(label, 100))
                self.assertEqual(retention.protected(archive, protection), expected)


if __name__ == '__main__':
    unittest.main()
