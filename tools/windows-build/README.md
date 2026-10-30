# Dedicated Windows release builds

See [restore storage](RESTORE-STORAGE.md) for recovery inputs, secure installation,
scheduled tasks, verification and retention controls.

Merged storage commit `71a5eb44` was deployed and reboot-verified on 2026-10-25.
The installer preserved credentials and release-protection rules; startup SMB
reconnect, Network Service archiving/local cleanup, production retention dry run,
isolated expiry tests and the post-reboot readiness workflow passed. See the
[deployment record](RESTORE-STORAGE.md#deployment-verification-2026-10-25) for
script hashes, test scope and rollback information.

The manual `Windows release package (self-hosted)` workflow uses the dedicated Windows Server 2022 runner. It has no push or pull-request trigger. Only the default branch can dispatch this job. Diagnostic iterations use the local workstation and retain its build directory.

Run its default readiness check to verify MSVC, SDK linking, executable execution, Qt and matching EXE/PDB identity under the runner service account. This does not build Vita3K.

For a release package, disable `preflight_only` and supply an exact reviewed fork commit and explicit build label. The current support patches target the pinned Plus-derived source; unsupported sources fail patch application. Patches supply the missing fork metadata header/bounded LibAtrac9 API, bound ATRAC9 input, label the separate build and disable upstream updater eligibility. The corrected NGS caller must already be present in the selected source. Build/test output includes full symbols, source/dependency diffs and a file-hash manifest. Packages expire after three days; publishing a GitHub release remains a separate action.

Runner tools are located through `VITA3K_TOOLCHAIN_ROOT`: VS2022 with C++/CMake/Windows 11 SDK, Git, Python313, Qt/6.11.0/msvc2022_64 and vcpkg pinned to `77df67cfff9c12ccfdb52284e07c87c75092f723`. Build output must reside on native Windows storage. MSVC archive writing and Qt locking failed when output was on the WSL share. Source may remain in the WSL checkout for local diagnostic builds.

The runner uses Network Service, not the administrator account. Build caches persist on the server. The inherited hosted Build CI workflow is disabled until its heavy Windows jobs are routed appropriately. Ordinary non-Windows checks remain available. This manual workflow does not run external pull requests on this machine. Private handoffs, machine credentials, game files, saves and raw traces are excluded from this public recipe.

`MaintainStorage.ps1` is the dedicated runner's post-job maintenance hook. Its
installed copy resides outside the checkout at
`C:\Vita3K-Build\maintenance\MaintainStorage.ps1`, referenced by
`ACTIONS_RUNNER_HOOK_JOB_COMPLETED` in the runner's `.env`. Restart the runner
service only while idle when changing that configuration. The hook runs after
workflow steps, including artifact upload, as Network Service.

The server has an SMB global mapping at `Y:`, restricted to SYSTEM, the runner
service and administrators. A reboot test showed that the SMB persistence flag
alone did not restore the mapping. The `Vita3K-Restore-TrueNAS-Archive` startup
task runs `RestoreArchive.ps1` as SYSTEM to reconnect with network retries.
Its installed script and machine-DPAPI-encrypted credential configuration reside
in `C:\ProgramData\Vita3KRunnerStorage`, accessible only to SYSTEM and
administrators. Credentials are provisioned separately; they do not belong in
this repository. Active compilation remains on native Windows storage.
The hook checks package provenance and every manifest hash, confirms the retained
EXE/PDB copies match the compiler output, copies packages/downloads/evidence into
an archive directory named by repository, build label and run ID, and verifies
every copied file by SHA-256. Only then does it remove `native-build`. Finished
local packages, source checkouts, toolchains and dependency caches remain.

The three historical workspace names are explicitly allowed to account for
repository renames. Unknown paths, reparse points, compiler activity, mismatched
packages and inaccessible archives prevent cleanup. Archive failures produce a
warning and retain local files. Failed builds without a finished package retain
their intermediates for diagnosis. Archive retention is independent of GitHub's
three-day artifact expiration.

`RetainArchives.py` handles NAS retention separately. The dedicated server runs
it as SYSTEM through `Vita3K-Archive-Retention`, daily at 03:30 server time, with
catch-up after downtime and three retries at 15-minute intervals. Its script,
release-protection ledger and last-run report live in the administrator/SYSTEM-only
directory `C:\ProgramData\Vita3KArchiveRetention`. The policy keeps unreleased
archives for 30 days and always keeps the newest five unreleased archives across
the historical repository names. Both conditions must expire before deletion.

Every run fetches all public releases, prereleases and tags from the canonical
GitHub repository. Release tags, their source commits and asset build labels
protect the entire archive, including its EXE, full PDB and evidence. The initial
ledger also includes authenticated draft-release records. Protection is sticky:
removing a release later does not remove an existing preservation rule. A `KEEP`
file in an archive pins it indefinitely. Experimental tags alone do not prevent
expiry. New unpublished drafts without a preservation rule should be pinned
with `KEEP` if they must outlive the ordinary unreleased-build policy.

An unavailable/incomplete GitHub inventory or invalid protection ledger aborts
cleanup. Unknown, unreadable or invalid
archives are retained; eligible archives must have consistent provenance,
matching recorded EXE/PDB identities and valid hashes for every packaged file.
Only complete expired build directories are removed; symbol files are never
pruned separately. The archiver and retention process share an exclusive NAS
lock to prevent copying and deletion from overlapping. The protected state
directory records the last-run report and a durable deletion journal. Run the Python tool
without `--apply` to inspect the policy without removing archives.
