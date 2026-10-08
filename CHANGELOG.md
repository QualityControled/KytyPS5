# Changelog

This list covers emulator changes after `KytyPS5-2026-10-07-d2413fc`. Experimental settings are opt-in. Owned fixtures and captured CPU validation are separate from actual game observations.

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
- Menu performance remains low; no supported FPS-gain claim is made.
- Music silence is user-reported and unresolved.
- Finite returning-call and certified image-status work is not included merely because its owned fixtures pass.
