"""Split one optimized EXE/PDB build into runtime and matching diagnostics ZIPs."""
import hashlib
import json
from pathlib import Path
import zipfile


def package_downloads(package: Path, destination: Path, label: str):
    destination.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((package / 'provenance.json').read_text())
    if manifest['buildLabel'] != label:
        raise ValueError('Package label mismatch')
    files = sorted(p for p in package.rglob('*') if p.is_file())
    for record in manifest['files']:
        path = package / record['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']:
            raise ValueError(f"Payload hash mismatch: {record['path']}")
    downloads = []
    for kind in ('runtime', 'diagnostics'):
        path = destination / f'Vita3K-{label}-{kind}.zip'
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for file in files:
                relative = file.relative_to(package)
                diagnostic = file.suffix.lower() == '.pdb' or relative.parts[0] == 'build-evidence'
                if (diagnostic == (kind == 'diagnostics')) or relative.as_posix() == 'provenance.json':
                    archive.write(file, relative)
        downloads.append({'kind': kind, 'file': path.name, 'bytes': path.stat().st_size,
                          'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    (destination / 'downloads.json').write_text(json.dumps({
        'buildLabel': label, 'sourceCommit': manifest['sourceCommit'],
        'recipeCommit': manifest['recipeCommit'], 'exeIdentity': manifest['exeIdentity'],
        'pdbIdentity': manifest['pdbIdentity'], 'downloads': downloads,
        'note': 'Identical optimized EXE; extract matching diagnostics beside runtime for symbols. No unoptimized Debug build.'
    }, indent=2) + '\n')
    return downloads
