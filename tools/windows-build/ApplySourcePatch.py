"""Apply a source support patch, optionally accepting its exact existing result."""
import argparse
from pathlib import Path
import subprocess


def apply(source: Path, patch: Path, allow_applied: bool = False):
    command = ['git', '-C', str(source), 'apply']
    patch = str(patch.resolve())
    check = subprocess.run([*command, '--check', patch], capture_output=True)
    if check.returncode == 0:
        subprocess.run([*command, patch], check=True)
        return 'applied'
    if allow_applied:
        reverse = subprocess.run([*command, '--reverse', '--check', patch], capture_output=True)
        if reverse.returncode == 0:
            return 'already-applied'
    raise RuntimeError('Source support patch does not match: ' + patch)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--patch', type=Path, required=True)
    parser.add_argument('--allow-applied', action='store_true')
    args = parser.parse_args()
    print(apply(args.source, args.patch, args.allow_applied))
