# Restore Windows runner storage

This procedure restores the installed post-job archiver, automatic SMB reconnect
and daily archive retention. It assumes the native toolchain and GitHub runner
are already installed. It does not register a runner, install compiler tools,
resize disks or change the runner service account.

## Recovery inputs

- A reviewed checkout containing `InstallStorage.ps1`, `MaintainStorage.ps1`,
  `RestoreArchive.ps1` and `RetainArchives.py` together.
- An idle runner at `C:\b\runner`, registered as a Windows service using
  `NT AUTHORITY\NETWORK SERVICE`, with workspaces under `C:\b\_work`.
- Python 3.13 at `C:\Vita3K-Build\tools\Python313\python.exe`.
- The private SMB archive UNC path and a credential allowed to read/write it.
- A backup of `C:\ProgramData\Vita3KArchiveRetention\protected-builds.json`.
  Its `labels`, `sources` and `assets` arrays preserve historical releases and
  known drafts. Keep this ledger outside the public repository. Do not replace
  a lost ledger with an empty file and assume historical protection is restored.

Back up the protection ledger after preservation rules change. For example,
from elevated PowerShell on the configured runner:

```powershell
New-Item -ItemType Directory -Path 'Y:\_runner-recovery' -Force | Out-Null
Copy-Item 'C:\ProgramData\Vita3KArchiveRetention\protected-builds.json' `
  'Y:\_runner-recovery\protected-builds.json' -Force
```

The retention tool only manages the three named repository directories;
`_runner-recovery` is outside its deletion scope. Keep another backup of the
ledger if protection must survive NAS/pool loss. A missing ledger can be rebuilt
from an authenticated GitHub release inventory including drafts, but deleted
releases and earlier manual preservation rules require the original backup.

## Installation

Copy the saved protection ledger to a private local restore directory. Run the
installer from an elevated Windows PowerShell console on the target runner:

```powershell
$archivePath = Read-Host 'TrueNAS archive UNC path'
$credential = Get-Credential -Message 'SMB archive credential'
.\tools\windows-build\InstallStorage.ps1 `
  -ArchivePath $archivePath `
  -SmbCredential $credential `
  -ProtectionSeedPath 'C:\Restore\protected-builds.json'
Remove-Variable credential
```

The password is prompted securely and is never passed on the command line or
written to the checkout. The installer encrypts it with Windows machine DPAPI.
The encrypted configuration and startup script are accessible only to SYSTEM
and administrators. DPAPI data from the old machine is not a portable recovery
credential: supply the SMB credential again after replacing Windows.

The installer refuses active builds, conflicting hooks and a `Y:` mapping to
another destination. It stops the idle runner service while updating its hook,
merges the supplied protection ledger with surviving local rules, restores the
archive mapping, and requires a successful retention dry run before registering
cleanup. It restarts the runner if it was previously running. A validation
failure may leave staged files and existing storage tasks disabled; inspect the
error and rerun the installer to validate and re-enable them. Existing triggers
are suspended and active task instances rejected before files or state change.
The installer removes unrelated explicit ACL grants from surviving directories
and files, and refuses installation through junctions. It never
silently replaces the archive with a different share or clears release rules.

Installed components:

| Component | Installed location / behavior |
| --- | --- |
| Completed-job hook | `C:\Vita3K-Build\maintenance\MaintainStorage.ps1`, referenced in `C:\b\runner\.env` |
| Startup reconnect | `Vita3K-Restore-TrueNAS-Archive`, runs as SYSTEM at boot, retries network readiness |
| Protected SMB config | `C:\ProgramData\Vita3KRunnerStorage\credential.json` |
| Archive retention | `Vita3K-Archive-Retention`, runs as SYSTEM daily at 03:30 in the server's timezone |
| Retention script/state/logs | `C:\ProgramData\Vita3KArchiveRetention` |
| SMB mapping | `Y:`, restricted to SYSTEM, administrators and Network Service |

The current server uses Pacific time, so 03:30 there is 06:30 Eastern. The
installer uses the target server's timezone and does not change it. Both tasks
catch up after downtime; reconnect retries three times at one-minute intervals
after script failure, and retention retries three times at 15-minute intervals.

## Verification

```powershell
Get-ScheduledTask -TaskName 'Vita3K-Restore-TrueNAS-Archive','Vita3K-Archive-Retention'
Get-SmbGlobalMapping -LocalPath 'Y:'
Get-Service -Name 'actions.runner*'
Get-Content 'C:\ProgramData\Vita3KRunnerStorage\restore-result.json'
& 'C:\Vita3K-Build\tools\Python313\python.exe' `
  'C:\ProgramData\Vita3KArchiveRetention\RetainArchives.py' `
  --state 'C:\ProgramData\Vita3KArchiveRetention'
```

The Python command is a dry run: it refreshes preservation state and reports
decisions without deleting archives. Verify read/write access as Network Service,
not only as the administrator. Run the existing Windows workflow with
`preflight_only=true` and inspect its `Complete runner` step for the hook.
Finally, reboot only while idle and check that `Y:` returns without manually
mapping it, the runner returns online automatically, and the startup receipt
contains the new boot time. The runner uses delayed automatic service startup.

The merged installer has been replayed successfully on the existing runner;
the deployment and reboot results are recorded below. Its seed validation,
junction guards and ACL reset also passed native Windows fixture tests; task
suspension and stale SMB reconnect have isolated mocked regression tests.
Run `Test-StorageSafety.ps1` as administrator for these Windows tests. They load
function definitions without executing the installer or changing live tasks or
SMB mappings. Run Python regressions from this directory with
`python -m unittest test_archive_retention.py`; GitHub also runs the Python
regressions for changes to these tools.

### Deployment verification: 2026-10-25

Deployed merged commit
[`71a5eb44daa966733f7ce0a86eeeb05046cfd996`](https://github.com/zipkindev/Vita3K-Go/commit/71a5eb44daa966733f7ce0a86eeeb05046cfd996)
through `InstallStorage.ps1` on the idle runner. The installed archiving,
startup-reconnect and retention scripts matched the merged source by SHA-256,
including after reboot. The existing SMB credential, archive destination and
every prior release-protection rule were preserved.

These results describe the October 25 verification, not a live health check.

| Check | Verified result |
| --- | --- |
| Installer | Completed successfully; both production storage tasks remained enabled as SYSTEM. |
| Automatic startup reconnect | Rebooted at 03:20:27 Eastern (07:20:27 UTC). The startup receipt matched the new boot and restored `Y:` on its first attempt without manual remapping. |
| Service-account SMB access | Network Service created, read and removed a disposable probe after reboot. |
| Archiving and local cleanup | Network Service archived and hash-verified a retained package with 133 manifest entries, produced a fresh verification receipt, and removed disposable compiler-output copies. |
| Production retention | A non-deleting SYSTEM dry run retained all four real archives. The production deletion task was not manually run during this deployment verification. |
| Expiry and preservation | The installed retention script ran as SYSTEM against an isolated NAS fixture root. It deleted one expired unreleased fixture, retained the newest five, preserved the protected executable/symbol pair, and recorded deletion-started/deleted journal entries. Disposable fixtures were then removed. |
| GitHub readiness | [Run 40172546737](https://github.com/zipkindev/Vita3K-Go/actions/runs/40172546737) passed after reboot and invoked the deployed hook in `Complete runner`. |
| Final health | Runner service and listener running, GitHub runner online and idle, both storage tasks enabled/ready, and approximately 44.4 GiB free on `C:`. |

At verification, retention was scheduled daily at 03:30 Pacific / 06:30 Eastern with the
30-day/newest-five policy. The archive write/delete exclusion lock had already
passed a contention test. All 17 Python regressions, native Windows fixture tests,
formatting checks and CodeQL passed before the deployment commit was merged.
The mocked edge-case tests do not establish that a stale-credential outage was
injected into the live SMB mapping.

Installed script hashes:

```text
MaintainStorage.ps1  1aef706e3e52730f459885a8d76f59f593d72391bcee0369dbc6b510a4d6ad36
RestoreArchive.ps1   a00075e2311a7b6231c5d2fc1a9d763a201c977ceacfc320913bc1f83604d3f0
RetainArchives.py    711bbb797ed8593c9d2fdf5b464d18650b1b5f80832b9d38f4366e08064cd533
```

A private rollback snapshot was saved under
`C:\ProgramData\Vita3KStorageDeployments\merged-<timestamp>-<id>\before`,
with sibling `source` and `deployment.json` entries in the deployment directory.
The `before` snapshot contains the prior
scripts, task definitions, protection ledger and encrypted credential config;
the deployment directory is restricted to SYSTEM and administrators. Credentials,
protection-ledger contents and raw machine evidence remain outside this public
repository. The encrypted credential backup remains tied to the original
Windows machine; replacement-host recovery still requires SMB credentials.

### Windows build verification: 2026-10-30 UTC

[Windows run 40996613182](https://github.com/zipkindev/Vita3K-Go/actions/runs/40996613182)
built merged source
[`f14b2e691181fcf3f8ae46b638259c9f0e4f1af0`](https://github.com/zipkindev/Vita3K-Go/commit/f14b2e691181fcf3f8ae46b638259c9f0e4f1af0)
using recipe
[`02f03d1ca79f46f5a643b358e3f184c5371097d6`](https://github.com/zipkindev/Vita3K-Go/commit/02f03d1ca79f46f5a643b358e3f184c5371097d6)
and label `master-storage-verify-20261030.2`. The recipe includes the
[PR #47 correction](https://github.com/zipkindev/Vita3K-Go/pull/47) that accepts
the exact ATRAC9 input-span fix already present in the source. The build used
the documented support patches and retained their diffs in its evidence.

These results were recorded at approximately 03:30–03:34 UTC on October 30
(23:30–23:34 Eastern on October 29) and describe the state at verification time.

| Check | Verified result |
| --- | --- |
| Build and tests | Optimized `RelWithDebInfo` build with full symbols completed; all 38 Windows tests passed, including `atrac9-input-span`. |
| Matching symbols | The actual EXE and full PDB had GUID `156dded6-f0c1-4c88-b6cd-d0211ca1daed`, age `1`, in both the TrueNAS package and downloaded GitHub artifact. |
| TrueNAS archiving | The installed completion hook ran as Network Service, copied the package, downloads and build evidence, and wrote a verified archive receipt at 03:30:39 UTC. Independent Network Service verification checked all 134 manifest files and the runtime/diagnostics ZIP payloads. |
| Uploaded artifact | Downloaded the GitHub artifact and independently checked ZIP hashes, all 134 payload files, and the EXE/PDB identity. The runtime ZIP contained the EXE; the diagnostics ZIP contained its matching PDB and build evidence. |
| Automatic local cleanup | The completion hook removed `native-build` after archive verification. Compiler output was observed at 8.91 GiB before cleanup; local preserved package copies still matched the archived EXE/PDB hashes afterward. |
| Retention | A non-deleting SYSTEM dry run evaluated eight archives under the 30-day/newest-five policy, retained this build among the newest unreleased builds, and preserved every prior release-protection rule. No archives were deleted during this check. |
| Final health | `C:` had 41.99 GiB free; the runner service was running, GitHub reported it online/idle, `Y:` was healthy, and both production storage tasks were enabled/ready as SYSTEM with last result `0`. Temporary verification tasks and fixtures were removed. |

The archive directory relative to the configured TrueNAS root is
`Vita3K-Go\master-storage-verify-20261030.2-40996613182`; it contains
`archive-verified.json`, `windows-package`, `windows-downloads` and `build-evidence`.
GitHub downloads retain the workflow's three-day expiration; NAS retention follows
the separate policy above.

Verified binary hashes:

```text
Vita3K.exe  d1e3711d259ffbaa9a5b26354bb975766226c74ac50497eebf281f8569295874
Vita3K.pdb  3b92b787ece2638565dc1451767f308785270fe95ab8c0f71d09958fe552a100
```

The installed maintenance hook matched the merged Git object's SHA-256 recorded
above. Its Windows checkout copy used CRLF line endings; normalized script
contents matched. Raw machine evidence and protection-ledger contents remain
outside this public repository.

### Windows privacy build verification: 2026-10-30

[Windows run 41071935435](https://github.com/zipkindev/Vita3K-Go/actions/runs/41071935435) succeeded using merged
[`afaba9db9749d1b671bc0a0af3d5f2466cefac6b`](https://github.com/zipkindev/Vita3K-Go/commit/afaba9db9749d1b671bc0a0af3d5f2466cefac6b)
for both source and recipe, with label `master-privacy-verify-20261030.2`.
The job ran from 10:29:26 to 10:43:03 Eastern on October 30. These are dated build and post-build verification results.

The first full privacy-verification build passed all 38 native tests but blocked
public packaging because escaped local paths in FFmpeg compiler records were
mistaken for UNC roots. [PR #50](https://github.com/zipkindev/Vita3K-Go/pull/50) corrected that distinction;
all 32 Python regressions, formatting and CodeQL checks passed before merge.
The fresh master run then passed the normal privacy gate and uploaded its
runtime and diagnostics ZIPs. Actual UNC shares, personal-profile paths and
private addresses remain blocked; full PDBs are preserved.

| Check | Verified result |
| --- | --- |
| Build and tests | Optimized `RelWithDebInfo`/LTO build succeeded; all 38 Windows tests passed. Source and packaging recipe both matched the merged commit above. |
| Matching full symbols | EXE and full PDB GUID `5896e5ab-cbd4-4a02-b82c-46e405cfb721`, age `1`. The 249,221,120-byte PDB was unchanged from the private original. Windows DbgHelp resolved 27 native full-debug functions with exact address lookups. |
| Uploaded packages | Downloaded the actual GitHub artifact and verified both ZIP hashes and every manifest hash across 130 public files, including provenance. Public EXE/PDB hashes matched the private local and archived originals byte-for-byte. |
| Privacy inspection | Scanned 41 public text files and 14 PNG metadata chunks, checked the full PDB, and searched payloads for known personal/network identifiers. No personal-path or internal host/share markers were detected. The executable's PDB reference was the basename `Vita3K.pdb`; public identities contained GUID and age only. |
| Public evidence | Only source/dependency patches, the dependency manifest, pinned submodules and `public-summary.json` were included. Raw machine logs, CMake caches, Ninja configuration and local evidence scripts were absent. |
| TrueNAS archiving | The completion hook copied and hash-verified the private package, downloads and raw evidence, with 134 private manifest entries and a verified archive receipt. Independent checks confirmed that archived EXE/PDB hashes matched the preserved build and downloaded public packages. |
| Automatic cleanup | The hook removed `native-build` after archive verification. `C:` had 42.64 GiB free afterward and 42.74 GiB after temporary candidate files were removed. The archive share remained reachable during the build. |

Full symbols retain neutral build paths and the documented Qt vendor debug-path
exception. The inspection found no user-specific paths or private host/share
identifiers. GitHub's
self-hosted runner/job metadata is separate from package contents. Raw machine
evidence and credentials remain private.

The archive directory relative to the configured TrueNAS root is
`Vita3K-Go\master-privacy-verify-20261030.2-41071935435`, with
`archive-verified.json`, `windows-package`, `windows-downloads` and `build-evidence`.
The GitHub artifact is
`windows-master-privacy-verify-20261030.2-41071935435`, subject to the workflow's
three-day expiration. NAS retention continues to use the 30-day/newest-five
policy and existing release-preservation rules. Startup reconnect and scheduled
retention verification remain documented in the earlier dated records.

Verified binary hashes:

```text
Vita3K.exe  f197ed61dfe22ba258fbc5abcbdaa0fccc207499383ca7daa77ae3b474d14c8e
Vita3K.pdb  54addac989d9eb6126e17d552bd2cee7715c837cb08ea94ccf4dbd2418e56b65
```

## Retention and recovery controls

Unreleased archives must be older than 30 days **and** outside the newest five
before deletion. Released builds, prereleases, known draft rules, matching release
source commits and asset build labels protect the whole EXE/PDB/evidence folder.
Create an empty `KEEP` file inside an archive to pin it indefinitely. New drafts
without an existing protection rule should be pinned this way if they must
outlive the unreleased retention window.

GitHub lookup failures, corrupt provenance, mismatched symbols and bad package
hashes prevent deletion. Archive copying and pruning use the same exclusive NAS
lock. The last report is `last-retention.json`, task output is `task.log`, and
actual deletion attempts are recorded in `deletion-journal.jsonl` under the
retention state directory. To suspend pruning while investigating:

```powershell
Disable-ScheduledTask -TaskName 'Vita3K-Archive-Retention'
```

Disabling this task does not disable SMB reconnect or post-build archiving.
Re-enable it with `Enable-ScheduledTask` after checking the protection ledger.
