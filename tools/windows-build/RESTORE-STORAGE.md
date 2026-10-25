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

Retention remains scheduled daily at 03:30 Pacific / 06:30 Eastern with the
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
alongside the staged merged source and `deployment.json`. It contains the prior
scripts, task definitions, protection ledger and encrypted credential config;
the deployment directory is restricted to SYSTEM and administrators. Credentials,
protection-ledger contents and raw machine evidence remain outside this public
repository. The encrypted credential backup remains tied to the original
Windows machine; replacement-host recovery still requires SMB credentials.

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
