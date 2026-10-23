# Dedicated Windows release builds

The manual `Windows release package (self-hosted)` workflow uses the dedicated Windows Server 2022 runner. It has no push or pull-request trigger. Only the default branch can dispatch this job. Diagnostic iterations use the local workstation and retain its build directory.

Run its default readiness check to verify MSVC, SDK linking, executable execution, Qt and matching EXE/PDB identity under the runner service account. This does not build Vita3K.

For a release package, disable `preflight_only` and supply an exact reviewed fork commit and explicit build label. The current support patches target the pinned Plus-derived source; unsupported sources fail patch application. Patches supply the missing fork metadata header/bounded LibAtrac9 API, bound ATRAC9 input, label the separate build and disable upstream updater eligibility. The corrected NGS caller must already be present in the selected source. Build/test output includes full symbols, source/dependency diffs and a file-hash manifest. Packages expire after three days; publishing a GitHub release remains a separate action.

Runner tools are located through `VITA3K_TOOLCHAIN_ROOT`: VS2022 with C++/CMake/Windows 11 SDK, Git, Python313, Qt/6.11.0/msvc2022_64 and vcpkg pinned to `77df67cfff9c12ccfdb52284e07c87c75092f723`. Build output must reside on native Windows storage. MSVC archive writing and Qt locking failed when output was on the WSL share. Source may remain in the WSL checkout for local diagnostic builds.

The runner uses Network Service, not the administrator account. Build caches persist on the server. Existing upstream workflows remain disabled; external pull requests do not run jobs on this machine. Private handoffs, machine credentials, game files, saves and raw traces are excluded from this public recipe.
