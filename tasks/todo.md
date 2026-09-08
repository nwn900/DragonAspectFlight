# Dragon Aspect Flight review follow-up

Plan: [plan.md](plan.md). Review date: 2026-09-08. Implementation commit:
`3f8096080340c82cd2d2efa5004468b57f15355e`.

## Completed review and static implementation

- [x] Read handoff and relevant source; separate combat development from the public compatibility port.
- [x] Check graph freshness and use source fallback for stale ranges.
- [x] Reproduce the pre-fix clean-export animation failures: 8 of 10.
- [x] Rebuild the unified SE/AE/VR Release DLL and standalone tests.
- [x] Implement recoverable animation/metadata transactions and fault-injection tests.
- [x] Validate every serialized track, metadata field, finite value, and quaternion seam.
- [x] Inspect interleaved channel fields and fail closed for unsupported float/root-motion data.
- [x] Reject manifest collisions, unsafe paths, invalid hashes, and report-name collisions.
- [x] Reconcile the reviewed Data tree; workspace and clean export both pass animation 10/10.
- [x] Make duplicate flight activation a no-op and roll back failed worker startup.
- [x] Bind controller restoration to its owner and abort on replacement/essential gate loss.
- [x] Embed source/Data/CommonLib candidate identity in startup diagnostics.
- [x] Checkpoint B: CTest 9/9, Python 51/51, structured logging 9/9, PE, and Address Library gates pass.

## Remaining work before any release

- [ ] Obtain runtime gameplay acceptance on the exact candidate: draw/sheathe, normal/power attacks, weapon swaps, block/bash, repeated shouts and landing, descent, and magicka recovery.
- [ ] Capture actual OAR requested-original and selected-replacement traces; retain `unknown` when the API/log does not expose them.
- [ ] Qualify the 1.7.104 layout/input/SkyrimVM paths in-game for SE/AE/VR; static BIN and compilation are not sufficient.
- [ ] Decide whether to add a verified production interleaved HKX codec or keep rejecting those inputs.
- [ ] Keep private symbols beside any diagnostic build; never put a PDB in the mod payload.
- [ ] After gameplay acceptance, package a rootless release, independently extract/verify it, then deploy with a fresh rollback backup.

The public `main`/v1.6.0 compatibility port and its installed Nexus-compat
folder remain unchanged. No Skyrim session, release publication, remote push,
or modlist deployment was performed for this candidate.
