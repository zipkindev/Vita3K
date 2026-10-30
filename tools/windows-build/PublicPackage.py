"""Create a public payload without publishing private build-machine evidence."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil

from verify_pe_pdb import pdb_identity, pe_identity


EVIDENCE = frozenset({
    'source.patch', 'libatrac9.patch', 'dynarmic.patch',
    'dependency-patches.json', 'submodules.txt',
})
PRIVATE_TEXT = re.compile(
    r'(?i)(?:[A-Z]:[\\/](?:Users|Windows[\\/]ServiceProfiles)[\\/]|/(?:home|Users)/[^/\s]+/'
    r'|\\\\[\w.$-]+\\|\b(?:192\.168(?:\.\d{1,3}){2}'
    r'|10(?:\.\d{1,3}){3}|172\.(?:1[6-9]|2\d|3[01])(?:\.\d{1,3}){2})\b'
    r'|-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----'
    r'|\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,})\b)')


def read_text(path):
    data = Path(path).read_bytes()
    return data.decode('utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig')


def relative_path(value):
    path = PurePosixPath(value.replace('\\', '/'))
    if path.is_absolute() or ':' in str(path) or '..' in path.parts or not path.parts:
        raise ValueError('Invalid package-relative path')
    return path


def public_text(text, name):
    if PRIVATE_TEXT.search(text):
        raise ValueError('Private metadata in public payload: ' + name)
    return text


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def public_identity(identity):
    # The EXE's actual RSDS record is checked independently. Do not repeat its
    # original absolute build path in public JSON.
    return {'guid': identity['guid'], 'age': identity['age']}


def validate_symbol_privacy(path):
    """Reject personal/network paths in newly built full symbols.

    Qt's distributed static debug data uses its vendor build account. That
    published third-party prefix is not a runner/user identity.
    """
    data = path.read_bytes()
    for text in (data.decode('latin-1'), data.decode('utf-16le', errors='ignore')):
        text = re.sub(r'(?i)C:[\\/]Users[\\/]qt[\\/]work[\\/]', 'qt-vendor/', text)
        public_text(text, 'full symbols')


def prepare_public_package(package, destination, *, allow_legacy_symbol_paths=False):
    package, destination = Path(package), Path(destination)
    original = json.loads(read_text(package / 'provenance.json'))
    records = {}
    for entry in original['files']:
        relative = relative_path(entry['path'])
        if str(relative) in ('provenance.json', 'build-evidence/public-summary.json'):
            raise ValueError('Reserved package manifest path')
        public_text(str(relative), 'manifest path')
        if str(relative).casefold() in {key.casefold() for key in records}:
            raise ValueError('Duplicate package manifest path')
        path = package / relative
        parents = [package.joinpath(*relative.parts[:i]) for i in range(len(relative.parts))]
        if (any(part.is_symlink() for part in (path, *parents))
                or not path.is_file() or sha(path) != entry['sha256'].lower()):
            raise ValueError('Private package manifest mismatch: ' + str(relative))
        records[str(relative)] = path
    if not {'Vita3K.exe', 'Vita3K.pdb'} <= records.keys():
        raise ValueError('Package lacks the executable and matching symbols')
    exe = pe_identity(records['Vita3K.exe'].read_bytes())
    pdb = pdb_identity(records['Vita3K.pdb'].read_bytes())
    if any(exe[key] != pdb[key] for key in ('guid', 'age')):
        raise ValueError('Private EXE/PDB mismatch')
    if not allow_legacy_symbol_paths and exe['pdbPath'] != 'Vita3K.pdb':
        raise ValueError('New executable must use a basename-only PDB reference')
    destination.mkdir(parents=True, exist_ok=False)
    for name, source in records.items():
        relative = PurePosixPath(name)
        if relative.parts[0] == 'build-evidence' and (
                len(relative.parts) != 2 or relative.name not in EVIDENCE):
            continue
        if (source.suffix.lower() in ('.log', '.ninja', '.py', '.ps1', '.bat', '.cmd')
                or source.name.lower() == 'cmakecache.txt'):
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.suffix.lower() == '.pdb':
            if not allow_legacy_symbol_paths:
                validate_symbol_privacy(source)
            shutil.copy2(source, target)
        elif relative.parts[0] == 'build-evidence':
            target.write_text(public_text(read_text(source), name), encoding='utf-8', newline='\n')
        else:
            if source.suffix.lower() in ('.txt', '.md', '.json', '.ini', '.conf', '.xml', '.ps1', '.py'):
                public_text(read_text(source), name)
            shutil.copy2(source, target)
    summary = {
        'schema': 1,
        'symbols': 'Original full PDB, unchanged, with matching GUID and age.',
        'evidence': 'Source/dependency patches and pinned dependencies; raw machine logs and build configuration retained privately.',
    }
    tests = package / 'build-evidence/tests.log'
    if tests.exists():
        text = read_text(tests)
        match = re.search(r'100% tests passed(?:, 0 tests failed)? out of (\d+)', text)
        if not match:
            raise ValueError('Missing passing test summary')
        summary['testsPassed'] = int(match[1])
    evidence = destination / 'build-evidence'
    evidence.mkdir(exist_ok=True)
    (evidence / 'public-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    manifest = {key: original[key] for key in (
        'buildLabel', 'sourceRepository', 'sourceCommit', 'recipeCommit', 'runId',
        'vcpkgCommit', 'qtVersion', 'configuration', 'lto', 'sourceMatch',
    ) if key in original}
    manifest.update({'exeIdentity': public_identity(exe), 'pdbIdentity': public_identity(pdb),
                     'symbolPolicy': 'full-symbols', 'privacySchema': 1})
    public_text(json.dumps(manifest), 'provenance.json')
    manifest['files'] = [{'path': str(path.relative_to(destination)).replace('\\', '/'),
                          'sha256': sha(path)}
                         for path in sorted(destination.rglob('*')) if path.is_file()]
    (destination / 'provenance.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if any(sha(destination / name) != sha(records[name]) for name in ('Vita3K.exe', 'Vita3K.pdb')):
        raise ValueError('Released executable or full symbols changed')
    return manifest
