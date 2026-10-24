# Vita3K-Go

Performance and diagnostic development focused on **Windows on the Lenovo Legion Go**, with WipEout 2048 as the primary test workload. The changes use shared emulator paths and automatic per-game learning, so other games with compatible textures can benefit without game-specific profiles or track lists. Benefits depend on the game's actual bottleneck. Steam Deck, ROG Ally and other devices require their own validation.

[Downloads](https://github.com/zipkindev/Vita3K-Go/releases) · [Community findings and measurements](docs/performance-findings.md) · [Setup and dependencies](docs/windows-setup.md) · [Using matching symbols](docs/windows-symbols.md)

## Credits and source origins

This project builds on the work of these communities:

- [Vita3K](https://github.com/Vita3K/Vita3K): the original emulator and primary upstream. This repository remains in its GitHub fork network.
- [Vita3K-Plus](https://github.com/nckstwrt/Vita3K-Plus/tree/all-enhancements): the source baseline for this experimental line, including its scheduling, compatibility and frontend work.
- [Vita3K-nx](https://github.com/NaGaa95/Vita3K-nx): selected shared-code correctness fixes. Switch-specific changes are not assumed useful on Windows.

Our work adds measured PVRTC decoded-texture caching, bounded parallel decoding, automatic per-game texture learning and preparation, lightweight diagnostics, exact-symbol release packages and Legion Go validation. Async pipeline compilation is an existing emulator capability; our DOA5 Plus work identifies and tests its effect rather than claiming authorship of that feature.

We review selected imports instead of automatically merging entire fork histories. [Integration provenance](docs/curated-candidate-1.md) records the mainline NGS handle-validation change and nx pipeline-cache validation change. Original authorship, copyright and license notices are preserved. This is an experimental fork, not an official upstream release. The candidate branch carries emulator changes; the default branch currently carries build infrastructure on a separate history. Release manifests identify the exact source and recipe commits, patches, submodules, toolchain and EXE/PDB hashes.

## Enhancements

- **Decoded texture cache:** reuse decoded PVRTC textures in a configurable RAM budget, reducing repeated CPU decompression.
- **Parallel texture decoding:** the caller and one helper share eligible decode work; small textures keep the serial path.
- **Automatic texture preparation:** learn compressed inputs per game, merge previous compatible learning and prepare selected textures at the next game launch. The bounded history survives eviction of decoded copies.
- **Optional diagnostics:** texture reuse counters, JIT compilation profiling and synchronization-wait summaries help distinguish remaining causes of hitches. These measure behavior; they do not warm the JIT.
- **Reproducible Windows packages:** one optimized executable, an optional exact matching PDB, dependency patches and build evidence.

## What testing shows so far

Legion Go captures show fewer long presentation stalls in tested workloads. These are frame-presentation and CPU-trace measurements, **not measured controller-to-display input latency** or a promise of gains in every game.

| Workload | Observed result |
|---|---|
| WipEout 2048, Ubermall, decoded cache off versus 256 MiB | Intervals above 50 ms fell from 10 to 1; maximum interval fell from 242.36 to 63.84 ms in a sequential 45-second pair. |
| WipEout 2048, Chenghou Rapier, learned preparation Off versus On | An earlier 90-second pair had 6 versus 0 intervals above 50 ms; maximum interval 161.75 versus 36.62 ms. |
| WipEout 2048, `.13`, fresh process with merged learning at 512 MiB | A clean 90-second run had median 33.36 ms, p99 35.30 ms and one 97.01 ms interval near the start. The owner reported “felt flawless.” All 7,281 prior texture inputs were retained and 68 added on normal close. |
| DOA5 Plus, `.11`, synchronous versus async pipeline compilation | At 1×, intervals above 50 ms fell from 7 to 0; maximum interval 298.20 versus 36.35 ms. A subsequent 2× async run also had no intervals above 50 ms, with correct graphics reported. |

The DOA5 improvement was associated with moving Vulkan pipeline compilation off the renderer, not demonstrated PVRTC cache gains. Sequential order, retained disk caches, changing opponents/effects and unknown exact fight stages limit causal comparisons. The `.12` preload-retention experiment worsened cache reuse and was not adopted. [The findings report](docs/performance-findings.md) includes unsuccessful experiments, trace evidence, build identities and open questions.

## Texture decoding and automatic learning

Open **Settings → Texture Decoding (Perf. Tweaks)**, globally or per game. Select a nonzero **Decoded Texture Cache** budget and enable **Automatic Texture Preparation**. Play normally and close normally to save learning. Later game launches validate and prepare compatible learned inputs, then continue learning. No manual race selection, profile management or game-specific whitelist is needed. This is dynamic learning across launches, not prediction of unseen textures or a job scheduled at each race loading screen.

Decoded cache choices are Off, 128, 256 and 512 MiB, with at most 16,384 entries. Learning keeps up to 64 MiB of compressed-input history plus bounded metadata, separately from the decoded RAM budget. Saved profiles are limited to 64 MiB per game and 1 GiB total; older observations/profiles can be evicted at their bounds. Preparation has a five-second soft deadline and can prepare only part of a profile that exceeds the RAM budget. Extra loading work and bounded scratch memory are costs of preparation. **Clear This Game's Texture Profile** and **Clear All Saved Texture Profiles** remove managed profiles when no game is running; saves and shader caches remain intact.

The current texture path supports eligible PVRTC I/II 2/4 bpp inputs. Other formats, GPU saturation, pipeline compilation, guest JIT work, I/O and synchronization can still cause stalls. Preparation runs on the CPU before guest execution; it does not preload GPU resources or warm JIT code. Game-derived learned profiles stay local. See [automatic texture behavior and limits](docs/automatic-textures.md).

Graphics → **Texture Import and Export** contains texture replacement/export controls. Optional texture, JIT and synchronization telemetry is under **Diagnostics** inside the texture-decoding tab. Diagnostic switches default off; they add measurement work and save reports on normal close. Changes to these settings apply at the next game launch. [Texture reuse](docs/texture-reuse-telemetry.md) and [synchronization telemetry](docs/synchronization-telemetry.md) document their interpretation limits.

## Downloads, symbols and contributing findings

Each release uses **one optimized build with two downloads**: the runtime ZIP contains the EXE and dependencies; the optional diagnostics ZIP adds the exact matching PDB and build evidence. Symbols live in a separate file and do not require an unoptimized Debug executable. Match the EXE hash and PDB GUID/age to the release manifest. [Symbol instructions](docs/windows-symbols.md) cover installation, verification and WinDbg use; [setup instructions](docs/windows-setup.md) explain bundled libraries, ATRAC9 audio and user prerequisites.

Release notes should identify changes, observed results, limitations and exact provenance. Share reproducible steps, game/version, hardware, graphics settings, cache/preparation state, build label and whether a capture contains continuous gameplay. Read [the findings report](docs/performance-findings.md) before attributing a hitch to a particular subsystem. Raw dumps/traces can include private data; review them before sharing and keep game files, saves, credentials and learned texture bytes out of public issues.

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
