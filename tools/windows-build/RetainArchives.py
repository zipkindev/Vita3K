"""Daily retention for verified Windows build archives. Defaults to a dry run."""
import argparse
from contextlib import contextmanager
import datetime as dt
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import urllib.request

REPOSITORY = 'zipkindev/Vita3K-Go'
ALIASES = ('Vita3K', 'Vita3K-ZipZ', 'Vita3K-Go')
UTC = dt.timezone.utc


def listing(resource):
    """Read every page; any incomplete/error response aborts retention."""
    result = []
    for page in range(1, 1001):
        url = f'https://api.github.com/repos/{REPOSITORY}/{resource}?per_page=100&page={page}'
        request = urllib.request.Request(url, headers={
            'Accept': 'application/vnd.github+json',
            'User-Agent': 'Vita3K-Runner-Archive-Retention',
        })
        with urllib.request.urlopen(request, timeout=30) as response:
            values = json.load(response)
        if not isinstance(values, list):
            raise ValueError('GitHub inventory was not a list')
        result.extend(values)
        if len(values) < 100:
            return result
    raise ValueError('GitHub inventory exceeded pagination limit')


def refresh_protection(previous, releases, tags):
    labels = set(previous.get('labels', []))
    sources = set(previous.get('sources', []))
    assets = set(previous.get('assets', []))
    tag_commits = {}
    for tag in tags:
        name, commit = tag['name'], tag['commit']['sha']
        if not isinstance(name, str) or not re.fullmatch('[0-9a-f]{40}', commit):
            raise ValueError('Invalid GitHub tag inventory')
        tag_commits[name] = commit
    for label in labels:
        if label in tag_commits:
            sources.add(tag_commits[label])
    for release in releases:
        label = release['tag_name']
        if not isinstance(label, str) or not label:
            raise ValueError('Invalid release identity')
        if not release.get('draft') and label not in tag_commits:
            raise ValueError('Published release missing from tag inventory; refusing cleanup')
        labels.add(label)
        if label in tag_commits:
            sources.add(tag_commits[label])
        assets.update(asset['name'] for asset in release['assets'])
        target = release.get('target_commitish', '')
        if re.fullmatch('[0-9a-f]{40}', target):
            sources.add(target)
    return {'labels': sorted(labels), 'sources': sorted(sources), 'assets': sorted(assets)}


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, path)


@contextmanager
def archive_lock(root):
    # On Windows, the open handle also excludes the PowerShell hook's FileShare.None.
    with (root / '.maintenance.lock').open('a+b') as stream:
        if os.name == 'nt':
            import msvcrt
            if stream.seek(0, 2) == 0:
                stream.write(b'0')
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def plain_tree(root):
    """Never follow SMB/NTFS links, junctions, or other reparse points."""
    for directory, dirs, files in os.walk(root, followlinks=False):
        for path in [Path(directory), *(Path(directory) / name for name in dirs + files)]:
            info = path.lstat()
            if path.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
                raise ValueError('Archive contains a link or reparse point')


def inspect_archive(folder):
    plain_tree(folder)
    package = folder / 'windows-package'
    manifest = json.loads((package / 'provenance.json').read_text(encoding='utf-8-sig'))
    receipt = json.loads((folder / 'archive-verified.json').read_text(encoding='utf-8-sig'))
    label, run, source = manifest['buildLabel'], str(manifest['runId']), manifest['sourceCommit']
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', label):
        raise ValueError('Invalid build label')
    if not re.fullmatch(r'\d+', run) or not re.fullmatch('[0-9a-f]{40}', source):
        raise ValueError('Invalid source/run identity')
    if folder.name != f'{label}-{run}':
        raise ValueError('Directory and package identity differ')
    if manifest.get('sourceRepository') not in [f'https://github.com/zipkindev/{name}' for name in ALIASES]:
        raise ValueError('Unknown source repository')
    if receipt.get('Verified') is not True or str(receipt.get('Run')) != run or receipt.get('Source') != source:
        raise ValueError('Missing or mismatched verified archive receipt')
    exe, pdb = manifest['exeIdentity'], manifest['pdbIdentity']
    if not exe.get('guid') or any(exe[key] != pdb[key] for key in ('guid', 'age')):
        raise ValueError('EXE/PDB identity mismatch')
    files = manifest['files']
    if not files or not {'Vita3K.exe', 'Vita3K.pdb'}.issubset({item['path'] for item in files}):
        raise ValueError('Manifest lacks the executable or its symbols')
    for item in files:
        relative = item['path'].replace('\\', '/')
        if relative.startswith('/') or ':' in relative or '..' in relative.split('/'):
            raise ValueError('Manifest path outside archive')
        if not re.fullmatch('[0-9a-fA-F]{64}', item['sha256']):
            raise ValueError('Invalid file hash')
    created = dt.datetime.fromisoformat(receipt['Utc'].replace('Z', '+00:00'))
    if created.tzinfo is None:
        raise ValueError('Archive timestamp lacks a timezone')
    return {'folder': folder, 'label': label, 'source': source, 'created': created, 'manifest': manifest}


def protected(archive, protection):
    return (archive['label'] in protection['labels'] or archive['source'] in protection['sources']
            or any(archive['label'] in asset for asset in protection['assets'])
            or (archive['folder'] / 'KEEP').exists())


def verify_hashes(archive):
    package = archive['folder'] / 'windows-package'
    for item in archive['manifest']['files']:
        path = package / item['path'].replace('\\', '/')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest.lower() != item['sha256'].lower():
            raise ValueError(f'Package hash mismatch: {item["path"]}')


def record_deletion(state, archive, event):
    record = {'utc': dt.datetime.now(UTC).isoformat(), 'event': event,
              'path': str(archive['folder']), 'label': archive['label'],
              'source': archive['source']}
    with (state / 'deletion-journal.jsonl').open('a', encoding='utf-8') as stream:
        stream.write(json.dumps(record) + '\n')
        stream.flush()
        os.fsync(stream.fileno())


def retention_plan(archives, protection, now, days, keep):
    unreleased = sorted((a for a in archives if not protected(a, protection)),
                        key=lambda a: (a['created'], str(a['folder'])), reverse=True)
    newest = {a['folder'] for a in unreleased[:keep]}
    result = []
    for archive in archives:
        if protected(archive, protection):
            reason = 'release/source/manual protection'
        elif archive['folder'] in newest:
            reason = 'newest unreleased builds'
        elif now - archive['created'] < dt.timedelta(days=days):
            reason = 'within retention window'
        else:
            reason = 'expired unreleased build'
        result.append((archive, reason == 'expired unreleased build', reason))
    return result


def run(args):
    # Protection is sticky: removed releases never silently unprotect builds.
    state_file = args.state / 'protected-builds.json'
    previous = json.loads(state_file.read_text(encoding='utf-8-sig'))
    releases, tags = listing('releases'), listing('tags')
    if not tags:
        raise ValueError('Empty tag inventory; refusing cleanup')
    protection = refresh_protection(previous, releases, tags)
    atomic_json(state_file, protection)
    return apply_policy(args, protection)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('Y:/'))
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--days', type=int, default=30)
    parser.add_argument('--keep', type=int, default=5)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    if args.days < 1 or args.keep < 1 or not args.root.is_dir() or not args.state.is_dir():
        raise ValueError('Invalid policy or unavailable archive/state directory')
    with archive_lock(args.root):
        run(args)


def apply_policy(args, protection):
    archives, retained_invalid = [], []
    for alias in ALIASES:
        parent = args.root / alias
        if not parent.exists():
            continue
        if parent.is_symlink() or getattr(parent.lstat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('Archive repository directory is a reparse point')
        for folder in parent.iterdir():
            if not folder.is_dir():
                continue
            try:
                archives.append(inspect_archive(folder))
            except (ValueError, KeyError, TypeError, OSError) as error:
                retained_invalid.append({'path': str(folder), 'action': 'retain', 'reason': str(error)})
    now = dt.datetime.now(UTC)
    report = {'utc': now.isoformat(), 'identity': getpass.getuser(), 'apply': args.apply, 'days': args.days,
              'keepNewestUnreleased': args.keep, 'results': retained_invalid}
    for archive, eligible, reason in retention_plan(archives, protection, now, args.days, args.keep):
        action = 'would-delete' if eligible else 'retain'
        if eligible:
            # Recheck identity, links, manual pins and hashes immediately before deletion.
            checked = inspect_archive(archive['folder'])
            if checked['manifest'] != archive['manifest'] or checked['created'] != archive['created']:
                raise ValueError('Archive changed during retention; refusing deletion')
            if protected(checked, protection):
                action, reason = 'retain', 'protection changed during retention'
            else:
                verify_hashes(checked)
                if args.apply:
                    record_deletion(args.state, checked, 'delete-started')
                    shutil.rmtree(checked['folder'])
                    record_deletion(args.state, checked, 'deleted')
                    action = 'deleted'
        report['results'].append({'path': str(archive['folder']), 'label': archive['label'],
                                  'action': action, 'reason': reason})
    atomic_json(args.state / 'last-retention.json', report)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
