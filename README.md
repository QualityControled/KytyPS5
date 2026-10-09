# KytyPS5 - GT7 experimental rendering fork

This fork develops Gran Turismo 7 rendering and shader compatibility on Windows x64. It is based on [KytyPS5](https://github.com/KytyPS5/KytyPS5) and remains experimental.

**[Windows download and release notes: r36](https://github.com/QualityControled/KytyPS5/releases/tag/gt7-experimental-2026.10.09-r36)** | **[Changelog](CHANGELOG.md)** | **[Matching source](https://github.com/QualityControled/KytyPS5/tree/39d62a0ad073e9aab4de5374780a5b72aa315746)**

## Current GT7 result

World Map and Garage can be reached. Rotating vehicle geometry has been observed for the Fit Hybrid '14 and Civic Type R (EK) '97, including some interior, trim and badge details. Body paint, materials, lighting and shadows are still incorrect or incomplete. Music silence has been reported and remains unresolved. A selected replay/movie showed a moving car with incorrect appearance, then stopped at an invalid graphics-buffer-range guard. The responsible stage/resource has not been identified. A later Garage run also ended with a native fiber stack write access violation; its underlying cause is unresolved.

Playable races have not been established. Menu performance remains low, around four completed guest frames per second in measured scenes. There is no demonstrated 30 FPS result or confirmed performance improvement from the experimental lookup and polling options.

r36 adds an optional, bounded GPU device-loss report and a startup message showing whether requested Vulkan validation is actually available. Reporting remains off by default and depends on device support. It retains the original failure behavior; it does not repair vehicle appearance, crashes or low frame rates. The three-target native build passed. Seven registered CPU tests covering 19 authored diagnostic groups passed, as did four MRT and four preserved interpolation module checks. The mapping control used the previously built r35 CPU fixture. These are CPU checks. An isolated live validation test activated successfully and stopped at an invalid sampler/operator combination during a draw. A coordinate-conversion correction is under investigation. This does not establish the cause of previous crashes or incorrect vehicle shading; no device-loss report was invoked in that validation stop.

r35 ports the [upstream compact render-target export correction](https://github.com/KytyPS5/KytyPS5/commit/21a1346e27db4788515cf8d0954164a27ad637eb). Sparse active color slots now retain their physical export locations, and the occupancy mask is part of shader cache identity. The coherent ten-target build and eight selected CPU regression groups passed. Four authored MRT pixel modules passed native emission/default Vulkan 1.2 SPIR-V validation with physical export-location and occupancy-key checks; official reverse component mapping and four preserved interpolation modules plus rectangle controls also passed. A separate existing red-zone patcher CPU fixture passed nine modeled cases. No new GPU validation is claimed for this port. The first r35 trial showed a Civic vehicle preview that remained dark, flat and incorrectly shaded, with the GT7 logo and no Garage controls. The exact menu or preview phase was unconfirmed. The run closed normally; no visual, FPS or crash repair was established.

The previous r34 interpolation revision reached Garage, and the user reported that the vehicle looked about the same. Scapes Movies ran, but returning to World Map ended in a native read access violation; cause unresolved. That read crash is distinct from the earlier native fiber stack write fault. The existing guest red-zone option remains off in the portable starter; a controlled CPU patcher fixture cannot establish the cause of the game crash.

The GT7 launcher enables a declared lower-AA approximation and fixed-function resolve support. It also uses a terminal external-call probe: reaching a selected external shader call can deliberately stop the emulator before that callee executes. This build is intended for rendering development and testing.

## Download and start on Windows

1. Open the [r36 release](https://github.com/QualityControled/KytyPS5/releases/tag/gt7-experimental-2026.10.09-r36) and download `KytyPS5-GT7-Experimental-r36-39d62a0-Windows-x64.zip`. Download the checksum file as well if you want to verify the archive.
2. Extract the whole ZIP into a writable folder. Keep the DLLs, plugin folders and license files together with the executable; do not run it inside the ZIP.
3. Double-click `Start-GT7-Experimental.cmd`. Choose your GT7 game folder containing `eboot.bin`.
4. Keep the console open. Initial shader compilation can pause visible progress. A deliberate external-call stop or another unsupported operation may end the session.

The package also includes `Start-Launcher.cmd` for the original launcher interface. That path uses the upstream/default setup and does not automatically apply the GT7 experimental profile.

For an explicit game path, open PowerShell in the extracted folder and run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Start-GT7-Experimental.ps1 -GameFolder "E:\Games\GT7"
```

For a session that needs the GPU device-loss report, add `-GpuFaultDiagnostic` to the same PowerShell command. The option affects only that emulator child process. Device support is reported at startup; a requested report may be unavailable. The starter keeps Vulkan validation disabled, and no validation-layer package is bundled.

The portable wrapper creates fresh runtime save/cache and temporary files under `_Runtime`, with per-run memory backing under `_MemoryBacking`. Output is shown in the console; it does not automatically save a session log. `-MemoryBackingDirectory` can select another local backing directory. No games, saves, game licenses, captured shaders or caches are supplied with this download.

## Requirements and reporting

Use 64-bit Windows and a Vulkan 1.3-capable GPU with current drivers. The extracted package must be writable, and its backing directory must be on a local fixed NTFS volume with at least 13.5 GiB (about 14.5 GB) free. Compatibility outside the tested Windows configuration is unverified. The optional original Qt launcher also needs the [Microsoft Visual C++ x64 (v14) Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170), which is not bundled. [Official x64 download](https://aka.ms/vc14/vc_redist.x64.exe). Report the release/version, GPU and driver, the menu or action that failed, and a console log after checking it for information you do not want to share. Do not attach game files, saves or captured shader/resource data.

## Changes in this fork

Recent changes cover sample-state retention, a declared lower-AA profile, bounded fixed-function resolves, mapped blending and optional rendering diagnostics. r36 adds optional GPU device-loss reporting and validation startup confirmation, while retaining the r35 compact color-export routing correction. The earlier mixed pixel interpolation correction and rectangle behavior are retained. The cumulative source also contains the default-off R8 software comparison experiment, an optional exact pixel-shader trace selector and bounded failure-only diagnostic metadata. Earlier authored manual GPU tests of the R8 path are separate from the current MRT and earlier interpolation CPU checks; an actual software-enabled startup rejected unsupported status requests before Garage. The public starter leaves software comparison and all optional diagnostics off by default. See [CHANGELOG.md](CHANGELOG.md) for the changes and their validation limits.

## Source and license

r36 corresponds to [`39d62a0ad073e9aab4de5374780a5b72aa315746`](https://github.com/QualityControled/KytyPS5/tree/39d62a0ad073e9aab4de5374780a5b72aa315746). [Download the corresponding source ZIP](https://github.com/QualityControled/KytyPS5/archive/39d62a0ad073e9aab4de5374780a5b72aa315746.zip). For a complete build checkout, clone the pinned revision with submodules; GitHub source ZIPs do not include submodule contents. Build instructions remain in the [upstream project](https://github.com/KytyPS5/KytyPS5#developer-information).

KytyPS5 remains licensed under [GPL version 2](LICENSE). Original Kyty and third-party notices are retained in the source and the Windows package's `licenses` directory. This fork is not affiliated with Sony Interactive Entertainment or Polyphony Digital.
