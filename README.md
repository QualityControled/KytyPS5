# KytyPS5 - GT7 experimental rendering fork

This fork develops Gran Turismo 7 rendering and shader compatibility on Windows x64. It is based on [KytyPS5](https://github.com/KytyPS5/KytyPS5) and remains experimental.

**[Windows download and release notes: r34](https://github.com/QualityControled/KytyPS5/releases/tag/gt7-experimental-2026.10.08-r34)** | **[Changelog](CHANGELOG.md)** | **[Matching source](https://github.com/QualityControled/KytyPS5/tree/8c291f1bbab3db9036570a209b34875f1465ddfa)**

## Current GT7 result

World Map and Garage can be reached. Rotating vehicle geometry has been observed for the Fit Hybrid '14 and Civic Type R (EK) '97, including some interior, trim and badge details. Body paint, materials, lighting and shadows are still incorrect or incomplete. Music silence has been reported and remains unresolved. A selected replay/movie showed a moving car with incorrect appearance, then stopped at an invalid graphics-buffer-range guard. The responsible stage/resource has not been identified. A later Garage run also ended with a native fiber stack write access violation; its underlying cause is unresolved.

Playable races have not been established. Menu performance remains low, around four completed guest frames per second in measured scenes. There is no demonstrated 30 FPS result or confirmed performance improvement from the experimental lookup and polling options.

r34 ports the [upstream mixed pixel interpolation fix](https://github.com/KytyPS5/KytyPS5/commit/b38b7454a6bc21f5d68bea825214fac60f53f80a). When flat and smooth pixel inputs share one vertex export, ordinary primitives now retain that export's actual location and the requested first/last provoking vertex. Rectangle interpolation keeps its separate behavior. The coherent build, eight CPU regression groups and four authored native pixel modules with default Vulkan 1.2 SPIR-V validation passed. This build reached Garage, and the user reported that the vehicle looked about the same. No visible wheel, lighting, material or FPS improvement has been established. Scapes Movies ran, but returning to World Map ended in a native read access violation; cause unresolved.

The GT7 launcher enables a declared lower-AA approximation and fixed-function resolve support. It also uses a terminal external-call probe: reaching a selected external shader call can deliberately stop the emulator before that callee executes. This build is intended for rendering development and testing.

## Download and start on Windows

1. Open the [r34 release](https://github.com/QualityControled/KytyPS5/releases/tag/gt7-experimental-2026.10.08-r34) and download `KytyPS5-GT7-Experimental-r34-8c291f1-Windows-x64.zip`. Download the checksum file as well if you want to verify the archive.
2. Extract the whole ZIP into a writable folder. Keep the DLLs, plugin folders and license files together with the executable; do not run it inside the ZIP.
3. Double-click `Start-GT7-Experimental.cmd`. Choose your GT7 game folder containing `eboot.bin`.
4. Keep the console open. Initial shader compilation can pause visible progress. A deliberate external-call stop or another unsupported operation may end the session.

The package also includes `Start-Launcher.cmd` for the original launcher interface. That path uses the upstream/default setup and does not automatically apply the GT7 experimental profile.

For an explicit game path, open PowerShell in the extracted folder and run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Start-GT7-Experimental.ps1 -GameFolder "E:\Games\GT7"
```

The portable wrapper creates fresh runtime save/cache and temporary files under `_Runtime`, with per-run memory backing under `_MemoryBacking`. Output is shown in the console; it does not automatically save a session log. `-MemoryBackingDirectory` can select another local backing directory. No games, saves, game licenses, captured shaders or caches are supplied with this download.

## Requirements and reporting

Use 64-bit Windows and a Vulkan 1.3-capable GPU with current drivers. The extracted package must be writable, and its backing directory must be on a local fixed NTFS volume with at least 13.5 GiB (about 14.5 GB) free. Compatibility outside the tested Windows configuration is unverified. The optional original Qt launcher also needs the [Microsoft Visual C++ x64 (v14) Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170), which is not bundled. [Official x64 download](https://aka.ms/vc14/vc_redist.x64.exe). Report the release/version, GPU and driver, the menu or action that failed, and a console log after checking it for information you do not want to share. Do not attach game files, saves or captured shader/resource data.

## Changes in this fork

Recent changes cover sample-state retention, a declared lower-AA profile, bounded fixed-function resolves, mapped blending and optional rendering diagnostics. r34 adds the upstream mixed pixel interpolation correction and its cache identity, preserving rectangle interpolation. It also adds the default-off R8 software comparison experiment, an optional exact pixel-shader trace selector and bounded failure-only diagnostic metadata. Earlier authored manual GPU tests of the R8 path are separate from this interpolation port's CPU checks; an actual software-enabled startup rejected unsupported status requests before Garage. The public starter leaves software comparison and all optional diagnostics off. See [CHANGELOG.md](CHANGELOG.md) for the changes and their validation limits.

## Source and license

r34 corresponds to [`8c291f1bbab3db9036570a209b34875f1465ddfa`](https://github.com/QualityControled/KytyPS5/tree/8c291f1bbab3db9036570a209b34875f1465ddfa). [Download the corresponding source ZIP](https://github.com/QualityControled/KytyPS5/archive/8c291f1bbab3db9036570a209b34875f1465ddfa.zip). For a complete build checkout, clone the pinned revision with submodules; GitHub source ZIPs do not include submodule contents. Build instructions remain in the [upstream project](https://github.com/KytyPS5/KytyPS5#developer-information).

KytyPS5 remains licensed under [GPL version 2](LICENSE). Original Kyty and third-party notices are retained in the source and the Windows package's `licenses` directory. This fork is not affiliated with Sony Interactive Entertainment or Polyphony Digital.
