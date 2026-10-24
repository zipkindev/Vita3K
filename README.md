# Vita3K-Go

Performance and diagnostic development focused on Windows on the **Lenovo Legion Go**. Other AMD handhelds, including Steam Deck and ROG Ally, are potential secondary targets; they are not validated by the Legion Go measurements. Memory budgets remain configurable. Platform compatibility and performance require device-specific testing.

## Attribution and origins

This project builds on the work of several communities:

- [Vita3K](https://github.com/Vita3K/Vita3K): the original emulator and primary upstream reference. This repository remains in its GitHub fork network.
- [Vita3K-Plus](https://github.com/nckstwrt/Vita3K-Plus/tree/all-enhancements): the source baseline for this experimental line, including its scheduling, compatibility and frontend enhancements.
- [Vita3K-nx](https://github.com/NaGaa95/Vita3K-nx): selected applicable shared-code correctness fixes. Switch-specific changes are not assumed useful on Windows.
- Our additions: measured PVRTC decoded-texture caching, bounded parallel decoding, learned and automatic per-game texture preparation, lightweight telemetry, optimized symbol-enabled diagnostic packages and handheld testing.

We incorporate selected, reviewed changes rather than automatically merging entire fork histories. [Curated integration provenance](docs/curated-candidate-1.md) records the initial mainline NGS handle-validation import (`e9225f90f04cdf0d6d7c6fe617c0c70a5a0af84a`) and nx pipeline-cache read validation (`1881b9adb608bbb7e532c2d66e3a12aa0e97ae9e`). Original authorship, copyright and license notices are preserved. We claim our additions, not authorship of the upstream emulator or fork work.

[Diagnostic downloads](https://github.com/zipkindev/Vita3K-Go/releases) · [Usage and exact-symbol provenance](docs/windows-diagnostics.md) · [Automatic textures](docs/automatic-textures.md)

The candidate branch includes the Plus-derived patch line and selected imports. The default branch carries build infrastructure on its own source history; release manifests identify the exact candidate commit, support patches, submodules, recipe, toolchain and EXE/PDB hashes. This is experimental software, not an official upstream release. Historical measurements apply only to their documented build/device/settings.

## Automatic texture preparation

This experimental feature reduces repeated CPU decoding work for eligible PVRTC I/II textures. Enable **Automatic Texture Preparation** in **Settings → Performance** and select a nonzero **Decoded Texture Cache** budget globally or per game. The first session learns as you play; close the game normally to save its bounded profile. Later launches prepare compatible learned textures before guest execution and continue learning during normal use. No manual profile selection or race-specific setup is required.

RAM and disk limits are separate: decoded textures stay within the selected RAM budget (up to512MiB), while saved profiles contain compressed inputs, limited to64MiB per game and1GiB total. Least recently used saved profiles are pruned as space is needed. **Clear This Game's Texture Profile** and **Clear All Saved Texture Profiles** are available when no game is active; saves, game files and shader caches are preserved. Missing, changed or incompatible content falls back to ordinary decoding. See [behavior, limits and diagnostics](docs/automatic-textures.md).

The `.13` learning candidate, based on `.11`, retains a bounded compressed-input history after decoded-cache eviction and merges valid previous profiles. It uses up to64MiB additional RAM plus bounded metadata while learning; it does not increase the decoded cache budget. Merged archives may exceed what can be prepared in one launch, so preparation selects fitting entries within the existing budget/deadline. The `.12` retention-order experiment is excluded. See [release details and limitations](docs/releases/v1.3.3-win11merge.13.md); WipEout gameplay validation is pending.

On a Legion Go, the earlier `.5` prototype prepared3151learned WipEout textures in2.23s using about314MiB of the512MiB RAM budget. In one owner-confirmed fresh-process Chenghou Rapier pair, recorded intervals over50ms were6Off versus0Preload, maximum interval161.75ms versus36.62ms, and demand decodes1776versus2 over about89s of complete telemetry. Both90s races retained music/controls and the same EXE/PDB/settings. This was one sequential pair with differing effects/traffic; recording followed driving confirmation and can omit initial race seconds. It supports further testing, not universal speedup or input-latency claims. The newer automatic lifecycle, UI and storage controls require their own gameplay validation.

Preparation is CPU-side, using at most one decoder helper plus the caller, with no background preparation left competing during gameplay. It adds startup work, uses bounded scratch memory beyond the RAM payload budget, and does not preload GPU resources or warm CPU JIT code. It cannot predict truly unseen textures or fix unrelated GPU/CPU bottlenecks. Crashes do not save a new learned profile; normal exit does. Game-derived profile data stays local and must not be uploaded with bug reports.

## Performance controls

**Settings → Performance** (also available per game) groups decoded texture RAM budgets, parallel decoding, Automatic Texture Preparation and saved-profile clearing. Short descriptions explain the benefit and cost of each feature. Parallel decoding uses the caller and at most one helper; its checkbox selects serial decoding for comparison without changing the cache budget. The larger 16,384-entry limit is internal and still bounded by the chosen RAM budget. FPS Hack remains separately under GPU settings.

Optional texture, JIT and synchronization telemetry switches are in the same tab under **Diagnostics**. They default off and are measurement tools, not speedups or JIT prewarming. Settings persist globally/per game and require a new game launch. Normal closure saves reports under the portable cache's `diagnostics` directory. Each launch creates new reports; delete old diagnostic sessions when no longer needed. Texture counters are sampled once per second for at most four hours; JIT and wait reports use bounded session summaries and rolling detail. Diagnostic launchers can override output locations through documented environment variables. Reports should stay private. See [synchronization telemetry](docs/synchronization-telemetry.md).

## Releases and diagnostics

Each approved build produces a **runtime download** and a **matching diagnostics/symbols download** from the same optimized executable. Symbols are stored in a separate PDB; the runtime package excludes them. This is optimized RelWithDebInfo with full symbols, not an unoptimized Debug build. Use only the PDB whose GUID/age and EXE hash match your build. Per-release notes identify features, fixes, measured improvements, validation, limitations and exact provenance. Diagnostic candidates remain experimental until gameplay validation; see [release notes](docs/releases/v1.3.3-win11merge.6.md).

Original upstream documentation follows.

# Vita3K

[![C/C++ CI](https://github.com/Vita3K/Vita3K/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/Vita3K/Vita3K/actions/workflows/c-cpp.yml)
[![Release](https://img.shields.io/github/v/release/Vita3K/Vita3K-builds?include_prereleases)](https://github.com/Vita3K/Vita3K/releases)
[![Vita3K discord server](https://img.shields.io/discord/408916678911459329?color=5865F2&label=Vita3K%20&logo=discord&logoColor=white)](https://discord.gg/6aGwQzh)

## Introduction

Vita3K is an experimental PlayStation Vita emulator for Windows, Linux, macOS and Android.

* [Website](https://vita3k.org/) (information for users)
* [Wiki](https://github.com/Vita3K/Vita3K/wiki) (information for developers)
* [Discord server](https://discord.gg/MaWhJVH) (recommended)

## Compatibility

The emulator currently runs most homebrew programs and commercial games.

- [Homebrew compatibility page](https://vita3k.org/compatibility-homebrew.html)
- [Commercial compatibility page](https://vita3k.org/compatibility.html)

## Gallery

|               **Persona 4 Golden** by Atlus                   |                     **A Rose in the Twilight** by Nippon Ichi Software                         |
| :-----------------------------------------------------------: | :--------------------------------------------------------------------------------------------: |
| ![Persona 4 Golden screenshot](./_readme/screenshots/P4G.png) | ![A Rose in the Twilight screenshot](./_readme/screenshots/A%20Rose%20in%20the%20Twilight.png) |

|                  **Alone with You** by Benjamin Rivers                     |                 **VA-11 HALL-A** by Sukeban Games                    |
| :------------------------------------------------------------------------: | :------------------------------------------------------------------: |
| ![Alone with You screenshot](./_readme/screenshots/Alone%20With%20You.png) | ![VA-11 HALL-A screenshot](./_readme/screenshots/VA-11%20HALL-A.png) |

|              **Fruit Ninja** by Halfbrick Studios                  |                **Jetpack Joyride** by Halfbrick Studios                    |
| :----------------------------------------------------------------: | :------------------------------------------------------------------------: |
| ![Fruit Ninja Screenshot](./_readme/screenshots/Fruit%20Ninja.png) | ![Jetpack Joyride Screenshot](./_readme/screenshots/Jetpack%20Joyride.png) |

## License

Vita3K is licensed under the **GPLv2** license. This is largely dictated by external dependencies, most notably Unicorn.

## Downloads

You can download the latest builds from [here](https://github.com/Vita3K/Vita3K/releases/tag/continuous).

* Windows
  * Requirements:
    * [Microsoft Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe)
* Linux
  * Arch based:
    * [vita3k-bin](https://aur.archlinux.org/packages/vita3k-bin)<sup><small>AUR</small></sup>
    * [vita3k-git](https://aur.archlinux.org/packages/vita3k-git)<sup><small>AUR</small></sup>
  * Requirements:
    * xdg-desktop-portal
* Android
    * [Adreno drivers](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/)
* Others
  * [Download Artifact](https://github.com/Vita3K/Vita3K/actions?query=event%3Apush+is%3Asuccess+branch%3Amaster)
  * [Old builds](https://github.com/Vita3K/Vita3K-builds/releases)

## Building

Please see [`building.md`](./building.md).

## Running
Check our [quickstart guide](https://vita3k.org/quickstart) to make sure your computer meets the minimum requirements to run Vita3K.  
Don't forget to have your graphics driver up to date and to install the [Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/VC_redist.x64.exe) if you are a Windows user.  

## Bugs and issues
The project is in an early stage, so please be mindful when opening new issues. Expect crashes, glitches, low compatibility and poor performance.

## Thanks
Thanks go out to people who offered advice or otherwise made this project possible, such as Davee, korruptor, Rinnegatamante, ScHlAuChi, Simon Kilroy, TheFlow, xerpi, xyz, Yifan Lu and many others.

## Donations
[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/vita3k)
<br>
Thank you to the supporters and to all those who support us on our [ko-fi](https://ko-fi.com/vita3K).
* Among them, those who subscribed to the Nibble Tier and upper: **j0hnnybrav0, Mored4u, TacoOblivion, Undeadbob and uplush**

## Note
The purpose of this emulator is not to enable illegal activity. You can dump games from a Vita by using [NoNpDrm](https://github.com/TheOfficialFloW/NoNpDrm) or [FAGDec](https://github.com/CelesteBlue-dev/PSVita-RE-tools/tree/master/FAGDec/build). You can get homebrew programs from [VitaDB](https://www.rinnegatamante.eu/vitadb/#/).

PlayStation, PlayStation Vita and PlayStation Network are all registered trademarks of Sony Interactive Entertainment Inc. This emulator is not related to or endorsed by Sony, or derived from confidential materials belonging to Sony.

Texture reuse diagnostics: [bounded texture reuse telemetry](docs/texture-reuse-telemetry.md) explains the optional Performance setting, decoding after eviction, memory bounds and interpretation limits.
