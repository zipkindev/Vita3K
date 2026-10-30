# Dedicated Windows release builds

See [restore storage](RESTORE-STORAGE.md) for recovery inputs, secure installation,
scheduled tasks, verification and retention controls.

Merged storage commit `71a5eb44` was deployed and reboot-verified on 2026-10-25.
The installer preserved credentials and release-protection rules; startup SMB
reconnect, Network Service archiving/local cleanup, production retention dry run,
isolated expiry tests and the post-reboot readiness workflow passed. See the
[deployment record](RESTORE-STORAGE.md#deployment-verification-2026-10-25) for
script hashes, test scope and rollback information.

The latest full Windows build and privacy/storage verification passed on
2026-10-30 Eastern, using merged master `afaba9db` for source and recipe.
All 38 native tests passed; 130 downloaded public files were hash-verified;
the unchanged full PDB matched the EXE and resolved 27 native full-debug functions.
Privacy inspection found no personal-path or internal host/share markers, with
neutral build paths and the documented Qt vendor debug-path exception retained.
TrueNAS archiving verified 134 private manifest entries before local compiler
output was removed, leaving 42.74 GiB free after temporary verification cleanup.
See the [privacy build verification record](RESTORE-STORAGE.md#windows-privacy-build-verification-2026-10-30)
for the workflow, privacy-check correction, GUID/age, hashes and scope.

The manual `Windows release package (self-hosted)` workflow uses the dedicated Windows Server 2022 runner. It has no push or pull-request trigger. Only the default branch can dispatch this job. Diagnostic iterations use the local workstation and retain its build directory.

Run its default readiness check to verify MSVC, SDK linking, executable execution, Qt and matching EXE/PDB identity under the runner service account. This does not build Vita3K.

For a release package, disable `preflight_only` and supply an exact reviewed fork commit and explicit build label. The current support patches target the pinned Plus-derived source; unsupported sources fail patch application. Patches supply the missing fork metadata header/bounded LibAtrac9 API, bound ATRAC9 input, label the separate build and disable upstream updater eligibility. The corrected NGS caller must already be present in the selected source. Public downloads include unchanged full symbols, source/dependency diffs, a test-count summary and a file-hash manifest. Raw build/test output remains in the private archive. Packages expire after three days; publishing a GitHub release remains a separate action.

The ATRAC9 input-span patch accepts either the original source or the exact
already-patched code now in `master`. Other patch failures still stop the build.

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


## Public package privacy

`Collect.py` retains the complete `windows-package` and raw `build-evidence` for
private archiving. `PackageDownloads.py` creates a separate public payload through
`PublicPackage.py`; never upload the raw package or raw evidence directory.

The public evidence allowlist contains source/dependency patches, the dependency
patch manifest and pinned submodule status. A generated summary records the
passing native test count. CMake caches, Ninja files, compiler/configure/build/test
logs, local build scripts and other unrecognized evidence stay private. Public
provenance includes source/recipe commits and EXE/PDB GUID and age, without the
absolute PDB path. Every public payload hash and download checksum is regenerated.
Original source-match qualifications remain in provenance.

Full PDBs remain public and unchanged. Before a new package is published, its
manifest hashes and EXE/PDB identity must match, text must pass the privacy gate,
and full symbols must contain no detected personal-profile paths, home paths,
UNC paths, private IPv4 addresses or recognized key/token markers. Qt's published
vendor debug prefix is an explicit third-party exception. This gate catches
known metadata forms; it does not prove that arbitrary file contents contain no
sensitive information. Review the public payload before publishing.

The build sets linker temporary storage inside `native-build`, uses
`/PDBALTPATH:Vita3K.pdb`, and writes raw native tool output only to private files.
This keeps personal temporary paths out of new linker records and makes the
executable's debug reference portable. Source, build and toolchain roots should
use neutral local paths outside user profiles and network shares. GitHub may
still expose self-hosted runner metadata in workflow/job records independently
of package contents.

### Historical release sanitization, 2026-10-30

The seven Windows prereleases from `v1.3.3-win11diag.2` through
`v1.3.3-win11merge.46` receive evidence-only repackaging. Original assets
were backed up privately with verified SHA-256 hashes before any remote changes.
Runtime EXEs/DLLs and full PDBs are preserved byte-for-byte. Their GUID/age pairs
remain unchanged. Public ZIPs remove raw machine evidence, regenerate provenance
and checksums, and preserve the source/dependency patches and test summaries.
The legacy standalone local-build recipe uses explicit caller-supplied roots.

The owner explicitly chose to retain embedded paths in existing EXEs and full
PDBs. `allow_legacy_symbol_paths=True` supports only this reviewed historical
migration; normal release packaging cannot enable it. The original full debug
data stays public, including its historical embedded source/object paths. This
is an intentional exception, not a claim that those binary records were scrubbed.
Full private originals also remain available for recovery. Do not upload those
private backups or use them as new public evidence packages.


The native Windows packaging canary passed: an optimized MSVC LTCG executable
ran successfully; its basename-only RSDS reference and full PDB GUID/age matched;
packaging preserved EXE/PDB bytes and excluded a machine-specific cache canary.
Windows PowerShell parsed the updated build script successfully. All 30 Python
regression tests passed locally and in the PR's storage-test workflow.


The full-build symbol check distinguishes UNC roots from doubled separators in
quoted compiler records. Distributed FFmpeg debug records contain escaped local
paths; those separators do not identify a network share. Actual UNC roots,
including quoted/JSON values, NUL-delimited PDB records and compiler path options,
remain blocked along with personal-profile paths and private addresses.
