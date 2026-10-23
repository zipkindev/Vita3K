# Optimized Windows diagnostics

The diagnostic build is an optimized Vita3K Plus-derived experiment with matching native symbols. It is separately sourced and does not establish an exact rebuild of the published Vita3K Plus package. See the release provenance and the [Ubermall comparison](ubermall-cache-20261023.md).

## Downloads and isolation

The runtime ZIP contains the executable, runtime dependencies and version/provenance files. The optional symbols/source ZIP contains the matching PDB, exact source support patches and pinned submodule list. Match the release label **and EXE hash** before using its PDB; another build from the same source will have a different identity. Keep the EXE/PDB pair together. No games, firmware, licenses, saves, personal traces or access credentials are distributed.

Extract into a separate directory and configure your own installation. Close an existing emulator normally before copying a consistent portable save/config snapshot. Preserve your original installation and frontend mapping. This experimental build disables upstream automatic-update eligibility so a different binary cannot silently replace the measured executable.

## Decoded PVRTC cache trial

The cache is disabled by default. First verify audio, graphics and controls with caching disabled. In a PowerShell terminal opened in the diagnostic runtime directory:

```powershell
$env:VITA3K_DIAG_PVRTC_CACHE_MB = '0'
.\Vita3K.exe
```

After normal closure, repeat the same event with caching enabled:

```powershell
$env:VITA3K_DIAG_PVRTC_CACHE_MB = '256'
.\Vita3K.exe
```

The variable belongs to this terminal and its children; no machine-wide setting is needed. Close the terminal or remove the variable afterward. The maximum accepted budget is 512 MiB. Budget accounting covers compressed source snapshots and decoded pixel payload, with additional container bookkeeping. First-time content still needs decoding. The cache is in memory and clears when the emulator exits. Current session counters are logged on the first request and every 1,024 requests as `PVRTC-DECODE`; their cumulative values are not automatically a per-race hit rate.

## Repeatable measurements

Use the same binary, graphics, process priority/affinity, map/event and ship. Record whether the run is cold or warm and whether audio works. Capture only active gameplay; label loading/results/menu transitions. Do not clear existing caches between an intended matched comparison. Repeat both conditions, since combat effects and content loads vary.

PresentMon can collect per-process present intervals; WPR CPU sampling and scheduling provide the corresponding native work. Record the exact PID, EXE/PDB hashes, graphics profile, cache budget and the shared raw QPC frequency/window. WPR stop/rundown extends beyond the present recording: compare the actual frame window when correlating CPU work. Preserve lost-event/truncation checks. Native symbols do not fully symbolize generated guest JIT code, and CPU-present intervals are not input/display latency.

The public `tools/windows-build/verify_pe_pdb.py` verifies PE/PDB GUID and age. Exact symbol checks should reject mismatched symbols; detach a debugger before a timing capture. Keep raw machine-wide ETLs private until reviewed; share sanitized aggregate metrics, relevant stacks and reproduction details.

## Maintenance model

Use one source line and optimized build settings, with symbols packaged separately and additional instrumentation explicitly enabled. The PDB is not a slow Debug configuration. Symbol generation increases build/storage/download work; active tracing and extra logging can affect timing. Keep instrumented and noninstrumented measurements labeled. Avoid a second long-lived diagnostic code fork that would need every emulator fix ported separately.

Release recipes live in `tools/windows-build` on the fork default branch. They accept an exact reviewed source commit and build label, apply the required missing dependency/diagnostic support patches, build optimized full symbols, run CTest and verify the EXE/PDB identity. Main releases use the dedicated Windows self-hosted runner. Diagnostic iteration can use the existing local native build directory. Each rebuild needs a new identity/provenance record even when the code change is small.
