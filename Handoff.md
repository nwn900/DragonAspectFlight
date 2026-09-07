# Dragon Aspect Flight — Handoff

Last updated: 2026-09-07

This document records the repository, release, and installed-mod state at the
time of handoff. It separates static source/build evidence from behavior that
still requires a Skyrim playthrough.

## Executive status

There are two different artifacts in play:

| Artifact | Current state |
| --- | --- |
| Repository branch `agent/flight-combat` | Current commit `e3efa05c66005d565880f8d5e8e7d09317c62a93`, pushed to `origin`; implementation changes are in its parent `a1260bb68b58b528a6ad565468c516ec208dadad`. |
| Latest published GitHub release | `v1.6.0` on `main`; five-file, rootless ZIP with no bundled animation HKX files. |
| Active Nolvus installation | GitHub `v1.6.0` is installed in the enabled Nexus-compat folder. |
| Diagnosis-plan implementation | Built and statically tested in an isolated build directory, but not packaged as a public release and not deployed to the game. |
| Runtime gameplay validation | Not run for the diagnosis branch or for the `v1.6.0` deployment. |

The current installed variant is:

```text
C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\[NoDelete] Dragon Aspect Flight - Nexus v1.5 Compat 1.7.99-1.7.104
```

Housecarl confirms that this mod is enabled and is the VFS winner for its DLL,
INI, BDI file, and both OAR reference configurations. The separate full folder
below is disabled and was not changed by the `v1.6.0` deployment:

```text
C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\[NoDelete] Dragon Aspect Flight
```

## What the project does

Dragon Aspect Flight is a native SKSE plugin. It has no ESP/ESM/ESL plugin and
no Papyrus gameplay script. Behavior Data Injector (BDI) declares the graph
variables and Open Animation Replacer (OAR) selects flight-scoped animation
replacements. The normal Skyrim combat graph remains authoritative; DAF does
not require the generated Jumping Attack/Nemesis/Pandora behavior topology.

The intended split is:

- native code owns flight activation, movement/controller state, input routing,
  equipment observation, and graph-variable updates;
- BDI supplies the DAF graph variables;
- OAR supplies flight-scoped visual replacements;
- the installed combat framework remains responsible for normal attack,
  block, bash, draw/sheathe, spell, staff, bow, crossbow, and shout events;
- More Draconic Aspect Can Fly supplies the donor flight animations in the
  public no-bundled-animation package.

## Implemented on `agent/flight-combat` (`a1260bb`)

### Native runtime and lifecycle

- Flight activation preflights the loaded player, controller, and essential
  graph gates before claiming the flying state.
- `bInJumpState` is forced false only while DAF owns flight. Stopping flight no
  longer fabricates an on-ground controller state.
- The original no-friction state is captured and restored instead of being
  unconditionally cleared.
- Actor/3D unload paths abort safely, clear actor-bound transition state, and
  avoid rearming a no-actor observer.
- Worker start/stop/reset paths use serialized `jthread` lifecycle handling;
  the old detach fallback is gone.
- Queued update work uses session/generation checks and a single-flight lease,
  rejecting stale callbacks and releasing the lease on all handled exception
  paths.
- Equipment identity and epoch checks guard delayed draw/sheathe, block, and
  fallback actions against applying to a replacement weapon set.
- Monitor work is split so engine reads, settings/UI formatting, and HUD calls
  run on the game thread; the worker only queues a poll and sleeps.
- Structured diagnostic logging, bounded snapshots, and log rotation are
  present in the source and covered by a logging contract test.

These changes reduce unsafe state and lifetime transitions, but they do not by
themselves prove that every engine animation transition behaves correctly.

### Animation and source-repair tooling

- `tools/AerializeHkx.py` resolves tracks by binding identity instead of track
  position, validates skeleton/binding/metadata/loop seams, preserves additive
  and verified upper-body-only clips, rejects unsupported channels, and uses
  temporary output with atomic replacement.
- `tools/BuildFlightAnimationStack.py` classifies action/offset slots, refuses
  to use intro/outro/loop/dodge/dummy/roll clips as flight idles, protects
  magic/staff action slots, validates decoded XML, stages the complete OAR
  tree, and commits it only after validation.
- `tools/BuildFlightBaseAnimations.py` verifies that the declared HKX output
  was actually created and is non-empty.
- `tools/rebuild_flight_actions.py` builds a separate action staging tree from
  an exact-original manifest and SHA-256 inputs. It records unresolved inputs
  instead of inventing generic replacements.
- `tools/apply_daf_source_patch.py` applies hash-pinned source changes to a
  separate copy, preserves CRLF files, optionally checks Git blob IDs, emits a
  diff, and marks the result uncompiled and undeployed.

### Compatibility and build gates

- The native project builds one DLL for Skyrim SE, AE, and VR through
  CommonLibSSE-NG multi-targeting.
- The CMake capability gate requires an official CommonLibSSE-NG checkout with
  the generic `REL::IDDB::Format::SSEv5` / `header_v5_t` / `load_v5` parser. It
  intentionally does not require an upstream symbol named
  `SKSE::RUNTIME_SSE_1_7_104`.
- The external AE 1.7.104 Address Library BIN was validated as format 5,
  runtime `1.7.104.0`, 565,759 dense offsets, and 16/16 audited IDs. Its
  recorded SHA-256 is
  `8AAB3DD251D135B849BD983F86A4A205C920FA3E81F8E30C0E63CCFEF9423842`.
- Runtime layout compatibility beyond the generic Address Library parser still
  requires live validation on the target game build.

### Tests added

The branch registers coverage for native reducers, Address Library validation,
PE imports, structured logging, animation-stack policy, the diagnosis tools,
runtime repair contracts, CommonLib capability, and the final DLL import set.

## Static verification evidence

The diagnosis branch was built in:

```text
C:\tmp\DAF-diagnosis-plan-20260907-r1
```

Evidence recorded for that snapshot:

- Native Release rebuild completed successfully with MSVC. The only warning
  was the pre-existing `C4099` ImGuiMCP type warning in
  `SKSEMenuFramework.h:1386`.
- `ctest --test-dir C:\tmp\DAF-diagnosis-plan-20260907-r1 -C Release --output-on-failure`:
  **9/9 passed**.
- `tests/test_diagnosis_plan_tools.py`: **18/18 passed**.
- Repository HKX structural scan: 1,045 files (796 compressed and 249
  interleaved-uncompressed), with zero structural rejects.
- Release DLL: 990,208 bytes,
  SHA-256 `12930DDB676D7FCB493C529102926D7B6AD89FA94C5133C6556FC5544322136E`.
- PE validation passed for x64 with zero forbidden imports. `dumpbin` showed
  only `SKSEPlugin_Load`, `SKSEPlugin_Query`, and `SKSEPlugin_Version` exports
  and only CRT/Windows dependents.
- No Release PDB was emitted because `GenerateDebugInformation=false`.

These are static/build results. They are not evidence that a Skyrim session
will select the expected OAR winner or that weapon transitions, attacks,
blocking, bashing, shouts, or flight presentation are correct in play.

## Public release and installed state

GitHub `v1.6.0` is the latest published release as of this handoff. Its release
notes state:

- support for Skyrim 1.7.99 and 1.7.104 while retaining older SE, AE, and VR
  support;
- mid-flight shout reassignment through Favorites and Magic menus;
- exact OAR references to More Draconic Aspect Can Fly;
- no bundled animation HKX files; More Draconic Aspect Can Fly remains a
  requirement.

The downloaded release ZIP matched its published sidecar:

```text
DragonAspectFlight-1.6.0.zip
SHA-256: 21B0636C3773BBC4A8BB88790922CDF00161D42ACB6D4193B08E23FF60C31A2B
```

The enabled Nexus-compat folder contained exactly five files before and after
deployment. Only the DLL changed:

```text
new DLL SHA-256: 76B632EE6C12D7C09F8E6B9B3FD54EAB609B2C65C1B9DA46B5C69E55E994B990
```

The pre-update rollback copy is:

```text
C:\Users\micha\Desktop\Projects\Flying plugin\DragonAspectFlight\artifacts\deployment-backups\DragonAspectFlight-Nexus-v1.5-compat-before-v1.6.0-20260907-135606
```

No game or SKSE process was running during the copy. Housecarl re-resolved the
active profile afterward and confirmed the updated files still win. Runtime
testing was deliberately not performed.

## Known shortcomings and open risks

1. **No live acceptance pass.** The diagnosis branch has not been launched in
   Skyrim. Walking-on-air, stuck weapon state, attack blocking, bash/block
   ownership, shout behavior, and post-landing recovery remain runtime
   questions.
2. **No continuous presentation layer.** The animation repair remains offline
   composition and OAR aliasing. It does not provide a persistent, phase-
   synchronized lower-body flight layer blended under arbitrary combat clips.
3. **OAR/framework coverage is not universal.** A filename manifest and a high
   OAR priority do not prove that a particular Stances, MCO, custom weapon, or
   other framework original is requested and won. Actual original and selected
   replacement traces are still required.
4. **1.7.104 layout certainty is incomplete.** The build and BIN gates verify
   static address-library readiness. They do not reverse-engineer or prove
   every post-1.7.99 engine layout, vtable, input, or SkyrimVM assumption.
5. **Module-unload boundary remains.** Queued callbacks are session guarded,
   but the codebase still has raw SKSE task callbacks that capture singleton
   state until execution, and there is no `SKSEPlugin_Unload` entry point.
   Shutdown/reinitialize testing is still needed.
6. **Release/source divergence.** The deployed public `v1.6.0` package is from
   the `main` release line. It does not contain the newer diagnosis-plan source
   tools or the `a1260bb` DLL.
7. **Dirty local asset tree.** The checkout still has many unstaged Data
   animation/config/manifest edits and deletions, four untracked HKX aliases,
   plus generated `.claude/` and `help/` directories. They were intentionally
   excluded from the GitHub commit and must not be included with `git add -A`
   until their purpose and final hashes are reviewed.
8. **Release symbols are absent.** The Release build has no PDB, so future
   diagnosis must rely on the structured log fields and a separately retained
   symbol-enabled diagnostic build if native stack resolution is required.

## Changes by milestone

### Public `v1.6.0`

- Added 1.7.99 and 1.7.104 support alongside the existing SE/AE/VR targets.
- Added mid-flight shout reassignment from Favorites and Magic menus.
- Switched flight animation selection to exact More Draconic Aspect Can Fly OAR
  folders.
- Kept suppression active outside shout-selection menus.
- Removed bundled animation files from the public package.

### Diagnosis-plan branch (`a1260bb`)

- Added native graph/lifecycle/controller safety and stale-action protection.
- Added game-thread monitor polling and structured logging contracts.
- Added binding-safe HKX composition and atomic animation-stack staging.
- Added exact-original action rebuild and source-only patch utilities.
- Added CMake/CTest coverage for the above contracts.

### 2026-09-07 deployment

- Updated the enabled Nexus-compat folder to the GitHub `v1.6.0` release.
- Took a restorable backup before copying.
- Verified release, staging, live hashes, and Housecarl VFS winners.
- Did not enable the disabled full DAF folder, launch the game, or publish a new
  release from `a1260bb`.

## Recommended next steps

1. Treat `a1260bb` as a source snapshot, not as a gameplay-approved release.
2. Decide whether the dirty Data tree is intended for the next package. Review
   every deletion and untracked HKX file against an authoritative source
   manifest before staging it.
3. Build a fresh candidate from the reviewed source and validate the exact
   Address Library BIN for the game being tested.
4. Run a small controlled MO2 profile and capture the actual OAR original,
   selected replacement, graph variables, weapon state, equipment identity,
   action event, and controller state for each failure.
5. Run the in-game acceptance matrix: hover/directional flight, draw/sheathe,
   single/dual/two-handed weapons, block/bash, bow/crossbow, magic/staff,
   shouts/Whirlwind Sprint, exhaustion/landing, menus, cell/3D changes, and
   30/60/120 FPS behavior.
6. Only after gameplay acceptance, package the branch as a new release, verify
   its rootless layout and hashes, and deploy it with a new rollback backup.

## Important paths and commands

| Purpose | Path or command |
| --- | --- |
| Repository | `C:\Users\micha\Desktop\Projects\Flying plugin\DragonAspectFlight` |
| Branch | `agent/flight-combat` |
| Remote | `https://github.com/nwn900/DragonAspectFlight.git` |
| Native source | `src/FlightManager.cpp`, `src/DragonAspectMonitor.cpp`, `src/InputHandler.cpp` |
| Native headers | `include/DragonAspectFlight/FlightManager.h`, `FlightStateHelpers.h`, `Compatibility.h` |
| Animation tools | `tools/AerializeHkx.py`, `BuildFlightAnimationStack.py`, `rebuild_flight_actions.py` |
| Source-copy tool | `tools/apply_daf_source_patch.py` |
| Tests | `tests/` and CMake registrations in `CMakeLists.txt` |
| MO2 instance | `C:\Games\Nolvus\Instances\Nolvus Awakening\MO2` |
| Skyrim Data root | `C:\Games\Nolvus\Instances\Nolvus Awakening\STOCK GAME\Data` |
| SKSE log | `Documents\My Games\Skyrim Special Edition\SKSE\DragonAspectFlight.log` (or the corresponding VR folder) |

For a future whole-package deployment, use Housecarl to set the MO2 instance,
read the active mod and asset winners, stop if Skyrim/SKSE is running, back up
the exact enabled folder, copy the rootless release payload, and ask Housecarl
to re-resolve the winners. Housecarl's asset-placement writer is intended for
single-file, new patch mods; it is not a substitute for a reviewed whole
release-package update.

## Git working-tree boundary

The GitHub branch is clean at `e3efa05` relative to its remote branch. The
uncommitted Data animation/config edits and generated folders described above
are local-only. Preserve them for review; do not reset, clean, or force-push
them without an explicit decision.
