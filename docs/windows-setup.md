# Windows setup and dependencies

These packages target Windows x64; our performance validation uses the Lenovo Legion Go on Windows 11. Other handhelds and operating systems require their own testing.

## For players

1. Download the runtime ZIP for the desired release and extract the **whole archive** into a separate folder. Keep the bundled DLLs and plugin folders together with `Vita3K.exe`.
2. Install the supported Microsoft Visual C++ **x64** runtime from [Microsoft's official redistributable page](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist). It must be at least as recent as the MSVC tools used for the build. A missing `VCRUNTIME` or `MSVCP` DLL is a runtime-installation issue; do not repair it with DLLs from unrelated download sites.
3. Use a graphics driver supporting the selected renderer. Vulkan support comes from the GPU driver; the Vulkan SDK is not required to play. Our validation uses Vulkan on the Legion Go's AMD GPU.
4. Follow the upstream [Vita3K quickstart](https://vita3k.org/quickstart) for firmware, font and game setup. These are not supplied with our emulator package. Keep your working installation and saves backed up before testing a separate candidate.

The optional diagnostics ZIP adds matching symbols and evidence. It is not required for sound or normal gameplay. [Symbol setup](windows-symbols.md) explains how to use it.

## What the package includes

| Component | Purpose | Delivery in the Windows build |
|---|---|---|
| Qt | Settings/library interface, windowing and multimedia support | Qt runtime DLLs plus required plugins and data; no Qt developer installation needed |
| FFmpeg | Audio/video decoding, demuxing, resampling and conversion | Runtime libraries such as `avcodec`, `avformat`, `avutil`, `swresample` and `swscale` |
| LibAtrac9 | ATRAC9 audio decoding used by Vita game audio | Compiled statically into this build; no separate ATRAC9 DLL or player codec installation needed |
| Dynarmic | Guest ARM code translation on the x64 host | Built as part of the emulator, with source-pinned diagnostic observer support |
| PVRTC decoder and cache | CPU texture decompression, bounded decoded reuse and learned preparation | Part of the emulator; no external codec download needed |
| Microsoft C/C++ runtime | Runtime services for code built with MSVC | Install the supported x64 Visual C++ Redistributable separately |
| Vulkan/OpenGL driver | GPU access for the selected renderer | Supplied by your GPU vendor/driver installation, not the emulator's symbol package |

The release manifest lists the actual files and hashes; exact DLL versions depend on the release. Optional components, translations and other runtime assets are shipped as collected by the build recipe. Preserve the package layout rather than copying only the EXE. Original dependency licenses and notices continue to apply.

## What happened with ATRAC9 during development?

ATRAC9 is an audio format; **LibAtrac9** is the decoder library. Developers had to obtain its pinned source and apply the required bounded-input support patches to build this experimental line. That was a build prerequisite, not an instruction for players to find a proprietary codec or download a DLL.

This source declares LibAtrac9 as a static library. The Windows recipe initializes `external/LibAtrac9`, applies the bounded decoder patch and checks that the NGS caller passes the correct remaining superframe bytes. Early diagnostic audio issues were corrected before the cache comparison used for published performance claims. The diagnostics archive records the exact dependency revision and applied diff. Replacing it with an arbitrary DLL does not reproduce that build.

See the upstream [LibAtrac9 repository](https://github.com/Vita3K/LibAtrac9) and this fork's `external/CMakeLists.txt`. The codec and audio work build on upstream/fork contributions; attribution and licenses are preserved.

## For source builders

Players do not need these tools. Our reviewed Windows recipe uses Visual Studio 2022/MSVC with the C++ workload and Windows SDK, CMake/Ninja, Git, Python, Qt 6.11.0 for MSVC x64, a pinned vcpkg checkout and the project's recursive pinned submodules. Other fetched or bundled dependencies are identified by the source and build evidence.

A plain source checkout without recursive submodules and support patches is not equivalent to a diagnostic release. Read [the Windows build recipe](https://github.com/zipkindev/Vita3K-Go/tree/master/tools/windows-build) and the release's `build-evidence` for the exact toolchain, patches and tests. The recipe uses optimized RelWithDebInfo, LTO and full PDB output, then splits runtime and diagnostics into two archives. Builds of the same source can have different EXE/PDB identities; never substitute another build's PDB.

The upstream [building guide](../building.md) describes general development setup. Exact reproduction of our experimental release additionally requires its recipe commit, source commit, support patches, dependency patches and pinned submodule versions.
