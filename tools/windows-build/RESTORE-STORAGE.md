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
failure may leave staged files; inspect the error before retrying. It never
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

The installed setup passed a reboot test, service-account SMB read/write and
EXE/PDB hash checks, disposable compiler-output cleanup, readiness workflow
`40166748225`, native scheduled retention and an exclusive-lock contention test.
The restore installer itself has been syntax-checked; it has not been replayed
over the healthy runner. Run the retention regression tests from this directory
with `python -m unittest test_archive_retention.py`.

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
