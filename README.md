# KytyPS5 - GT7 experimental rendering fork

This fork develops Gran Turismo 7 rendering and shader compatibility on Windows x64. It is based on [KytyPS5](https://github.com/KytyPS5/KytyPS5) and remains experimental.

**[Windows download and release notes: r37](https://github.com/QualityControled/KytyPS5/releases/tag/gt7-experimental-2026.10.09-r37)** | **[Changelog](CHANGELOG.md)** | **[Matching source](https://github.com/QualityControled/KytyPS5/tree/266cccaee1301fbf279644d4c1390edf81082944)**

## Current GT7 result

World Map and Garage can be reached. Rotating vehicle geometry has been observed for the Fit Hybrid '14 and Civic Type R (EK) '97, including some interior, trim and badge details. Body paint, materials, lighting and shadows are still incorrect or incomplete. Music silence has been reported and remains unresolved. Replay/movie navigation and repeated menu transitions can end a session.

Playable races have not been established. Menu performance remains low, with around four completed guest frames per second in measured scenes and higher but variable rates in moving previews. No performance improvement is claimed for r37.

r37 converts unnormalized guest sampling coordinates before using normalized Vulkan samplers. Conversion is automatic for supported direct sampling, gathering and LOD queries, plus bounded indirect raw sampling. Existing rejection of indirect gathering and LOD queries remains unchanged. It retains the original sampler filtering, addressing and comparison settings, and converts spatial coordinates and explicit gradients using the bound image view's base-level dimensions. Array layers, comparison references, explicit LOD, bias and integer offsets retain their original units. Indirect sampler lookups use the selected descriptor's coordinate mode. Unnormalized cube sampling remains unsupported and is rejected.

r37 also ports the [upstream signed-normalized color-export correction](https://github.com/KytyPS5/KytyPS5/commit/b96871e2bff67570094b67012ddb62ba3601974c). Compressed SNORM16 exports preserve sign and normalized endpoints; the existing UNORM16 and half-float export paths are retained. This is a generic shader correction. Its use by the observed vehicle draws has not been established.

The original R8 software-comparison experiment retains its strict scope and remains disabled in the public starter. Existing sampler approximations and other unsupported shader paths are not expanded by the coordinate correction. Optional diagnostics remain off by default; Vulkan validation is used separately in private testing and is not enabled or bundled by this package.

**Validation status:** Normalization CPU preparation passed 38 authored native modules with SPIR-V validation. The checks cover direct implicit sampling, gathering and LOD queries, bounded indirect raw sampling with selected-descriptor and warm-state checks, and a byte-identical normalized-cube control. Original indirect gather and LOD-query rejection was checked separately.

Owned GPU readback passed 25 modules and 27 dispatches for explicit LZ and gradient sampling, including rectangular 2D/array views, nonzero base mips, all native/sampler coordinate-mode combinations and a live sampler-mode off/on/off sequence using the same compiled module. Returned RGBA values and four guard words matched; the requested validation layer was active with no logged VUID. This is a manual owned-image ABI test, not production texture-import or GT7 proof. Implicit sampling, gathering and LOD queries have CPU evidence only.

Signed-export CPU preparation passed five authored native modules: three signed pairs and unchanged UNORM16/half-float controls. It has no authored GPU readback or observed vehicle-export association.

The merged native build and ten registered CPU tests passed. The canonical normalization preparation reproduced all 38 authored modules; signed-export preparation passed five modules, and four MRT plus four preserved interpolation modules passed CPU emission/SPIR-V validation.

A private validation run of this native revision stopped during startup at an acquisition-semaphore synchronization error (VUID-vkAcquireNextImageKHR-semaphore-01779). The earlier unnormalized-sampler error was not logged before that stop, but the previously failing draw was not established as reached. The semaphore issue remains under investigation. No new vehicle appearance, crash, playable-race or FPS repair is established.

The GT7 starter enables a declared lower-AA approximation and fixed-function resolve support. It also uses a terminal external-call probe: reaching a selected external shader call can deliberately stop the emulator before that callee executes. This build is intended for rendering development and testing.

## Download and start on Windows

1. Open the [r37 release](https://github.com/QualityControled/KytyPS5/releases/tag/gt7-experimental-2026.10.09-r37) and download `KytyPS5-GT7-Experimental-r37-266ccca-Windows-x64.zip`. Download the checksum file as well if you want to verify the archive.
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

Recent changes cover sample-state retention, a declared lower-AA profile, bounded fixed-function resolves, mapped blending, compact color-export routing and mixed pixel interpolation. r37 adds sampled-coordinate conversion and signed-normalized compressed color exports. The cumulative source also contains the default-off R8 software-comparison experiment and optional bounded rendering diagnostics. See [CHANGELOG.md](CHANGELOG.md) for changes and validation limits.

## Source and license

r37 corresponds to [`266cccaee1301fbf279644d4c1390edf81082944`](https://github.com/QualityControled/KytyPS5/tree/266cccaee1301fbf279644d4c1390edf81082944). [Download the corresponding source ZIP](https://github.com/QualityControled/KytyPS5/archive/266cccaee1301fbf279644d4c1390edf81082944.zip). For a complete build checkout, clone the pinned revision with submodules; GitHub source ZIPs do not include submodule contents. Build instructions remain in the [upstream project](https://github.com/KytyPS5/KytyPS5#developer-information).

KytyPS5 remains licensed under [GPL version 2](LICENSE). Original Kyty and third-party notices are retained in the source and the Windows package's `licenses` directory. This fork is not affiliated with Sony Interactive Entertainment or Polyphony Digital.
