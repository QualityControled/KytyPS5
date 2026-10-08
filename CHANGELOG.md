# Changelog

This list covers emulator changes after `KytyPS5-2026-10-07-d2413fc`. Experimental settings are opt-in. Owned fixtures and captured CPU validation are separate from actual game observations.

## r35 - 2026-10-08 - 35d1798

- Port [upstream 21a1346](https://github.com/KytyPS5/KytyPS5/commit/21a1346e27db4788515cf8d0954164a27ad637eb) to preserve physical color-export locations when the active render-target mask has gaps. Retain reverse component mapping and distinguish occupancy in shader cache identity.
- The coherent ten-target build and eight selected CPU regression groups passed. Four authored MRT modules passed native CPU emission/default Vulkan 1.2 SPIR-V validation with physical export-location and occupancy-key assertions. Official reverse component mapping and four preserved interpolation modules plus rectangle controls passed. No new GPU validation is claimed for this port.
- The first r35 trial showed a Civic vehicle preview that remained dark, flat and incorrectly shaded, with the GT7 logo and no Garage controls. The exact menu or preview phase was unconfirmed. The run closed normally; no visual, FPS or crash repair was established. Targeted tracing retained 36 complete draw candidates in two signature groups and 360 pixel-shader image associations; these associations do not establish vehicle-draw identity or causality. Prior r34 vehicle appearance looked about the same; materials, lighting, shadows, details and performance remain unresolved. Scapes Movies ran, but returning to World Map ended in a native read access violation; cause unresolved. The earlier native fiber stack write fault is also unresolved.
- Later test observation (2026-10-08, 19:08 EDT): A separate run of the same r35 build with `--redzone` enabled ended with GPU timeline `device_lost` errors rather than a normal close. The last observed scene was a dark rotating Mini preview; its exact phase was unconfirmed, and a Scapes-to-World-Map return test was not completed. The cause is unresolved; no red-zone attribution, FPS gain or crash repair is established. The public starter remains unchanged with red-zone protection off, and the previously published Windows ZIP is unchanged.
- Separately, the existing owned CPU red-zone patcher fixture passed nine modeled cases. These deliberately controlled faults do not prove Windows overwrote the game's stack or that either crash is repaired. Red-zone protection remains off in the portable starter and first vehicle-preview trial.
- The portable starter retains the reviewed lower-AA/resolve, file backing and terminal external-call probe settings. Software R8 comparison, optional diagnostics and lookup remain off.

## r34 - 2026-10-08 - 8c291f1

- Port [upstream b38b745](https://github.com/KytyPS5/KytyPS5/commit/b38b7454a6bc21f5d68bea825214fac60f53f80a) to preserve real vertex-export locations when a pixel shader reads the same export with both flat and smooth interpolation. Respect first/last provoking-vertex selection for ordinary primitives and retain rectangle interpolation's separate mapping.
- Include interpolation mode in shader cache identity. Coherent build and eight CPU regression groups passed; four authored native pixel modules and rectangle regressions passed CPU emission/default Vulkan 1.2 SPIR-V validation. No new GPU test is claimed for this port.
- Add default-off experimental R8 point/bilinear software comparison while retaining supported native depth comparison. Its earlier authored tests passed 31 modules and 33 manual Vulkan dispatches; these tests are separate from the current interpolation checks and do not prove production texture imports or AMD comparison equivalence. An actual software-enabled startup rejected unsupported status requests before Garage, so the portable starter keeps the experiment off.
- Add an optional exact pixel-shader trace selector and bounded first-failure metadata for the R8 experiment. The report retains the original strict rejection and is limited to 32 KiB. Optional witnesses, vehicle traces and lookup trials also remain off.
- This revision reached Garage; the user reported that the vehicle looked about the same. Incorrect vehicle paint/materials/lighting/shadows, missing details, music silence and the prior native fiber crash remain unresolved. No wheel/lighting repair, playable race or FPS improvement is claimed. Scapes Movies ran, but returning to World Map ended in a native read access violation; cause unresolved.

## r33 - 2026-10-08 - 8f8d34d

- Raise the optional geometry-candidate trace limit from 32 KiB to 256 KiB per record so a larger binding record can be retained. The ordinary trace remains 32 KiB per record. Both modes retain the 64-record and 1 MiB total limits, duplicate suppression and scene activation control.
- CPU bound/flag checks and the coherent emulator/compute-test build passed. Rendering commands and shader admission are unchanged. The public starter leaves this and all optional diagnostic traces off.
- Actual geometry tracing retained 34 complete indexed/depth-tested candidates and 432 pixel-shader image associations, with zero oversized records. The bounded total was 1,046,290 bytes. The run closed normally while vehicle appearance remained dark or flat.
- A moving-preview lookup comparison measured about 8.53 versus 8.30 completed guest flips per second, but differing shader counts and camera motion prevent a matched A/B conclusion. Lookup remains off in the public starter.
- No vehicle material, lighting, shadow, crash or FPS repair is claimed. A native fiber stack write access violation observed in a prior Garage run remains unresolved.

## r32 - 2026-10-08 - 1f6917e

- Add an optional filter that retains draw candidates with a source index/vertex count of at least 128 or enabled depth testing. The label is a geometry candidate, not proof that a draw renders a vehicle.
- Exact flag, bounded trace and source checks passed. The first filtered capture reached its scene activation control but could not retain its first eligible record within the old 32 KiB record limit.
- This diagnostic filter does not change image contents, render commands or visual quality. It is disabled by default.

## r31 - 2026-10-08 - cf9ce5a

- Add opt-in metadata at the existing invalid-buffer-range failure guard, associating the requested range with current shader buffers or merged vertex-buffer slots. Records are bounded to 32 KiB, and the original range query and fatal rejection remain unchanged.
- The diagnostic is disabled by default, including in the portable GT7 starter. CPU gate/query/callback fixtures and three production translation-unit syntax checks passed; the coherent emulator and compute-test build completed. No new game/GPU validation is claimed for this update.
- The selected replay/movie guard failure is not fixed. Its responsible stage/resource and the remaining material, lighting and shadow faults are still unresolved. No FPS improvement is claimed.

## r30 - 2026-10-08 - 90d58d1

- Add a default-off vehicle draw trace that associates final shaders, image/view/sampler bindings and attachments. The trace is bounded and can be activated at a chosen scene boundary.
- Validate the diagnostic's limits, flags and source seams with CPU fixtures and a coherent build. No rendering commands or shader admission rules were added by this update.
- Vehicle geometry was visible in the earlier r29 runtime, but correct materials, paint, lighting and shadows remain unresolved. The trace is a diagnostic, not a visual repair.

## r29 - 2026-10-08 - 3e68886

World Map and Garage were reached, and rotating vehicle geometry was observed. The old fixed-function resolve rejection was passed. Correct lighting, paint, shadows, music and playable races remain unresolved; menu cadence remained around four completed guest frames per second. An earlier car-menu run also ended with GPU device loss; no specific cause or repair has been established.

| Commit | Emulator change | Validation boundary |
| --- | --- | --- |
| `03a2ef6` | Lower and bind indirect image writes. | Regression checks passed. |
| `91a279f` | Add bounded external-call diagnostics. | Reports context without executing the callee. |
| `c4b07d2` | Expand shader-library diagnostics. | Diagnostic evidence only. |
| `971a882` | Retain bounded library target snapshots. | Incomplete inputs reject. |
| `2b500ba` | Add a bounded external-program prototype and compute-input capture. | Owned compiler/GPU fixtures; actual returning calls unproved. |
| `b408790` | Improve IR use tracking and captured compute controls. | CPU regressions; no live FPS claim. |
| `7c30ce6` | Add terminal target probes and cache checkpoints. | Stops before selected callee execution. |
| `378f314` | Add conservative unwritten-register admission and pre-BVH probes. | Unknown or excluded targets reject. |
| `856089c` | Capture BVH ray/node records and stabilize shader IDs. | Owned same-winner tuple checks. |
| `b035f5a` | Inspect resident BDA entries and buffer owners. | Post-completion mapping evidence, not event-time proof. |
| `51a902c` | Observe bounded authentic resource reads. | Complete actual read capture; missing/conflicting reads reject. |
| `3dcf528` | Add structured terminal-call probes. | Captured CPU/SPIR-V and owned GPU checks. |
| `3c5aedc` | Add after-BVH result probes. | Owned GPU checks and an actual BVH result capture. |
| `a2fec9e` | Preserve a transported auxiliary pair inside the original record span. | Native cases; ambiguity/clobber guards retained. |
| `22ca879` | Remap image metadata only for surviving consumers. | CPU red/green and captured SPIR-V checks. |
| `8d87b63` | Report unsupported render-target sample state. | Existing rejection retained. |
| `2c30762` | Add optional Windows file-backed direct memory. | Production alias/protection/zeroing/reuse checks. |
| `84dce21` | Retain accepted raw sample registers and provenance. | Exact packet-body CPU checks. |
| `fac66f5` | Capture selected-callee resources on the CPU. | Strict identity/dependencies; no shader dispatch. |
| `ac27d58` | Retain pixel-shader sensitivity and add a lower-AA 2x trial. | Policy/owned rendering checks; approximate EQAA. |
| `5d9bd94` | Capture all current matching body contexts; add menu counters and facts without broad dumps. | Actual 28-context CPU capture, not GPU election. |
| `f6ffb81` | Permit explicit selected-only capture without broad dumps. | Exact gate checks; default dump behavior retained. |
| `f1e00a0` | Share exact scalar write widths across provenance checks. | Legal native red/green and full CPU regressions. |
| `dad2e76` | Add warm renderer counters and an optional empty-poll shortcut. | Scheduler checks; no convincing live gain. |
| `d9f3a06` | Measure draw phases and completed Vulkan wait calls. | Inclusive timings, not an exclusive time partition. |
| `e733690` | Add optional exact-image lookup through the first ownership page. | CPU parity and owned GPU off/on checks; live gain unproved. |
| `fe2b30d` | Correct mapped ONE/SRC_ALPHA blending with zero source-alpha contribution. | Arithmetic and owned rasterization/readback checks. |
| `2600317` | Track architecture-derived AA reset masks and admit a strictly absent-depth lower-AA case. | Policy/PM4 and native owned rendering checks. |
| `3e68886` | Support a bounded 2x-to-1x resolve at mip zero with extra destination mips. | Native edge/center readback and untouched-mip checks; actual resolve progressed. |

## Unresolved work

- Vehicle materials, lighting, shadows and uniform paint remain incorrect or incomplete. A selected replay/movie rendered a moving car with wrong appearance before an invalid graphics-buffer-range guard stopped it; the responsible stage/resource is unidentified.
- Playable races and actual returning external shader calls are unproved. Terminal probes can deliberately stop those paths.
- A native fiber stack write access violation observed in a Garage run remains unresolved. The diagnostic updates do not fix it.
- Menu performance remains low; no supported FPS-gain claim is made.
- Music silence is user-reported and unresolved.
- Finite returning-call and certified image-status work is not included merely because its owned fixtures pass.
