# Dragon Aspect Flight: source review and fix plan

Reviewed: 2026-09-08. Status: **implementation complete through Checkpoint B;
runtime acceptance and any release remain pending**.

## Scope and evidence boundary

Reviewed `Handoff.md` and the related runtime, animation-generation, validation, and build code in `DragonAspectFlight`, branch `agent/flight-combat`, implementation commit `3f8096080340c82cd2d2efa5004468b57f15355e` plus follow-up diagnostic hardening `b63c803`. Source line references below refer to this workspace snapshot.

The handoff distinguishes this experimental combat branch from the public `v1.6.0` basic-flight/compatibility release. This review does not establish that the findings occur in that separate public release. Its reported installation was not redeployed or independently re-audited here. Preserve the working port, its external-animation references, and the user's no-bundling requirement for that variant.

The review findings were implemented in the source/tools/tests and the reviewed
Data candidate was reconciled into commit `3f80960`. No installed mod, public
release, or remote branch was changed, and no game was launched. Static and
clean-export verification is complete; runtime behavior remains unproven.

Codebase Memory was tried first. Its `flying-plugin` generation was `2026-08-29T22:14:21Z`; coverage marked relevant files changed or untracked, and snippet ranges actually returned unrelated current code. Findings therefore use direct source reads. This is a targeted review, not an exhaustive audit of every engine relocation or combat path.

## Findings

### F1 — P1: the committed source and Data are not a passing release unit

Status: **resolved in `3f80960`**. The failure described below was the
pre-implementation reproduction; the committed Data now passes the clean
export gate.

Original evidence: `tools/test_build_flight_animation_stack.py:232,283,325`,
the pre-fix `Data/` tree, and the local-only Data warning in the old
`Handoff.md`.

Before the repair, the workspace animation suite passed **10/10** while the
same command against a fresh `git archive HEAD` export failed **8/10**. The
committed candidate now passes **10/10** in both locations. The original
failures included:

- Draw/sheath aliases and `mco_attack1` through `mco_attack10` still present where the new policy forbids them.
- Coverage manifest/file disagreement.
- Different greatsword, quarterstaff, one-handed/shield, and priority rules.
- Missing sprint/stance aliases.

The handoff's green suite is evidence for a dirty local source-plus-Data combination, not the committed GitHub tree. Compiling that committed tree can stage the old animation policy alongside the new runtime. This is a reproducible packaging/repository defect, not proof that it caused a particular gameplay report.

### F2 — P1: rollback failure can delete the last good animation tree

Status: **resolved in `3f80960`** with retained transaction/backup paths and
fault-injection regressions.

Evidence: `tools/BuildFlightAnimationStack.py:950-976`.

`_atomic_replace_directory` renames the old tree to a backup, attempts installation, and attempts restoration on failure. If restoration also fails, the `finally` block still deletes the backup. A private fault-injection test made both install and restore rename operations raise `PermissionError`; the result was `target_exists=false`, `old_content_copies=0`.

There is a second transaction boundary problem: the OAR tree is installed at line 1233, but coverage and hash manifests are written afterward at lines 1235-1245. A failure there can leave new assets with old metadata. The whole candidate, not just one directory, must be transactional.

### F3 — P1: round-trip verification does not verify the generated lower body

Status: **resolved in `3f80960`** with full-track, finite-value, metadata, and
atomic-output checks.

Evidence: `tools/AerializeHkx.py:348-373,469-510,551-589`.

The writer receives `composite`, but verification receives the original `action` and explicitly skips every replaced track. An arbitrarily corrupted pelvis translation therefore passes. The validation also checks sample counts without validating all scalar values; a NaN in an untouched upper-body translation passed both structure and round-trip checks in the review probe.

This means the code's strongest advertised protection does not establish that the intended lower-body result survived serialization. It also lets non-finite transforms evade numeric comparisons.

### F4 — P2: interleaved channel inspection makes an invalid assumption; real codec coverage is missing

Status: **partially resolved in `3f80960`**. Interleaved float/root-motion
fields are inspected and unsupported representations fail closed; a verified
production interleaved codec is still not available in this workspace.

Evidence: `tools/AerializeHkx.py:147-181,513-527`; local schema reports `help/rpt/hkaAnimation_1.rpt` and `hkaInterleavedUncompressedAnimation_0.rpt`; `tests/test_diagnosis_plan_tools.py:1-6`.

The inspector reports zero float tracks and no extracted motion for any interleaved animation without reading those fields. The local schema explicitly contains the inherited `numberOfFloatTracks` and `extractedMotion` fields and the interleaved `floats` array. These channels cannot be declared absent from the animation class name.

The installed PyNifly backend at `C:/Users/micha/Desktop/Projects/Blender mesh editor/tools/blender/portable/scripts/addons/io_scene_nifly/hkx/anim_skyrim.py` also rejected the actual `Flight Base 10 - Unarmed/h2h_attackleft.hkx` with `No hkaSplineCompressedAnimation found in HKX file`, although the DAF inspector accepted it. This establishes a limitation of that inspected local backend, not every PyNifly version.

A header/section census is not a successful decode/composite/write/reload test. The synthetic backend remains useful, but cannot prove production codec compatibility. No finding here establishes that the current assets actually contain unsupported float/root channels.

### F5 — P2: duplicate flight activation mutates presentation before its idempotency check

Status: **resolved in `3f80960`**; the already-flying check now precedes all
graph probes.

Evidence: `src/FlightManager.cpp:555-567,1014-1056`; queued dispatch at `2710-2741`; `src/Papyrus.cpp:12-16`.

`StartFlight` probes the graph before checking `_isFlying`. The probe writes `bDAF_FlightActive=false` and `iDAF_FlightState=0`. A duplicate start then returns at the already-flying guard, leaving those writes behind until a later successful update. The queued Papyrus start path can reach this method.

The source-level violation is confirmed; whether a one-update presentation gap is visible depends on runtime scheduling. The fix should make duplicate activation a true no-op, not add another animation event.

### F6 — P2: worker-start failure does not roll back acquired flight state

Status: **resolved in `3f80960`**; worker startup returns a result and StartFlight
rolls back the session on failure.

Evidence: `src/FlightManager.cpp:1056,1109-1113,1158-1175,5971-6031`.

Flight state, graph variables, and controller overrides are claimed before `StartUpdateThread`. That method returns `void`; join/start failure paths log and return, with thread-construction exceptions clearing only `_threadRunning`. The caller cannot detect failure and release the session. The exceptional path can leave flight active without its normal update pump.

This is a source-proven recovery gap, not a reproduced resource-exhaustion event in Skyrim.

### F7 — P2: controller/presentation ownership does not cover replacement or gate loss

Status: **resolved in `3f80960`** for the identified ownership/gate-loss paths;
live replacement behavior still needs a Skyrim acceptance trace.

Evidence: `include/DragonAspectFlight/FlightManager.h:180-183,294-295`; `src/FlightManager.cpp:1109-1113,1563-1570,6235-6247,6930-6968`.

Gravity/friction restoration is protected by a Boolean token, but the captured baseline has no controller identity or generation. Stop restores it to whatever controller the player currently returns. If a controller is replaced while the actor remains loaded, that can apply controller A's baseline to controller B.

The active update also ignores the Boolean result of `SetFlightGraphVariables` and continues into movement. Initial graph-gate preflight therefore does not cover graph replacement or later essential-gate loss. These are conditional lifecycle defects: the write paths are present, but the replacement/loss scenario was not reproduced in game.

### F8 — P2: exact-original manifests allow ambiguous outputs and incomplete provenance

Status: **resolved in `3f80960`** for duplicate/case-colliding targets,
generated-report collisions, path escapes, and backend/base provenance.

Evidence: `tools/rebuild_flight_actions.py:46-61,90-153`.

Duplicate targets pass manifest validation. A probe staged two distinct sources to `same.hkx`; the build succeeded with two manifest rows but only one matching the final file. Case-insensitive collisions are also relevant on Windows. Source hashes are pinned, but the flight-base contents and codec/tool identity are not recorded as content provenance.

This compromises the claim that a manifest uniquely identifies a reproducible output set.

## What is already improved, and should be retained

- Start refuses an absent/unloaded actor, absent controller, and missing initial graph gates.
- Stop no longer directly fabricates a grounded controller state and restores the captured friction value.
- Worker lifecycle no longer uses the previous detach fallback; update tasks have generation/session checks and a single-flight lease.
- The monitor worker queues work; actor/settings/HUD work is performed in its game-thread poll.
- Equipment identity/epoch reducers, bounded fallback logic, and correlated diagnostics are meaningful safeguards. Do not replace them with unconditional draw/sheath/attack-state writes.
- `tests/flight_state_tests.cpp` explicitly undefines `NDEBUG` before including `<cassert>`. Release assertions are active: this is **not** a finding.
- Magicka drain uses elapsed time (`FlightManager.cpp:3781-3804`); the existence of `TickSeconds` alone does **not** establish frame-rate-dependent drain.

## Recommended design direction

Keep the working public compatibility port isolated. For the combat branch, finish one narrowly supported, reproducible animation profile before promising arbitrary moveset support. OAR replacement of selected filenames is not a persistent lower-body blend layer; increasing priority or adding more generic aliases will not solve unknown action contracts.

The shortest credible route is: trustworthy transactions and validation, one pinned source-plus-Data candidate, then focused lifecycle fixes and actual animation-winner evidence. Defer a new behavior/masked-blend architecture until a curated profile demonstrates a specific continuity limitation that offline composites cannot meet. Such a redesign should be a separate prototype, not mixed into the repair candidate.

Longer term, replace the manager's parallel transition fields with one explicit weapon transaction containing identity, epoch, target, phase, deadline, and retry budget. Begin by putting existing behavior behind a tested engine adapter; do not combine that refactor with new animation semantics. Moving the same flags into another file alone would not simplify the design.

## Ordered implementation tasks

### Task 1 — establish the candidate boundary and reproduce the release gate

Status: **complete** in `3f80960`.

Dependencies: none. Scope: small; `Handoff.md`, animation test/CI entry point, candidate manifest.

The previously dirty Data was inventoried and reconciled without discarding
source assets. The handoff documents both the known-good public port and the
combat candidate by immutable source commit, Data manifest hash, DLL hash,
runtime profile, and required external sources.

Acceptance:

- A clean-export test reproduces the fixed candidate's passing release gate;
  the pre-fix 8/10 failure is retained as the named regression history.
- No existing Data edits, artifacts, or installed files are discarded.
- Port and combat candidate cannot be mistaken for each other in manifests/logs.

Verification: the exact `git archive HEAD` export and workspace both pass the
animation suite 10/10; the public compatibility port remains documented as a
separate artifact.

### Task 2 — make animation publication recoverable as one unit

Status: **complete** in `3f80960`.

Dependencies: Task 1. Scope: medium; `BuildFlightAnimationStack.py`, filesystem regression tests, one staging helper if needed.

Use explicit transaction phases. Delete an old backup only after complete installation succeeds. If restoration fails, retain its exact path and original error, and report recovery instructions. Stage OAR assets and their coverage/hash metadata together; do not publish the tree before its metadata is ready.

Acceptance:

- Failure injection at each rename and metadata-write boundary preserves either the old complete candidate or a discoverable intact backup.
- Double failure never deletes the last good tree.
- Successful publication has exact asset/coverage/hash parity, with no obsolete files.

Verification: Windows temporary-directory tests, including install failure, restore failure, and post-asset/pre-manifest failure. Do not use the modlist as the test target.

### Task 3 — verify all animation output, not just preserved tracks

Status: **complete** in `3f80960`.

Dependencies: Task 1. Scope: small; `AerializeHkx.py`, compositor tests.

Validate finite durations, annotations, and transform components, component dimensions, and valid rotations. Verify the written result against the expected composite for every track. Separately verify untouched tracks/events/bindings against the original to preserve the action contract. Use rotation-aware comparison so equivalent quaternion signs are not falsely rejected.

Acceptance:

- Corrupt replaced tracks, missing samples, NaN/Inf values, and invalid rotations fail before final-file replacement.
- Untouched action channels and metadata retain their declared tolerances.
- Rejected outputs leave an existing destination unchanged.

Verification: convert the review probes into regression tests; add valid quaternion-sign and tolerance-boundary cases.

### Task 4 — define and test a real binary codec contract

Status: **partially complete** in `3f80960`, with follow-up diagnostic
hardening in the current working change. Binary channel inspection and
fail-closed policy are implemented, but a production-verified interleaved
codec fixture is still an explicit follow-up.

Dependencies: Task 3. Scope: medium; `AerializeHkx.py`, codec integration tests/fixtures, backend lock/provenance documentation.

Read the common animation channel fields and fixups for each explicitly supported representation. Unknown or unverified channels must produce “unsupported/unverified,” not fabricated zero values. Pin the backend used for releases. Either support real interleaved inputs through a verified backend, or reject them clearly before a batch begins; do not silently normalize them through an untested conversion.

Acceptance:

- Representative real compressed and interleaved fixtures follow an explicit supported/rejected policy.
- Fixtures with float tracks or extracted motion are recognized correctly; no silent channel loss occurs.
- A real decode/write/reload test validates all Task 3 invariants for each supported format.

Verification: synthetic tests plus separate real-codec integration tests. Record backend revision and fixture hashes; use redistributable fixtures or fetch external fixtures under an explicit provenance policy.

The follow-up also preserves the detected compressed/interleaved
representation on the structural inspection record, so a later decode failure
reports the actual representation instead of `unknown`, and verifies skeleton
identity using every supported backend attribute spelling. These are diagnostic
integrity fixes; they do not claim interleaved codec support.

Checkpoint A: Tasks 2-4 safety tests are green; production interleaved codec
support remains intentionally unclaimed.

### Task 5 — rebuild one coherent source/Data/package candidate

Status: **complete for the static candidate** in `3f80960`; packaging and
publication remain deliberately pending runtime acceptance.

Dependencies: Tasks 1-4. Scope: manifest validation/provenance plus a reviewed
Data reconciliation commit.

Reject duplicate/case-colliding targets, report-file collisions, invalid SHA-256 values, and resolved path escapes before output work. Record base SHA-256, codec/tool revision, generation options, and exact source hashes. Resolve the local-only Data changes deliberately, preserving action-slot ownership and licensing boundaries. Do not indiscriminately commit every dirty file or add bundled animations to the public port.

Acceptance:

- Every target has exactly one source/policy and every manifest row matches a final file.
- The reviewed source commit alone reproduces the intended Data or verifies a pinned asset set; local untracked aliases are not required.
- Workspace and clean-export animation tests agree at 10/10; F1's 8 failures
  are resolved without weakening its checks.

Verification: fresh clean source export; full tests; independently extract the candidate archive and compare its complete manifest. Keep the old candidate intact.

### Task 6 — make flight acquisition idempotent and transactional

Status: **complete at source-contract/static-test level** in `3f80960`;
runtime fault injection through a game adapter remains future work.

Dependencies: Task 1. Scope: medium; `FlightManager.cpp/.h`, lifecycle adapter tests.

Handle already-flying activation before any graph write. Make initial presentation probing non-destructive where possible. Return an explicit start result from the update-pump lifecycle. Roll back a failed new session on the game thread after releasing locks, without accidentally retaining a ground observer or retry pump.

Acceptance:

- Repeated Start performs zero graph/controller writes and does not advance an existing session.
- Simulated thread-construction/join failure leaves no newly acquired flight override or stranded input ownership.
- Successful start retains the existing safe preflight and normal combat graph behavior.

Verification: inject graph/controller operations and worker creation into a narrow production-facing test seam. Source regex presence checks may supplement, but cannot replace, these tests.

### Task 7 — guard controller and graph generations throughout flight

Status: **complete at source-contract/static-test level** in `3f80960`;
runtime controller replacement and gate-loss traces remain future work.

Dependencies: Task 6. Scope: medium; `FlightManager.cpp/.h`, ownership/lifecycle tests.

Associate the baseline with the acquired controller's live identity/generation. Never dereference a stale saved controller. On replacement or absence, retire the old ownership safely and either abort or explicitly acquire a new verified baseline according to one documented policy. Distinguish essential graph-gate failures from optional diagnostic variables, and handle essential loss before further flight movement.

Acceptance:

- Controller A's saved values are never restored onto controller B.
- Missing/replaced controller and lost essential presentation gates have bounded, observable cleanup behavior.
- Repeated stop, actor unload, save/load, and a stable-controller flight preserve idempotence and current block/weapon ownership protections.

Verification: adapter-based controller-swap/gate-loss traces plus the existing reducer suite. Separately test unknown terrain height/contact confidence; `GetLandHeight` failure currently returns true, but the caller also requires water/controller contact, so it is not evidence of arbitrary midair landing by itself.

Checkpoint B: **passed** from commit `3f80960`; unified native rebuild and all
static/package gates are green. No deployment before gameplay review.

### Task 8 — close the diagnosis gap and obtain focused user acceptance

Status: **static identity/logging portion complete; runtime acceptance pending**.

Dependencies: Checkpoint B. Scope: two small changes: diagnostic evidence bundle, then test-candidate packaging.

Keep correlated action/session/equipment logs, but add precise source revision, Data manifest identity, dependency versions, and terminal reason summaries. Obtain actual requested-original/selected-replacement information through a verified OAR API or its supported diagnostics. If unavailable, say `unknown` and provide a capture procedure; do not label the equipment-derived expected family as the measured winner. Keep symbols privately alongside the exact Release DLL for crash analysis, not inside the mod payload. Profile synchronous logging before changing its delivery policy.

Acceptance:

- One evidence bundle distinguishes input consumed, request issued, transition pending/completed/failed, and known versus unknown animation selection.
- Runtime results are tied to the exact candidate and not mixed with old main/port logs.
- User acceptance is recorded separately for draw/sheath, normal/power attacks, weapon swaps, block/bash, repeated shouts followed by landing, descent, and magicka recovery.

Verification: parser/replay tests first. The user performs gameplay testing, initially with a small declared profile and then the Nolvus profile. Validate SE/AE/VR and the claimed newer-runtime combinations independently; BIN/header validation and compilation do not prove engine-layout or gameplay compatibility. Publish only the combinations actually established, with limitations stated.

## Verification performed during this review

- Rebuilt the unified Release plugin and `DragonAspectFlightFlightStateTests`
  from commit `3f80960` with `/t:Rebuild /m:1`: both exited 0.
- Fresh CTest invocation at `C:/tmp/DAF-diagnosis-plan-20260907-r1`: **9/9 passed**.
- `py -3 -m unittest discover -s tests -p "test_*.py"`: **53/53 passed**;
  structured logging pytest: **9/9 passed**.
- Workspace and exact committed-tree export
  `py -3 tools/test_build_flight_animation_stack.py`: **10/10 passed** each.
- PE validation passed for the rebuilt 993,280-byte x64 DLL with zero forbidden
  imports; exports/dependents were checked with `dumpbin`.
- Supplied 1.7.104 Address Library BIN passed format-5/runtime/hash/16-ID
  validation.
- Isolated probes were converted into regressions for lower-body verification,
  non-finite values, duplicate targets, rollback retention, metadata atomicity,
  controller ownership, graph-gate loss, and candidate identity logging.
- Probe/export directory: `C:/tmp/DAF-review-20260908-023206`; executable reproduction script: `review_probes.py`. Its destructive-failure simulation uses only private temporary fixtures, never source assets or installed files.

## Definition of done and remaining uncertainty

Each fix has a regression that failed on the reviewed version and passes after
the change, plus a clean-tree verification checkpoint. Existing green tests are
retained, not relaxed to conceal failures. Runtime-only observations remain
pending until the user tests the exact candidate.

No claim of perfect log-only reconstruction, arbitrary-modlist compatibility, or continuous masked flight blending is justified yet. Conversely, these findings do not justify discarding the working public port or rewriting all runtime state management. Fix the demonstrated failures first, preserve the proven safeguards, and use measured animation selection to choose the next gameplay change.
