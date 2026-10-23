"""Apply the source-pinned optional Dynarmic diagnostic patch, failing closed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def apply(source: Path):
    directory = source / 'tools/dependency-patches'
    manifest = directory / 'manifest.json'
    if not manifest.exists():
        return
    data = json.loads(manifest.read_text(encoding='utf-8'))
    if set(data) != {'schema', 'dynarmic'} or data['schema'] != 1:
        raise ValueError('Unexpected dependency patch manifest')
    entry = data['dynarmic']
    if set(entry) != {'baseCommit', 'file', 'sha256'} or entry['file'] != 'dynarmic-jit-observer.patch':
        raise ValueError('Unexpected Dynarmic patch descriptor')
    if not re.fullmatch('[0-9a-f]{40}', entry['baseCommit']) or not re.fullmatch('[0-9a-f]{64}', entry['sha256']):
        raise ValueError('Invalid dependency provenance')
    patch = directory / entry['file']
    if hashlib.sha256(patch.read_bytes()).hexdigest() != entry['sha256']:
        raise ValueError('Dynarmic source patch hash mismatch')
    dependency = source / 'external/dynarmic'
    def git(*args):
        return subprocess.check_output(['git', '-C', str(dependency), *args], text=True).strip()
    if git('rev-parse', 'HEAD') != entry['baseCommit'] or git('status', '--porcelain'):
        raise ValueError('Dynarmic base must match the clean pinned commit')
    subprocess.run(['git', '-C', str(dependency), 'apply', '--check', str(patch.resolve())], check=True)
    subprocess.run(['git', '-C', str(dependency), 'apply', str(patch.resolve())], check=True)
    print(json.dumps({'dependency': 'dynarmic', **entry}))


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path, required=True)
    apply(p.parse_args().source)
