# KytyPS5 - GT7 experimental rendering fork

This fork develops Gran Turismo 7 rendering and shader compatibility on Windows x64. It is based on [KytyPS5](https://github.com/KytyPS5/KytyPS5) and remains experimental.

**[Windows download and release notes: r33](https://github.com/QualityControled/KytyPS5/releases/tag/gt7-experimental-2026.10.08-r33)** | **[Changelog](CHANGELOG.md)** | **[Matching source](https://github.com/QualityControled/KytyPS5/tree/8f8d34d30313930404162aac88158a5f73c21eb2)**

## Current GT7 result

World Map and Garage can be reached. Rotating vehicle geometry has been observed for the Fit Hybrid '14 and Civic Type R (EK) '97, including some interior, trim and badge details. Body paint, materials, lighting and shadows are still incorrect or incomplete. Music silence has been reported and remains unresolved. A selected replay/movie showed a moving car with incorrect appearance, then stopped at an invalid graphics-buffer-range guard. The responsible stage/resource has not been identified. A later Garage run also ended with a native fiber stack write access violation; its underlying cause is unresolved.

Playable races have not been established. Menu performance remains low, around four completed guest frames per second in measured scenes. There is no demonstrated 30 FPS result or confirmed performance improvement from the experimental lookup and polling options.

The r33 diagnostic retained 34 complete indexed/depth-tested draw candidates and 432 pixel-shader image associations without an oversized record. Vehicle appearance remains dark or flat. The run closed normally; this does not fix the earlier native fiber crash. A moving-preview comparison of the optional lookup found no demonstrated performance gain, and the portable starter keeps lookup off.

The GT7 launcher enables a declared lower-AA approximation and fixed-function resolve support. It also uses a terminal external-call probe: reaching a selected external shader call can deliberately stop the emulator before that callee executes. This build is intended for rendering development and testing.

## Download and start on Windows

1. Open the [r33 release](https://github.com/QualityControled/KytyPS5/releases/tag/gt7-experimental-2026.10.08-r33) and download `KytyPS5-GT7-Experimental-r33-8f8d34d-Windows-x64.zip`. Download the checksum file as well if you want to verify the archive.
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

Recent changes cover indirect image writes, external-call provenance and bounded diagnostics, image metadata remapping, mapped blend semantics, sample-state retention, an opt-in lower-AA profile, and fixed-function 2-sample-to-1-sample resolves. r30 added an optional bounded final-binding trace for diagnosing vehicle materials. r31 adds default-off, failure-only metadata for invalid shader/vertex buffer ranges while retaining the existing stop. r32 adds an optional geometry-candidate filter to preserve the trace budget for larger or depth-tested draws. r33 raises only that filtered trace's per-record limit to 256 KiB, retaining 64 records and a 1 MiB total limit; the ordinary trace remains 32 KiB per record. All optional vehicle and failure traces stay off in the portable starter. These updates collect evidence and do not repair lighting, materials or the native fiber crash. See [CHANGELOG.md](CHANGELOG.md) for changes and the limits of their validation.

## Source and license

r33 corresponds to [`8f8d34d30313930404162aac88158a5f73c21eb2`](https://github.com/QualityControled/KytyPS5/tree/8f8d34d30313930404162aac88158a5f73c21eb2). [Download the corresponding source ZIP](https://github.com/QualityControled/KytyPS5/archive/8f8d34d30313930404162aac88158a5f73c21eb2.zip). For a complete build checkout, clone the pinned revision with submodules; GitHub source ZIPs do not include submodule contents. Build instructions remain in the [upstream project](https://github.com/KytyPS5/KytyPS5#developer-information).

KytyPS5 remains licensed under [GPL version 2](LICENSE). Original Kyty and third-party notices are retained in the source and the Windows package's `licenses` directory. This fork is not affiliated with Sony Interactive Entertainment or Polyphony Digital.
