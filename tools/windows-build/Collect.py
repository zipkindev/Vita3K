"""Collect a tested Windows release package with exact source and PDB provenance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from verify_pe_pdb import pe_identity, pdb_identity
p=argparse.ArgumentParser()
for name in ('source','build','evidence'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--label',required=True)
a=p.parse_args()
out=Path(os.environ['GITHUB_WORKSPACE'])/'windows-package'
if out.exists():raise SystemExit('Package destination already exists')
shutil.copytree(a.build/'bin/RelWithDebInfo',out)
if not (out/'Vita3K.pdb').exists():
 candidates=list(a.build.rglob('Vita3K.pdb'))
 if len(candidates)!=1:raise SystemExit('Missing or ambiguous linked PDB')
 shutil.copy2(candidates[0],out/'Vita3K.pdb')
ei=pe_identity((out/'Vita3K.exe').read_bytes());pi=pdb_identity((out/'Vita3K.pdb').read_bytes())
if any(ei[k]!=pi[k] for k in ('guid','age')):raise SystemExit('EXE/PDB mismatch')
for filename in ('README.md', 'COPYING.txt'):
 if (a.source/filename).exists():shutil.copy2(a.source/filename,out/filename)
notes=a.source/'docs/releases'/(a.label+'.md')
if notes.exists():shutil.copy2(notes,out/'RELEASE_NOTES.md')
shutil.copytree(a.evidence,out/'build-evidence')
shutil.copy2(a.build/'CMakeCache.txt',out/'build-evidence/CMakeCache.txt')
shutil.copy2(a.build/'CMakeFiles/impl-RelWithDebInfo.ninja',out/'build-evidence/impl-RelWithDebInfo.ninja')
def sha(f):
 h=hashlib.sha256()
 with f.open('rb') as stream:
  for b in iter(lambda:stream.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
head=subprocess.check_output(['git','-C',str(a.source),'rev-parse','HEAD'],text=True).strip()
manifest={'buildLabel':a.label,'sourceRepository':os.environ['GITHUB_SERVER_URL']+'/'+os.environ['GITHUB_REPOSITORY'],'sourceCommit':head,'recipeCommit':os.environ['GITHUB_SHA'],'runId':os.environ['GITHUB_RUN_ID'],'vcpkgCommit':'77df67cfff9c12ccfdb52284e07c87c75092f723','qtVersion':'6.11.0','configuration':'RelWithDebInfo','lto':'ALWAYS','exeIdentity':ei,'pdbIdentity':pi,'sourceMatch':'Separately sourced Plus baseline with included support patches; published binary/source match unverified','files':[{'path':str(f.relative_to(out)).replace('\\','/'),'sha256':sha(f)} for f in sorted(out.rglob('*')) if f.is_file()]}
(out/'provenance.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'package':str(out),'sourceCommit':head,'exeSha256':sha(out/'Vita3K.exe'),'pdbSha256':sha(out/'Vita3K.pdb'),'pdbIdentity':pi},indent=2))

from PackageDownloads import package_downloads
print(json.dumps(package_downloads(out,Path(os.environ['GITHUB_WORKSPACE'])/'windows-downloads',a.label),indent=2))
