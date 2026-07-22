# KytyPS5

[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4.svg)](#system-requirements)
[![Status](https://img.shields.io/badge/status-early%20development-orange.svg)](#current-status)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

KytyPS5 is a free and open-source PlayStation 5 emulator written in C++ for Windows. It is based on
a heavily modified version of [Kyty](https://github.com/InoriRus/Kyty). The project is in an early
stage of development, so compatibility is limited and behavior may change significantly between
builds.

> [!NOTE]
> This repository is a personal fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5) containing
> boot and rendering fixes found while debugging *TMNT: Mutants Unleashed*. See
> [Fork Changes](#fork-changes) for what differs from upstream, and
> [Special Thanks](#special-thanks) for the projects those fixes came from.

> [!IMPORTANT]
> KytyPS5 is not affiliated with Sony Interactive Entertainment or PlayStation. The project does
> not distribute games or copyrighted system software. Use only game files that you have obtained
> legally.

## Current Status

KytyPS5 can boot 2D games and a selection of 3D games, including titles built with Unreal Engine
4/5, Unity, and custom engines. No external low-level emulation modules are currently required.

Development is focused on compatibility and boot reliability.

Linux support is planned, but Windows is the only supported platform at this time.

## Fork Changes

These changes were made while debugging a Unity (IL2CPP) PS5 title, *TMNT: Mutants Unleashed*, that
previously showed only a black screen. It now boots to its title screen and runs at 60 fps. Each
change is described with the symptom it fixed so the reasoning can be checked.

**Loader and kernel**

- **64-bit page mask for fault addresses.** `& ~0xFFFU` is a 32-bit constant, so it truncated fault
  addresses above 4 GB and demand-commit resolved the wrong page. Any first touch above 4 GB then
  re-faulted forever on the same instruction, pegging one core with no log output.
- **Chunked demand-commit.** Committing a single 4 KB page per fault turned a conservative
  garbage collector's heap scan into a multi-minute fault storm; reserved ranges are now committed
  in bounded chunks.
- **Guest code runs on the guest stack again.** The stack switch in `RunOnGuestStack` and
  `RunEntry` had been removed, so Boehm GC derived a scan range spanning gigabytes of unrelated
  address space.
- **The access-violation handler no longer allocates in free address space.** Committing memory at
  arbitrary faulted addresses consumed the address space Unity's flip, EOP and workload thread
  stacks needed, so those threads failed to start.
- **Faults and illegal instructions fail loudly.** Instruction-skipping fallbacks silently
  desynchronised guest state and hid real crashes; they now log the faulting opcode and stop.
- **POSIX exports return `-1` and set `errno`.** `open`, `close` and `write` were bound to the raw
  `sceKernel*` entry points, which return an `0x8002xxxx` status. libc callers stored that as a
  valid descriptor and later dereferenced it.
- **`sceKernelSyncOnAddressWait`/`Wake` implemented.** Both NIDs previously resolved to one no-op
  stub. They are a futex-style pair, and Unity's job system busy-spins forever without them.
- **`strcpy` NID corrected.** `kiZSXIWd9vg` is `strcpy`, not `realloc` (`Y7aJ1uydPMo`). Every guest
  `strcpy(dest, src)` was executing `realloc(dest, (size_t)src)`: nothing was copied and each call
  requested a ~143 MB allocation, leaving asset and path strings as garbage. This was the root
  cause of the "data file is corrupted" boot failure.
- **`read`/`pread` on directory descriptors** return directory data instead of `EISDIR`.

**Graphics**

- **Depth extent fallbacks.** A depth surface can be bound with valid base addresses but no extent
  register. The extent is now taken, in order, from `DB_DEPTH_SIZE_XY`, the render-target register
  blob, the encoded `DB_DEPTH_SIZE` pitch/height, a bound colour target, the viewport transform, or
  the screen scissor. The viewport case is what unblocked rendering: the guest expresses that
  extent only as floats, so no integer size register carries it.
- **Zero-extent depth attachments are dropped** rather than aborting, when the guest has written an
  explicitly zero size and the real clear is performed by the HTile compute path.
- **Texture metadata re-registration.** Re-registering metadata at an address with a different
  surface shape is a reallocation, not a growth, and now replaces the entry instead of aborting.
- **Depth-target reallocation.** A depth allocation reused for a differently shaped surface is
  retired, mirroring the existing colour-target path. Depth and stencil contents are ephemeral, so
  GPU ownership of the range is released rather than written back.
- **Compute queues share the graphics queue family.** Readback buffers use exclusive sharing, which
  Vulkan only permits within one family; compute queues previously spilled into a second family.
- **Graphics lane-mask model no longer trusts `subgroupSize`.** `VkPhysicalDeviceVulkan11Properties::subgroupSize`
  is the device's *default* width, not a per-stage guarantee. RDNA reports 64 while running fragment
  shaders at wave32, so shaders compiled for a native 64-lane wave silently got 32-lane EXEC/ballot
  semantics and predicated colour exports collapsed to one lane per wave — on AMD a wave64 pixel
  shader covers an 8x8 fragment tile, so every colour surface was written at exactly one pixel per
  8x8 block. The fixed-width model is now used only when the width cannot vary
  (`minSubgroupSize == maxSubgroupSize`); otherwise the width-independent per-invocation model is
  selected. Pinning via subgroup size control is not an alternative for graphics, because
  `requiredSubgroupSizeStages` commonly omits the vertex stage.
- **Sampled depth ranges larger than the depth target are tolerated.** A shader may sample a whole
  shadow atlas through one descriptor while only a sub-region of it is bound as a depth target. That
  lookup previously aborted. It now binds the live depth image (in width, height, or both);
  ambiguous matches and the barrier path remain strict.
- **HTile metadata overlapping a clean depth target retires it.** Re-registering depth metadata for
  a new shadow-atlas cascade can overlap the pages of a previous cascade's depth target. The overlap
  classifier only handled colour targets and textures, so a depth target aborted; a clean one now
  retires like a colour target (the retire path already supported depth).
- **A new render target overlapping a clean one at a different base retires it.** When the guest
  allocates a render target whose pages overlap a still-cached, unmodified target left at another
  base (a freed target's memory reused by a larger one), the classifier aborted. A clean target is
  now retired and rebuilt from guest memory; `RequireRetirementIsolation` verifies no tracked page
  alias is left behind.
- **Guest-reserved placeholder ranges are demand-committed on write.** A title can reserve a large
  placeholder virtual range and write into it directly, expecting demand-commit rather than mapping
  backing first (TMNT's main heap allocator does this). The generic fault path cannot service that —
  a plain `VirtualAlloc(MEM_COMMIT)` cannot commit a page of a placeholder. The kernel's own commit
  path handles it now, confined to ranges the guest explicitly reserved so it never commits at
  arbitrary faulted addresses (which previously stole address space and produced a black screen).

**Known limitations**

The depth extent is a heuristic. A title can bind a depth surface with valid base addresses but no
extent register at all — TMNT programs none of the three, so the extent is always inferred from a
bound colour target, the viewport, or the screen scissor. The inferred value describes the region
being rendered rather than the surface allocation, so when one allocation is shared by passes with
different viewports it can under- or overestimate the surface size.

The consequence is visible in shadow-atlas sampling: the bound depth image is smaller than the
descriptor describes, so normalized coordinates rescale and shadows sampled that way are
geometrically wrong. Fixing this properly requires sizing depth images to the whole guest
allocation and rendering into a sub-region, which Vulkan permits — a render area may be smaller
than its attachment.

## Bugs and Issues

The project is in an early stage, so please be mindful when opening new issues. Expect crashes,
graphical glitches, low compatibility, and poor performance.

## Screenshots

<table align="center">
  <tr>
    <td align="center">
      <strong>Disgaea 6</strong><br>
      <img src="docs/screenshots/ps5-01.png" width="300" alt="Disgaea 6 running in KytyPS5">
    </td>
    <td align="center">
      <strong>Dreaming Sarah</strong><br>
      <img src="docs/screenshots/ps5-03.png" width="300" alt="Dreaming Sarah running in KytyPS5">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Minecraft Legends</strong><br>
      <img src="docs/screenshots/ps5-04.png" width="300" alt="Minecraft Legends running in KytyPS5">
    </td>
    <td align="center">
      <strong>SILENT HILL: The Short Message</strong><br>
      <img src="docs/screenshots/ps5-05.png" width="300" alt="SILENT HILL: The Short Message running in KytyPS5">
    </td>
  </tr>
</table>

## Contributing

Testing games and submitting detailed bug reports are useful ways to contribute. Search existing
issues first, then use the **Game Emulation Bug Report** template and attach the complete log file.

Code contributions should be focused, build successfully on Windows, and include relevant tests
where practical. Because KytyPS5 is still evolving quickly, consider opening an issue before
starting a large change.

## Developer Information

The PS5 graphics architecture is based on AMD RDNA 2. Use AMD's
[RDNA 2 Instruction Set Architecture Reference Guide (document 70648)](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)
as the primary instruction-encoding reference when working on shader decoding and recompilation.

Important areas of the codebase:

- [`src/graphics/shader/recompiler`](src/graphics/shader/recompiler) — instruction decoding,
  intermediate representation, control flow, resource tracking, and SPIR-V emission
- [`src/graphics/guest_gpu`](src/graphics/guest_gpu) — PS5 (Prospero) GPU formats and command processing
- [`src/graphics/host_gpu`](src/graphics/host_gpu) — Vulkan host backend and resource management
- [`tests`](tests) — focused memory, shader, and resource-tracking regression tests

The renderer targets Vulkan 1.3. Keep shader changes aligned with both the RDNA 2 ISA semantics and
the Vulkan/SPIR-V validation rules.

## Building

### System requirements

- Windows 10 version 1803
- A 64-bit x86 processor
- A Vulkan 1.3-capable GPU with current drivers

### Build requirements

- Git
- CMake 3.12 or newer
- Ninja
- Visual Studio 2022 or Build Tools 2022 with the **Desktop development with C++** workload and
  **C++ Clang tools for Windows** component
- Qt 6 for MSVC 2022 64-bit, including Concurrent, Network, and Widgets
- Vulkan SDK 1.3 or newer

The Microsoft C++ compiler (`cl.exe`) is not supported; use `clang-cl`.

Open an **x64 Native Tools Command Prompt for Visual Studio 2022** (or the equivalent Developer
PowerShell), change to the repository root, and initialize the dependencies:

```powershell
git submodule update --init --recursive
```

Configure the project. Replace the Qt path with the version installed on your system:

```powershell
cmake -S src -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
```

Build the launcher and stage a runnable installation:

```powershell
cmake --build _Build/windows --target launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

The finished application and its runtime dependencies will be placed in
`_Build/windows/install`.

### Visual Studio Code

A ready-made Visual Studio Code setup is included in [`.vscode`](.vscode). It configures CMake
Tools to build the project with Ninja and `clang-cl` and provides launch profiles for both
`launcher.exe` and `kyty_emulator.exe`.

Before using it:

1. Install the **CMake Tools** and **C/C++** extensions in Visual Studio Code.
2. Update `CMAKE_PREFIX_PATH` in [`.vscode/settings.json`](.vscode/settings.json) to point to your
   Qt 6 MSVC installation.
3. Update the `--game` path in [`.vscode/launch.json`](.vscode/launch.json) for the
   **Debug kyty_emulator** profile.
4. Open the repository in an x64 Visual Studio developer environment, configure the CMake project,
   and select a launch profile from **Run and Debug**.

## Running

Update your graphics driver before reporting rendering problems.

To use the graphical launcher:

```powershell
.\_Build\windows\install\launcher.exe
```

On first launch, add one or more game folders in the global settings. The launcher searches those
folders recursively for game directories containing `eboot.bin`. Select a detected game and run it
from the game list.

The emulator can also be started directly with a legally obtained game directory or ELF file:

```powershell
.\_Build\windows\install\kyty_emulator.exe --game "D:\Games\ExampleGame"
```

Run `kyty_emulator.exe --help` to see the available graphics, logging, validation, profiling, and
debugging options.

### AI Use

AI tools may be used for research, reverse engineering, and development assistance. Contributors
must fully understand, review, and test all code they submit and remain responsible for its
correctness. Repository communication, including pull-request descriptions, code comments, and
issue comments, must come from the human contributor rather than an autonomous AI agent.

Pull requests that include AI-assisted or AI-generated work should disclose the scope of the AI
involvement and describe the human review and testing performed before submission. Unverified or
untested generated changes may be closed without review.

## License

KytyPS5 is licensed under the [GNU General Public License version 2](LICENSE)
(`GPL-2.0-only`).

This project is based on the original [Kyty](https://github.com/InoriRus/Kyty), which was released
under the MIT License. Kyty's original copyright and license notice are preserved in
[`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt). Third-party components remain subject to the
licenses included with those components.

## Special Thanks

- [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5) — the upstream project this repository is
  forked from. Everything here is a small delta on top of their work.
- [InoriRus/Kyty](https://github.com/InoriRus/Kyty) — KytyPS5 is based on a heavily modified version
  of the original Kyty project.
- [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4) — reference for memory-model
  understanding and the AVPlayer implementation.
- [sharpemu/sharpemu](https://github.com/sharpemu/sharpemu) — an experimental PS5 emulator written
  in C# by par274. Its export tables were used as a cross-reference to verify this fork's NID
  mappings, which is how three bugs in the list above were found: the `strcpy`/`realloc` mix-up,
  the POSIX `open`/`close`/`write` error convention, and the fact that
  `sceKernelSyncOnAddressWait`/`Wake` are a real futex pair rather than a single stub. SharpEmu's
  `KernelMemoryCompatExports.cs` also documents the null-dereference symptom that a leaked
  `0x8002xxxx` status produces in Unity's IL2CPP file layer, which matched the crash seen here.

### Tools and references

- The [Vulkan](https://www.vulkan.org/) specification and validation layers.
- [Capstone](https://www.capstone-engine.org/) — used to disassemble guest code while tracing the
  Unity boot path.
- AMD GCN/RDNA register documentation, for the PM4 context registers referenced in
  `src/graphics/guest_gpu`.
- Debugging for this fork was carried out with AI assistance (Claude Code); see
  [AI Use](#ai-use). All changes were built and exercised against a real title before being
  committed.
