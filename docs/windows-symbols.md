# Using the Windows symbol package

Vita3K-Go produces one optimized executable and two downloads from the same build:

| Download | Contents | Needed to play? |
|---|---|---|
| `Vita3K-<release>-runtime.zip` | EXE, runtime DLLs/plugins, README and provenance | Yes |
| `Vita3K-<release>-diagnostics.zip` | Matching `Vita3K.pdb`, build logs, tests, dependency patches and provenance | No; for debugging/profiling |

The PDB maps native addresses to functions, source locations and available types. It is not an installer, codec or game patch. Placing it beside the EXE does not turn on telemetry. The runtime remains optimized; symbols do not require switching to an unoptimized Debug executable. Optimization can inline/fold functions or remove local variables, so source stepping and variable inspection have limits. Microsoft explains [release symbols](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/debugging-with-symbols).

## Install and verify the pair

1. Download both archives from the **same release** and keep `downloads.json` as well. Check archive SHA-256 values against that file, using `Get-FileHash -Algorithm SHA256` in PowerShell.
2. Extract the runtime into its own installation folder. Extract diagnostics into the **same folder**, preserving its directory structure. `Vita3K.exe` and `Vita3K.pdb` should be siblings. Do not mix releases or overwrite your working installation to test a new candidate.
3. In PowerShell, change to that folder and run the following check. It compares the two files with the build manifest, not just their names:

```powershell
$manifest = Get-Content .\provenance.json -Raw | ConvertFrom-Json
foreach ($name in @('Vita3K.exe', 'Vita3K.pdb')) {
    $entry = @($manifest.files | Where-Object { $_.path -eq $name })
    if ($entry.Count -ne 1) { throw "Missing manifest record: $name" }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $name).Hash
    if ($actual -ine $entry[0].sha256) { throw "Wrong file for this build: $name" }
}
$manifest | Select-Object buildLabel, sourceCommit, recipeCommit, exeIdentity, pdbIdentity
```

The embedded EXE CodeView/RSDS GUID and age must match the PDB GUID and age. The manifest records those identities and the release pipeline checks the actual pair. **The same source commit or version label is not sufficient**: another compilation can generate a different pair. Keep original binaries and symbols for every captured trace or crash.

## Load symbols in WinDbg

Install Microsoft's [Windows debugging tools](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/debugger-download-tools). They are optional and are not bundled with the emulator. For crash analysis, open the dump in WinDbg and use commands like these, replacing the example installation path with your own:

```text
.symfix C:\Symbols
.sympath+ C:\Emulators\Vita3K-Go
.symopt- 0x40
.symopt+ 0x400
.reload /f Vita3K.exe
lmv m Vita3K
```

The local folder supplies this build's Vita3K PDB; Microsoft's symbol server supplies available Microsoft modules, not our private emulator symbols. `lmv m Vita3K` should show the correct module and loaded PDB/private symbols, not export-only symbols or a mismatch. The symbol options above disable loading arbitrary mismatched symbols and request exact matches. See Microsoft's [symbol path instructions](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/symbol-path).

For source browsing, check out the recorded `sourceCommit` and consult `build-evidence/source.patch`, the dependency patches and submodule list. A checkout alone omits build-recipe support patches; full source correspondence includes those patches. Build-machine paths embedded in a PDB are not paths that must exist on your computer. Provide your local source location to the debugger as needed.

Advanced CDB users can perform a noninvasive, nonsuspending live symbol check with `-pvr`, an explicit symbol directory and `qd` to detach. Wait for automatic texture preparation to finish first. Ordinary invasive attachment, breakpoints and stepping can pause the emulator; do not run them during a performance capture intended to represent uninterrupted play. Prefer postprocessing the saved trace or dump.

## Performance traces and telemetry

For WPA/ETW analysis, keep the exact EXE/PDB available and add their folder to the analyzer's symbol search path. Verify the recorded module identity before attributing samples. A PDB for Vita3K does not provide AMD driver internals, JIT-generated guest-code source mapping or symbols for every dependency. Missing third-party private symbols can limit attribution.

Optional texture, JIT and synchronization diagnostics are controlled separately in **Settings → Texture Decoding (Perf. Tweaks) → Diagnostics**. They default off, add measurement work and normally save reports on a clean game shutdown. A PDB alone neither starts recording nor improves frame rate.

## What to include in a report

Provide the release label, source/recipe commit, EXE hash, game/version, hardware/driver, settings and reproduction steps. State cache/preparation state and whether gameplay was continuous. Record the exact time window and report any pauses, deaths or transitions. Attach reviewed numeric summaries and symbol-resolution errors when useful. Dumps and raw traces can contain memory, paths or other personal/game data; review them before public sharing. Learned texture packs and game files are not diagnostic attachments.
