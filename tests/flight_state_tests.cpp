#include "DragonAspectFlight/FlightStateHelpers.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>

using namespace DragonAspectFlight::State;

namespace
{
	void TestWeaponEquipmentEpochTrackerAdvancesAba()
	{
		const WeaponEquipmentIdentity weaponA{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity weaponB{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };

		WeaponEquipmentEpochTrackerState tracker{};
		const auto first = ObserveWeaponEquipmentEpoch(tracker, weaponA, true);
		assert(first.initialized);
		assert(!first.identityChanged);
		assert(first.state.epoch == 1);

		const auto b = ObserveWeaponEquipmentEpoch(first.state, weaponB, true);
		assert(!b.initialized);
		assert(b.identityChanged);
		assert(b.previousEpoch == 1);
		assert(b.state.epoch == 2);

		// Returning to the old form is a new incarnation, not a replay of epoch 1.
		const auto aAgain = ObserveWeaponEquipmentEpoch(b.state, weaponA, true);
		assert(aAgain.identityChanged);
		assert(aAgain.previousEpoch == 2);
		assert(aAgain.state.epoch == 3);
		assert(!ShouldApplyCurrentEquipmentAction(
			true,
			true,
			weaponA,
			weaponA,
			first.state.epoch,
			aAgain.state.epoch));

		const auto stable = ObserveWeaponEquipmentEpoch(aAgain.state, weaponA, true);
		assert(!stable.initialized);
		assert(!stable.identityChanged);
		assert(stable.state.epoch == 3);
		// Start/stop and lifecycle reinitialization observe the same incarnation;
		// session barriers invalidate queues without manufacturing an epoch.
		const auto stableStop = ObserveWeaponEquipmentEpoch(stable.state, weaponA, true);
		assert(stableStop.state.epoch == 3);
		const auto lifecycleReset = ObserveWeaponEquipmentEpoch(
			WeaponEquipmentEpochTrackerState{ {}, stableStop.state.epoch, false },
			weaponA,
			true);
		assert(lifecycleReset.initialized);
		assert(lifecycleReset.state.epoch == 3);

		// An unloaded actor cannot manufacture a wildcard epoch or identity change.
		const auto unloaded = ObserveWeaponEquipmentEpoch(stable.state, {}, false);
		assert(!unloaded.identityCaptured);
		assert(!unloaded.identityChanged);
		assert(unloaded.state.epoch == 3);

		// Counter exhaustion is fail-closed: the terminal value is never wrapped
		// back to 1, and no captured action can use it as a valid lease.
		const WeaponEquipmentEpochTrackerState exhausted{
			weaponA, std::numeric_limits<std::uint64_t>::max(), true };
		const auto terminal = ObserveWeaponEquipmentEpoch(exhausted, weaponB, true);
		assert(terminal.identityChanged);
		assert(terminal.state.epoch == std::numeric_limits<std::uint64_t>::max());
		assert(!ShouldApplyCurrentEquipmentAction(
			true,
			true,
			weaponB,
			weaponB,
			terminal.state.epoch,
			terminal.state.epoch));
	}

	void TestNativeBlockActionRequiresNonzeroIdentityEpoch()
	{
		const WeaponEquipmentIdentity twoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };

		// A captured queued action without an epoch is not tied to one equipment
		// incarnation.  It must not be accepted as a current action merely because
		// its form identity happens to match.
		assert(!ShouldApplyFlightBlockAction(
			true, true, twoHanded, twoHanded, 0, 7));
	}

	void TestReadyAndGroundAbaActionsRejectRetiredIncarnations()
	{
		const WeaponEquipmentIdentity weaponA{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity weaponB{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		WeaponEquipmentEpochTrackerState tracker{};
		const auto first = ObserveWeaponEquipmentEpoch(tracker, weaponA, true);
		const auto b = ObserveWeaponEquipmentEpoch(first.state, weaponB, true);
		const auto aAgain = ObserveWeaponEquipmentEpoch(b.state, weaponA, true);

		// The real producer seam observes every incarnation, so delayed Ready and
		// ground envelopes for A cannot pass after A -> B -> A, even though the form
		// identity has returned to A.
		assert(!ShouldApplyCurrentEquipmentAction(
			true, true, weaponA, weaponA, first.state.epoch, aAgain.state.epoch));
		assert(!ShouldApplyCurrentEquipmentAction(
			true, true, weaponB, weaponA, b.state.epoch, aAgain.state.epoch));
		assert(ShouldApplyCurrentEquipmentAction(
			true, true, weaponA, weaponA, aAgain.state.epoch, aAgain.state.epoch));
	}

	void TestBlockRequestSwapReleaseAndStopAreMetadataOnly()
	{
		const WeaponEquipmentIdentity weaponA{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity weaponB{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const auto requestA = DecideFlightBlockAction(
			true, true, true, weaponA, weaponA, false, 1, 1);
		assert(requestA.accepted && !requestA.stale && !requestA.clearNative);

		// A release queued for the retired A incarnation cannot clear replacement B
		// state, even when the old request carried a local ownership latch.
		const auto staleRelease = DecideFlightBlockAction(
			false, true, true, weaponA, weaponB, true, 1, 2);
		assert(!staleRelease.accepted && staleRelease.stale && !staleRelease.clearNative);

		// Stop/unload cleanup is logical metadata convergence only; it never claims
		// to have released shared native state without producer provenance.
		const auto stopCleanup = DecideFlightBlockAction(
			false, false, false, weaponA, weaponB, true);
		assert(stopCleanup.accepted && !stopCleanup.stale && !stopCleanup.clearNative);
	}

	void TestNativeBlockLeaseRequiresExactClaimedState()
	{
		const WeaponEquipmentIdentity twoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const NativeBlockStateSnapshot claimedState{ true, true, true };
		const NativeBlockLeaseToken lease{
			twoHanded, 11, 1, claimedState, true };

		const auto exact = DecideNativeBlockLeaseRelease(
			lease, true, twoHanded, 11, claimedState);
		assert(exact.tokenCurrent);
		assert(exact.nativeStateMatches);
		assert(!exact.externalConflict);
		assert(exact.clearNative);

		// A competing writer that changed the native state invalidates the lease;
		// an unconditional DAF release would erase that writer's state.
		const auto competingWriter = DecideNativeBlockLeaseRelease(
			lease, true, twoHanded, 11,
			NativeBlockStateSnapshot{ true, false, true });
		assert(competingWriter.tokenCurrent);
		assert(!competingWriter.nativeStateMatches);
		assert(competingWriter.externalConflict);
		assert(!competingWriter.clearNative);

		// Some external transitions can preserve the same booleans, so callers may
		// carry an independently observed mutation marker as an additional guard.
		const auto markedConflict = DecideNativeBlockLeaseRelease(
			lease, true, twoHanded, 11, claimedState, true);
		assert(markedConflict.externalConflict);
		assert(!markedConflict.clearNative);
	}

	void TestNativeBlockLeaseCannotReleaseAfterEquipmentSwap()
	{
		const WeaponEquipmentIdentity oldTwoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity replacementTwoHanded{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const NativeBlockStateSnapshot claimedState{ true, true, true };
		const NativeBlockLeaseToken lease{
			oldTwoHanded, 11, 1, claimedState, true };
		const auto request = DecideFlightBlockAction(
			true, true, true, oldTwoHanded, oldTwoHanded, false, 11, 11);
		assert(request.accepted && !request.stale && !request.clearNative);

		const auto staleRelease = DecideNativeBlockLeaseRelease(
			lease, true, replacementTwoHanded, 12, claimedState);
		assert(!staleRelease.tokenCurrent);
		assert(staleRelease.stale);
		assert(!staleRelease.clearNative);
	}

	void TestQueuedEquipmentActionRequiresCurrentEpoch()
	{
		const WeaponEquipmentIdentity twoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		assert(ShouldApplyCurrentEquipmentAction(
			true, true, twoHanded, twoHanded, 11, 11));
		assert(!ShouldApplyCurrentEquipmentAction(
			true, true, twoHanded, twoHanded, 11, 12));
		assert(!ShouldApplyCurrentEquipmentAction(
			true, true, twoHanded, twoHanded, 0, 12));
		// A -> B -> A is still stale: the form identity returned to A, but its
		// equipment incarnation is different.
		assert(!ShouldApplyCurrentEquipmentAction(
			true, true, twoHanded, twoHanded, 11, 13));
		assert(!ShouldApplyCurrentEquipmentAction(
			true, false, twoHanded, twoHanded, 11, 11));
		const WeaponEquipmentIdentity unknownFamily{
			twoHanded.rightFormId,
			twoHanded.leftFormId,
			twoHanded.rightWeaponType,
			twoHanded.leftWeaponType,
			WeaponEquipmentFamily::kUnknown };
		assert(!ShouldApplyCurrentEquipmentAction(
			true, true, unknownFamily, twoHanded, 11, 11));
	}

	void TestStopClearsStaleWeaponTransition()
	{
		const auto state = ReduceFlightStop(FlightStopState{
			true, false, true, true, false, true, true,
			WeaponTransitionState{ true, true, true, false, 17 } });
		assert(!state.flying);
		assert(!state.combatActive);
		assert(!state.blockRequested);
		assert(!state.launchHeld);
		assert(!state.boostHeld);
		assert(!state.weapon.pending);
		assert(!state.weapon.targetDrawn);
		assert(!state.weapon.nativeFallbackArmed);
		assert(!state.weapon.postFlight);
		assert(state.weapon.sequence == 17);
		assert(!ShouldPumpWeaponTransition(false, state.weapon.pending));
	}

	void TestStopClearsStaleSheatheTransition()
	{
		const auto state = ReduceFlightStop(FlightStopState{
			true, true, false, false, false, false, false,
			WeaponTransitionState{ true, false, false, false, 23 } });
		assert(!state.flying);
		assert(!state.weapon.pending);
		assert(!state.weapon.targetDrawn);
		assert(!state.weapon.postFlight);
	}

	void TestWeaponToggleUsesLogicalState()
	{
		// The actor may already report kDrawing on the edge; the logical DAF
		// state is still sheathed, so this edge requests drawn=true.
		assert(ResolveWeaponToggleTarget(
			false, false, false, ObservedWeaponEdge::kDrawing));
		// Likewise a kSheathing transient must not turn a logical drawn state
		// into another draw request.
		assert(!ResolveWeaponToggleTarget(
			true, false, false, ObservedWeaponEdge::kSheathing));
		// A repeated edge follows the already-pending logical target, not the
		// actor's transient state, and therefore requests the opposite target.
		assert(!ResolveWeaponToggleTarget(
			false, true, true, ObservedWeaponEdge::kSheathing));
		assert(ResolveWeaponToggleTarget(
			true, true, false, ObservedWeaponEdge::kDrawing));
	}

	void TestEquipmentSwapMustNotToggleReadiness()
	{
		const WeaponEquipmentIdentity oldTwoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity newTwoHanded{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const auto decision = DecideWeaponEquipmentChange(oldTwoHanded, newTwoHanded);
		assert(decision.change == WeaponEquipmentChange::kEquipmentSwap);
		assert(decision.preserveCombatReady);
		assert(ResolveWeaponToggleTarget(
			true, false, false, ObservedWeaponEdge::kDrawn, true));
	}

	void TestEquipmentSwapRevokesOnlyDafBlockOwnership()
	{
		assert(ShouldRevokeFlightBlockOnEquipmentSwap(true, true));
		assert(!ShouldRevokeFlightBlockOnEquipmentSwap(true, false));
		assert(!ShouldRevokeFlightBlockOnEquipmentSwap(false, true));
		// A native write remains DAF-owned even if the logical latch was already
		// reconciled by another path; the equipment edge must revoke that write.
		assert(ShouldRevokeFlightBlockOnEquipmentSwap(true, false, true));
		assert(!ShouldRevokeFlightBlockOnEquipmentSwap(false, false, true));
	}

	void TestQueuedBlockActionRequiresCurrentEquipmentIdentity()
	{
		const WeaponEquipmentIdentity oldTwoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity newTwoHanded{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };

		assert(ShouldApplyFlightBlockAction(
			true, true, oldTwoHanded, oldTwoHanded, 11, 11));
		assert(!ShouldApplyFlightBlockAction(
			true, true, oldTwoHanded, newTwoHanded, 11, 11));
		assert(!ShouldApplyFlightBlockAction(
			true, false, oldTwoHanded, newTwoHanded, 11, 11));
		// Lifecycle releases may be uncaptured; native ownership is checked by the
		// manager before any shared actor/graph state is touched.
		assert(ShouldApplyFlightBlockAction(false, false, oldTwoHanded, newTwoHanded));
	}

	void TestFlightBlockReleaseRequiresCurrentEpochAndOwnership()
	{
		const WeaponEquipmentIdentity oldTwoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity newTwoHanded{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };

		// A release captured for the retired item must not clear the replacement
		// item's native block state, even when the old action still carries a DAF
		// ownership latch.
		const auto staleRelease = DecideFlightBlockAction(
			false, true, true, oldTwoHanded, newTwoHanded, true, 11, 12);
		assert(!staleRelease.accepted);
		assert(staleRelease.stale);
		assert(!staleRelease.clearNative);

		// A same-identity release may clear only state DAF actually claimed.
		const auto ownedRelease = DecideFlightBlockAction(
			false, true, true, oldTwoHanded, oldTwoHanded, true, 11, 11);
		assert(ownedRelease.accepted);
		assert(!ownedRelease.stale);
		assert(ownedRelease.clearNative);

		const auto vanillaRelease = DecideFlightBlockAction(
			false, true, true, oldTwoHanded, oldTwoHanded, false, 11, 11);
		assert(vanillaRelease.accepted);
		assert(!vanillaRelease.clearNative);

		// Requests, unlike cleanup releases, cannot be issued without an exact
		// identity epoch.
		const auto unboundRequest = DecideFlightBlockAction(
			true, false, true, oldTwoHanded, oldTwoHanded, false, 0, 11);
		assert(!unboundRequest.accepted);
		assert(unboundRequest.stale);
		assert(!unboundRequest.clearNative);
		const auto unboundLoadedRelease = DecideFlightBlockAction(
			false, false, true, oldTwoHanded, oldTwoHanded, true, 0, 11);
		assert(unboundLoadedRelease.accepted);
		assert(!unboundLoadedRelease.clearNative);

		// Lifecycle cleanup on an unloaded actor consumes metadata but must not
		// claim that native state was released.
		const auto unloadedCleanup = DecideFlightBlockAction(
			false, false, false, oldTwoHanded, newTwoHanded, true);
		assert(unloadedCleanup.accepted);
		assert(!unloadedCleanup.clearNative);
	}

	void TestEquipmentSwapRebaseArmsCurrentIdentityFallback()
	{
		// Greatsword remains sheathed after the replacement identity settles: the
		// current identity must receive a bounded fallback without another timeout.
		assert(ShouldArmNativeFallbackAfterEquipmentRebase(true, true, true, true, false));
		assert(!ShouldArmNativeFallbackAfterEquipmentRebase(true, true, true, true, true));
		assert(!ShouldArmNativeFallbackAfterEquipmentRebase(false, true, true, true, false));
		assert(!ShouldArmNativeFallbackAfterEquipmentRebase(true, false, true, true, false));
		assert(!ShouldArmNativeFallbackAfterEquipmentRebase(true, true, false, true, false));
	}

	void TestStaleFallbackMustAbortAfterEquipmentSwap()
	{
		const WeaponEquipmentIdentity oldTwoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity newTwoHanded{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const auto decision = DecideWeaponEquipmentChange(oldTwoHanded, newTwoHanded);
		assert(decision.disarmNativeFallback);
		assert(DecideWeaponFallbackObservation(
			true, true, WeaponFallbackMotion::kStable, false, false,
			decision.change == WeaponEquipmentChange::kEquipmentSwap) ==
			WeaponFallbackDecision::kAbort);
	}

	void TestTwoHandToOneHandSwapPreservesReadiness()
	{
		const WeaponEquipmentIdentity oldTwoHanded{
			0x00001392U, 0, 10, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity newOneHanded{
			0x00001394U, 0, 1, -1, WeaponEquipmentFamily::kOneHanded };
		const auto decision = DecideWeaponEquipmentChange(oldTwoHanded, newOneHanded);
		assert(decision.change == WeaponEquipmentChange::kEquipmentSwap);
		assert(decision.cancelPending);
		assert(decision.preserveCombatReady);
	}

	void TestGroundObserverIdentityMismatchCancels()
	{
		const WeaponEquipmentIdentity oldTwoHanded{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity newOneHanded{
			0x00001394U, 0, 1, -1, WeaponEquipmentFamily::kOneHanded };
		const auto decision = DecideWeaponEquipmentChange(oldTwoHanded, newOneHanded);
		assert(decision.change == WeaponEquipmentChange::kEquipmentSwap);
		assert(decision.cancelPending && decision.disarmNativeFallback);
	}

	void TestEquipmentSwapSettleWaitsForEdgeBeforeStableTarget()
	{
		auto state = ArmWeaponEquipmentSwap();
		const auto preEdgeStable = ObserveWeaponEquipmentSwap(
			state,
			WeaponEquipmentSwapObservation::kStableAtLogicalTarget);
		// A replacement can publish the new identity before Skyrim publishes its
		// WantToSheathe/WantToDraw edge.  This tick must not clear the hold.
		assert(preEdgeStable.pending);
		assert(!preEdgeStable.transitionEdgeObserved);

		const auto edge = ObserveWeaponEquipmentSwap(
			preEdgeStable,
			WeaponEquipmentSwapObservation::kTransitionEdge);
		assert(edge.pending);
		assert(edge.transitionEdgeObserved);

		const auto settled = ObserveWeaponEquipmentSwap(
			edge,
			WeaponEquipmentSwapObservation::kStableAtLogicalTarget);
		assert(!settled.pending);
	}

	void TestEquipmentSwapStableGraceTimeoutIsBounded()
	{
		auto state = ArmWeaponEquipmentSwap();
		for (std::uint16_t i = 0; i < kWeaponEquipmentSwapStableGraceUpdates; ++i) {
			state = ObserveWeaponEquipmentSwap(
				state,
				WeaponEquipmentSwapObservation::kStableAtLogicalTarget);
		}
		assert(!state.pending);
		assert(!state.readinessPreserved);
		assert(!state.reconciliationRequired);
		assert(!IsWeaponEquipmentSwapPinned(state));
		state = ArmWeaponEquipmentSwap();
		for (std::uint16_t i = 0; i < kWeaponEquipmentSwapStableGraceUpdates; ++i) {
			state = ObserveWeaponEquipmentSwap(
				state,
				WeaponEquipmentSwapObservation::kStableAwayFromTarget);
		}
		assert(!state.pending);
		assert(!state.readinessPreserved);
		assert(state.reconciliationRequired);
		// An explicit Ready command is also allowed to consume the hold rather
		// than being mistaken for a stale item transition.
		state = ArmWeaponEquipmentSwap();
		state = ObserveWeaponEquipmentSwap(
			state,
			WeaponEquipmentSwapObservation::kStableAtLogicalTarget,
			true);
		assert(!state.pending);
	}

	void TestEquipmentSwapRepeatedEdgesHitAbsoluteObservationBound()
	{
		auto state = ArmWeaponEquipmentSwap(23);
		for (std::uint16_t i = 0; i < kWeaponEquipmentSwapStableGraceUpdates + 1; ++i) {
			state = ObserveWeaponEquipmentSwap(
				state,
				WeaponEquipmentSwapObservation::kTransitionEdge);
		}
		assert(!state.pending);
		assert(state.reconciliationRequired);
		assert(state.identityEpoch == 23);
	}

	void TestWeaponTransitionExpiryKeepsTargetUntilRecoveryFailure()
	{
		const auto recovery = DecideWeaponTransitionExpiry(
			true, false, true, true, false);
		assert(recovery.disposition == WeaponTransitionExpiryDisposition::kRequestCurrentIdentityRecovery);
		assert(recovery.targetAuthoritative);

		const auto terminal = DecideWeaponTransitionExpiry(
			true, false, true, true, true);
		assert(terminal.disposition == WeaponTransitionExpiryDisposition::kTerminalFailure);
		assert(!terminal.targetAuthoritative);

		const auto identityFailure = DecideWeaponTransitionExpiry(
			true, false, true, false, false);
		assert(identityFailure.disposition == WeaponTransitionExpiryDisposition::kTerminalFailure);
	}

	void TestNativeFallbackPostCallRequiresExactStableRawState()
	{
		const NativeFallbackGateRequest request{ 74, 11, 9, 2 };
		for (const auto weaponType : { 5, 6, 10 }) {
			const WeaponEquipmentIdentity identity{
				0x00001392U + static_cast<std::uint32_t>(weaponType),
				0,
				weaponType,
				-1,
				WeaponEquipmentFamily::kTwoHanded };
			assert(IsTwoHandedWeaponTypeValue(identity.rightWeaponType));
			assert(ResolveWeaponEquipmentFamily(
				identity.rightWeaponType,
				identity.leftWeaponType,
				false) == WeaponEquipmentFamily::kTwoHanded);
			const auto drawn = DecideNativeFallbackRevalidation(
				74,
				9,
				request,
				true,
				true,
				2,
				NativeFallbackWeaponState::kDrawn,
				true);
			assert(drawn.commit);
			assert(!drawn.weaponStateMismatch);

			const auto drawing = DecideNativeFallbackRevalidation(
				74,
				9,
				request,
				true,
				true,
				2,
				NativeFallbackWeaponState::kDrawing,
				true);
			assert(!drawing.commit);
			assert(drawing.weaponStateMismatch);
			assert(drawing.weaponStatePending);
		}

		const auto sheathed = DecideNativeFallbackRevalidation(
			74,
			9,
			request,
			true,
			true,
			2,
			NativeFallbackWeaponState::kSheathed,
			false);
		assert(sheathed.commit);
		const auto unknown = DecideNativeFallbackRevalidation(
			74,
			9,
			request,
			true,
			true,
			2,
			NativeFallbackWeaponState::kUnknown,
			false);
		assert(!unknown.commit);
		assert(unknown.weaponStateMismatch);
		assert(!unknown.weaponStatePending);

		const auto sheathing = DecideNativeFallbackRevalidation(
			74,
			9,
			request,
			true,
			true,
			2,
			NativeFallbackWeaponState::kSheathing,
			false);
		assert(!sheathing.commit);
		assert(sheathing.weaponStateMismatch);
		assert(sheathing.weaponStatePending);
	}

	void TestTerminalRecoveryPreservesRequestedTargetForTransitionalRawState()
	{
		const auto drawRecovery = DecideWeaponTransitionTerminalRecovery(
			true,
			true,
			true,
			NativeFallbackWeaponState::kDrawing,
			false);
		assert(drawRecovery.targetDrawn);
		assert(drawRecovery.retryNativeFallback);
		assert(!drawRecovery.terminalFailure);

		const auto sheatheRecovery = DecideWeaponTransitionTerminalRecovery(
			false,
			true,
			true,
			NativeFallbackWeaponState::kSheathing,
			false);
		assert(!sheatheRecovery.targetDrawn);
		assert(sheatheRecovery.retryNativeFallback);
		assert(!sheatheRecovery.terminalFailure);

		// A second bounded recovery does not manufacture success, but it still must
		// leave the requested logical target intact instead of promoting kSheathing
		// through ActorState::IsWeaponDrawn().
		const auto exhausted = DecideWeaponTransitionTerminalRecovery(
			false,
			true,
			true,
			NativeFallbackWeaponState::kSheathing,
			true);
		assert(!exhausted.targetDrawn);
		assert(!exhausted.retryNativeFallback);
		assert(exhausted.terminalFailure);
	}

	void TestTerminalRecoveryStableOppositePreservesRequestedTarget()
	{
		const auto exactDraw = DecideWeaponTransitionTerminalRecovery(
			true,
			true,
			true,
			NativeFallbackWeaponState::kDrawn,
			false);
		assert(exactDraw.targetDrawn);
		assert(!exactDraw.retryNativeFallback);
		assert(!exactDraw.terminalFailure);
		const auto exactSheathe = DecideWeaponTransitionTerminalRecovery(
			false,
			true,
			true,
			NativeFallbackWeaponState::kSheathed,
			false);
		assert(!exactSheathe.targetDrawn);
		assert(!exactSheathe.retryNativeFallback);
		assert(!exactSheathe.terminalFailure);

		// A terminal sheathe failure can report stable kDrawn; that physical state
		// must not replace the requested logical target.
		const auto failedSheathe = DecideWeaponTransitionTerminalRecovery(
			false,
			true,
			true,
			NativeFallbackWeaponState::kDrawn,
			false);
		assert(!failedSheathe.targetDrawn);
		assert(!failedSheathe.retryNativeFallback);
		assert(failedSheathe.terminalFailure);

		// The symmetric failed draw must preserve target=true when the actor is
		// stably sheathed; only an exact target is success.
		const auto failedDraw = DecideWeaponTransitionTerminalRecovery(
			true,
			true,
			true,
			NativeFallbackWeaponState::kSheathed,
			false);
		assert(failedDraw.targetDrawn);
		assert(!failedDraw.retryNativeFallback);
		assert(failedDraw.terminalFailure);
	}

	void TestTerminalFailureQuarantineSequence()
	{
		constexpr std::uint64_t session = 41;
		constexpr std::uint64_t identityEpoch = 9;

		// A failed sheathe may remain stably drawn.  Every passive tick keeps the
		// requested target and has no permission to reconcile or re-arm fallback.
		auto sheatheFailure = ReduceWeaponTransitionTerminalFailureHold(
			{},
			WeaponTransitionTerminalFailureHoldEvent::kTerminalFailure,
			false,
			session,
			identityEpoch);
		assert(sheatheFailure.state.active);
		assert(!sheatheFailure.state.targetDrawn);
		assert(sheatheFailure.preserveTarget);
		assert(!sheatheFailure.allowPassiveReconciliation);
		assert(!sheatheFailure.rearmNativeFallback);
		for (int i = 0; i < 4; ++i) {
			sheatheFailure = ReduceWeaponTransitionTerminalFailureHold(
				sheatheFailure.state,
				WeaponTransitionTerminalFailureHoldEvent::kPassiveObservation,
				false,
				session,
				identityEpoch,
				false);
			assert(sheatheFailure.state.active);
			assert(!sheatheFailure.state.targetDrawn);
			assert(sheatheFailure.preserveTarget);
			assert(!sheatheFailure.allowPassiveReconciliation);
			assert(!sheatheFailure.rearmNativeFallback);
		}

		// The symmetric failed draw preserves target=true while raw state is
		// stably sheathed, with the same bounded hold semantics.
		auto drawFailure = ReduceWeaponTransitionTerminalFailureHold(
			{},
			WeaponTransitionTerminalFailureHoldEvent::kTerminalFailure,
			true,
			session,
			identityEpoch);
		assert(drawFailure.state.active);
		assert(drawFailure.state.targetDrawn);
		for (int i = 0; i < 3; ++i) {
			drawFailure = ReduceWeaponTransitionTerminalFailureHold(
				drawFailure.state,
				WeaponTransitionTerminalFailureHoldEvent::kPassiveObservation,
				true,
				session,
				identityEpoch,
				false);
			assert(drawFailure.state.active);
			assert(drawFailure.state.targetDrawn);
			assert(drawFailure.preserveTarget);
			assert(!drawFailure.allowPassiveReconciliation);
			assert(!drawFailure.rearmNativeFallback);
		}

		const auto explicitReady = ReduceWeaponTransitionTerminalFailureHold(
			sheatheFailure.state,
			WeaponTransitionTerminalFailureHoldEvent::kExplicitReady);
		assert(!explicitReady.state.active);
		assert(explicitReady.allowPassiveReconciliation);
		assert(!explicitReady.rearmNativeFallback);

		// Generic Ready-generation expiry/protected-state cleanup is not a fresh
		// request.  It must leave the quarantine intact so the following passive
		// observation cannot reconcile from the opposite physical state.
		const auto readyExpiryCleanup = ReduceWeaponTransitionTerminalFailureHold(
			sheatheFailure.state,
			WeaponTransitionTerminalFailureHoldEvent::kReadyGenerationCleanup);
		assert(readyExpiryCleanup.state.active);
		assert(!readyExpiryCleanup.state.targetDrawn);
		assert(readyExpiryCleanup.preserveTarget);
		assert(!readyExpiryCleanup.allowPassiveReconciliation);
		const auto passiveAfterReadyExpiry = ReduceWeaponTransitionTerminalFailureHold(
			readyExpiryCleanup.state,
			WeaponTransitionTerminalFailureHoldEvent::kPassiveObservation,
			false,
			session,
			identityEpoch,
			false);
		assert(passiveAfterReadyExpiry.state.active);
		assert(!passiveAfterReadyExpiry.state.targetDrawn);
		assert(passiveAfterReadyExpiry.preserveTarget);
		assert(!passiveAfterReadyExpiry.allowPassiveReconciliation);
		const auto acceptedReadyAfterExpiry = ReduceWeaponTransitionTerminalFailureHold(
			passiveAfterReadyExpiry.state,
			WeaponTransitionTerminalFailureHoldEvent::kExplicitReady);
		assert(!acceptedReadyAfterExpiry.state.active);
		assert(acceptedReadyAfterExpiry.allowPassiveReconciliation);

		const auto equipmentChanged = ReduceWeaponTransitionTerminalFailureHold(
			drawFailure.state,
			WeaponTransitionTerminalFailureHoldEvent::kEquipmentIdentityChange);
		assert(!equipmentChanged.state.active);
		assert(equipmentChanged.allowPassiveReconciliation);

		const auto sessionReset = ReduceWeaponTransitionTerminalFailureHold(
			sheatheFailure.state,
			WeaponTransitionTerminalFailureHoldEvent::kSessionReset);
		assert(!sessionReset.state.active);
		assert(sessionReset.allowPassiveReconciliation);

		const auto converged = ReduceWeaponTransitionTerminalFailureHold(
			drawFailure.state,
			WeaponTransitionTerminalFailureHoldEvent::kPassiveObservation,
			true,
			session,
			identityEpoch,
			true);
		assert(!converged.state.active);
		assert(converged.allowPassiveReconciliation);
	}

	void TestEquipmentSwapOrderingPreservesReadinessOnTimeout()
	{
		// The production UpdateFlight ordering must hold DAF's logical readiness
		// before the edge, during the edge, and after an unproven timeout.
		auto state = ArmWeaponEquipmentSwap();
		assert(!ShouldReconcileFlightCombatReady(true, true, false, state.pending, false));
		state = ObserveWeaponEquipmentSwap(
			state,
			WeaponEquipmentSwapObservation::kTransitionEdge);
		assert(!ShouldReconcileFlightCombatReady(true, true, false, state.pending, false));

		state = ArmWeaponEquipmentSwap();
		for (std::uint16_t i = 0; i < kWeaponEquipmentSwapStableGraceUpdates; ++i) {
			state = ObserveWeaponEquipmentSwap(
				state,
				WeaponEquipmentSwapObservation::kStableAwayFromTarget);
		}
		assert(!state.pending);
		assert(ShouldReconcileFlightCombatReady(
			true, true, false, state.pending, true, state.readinessPreserved,
			state.reconciliationRequired));
		state = ObserveWeaponEquipmentSwap(
			state,
			WeaponEquipmentSwapObservation::kStableAtLogicalTarget);
		assert(!state.readinessPreserved);
		assert(!state.reconciliationRequired);
		state = ObserveWeaponEquipmentSwap(state, WeaponEquipmentSwapObservation::kStableAtLogicalTarget, true);
		assert(!state.readinessPreserved);
		assert(ShouldReconcileFlightCombatReady(true, true, false, false, false));
	}

	void TestEquipmentSwapExpirySchedulesCurrentIdentityReconciliation()
	{
		// Expiry ends the old identity-bound transaction.  It must not become an
		// unbounded readiness pin; the current identity may now be reconciled.
		auto state = ArmWeaponEquipmentSwap();
		for (std::uint16_t i = 0; i < kWeaponEquipmentSwapStableGraceUpdates; ++i) {
			state = ObserveWeaponEquipmentSwap(
				state,
				WeaponEquipmentSwapObservation::kStableAwayFromTarget);
		}
		assert(!state.pending);
		assert(!state.readinessPreserved);
		assert(state.reconciliationRequired);
		assert(!IsWeaponEquipmentSwapPinned(state));
		assert(ShouldReconcileFlightCombatReady(
			true, true, false, state.pending, true, state.readinessPreserved,
			state.reconciliationRequired));

		const auto lateEdge = ObserveWeaponEquipmentSwap(
			state,
			WeaponEquipmentSwapObservation::kTransitionEdge);
		assert(!lateEdge.pending);
		assert(!lateEdge.readinessPreserved);
		assert(!IsWeaponEquipmentSwapPinned(lateEdge));

		const auto expired = ObserveWeaponEquipmentSwap(
			lateEdge,
			WeaponEquipmentSwapObservation::kStableAtLogicalTarget);
		assert(!expired.readinessPreserved);

		const auto explicitReady = ObserveWeaponEquipmentSwap(
			expired,
			WeaponEquipmentSwapObservation::kStableAtLogicalTarget,
			true);
		assert(!explicitReady.pending);
		assert(!explicitReady.readinessPreserved);
		assert(!explicitReady.transitionEdgeObserved);
	}

	void TestEquipmentSwapTracksEitherHandAndCustomTwoHandedFallback()
	{
		const WeaponEquipmentIdentity oldRightOnly{
			0x00001392U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity newRightOnly{
			0x00001393U, 0, 6, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity oldLeftOnly{
			0, 0x00002001U, -1, 1, WeaponEquipmentFamily::kOneHanded };
		const WeaponEquipmentIdentity newLeftOnly{
			0, 0x00002002U, -1, 1, WeaponEquipmentFamily::kOneHanded };
		assert(DecideWeaponEquipmentChange(oldRightOnly, newRightOnly).change ==
			WeaponEquipmentChange::kEquipmentSwap);
		assert(DecideWeaponEquipmentChange(oldLeftOnly, newLeftOnly).change ==
			WeaponEquipmentChange::kEquipmentSwap);
		assert(ResolveWeaponEquipmentFamily(
			-1, -1, false, false, false, true, false) == WeaponEquipmentFamily::kTwoHanded);
		assert(ResolveWeaponEquipmentFamily(5, -1, false) == WeaponEquipmentFamily::kTwoHanded);
		assert(ResolveWeaponEquipmentFamily(6, -1, false) == WeaponEquipmentFamily::kTwoHanded);
		assert(ResolveWeaponEquipmentFamily(10, -1, false) == WeaponEquipmentFamily::kTwoHanded);
		assert(IsBlockCapableWeaponFamily(WeaponEquipmentFamily::kTwoHanded));
		assert(!IsBlockCapableWeaponFamily(WeaponEquipmentFamily::kUnarmed));

		// Every two-handed family follows the same identity-bound bounded
		// transaction.  No type-specific branch may retain a post-expiry pin.
		const std::int32_t matrix[] = { 5, 6, 10, -1 };
		for (const auto weaponType : matrix) {
			const auto oldIdentity = WeaponEquipmentIdentity{
				0x00003000U + static_cast<std::uint32_t>(weaponType + 1),
				0,
				weaponType,
				-1,
				WeaponEquipmentFamily::kTwoHanded };
			const auto newIdentity = WeaponEquipmentIdentity{
				oldIdentity.rightFormId + 1,
				0,
				weaponType,
				-1,
				WeaponEquipmentFamily::kTwoHanded };
			assert(DecideWeaponEquipmentChange(oldIdentity, newIdentity).cancelPending);
			auto settle = ArmWeaponEquipmentSwap(17);
			for (std::uint16_t i = 0; i < kWeaponEquipmentSwapStableGraceUpdates; ++i) {
				settle = ObserveWeaponEquipmentSwap(
					settle,
					WeaponEquipmentSwapObservation::kStableAwayFromTarget);
			}
			assert(!settle.pending && !settle.readinessPreserved);
			assert(settle.reconciliationRequired && settle.identityEpoch == 17);
			assert(ShouldReconcileFlightCombatReady(
				true, true, false, false, true, false, settle.reconciliationRequired));
		}
	}

	void TestTypeMinusOneRequiresActualWeaponPredicate()
	{
		// A custom TESObjectWEAP with an unsupported type still follows the
		// Greatsword route.  A shield or unrelated equipped form must not.
		assert(ResolveWeaponEquipmentFamily(
			-1, -1, false, false, false, true, false) == WeaponEquipmentFamily::kTwoHanded);
		assert(ResolveWeaponEquipmentFamily(
			-1, -1, false, false, false, false, false) == WeaponEquipmentFamily::kUnarmed);
		assert(ResolveWeaponEquipmentFamily(10, -1, false) == WeaponEquipmentFamily::kTwoHanded);
	}

	void TestGroundObserverCancellationAndReadyReplacement()
	{
		auto state = GroundWeaponObserverState{ true, true, 7 };
		const auto replaced = ReduceGroundWeaponObserver(
			state,
			GroundWeaponObserverEvent::kReadyEdge);
		assert(replaced.armed && replaced.replaced);
		assert(replaced.state.pending && !replaced.state.fallbackIssued);
		assert(replaced.state.edgeSequence == 8);

		const auto cancelled = ReduceGroundWeaponObserver(
			replaced.state,
			GroundWeaponObserverEvent::kIdentityChanged);
		assert(cancelled.cancelled);
		assert(!cancelled.state.pending && !cancelled.state.fallbackIssued);

		const auto newReady = ReduceGroundWeaponObserver(
			cancelled.state,
			GroundWeaponObserverEvent::kReadyEdge);
		assert(newReady.armed && !newReady.replaced);
		assert(newReady.state.pending && newReady.state.edgeSequence == 9);

		const auto reset = ReduceGroundWeaponObserver(
			newReady.state,
			GroundWeaponObserverEvent::kSessionReset);
		assert(reset.cancelled);
		assert(!reset.state.pending && reset.state.edgeSequence == 0);
	}

	void TestGroundObserverBaselineUsesLiveIdentity()
	{
		const WeaponEquipmentIdentity preEdge{
			0x00001392U, 0, 10, -1, WeaponEquipmentFamily::kTwoHanded };
		const WeaponEquipmentIdentity liveAfterEdge{
			0x00001394U, 0, 1, -1, WeaponEquipmentFamily::kOneHanded };
		const auto rebased = DecideGroundWeaponObserverBaseline(
			preEdge, true, liveAfterEdge, true);
		assert(rebased.captured);
		assert(SameWeaponEquipmentIdentity(rebased.identity, liveAfterEdge));

		const auto actionFallback = DecideGroundWeaponObserverBaseline(
			preEdge, true, {}, false);
		assert(actionFallback.captured);
		assert(SameWeaponEquipmentIdentity(actionFallback.identity, preEdge));
	}

	void TestRepeatedStopPreservesGroundObserverAndAllowsOneFallback()
	{
		// The first stop hands a still-transitioning actor to a ground observer.
		const auto firstStop = DecideLandingWeaponHandoff(
			true, true, true, true, true, true);
		assert(firstStop.armGroundObserver);
		const auto armed = ReduceGroundWeaponObserver(
			{}, GroundWeaponObserverEvent::kReadyEdge);
		assert(armed.armed && armed.state.pending);

		// A second idempotent stop must keep the observer and its pump alive when
		// the current identity/session/epoch are still authoritative.
		const auto repeatedStop = ReduceGroundObserverAfterStop(
			armed.state, false, true, true, true, true);
		assert(repeatedStop.preserve && repeatedStop.keepPump);
		assert(repeatedStop.state.pending);
		const auto actorChanged = ReduceGroundObserverAfterStop(
			repeatedStop.state, false, true, true, true, true, false);
		assert(actorChanged.rebase && actorChanged.keepPump);
		assert(actorChanged.state.pending);

		// Repeated drawing edges get one bounded progress extension, followed by
		// exactly one delayed directional fallback.  A newer Ready edge may replace
		// that target, including the opposite kSheathing direction.
		const auto progress = DecideWeaponFallbackObservation(
			false, false, WeaponFallbackMotion::kTowardTarget, false, false);
		assert(progress == WeaponFallbackDecision::kDeferForProgress);
		const auto newerReady = ReduceGroundWeaponObserver(
			repeatedStop.state, GroundWeaponObserverEvent::kReadyEdge);
		assert(newerReady.replaced && newerReady.state.pending);
		const auto delayedFallback = DecideWeaponFallbackObservation(
			true, false, WeaponFallbackMotion::kTowardTarget, true, false);
		assert(delayedFallback == WeaponFallbackDecision::kFallback);
		const auto afterFallback = DecideWeaponFallbackObservation(
			false, true, WeaponFallbackMotion::kTowardTarget, true, true);
		assert(afterFallback == WeaponFallbackDecision::kWait);
		const auto fallbackExpired = DecideWeaponFallbackObservation(
			true, true, WeaponFallbackMotion::kTowardTarget, true, true);
		assert(fallbackExpired == WeaponFallbackDecision::kAbort);
	}

	void TestReadyGenerationLeaseBelowAtAndAboveThreshold()
	{
		ReadyGenerationBarrierState state = ResetReadyGenerationBarrier({}, 101, 0);
		const ReadyGenerationToken token{ 101, 1 };
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, token).state;
		const auto below = DecideReadyGenerationLease(
			state, token, kReadyGenerationLeaseMilliseconds - 1);
		assert(below.pending && below.exactToken && !below.expired);
		const auto at = DecideReadyGenerationLease(
			state, token, kReadyGenerationLeaseMilliseconds);
		assert(at.pending && at.exactToken && at.expired);
		const auto above = DecideReadyGenerationLease(
			state, token, kReadyGenerationLeaseMilliseconds + 1);
		assert(above.pending && above.exactToken && above.expired);

		const auto newer = ReduceReadyGenerationBarrier(
			state,
			ReadyGenerationBarrierEvent::kAnnounce,
			ReadyGenerationToken{ 101, 2 }).state;
		const auto oldLease = DecideReadyGenerationLease(
			newer, token, kReadyGenerationLeaseMilliseconds + 1);
		assert(!oldLease.pending && !oldLease.exactToken && !oldLease.expired);
	}

	void TestStaleDirectionalRecoveryRebasesEquipmentReadyAndStopBoundaries()
	{
		const auto equipment = DecideNativeFallbackStaleRecovery(
			false, false, true, false, false, false, true, true, false, false, false);
		assert(equipment.disposition ==
			NativeFallbackStaleRecoveryDisposition::kRebaseFlightTransition);
		assert(equipment.rebased && !equipment.preserveNewerObserver);

		// A concurrent flight start clears the old observer before this stale
		// directional call returns; the current airborne identity still needs a
		// non-recursive flight rebase rather than a terminal failure.
		const auto observerDuringFlight = DecideNativeFallbackStaleRecovery(
			false, false, true, false, true, true, true, true, true, false, false);
		assert(observerDuringFlight.disposition ==
			NativeFallbackStaleRecoveryDisposition::kRebaseFlightTransition);
		assert(observerDuringFlight.rebased && !observerDuringFlight.failed);

		const auto newerReady = DecideNativeFallbackStaleRecovery(
			false, true, false, false, false, false, true, true, false, true, true);
		assert(newerReady.disposition ==
			NativeFallbackStaleRecoveryDisposition::kPreserveNewerObserver);
		assert(newerReady.preserveNewerObserver && !newerReady.rebased);

		const auto stopBoundary = DecideNativeFallbackStaleRecovery(
			true, false, false, true, true, false, false, true, true, false, false);
		assert(stopBoundary.disposition ==
			NativeFallbackStaleRecoveryDisposition::kRebaseGroundObserver);
		assert(stopBoundary.rebased && !stopBoundary.preserveNewerObserver);
	}

	void TestReadyGenerationBlocksOlderUpdateUntilApplied()
	{
		ReadyGenerationBarrierState state = ResetReadyGenerationBarrier({}, 41, 0);
		const ReadyGenerationToken token{ 41, 1 };
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, token).state;
		const auto update = DecideReadyGenerationUpdate(state, true, true, false);
		assert(update.barrierPending);
		assert(update.preserveReadiness);
		assert(update.suppressNativeFallback);
		assert(!update.allowEquipmentMutation);

		const auto applied = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kApply, token);
		assert(applied.accepted);
		assert(!ReadyGenerationBarrierPending(applied.state));
		const auto resumed = DecideReadyGenerationUpdate(applied.state, true, true, false);
		assert(!resumed.barrierPending);
		assert(!resumed.suppressNativeFallback);
		assert(resumed.allowEquipmentMutation);
	}

	void TestReadyGenerationOutOfOrderAndRejectedRetirement()
	{
		ReadyGenerationBarrierState state = ResetReadyGenerationBarrier({}, 41, 0);
		const ReadyGenerationToken olderToken{ 41, 1 };
		const ReadyGenerationToken newerToken{ 41, 2 };
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, olderToken).state;
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, newerToken).state;

		const auto newer = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kApply, newerToken);
		assert(newer.accepted);
		assert(newer.state.applied == 2 && newer.state.retired == 2);
		const auto older = ReduceReadyGenerationBarrier(
			newer.state, ReadyGenerationBarrierEvent::kApply, olderToken);
		assert(older.stale && !older.accepted);
		assert(older.state.retired == 2);

		ReadyGenerationBarrierState rejectedState = ResetReadyGenerationBarrier({}, 41, 0);
		const ReadyGenerationToken rejectedToken{ 41, 3 };
		rejectedState = ReduceReadyGenerationBarrier(
			rejectedState, ReadyGenerationBarrierEvent::kAnnounce, rejectedToken).state;
		const auto rejected = ReduceReadyGenerationBarrier(
			rejectedState, ReadyGenerationBarrierEvent::kRetire, rejectedToken);
		assert(rejected.accepted && rejected.state.retired == 3);
		assert(!ReadyGenerationBarrierPending(rejected.state));
	}

	void TestReadyGenerationRejectsOlderApplyAndPreservesLatestLease()
	{
		const ReadyGenerationToken olderToken{ 101, 1 };
		const ReadyGenerationToken newerToken{ 101, 2 };
		ReadyGenerationBarrierState state = ResetReadyGenerationBarrier({}, 101, 0);
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, olderToken).state;
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, newerToken).state;

		const auto leaseBeforeOlderApply = DecideReadyGenerationLease(state, newerToken, 0);
		assert(leaseBeforeOlderApply.pending && leaseBeforeOlderApply.exactToken);

		const auto olderApply = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kApply, olderToken);
		assert(olderApply.stale && !olderApply.accepted);
		assert(olderApply.state.announced == 2);
		assert(olderApply.state.applied == state.applied);
		assert(olderApply.state.retired == state.retired);
		const auto leaseAfterOlderApply = DecideReadyGenerationLease(
			olderApply.state, newerToken, 0);
		assert(leaseAfterOlderApply.pending && leaseAfterOlderApply.exactToken);

		// Retire remains intentionally older-first: it may advance the retired
		// watermark without consuming the newer generation's lease.
		const auto olderRetire = ReduceReadyGenerationBarrier(
			olderApply.state, ReadyGenerationBarrierEvent::kRetire, olderToken);
		assert(olderRetire.accepted && olderRetire.state.retired == 1);
		assert(ReadyGenerationBarrierPending(olderRetire.state));

		const auto newerApply = ReduceReadyGenerationBarrier(
			olderRetire.state, ReadyGenerationBarrierEvent::kApply, newerToken);
		assert(newerApply.accepted && newerApply.state.applied == 2);
		assert(newerApply.state.retired == 2);
		assert(!ReadyGenerationBarrierPending(newerApply.state));

		const auto lateOlderApply = ReduceReadyGenerationBarrier(
			newerApply.state, ReadyGenerationBarrierEvent::kApply, olderToken);
		assert(lateOlderApply.stale && !lateOlderApply.accepted);
		assert(lateOlderApply.state.applied == 2 && lateOlderApply.state.retired == 2);
	}

	void TestReadyGenerationResetInvalidatesSessionAndBarrier()
	{
		const auto lifecycle = DecideLifecycleInvalidation(41, 9);
		assert(lifecycle.oldSession == 41);
		assert(lifecycle.newSession == 42);
		assert(lifecycle.readyGenerationFloor == 9);

		ReadyGenerationBarrierState state{ 41, 8, 7, 7 };
		const auto reset = ResetReadyGenerationBarrier(
			state, lifecycle.newSession, lifecycle.readyGenerationFloor);
		assert(reset.session == 42);
		assert(reset.announced == 9 && reset.applied == 9 && reset.retired == 9);
		assert(!ReadyGenerationBarrierPending(reset));
	}

	void TestReadyGenerationSessionBindingAndExactRetirement()
	{
		const auto stopped = ResetReadyGenerationBarrier({}, 12, 1);
		const ReadyGenerationToken oldFlightToken{ 11, 1 };
		const auto staleAnnouncement = ReduceReadyGenerationBarrier(
			stopped, ReadyGenerationBarrierEvent::kAnnounce, oldFlightToken);
		assert(staleAnnouncement.stale && !staleAnnouncement.accepted);
		assert(staleAnnouncement.state.session == 12);
		assert(staleAnnouncement.state.announced == 1);

		const ReadyGenerationToken newSessionToken{ 12, 2 };
		const auto currentAnnouncement = ReduceReadyGenerationBarrier(
			staleAnnouncement.state, ReadyGenerationBarrierEvent::kAnnounce, newSessionToken);
		assert(currentAnnouncement.accepted);
		const auto currentApply = ReduceReadyGenerationBarrier(
			currentAnnouncement.state, ReadyGenerationBarrierEvent::kApply, newSessionToken);
		assert(currentApply.accepted && currentApply.state.retired == 2);

		const auto oldRetire = ReduceReadyGenerationBarrier(
			currentApply.state, ReadyGenerationBarrierEvent::kRetire, oldFlightToken);
		assert(oldRetire.stale && !oldRetire.accepted);
		assert(oldRetire.state.session == 12 && oldRetire.state.retired == 2);

		assert(DecideReadyGenerationTaskDisposition(oldFlightToken, 12) ==
			ReadyGenerationTaskDisposition::kRetireOnlyStaleSession);
		assert(DecideReadyGenerationTaskDisposition(newSessionToken, 12) ==
			ReadyGenerationTaskDisposition::kApplyCurrentSession);
	}

	void TestReadyGenerationStopStartAndLifecycleRejectDelayedTokens()
	{
		const auto oldState = ResetReadyGenerationBarrier({}, 21, 4);
		const ReadyGenerationToken delayed{ 21, 5 };
		const auto started = ResetReadyGenerationBarrier(oldState, 22, 5);
		const auto delayedApply = ReduceReadyGenerationBarrier(
			started, ReadyGenerationBarrierEvent::kApply, delayed);
		assert(delayedApply.stale && !delayedApply.accepted);
		assert(delayedApply.state.session == 22);
		assert(delayedApply.state.retired == 5);

		const auto lifecycle = DecideLifecycleInvalidation(22, 5);
		const auto reset = ResetReadyGenerationBarrier(
			started, lifecycle.newSession, lifecycle.readyGenerationFloor);
		const auto oldLifecycleApply = ReduceReadyGenerationBarrier(
			reset, ReadyGenerationBarrierEvent::kApply, ReadyGenerationToken{ 22, 6 });
		assert(oldLifecycleApply.stale && !oldLifecycleApply.accepted);
		assert(oldLifecycleApply.state.session == 23);

		const ReadyGenerationToken sameSession{ 23, 7 };
		const auto sameSessionState = ReduceReadyGenerationBarrier(
			reset, ReadyGenerationBarrierEvent::kAnnounce, sameSession).state;
		const auto sameSessionApply = ReduceReadyGenerationBarrier(
			sameSessionState, ReadyGenerationBarrierEvent::kApply, sameSession);
		assert(sameSessionApply.accepted && sameSessionApply.state.retired == 7);
	}

	void TestReadyGenerationNativeFallbackGateLinearization()
	{
		const NativeFallbackGateRequest request{ 31, 9, 4, 1 };
		const auto beforeReady = DecideNativeFallbackGate(
			31, 4, request, true, true, true, false, false, 1);
		assert(beforeReady.execute);

		const auto afterReady = DecideNativeFallbackGate(
			31, 5, request, true, true, true, false, false, 1);
		assert(!afterReady.execute);
		assert(afterReady.newerReady);
		assert(afterReady.readyGenerationChanged);

		const auto staleSession = DecideNativeFallbackGate(
			32, 4, request, true, true, true, false, false, 1);
		assert(!staleSession.execute && staleSession.sessionMismatch);
		const auto staleIdentity = DecideNativeFallbackGate(
			31, 4, request, false, true, true, false, false, 1);
		assert(!staleIdentity.execute && staleIdentity.identityMismatch);
		const auto staleTransition = DecideNativeFallbackGate(
			31, 4, request, true, false, true, false, false, 1);
		assert(!staleTransition.execute && staleTransition.transitionMismatch);
	}

	void TestReadyGenerationFailureDispositionRetiresOnlyItsToken()
	{
		const ReadyGenerationToken older{ 51, 1 };
		const ReadyGenerationToken newer{ 51, 2 };
		assert(DecideReadyGenerationTaskDisposition(older, 52) ==
			ReadyGenerationTaskDisposition::kRetireOnlyStaleSession);
		assert(DecideReadyGenerationTaskDisposition(newer, 51) ==
			ReadyGenerationTaskDisposition::kApplyCurrentSession);

		ReadyGenerationBarrierState state = ResetReadyGenerationBarrier({}, 51, 0);
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, older).state;
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, newer).state;
		const auto newerRetired = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kRetire, newer);
		assert(newerRetired.accepted);
		const auto olderRetired = ReduceReadyGenerationBarrier(
			newerRetired.state, ReadyGenerationBarrierEvent::kRetire, older);
		assert(olderRetired.stale && !olderRetired.accepted);
		assert(olderRetired.state.retired == 2);

		ReadyGenerationBarrierState reversed = ResetReadyGenerationBarrier({}, 51, 0);
		reversed = ReduceReadyGenerationBarrier(
			reversed, ReadyGenerationBarrierEvent::kAnnounce, older).state;
		reversed = ReduceReadyGenerationBarrier(
			reversed, ReadyGenerationBarrierEvent::kAnnounce, newer).state;
		const auto olderFirst = ReduceReadyGenerationBarrier(
			reversed, ReadyGenerationBarrierEvent::kRetire, older);
		assert(olderFirst.accepted && olderFirst.state.retired == 1);
		const auto newerAfterOlder = ReduceReadyGenerationBarrier(
			olderFirst.state, ReadyGenerationBarrierEvent::kRetire, newer);
		assert(newerAfterOlder.accepted && newerAfterOlder.state.retired == 2);
	}

	void TestInvalidInputSnapshotDefersAndRejectsStaleSession()
	{
		const auto deferred = DecideInputSnapshotDispatch(false, true, 7, 7);
		assert(deferred.disposition == InputSnapshotDispatchDisposition::kDeferUntilRefresh);
		const auto dispatched = DecideInputSnapshotDispatch(true, true, 7, 7);
		assert(dispatched.disposition == InputSnapshotDispatchDisposition::kDispatch);
		const auto rejected = DecideInputSnapshotDispatch(true, true, 7, 8);
		assert(rejected.disposition == InputSnapshotDispatchDisposition::kRejectSession);
		const auto dropped = DecideInputSnapshotDispatch(false, false, 7, 7);
		assert(dropped.disposition == InputSnapshotDispatchDisposition::kRejectSession);

		DeferredInputQueueState queue{ 4, true, 3 };
		const auto preserved = DecideDeferredInputQueue(queue, true, false, 4);
		assert(preserved.enqueue && preserved.replaceOldest && preserved.state.readyPending);
		const auto ready = DecideDeferredInputQueue(queue, true, true, 4);
		assert(ready.enqueue && ready.replaceOldest && ready.state.readyPending);
		const auto bounded = DecideDeferredInputQueue({}, true, true, 4);
		assert(bounded.enqueue && bounded.state.count == 1 && bounded.state.readyPending);
		const auto allReady = DecideDeferredInputQueue(
			DeferredInputQueueState{ 4, true, 4 }, true, false, 4);
		assert(!allReady.enqueue && !allReady.replaceOldest);
		const auto latestReady = DecideDeferredInputQueue(
			DeferredInputQueueState{ 4, true, 4 }, true, true, 4);
		assert(latestReady.enqueue && latestReady.replaceOldest &&
			latestReady.state.readyCount == 4);
	}

	void TestReadyGenerationFailureCancelsOnlyItsProtectedState()
	{
		const auto initial = ResetReadyGenerationBarrier({}, 61, 0);
		const ReadyGenerationToken older{ 61, 1 };
		const ReadyGenerationToken newer{ 61, 2 };
		auto state = ReduceReadyGenerationBarrier(
			initial, ReadyGenerationBarrierEvent::kAnnounce, older).state;
		state = ReduceReadyGenerationBarrier(
			state, ReadyGenerationBarrierEvent::kAnnounce, newer).state;
		const auto announcedState = state;
		const auto failedOlder = DecideReadyGenerationFailure(
			state, older, 61, 61);
		assert(failedOlder.retireAccepted);
		assert(!failedOlder.cancelProtectedState);
		assert(failedOlder.barrier.state.retired == 1);

		state = failedOlder.barrier.state;
		const auto failedLatest = DecideReadyGenerationFailure(
			state, newer, 61, 61);
		assert(failedLatest.retireAccepted);
		assert(failedLatest.cancelProtectedState);
		assert(failedLatest.barrier.state.retired == 2);

		const auto staleEnvelope = DecideReadyGenerationFailure(
			state, newer, 60, 61);
		assert(!staleEnvelope.envelopeValid);
		assert(!staleEnvelope.retireAccepted);
		assert(!staleEnvelope.cancelProtectedState);
		assert(staleEnvelope.barrier.state.session == state.session);
		assert(staleEnvelope.barrier.state.retired == state.retired);

		// An older generation in the same source/current session remains a valid
		// retire-only cleanup.  It may advance its own watermark, but cannot clear
		// the newer generation's protected state.
		const auto validStale = DecideReadyGenerationFailure(
			announcedState, older, 61, 61);
		assert(validStale.envelopeValid);
		assert(validStale.retireAccepted);
		assert(validStale.barrier.state.retired == older.generation);
		assert(!validStale.cancelProtectedState);
	}

	void TestNativeFallbackGateRejectsObsoleteGroundObserver()
	{
		const NativeFallbackGateRequest request{ 71, 9, 4, 1 };
		const auto obsolete = DecideNativeFallbackGate(
			71, 5, request, true, true, true, true, false, 1);
		assert(!obsolete.execute);
		assert(obsolete.abortGroundObserver);
		assert(!obsolete.retryableEngineUnavailable);

		const auto unavailable = DecideNativeFallbackGate(
			71, 4, request, true, true, false, true, false, 1);
		assert(!unavailable.execute);
		assert(!unavailable.abortGroundObserver);
		assert(unavailable.retryableEngineUnavailable);
	}

	void TestLifecycleInputResetAndOneSnapshotReplay()
	{
		const auto reset = DecideInputLifecycleReset(72);
		assert(reset.publishedSession == 72);
		assert(!reset.snapshotValid && reset.deferredCleared && reset.requestRefresh);

		const auto start = DecideDeferredInputReplay(2, true, false, 72, 72);
		assert(start.disposition == InputSnapshotDispatchDisposition::kDispatch);
		assert(start.consumeOne && start.scheduleNextRefresh);
		const auto ready = DecideDeferredInputReplay(1, true, false, 72, 72);
		assert(ready.disposition == InputSnapshotDispatchDisposition::kDispatch);
		assert(ready.consumeOne && !ready.scheduleNextRefresh);
		const auto suppressed = DecideDeferredInputReplay(1, true, true, 72, 72);
		assert(suppressed.disposition == InputSnapshotDispatchDisposition::kSuppress);
		assert(suppressed.consumeOne && !suppressed.scheduleNextRefresh);

		const auto stale = DecideReadyGenerationEnvelope(
			ReadyGenerationToken{ 72, 5 }, 71, 72, 72);
		assert(!stale.valid && stale.sourceSessionMismatch);
		assert(!stale.mayPrepareOrClear);
	}

	void TestNativeFallbackGateReentrantReadyAnnouncement()
	{
		const NativeFallbackGateRequest request{ 73, 10, 8, 1 };
		const auto beforeReentrantReady = DecideNativeFallbackGate(
			73, 8, request, true, true, true, false, false, 1);
		assert(beforeReentrantReady.execute);
		// A Ready announcement re-entering while the native call is in progress
		// advances the generation; a later validation must skip the old request.
		const auto afterReentrantReady = DecideNativeFallbackGate(
			73, 9, request, true, true, true, false, false, 1);
		assert(!afterReentrantReady.execute && afterReentrantReady.newerReady);
	}

	void TestNativeFallbackReservationRevalidatesIdentityEpoch()
	{
		const NativeFallbackGateRequest request{ 73, 10, 8, 41 };
		const auto staleEpoch = DecideNativeFallbackGate(
			73, 8, request, true, true, true, false, false, 42);
		assert(!staleEpoch.execute && staleEpoch.identityEpochMismatch);

		const auto staleAfterEngineCall = DecideNativeFallbackRevalidation(
			73, 8, request, true, true, 42);
		assert(!staleAfterEngineCall.commit);
		assert(staleAfterEngineCall.identityEpochMismatch);

		const auto currentAfterEngineCall = DecideNativeFallbackRevalidation(
			73,
			8,
			request,
			true,
			true,
			41,
			NativeFallbackWeaponState::kDrawn,
			true);
		assert(currentAfterEngineCall.commit);

		const NativeFallbackGateRequest missingEpoch{ 73, 10, 8, 0 };
		const auto staleMissingEpoch = DecideNativeFallbackGate(
			73, 8, missingEpoch, true, true, true, false, false, 41);
		assert(!staleMissingEpoch.execute && staleMissingEpoch.identityEpochMismatch);
	}

	void TestCombatInputPolicyKeepsNormalAttacksPassthrough()
	{
		const auto leftNormal = DecideFlightCombatInputPolicy(
			true, FlightCombatInputChannel::kLeftAttackBlock, false, true, false, false);
		assert(!leftNormal.consume);
		assert(!leftNormal.queueBlockRequest && leftNormal.queueBeginCombat);

		const auto leftHeld = DecideFlightCombatInputPolicy(
			true, FlightCombatInputChannel::kLeftAttackBlock, false, false, false, true);
		assert(!leftHeld.consume && leftHeld.queueBlockRequest && !leftHeld.queueBeginCombat);

		const auto leftHeldAgain = DecideFlightCombatInputPolicy(
			true, FlightCombatInputChannel::kLeftAttackBlock, true, false, false, true);
		assert(!leftHeldAgain.consume && !leftHeldAgain.queueBlockRequest &&
			!leftHeldAgain.queueBlockRelease);

		const auto rightNormal = DecideFlightCombatInputPolicy(
			true, FlightCombatInputChannel::kRightAttackBlock, false, true, false, false);
		assert(!rightNormal.consume);
		assert(!rightNormal.queueBlockRequest && rightNormal.queueBeginCombat);

		const auto leftRelease = DecideFlightCombatInputPolicy(
			true, FlightCombatInputChannel::kLeftAttackBlock, true, false, true, false);
		assert(!leftRelease.consume && leftRelease.queueBlockRelease);
		const auto staleLeftRelease = DecideFlightCombatInputPolicy(
			true, FlightCombatInputChannel::kLeftAttackBlock, false, false, true, false);
		assert(!staleLeftRelease.consume && !staleLeftRelease.queueBlockRequest &&
			!staleLeftRelease.queueBlockRelease);

		const auto dualHeld = DecideFlightCombatInputPolicy(
			true, FlightCombatInputChannel::kDualAttack, false, false, false, true);
		assert(!dualHeld.consume && !dualHeld.queueBeginCombat &&
			!dualHeld.queueBlockRequest && !dualHeld.queueBlockRelease);
		const auto rightRelease = DecideFlightCombatInputPolicy(
			true, FlightCombatInputChannel::kRightAttackBlock, true, false, true, false);
		assert(!rightRelease.consume && !rightRelease.queueBlockRequest &&
			!rightRelease.queueBlockRelease);
		assert(ShouldReleaseFlightBlockForReadyEdge(true, true, true));
		assert(!ShouldReleaseFlightBlockForReadyEdge(true, false, true));
		assert(!ShouldReleaseFlightBlockForReadyEdge(true, true, false));
		assert(!ShouldReleaseFlightBlockForReadyEdge(false, true, true));
	}

	void TestRuntimeBoundaryDefersAndRebindsSameDispatchEdges()
	{
		RuntimeDispatchBoundaryState boundary{};
		const auto start = ReduceRuntimeDispatchBoundary(
			boundary, FlightInputAction::kStartFlight, true);
		assert(start.boundaryQueued && !start.deferSemanticEdge);
		const auto readyAfterStart = ReduceRuntimeDispatchBoundary(
			start.state, FlightInputAction::kToggleCombatReady, true);
		assert(readyAfterStart.deferSemanticEdge && readyAfterStart.rebindSession);

		const auto stop = ReduceRuntimeDispatchBoundary(
			RuntimeDispatchBoundaryState{}, FlightInputAction::kStopFlight, true);
		const auto readyAfterStop = ReduceRuntimeDispatchBoundary(
			stop.state, FlightInputAction::kToggleCombatReady, true);
		assert(readyAfterStop.deferSemanticEdge && readyAfterStop.rebindSession);
		const auto restarted = ReduceRuntimeDispatchBoundary(
			RuntimeDispatchBoundaryState{}, FlightInputAction::kStartFlight, true);
		const auto readyAfterRestart = ReduceRuntimeDispatchBoundary(
			restarted.state, FlightInputAction::kToggleCombatReady, true);
		assert(readyAfterRestart.deferSemanticEdge && readyAfterRestart.rebindSession);

		const auto held = ReduceRuntimeDispatchBoundary(
			start.state, FlightInputAction::kToggleCombatReady, false);
		assert(!held.deferSemanticEdge);

		const auto beforeBoundaryRefresh = DecideDeferredInputReplay(
			1, true, false, 12, 12, true, false);
		assert(beforeBoundaryRefresh.disposition == InputSnapshotDispatchDisposition::kDeferUntilRefresh);
		assert(!beforeBoundaryRefresh.consumeOne && beforeBoundaryRefresh.scheduleNextRefresh);
		const auto afterBoundaryRefresh = DecideDeferredInputReplay(
			1, true, false, 12, 13, true, true);
		assert(afterBoundaryRefresh.disposition == InputSnapshotDispatchDisposition::kDispatch);
		assert(afterBoundaryRefresh.consumeOne && !afterBoundaryRefresh.scheduleNextRefresh);
	}

	void TestDeferredRebindNeverCrossesActorOrUnloadBoundary()
	{
		assert(ShouldPreserveDeferredInputRebind(true, false, false, false));
		assert(ShouldPreserveDeferredInputRebind(false, false, false, true));
		assert(!ShouldPreserveDeferredInputRebind(true, true, false, false));
		assert(!ShouldPreserveDeferredInputRebind(true, false, true, true));
		assert(!ShouldPreserveDeferredInputRebind(false, true, true, true));
	}

	void TestNativeFallbackGateRejectsPendingReadyBarrier()
	{
		const NativeFallbackGateRequest request{ 81, 12, 6, 1 };
		const auto pending = DecideNativeFallbackGate(
			81, 6, request, true, true, true, true, true, 1);
		assert(!pending.execute);
		assert(pending.barrierPending && pending.definitiveRejection);
		assert(pending.abortGroundObserver);

		const auto retryable = DecideNativeFallbackGate(
			81, 6, request, true, true, false, true, false, 1);
		assert(!retryable.execute);
		assert(!retryable.definitiveRejection);
		assert(retryable.retryableEngineUnavailable);
	}

	void TestInvalidReadyTokenAndStaleDeferredDrain()
	{
		const auto invalidApply = ReduceReadyGenerationBarrier(
			ResetReadyGenerationBarrier({}, 91, 0),
			ReadyGenerationBarrierEvent::kApply,
			ReadyGenerationToken{});
		assert(invalidApply.blocked && !invalidApply.accepted);

		const auto stale = DecideDeferredInputReplay(2, true, false, 91, 92);
		assert(stale.disposition == InputSnapshotDispatchDisposition::kRejectSession);
		assert(stale.consumeOne && stale.scheduleNextRefresh);
	}

	void TestBoundaryDeferralBoundedAndUnavailableQueueIsNonRefreshing()
	{
		const auto firstMismatch = DecideDeferredInputReplay(
			2, true, false, 91, 91, true, false, 0);
		assert(firstMismatch.disposition == InputSnapshotDispatchDisposition::kDeferUntilRefresh);
		assert(!firstMismatch.consumeOne && firstMismatch.scheduleNextRefresh);
		assert(firstMismatch.nextBoundaryDeferralAttempts == 1);

		const auto successfulRefresh = DecideDeferredInputReplay(
			1, true, false, 91, 92, true, true, 1);
		assert(successfulRefresh.disposition == InputSnapshotDispatchDisposition::kDispatch);
		assert(successfulRefresh.consumeOne && !successfulRefresh.terminalDiscard);

		const auto secondMismatch = DecideDeferredInputReplay(
			2, true, false, 91, 91, true, false, 1);
		assert(secondMismatch.disposition == InputSnapshotDispatchDisposition::kDeferUntilRefresh);
		assert(!secondMismatch.consumeOne && secondMismatch.scheduleNextRefresh);

		const auto terminalMismatch = DecideDeferredInputReplay(
			2, true, false, 91, 91, true, false, 2);
		assert(terminalMismatch.disposition == InputSnapshotDispatchDisposition::kRejectSession);
		assert(terminalMismatch.consumeOne && terminalMismatch.terminalDiscard);
		assert(terminalMismatch.scheduleNextRefresh);
		const auto fifoNext = DecideDeferredInputReplay(
			1, true, false, 92, 92);
		assert(fifoNext.disposition == InputSnapshotDispatchDisposition::kDispatch);
		assert(fifoNext.consumeOne);

		const auto unavailable = DecideTaskInterfaceUnavailable(true);
		assert(unavailable.retireReadyToken);
		assert(!unavailable.refreshGameThreadState);
	}

	void TestGroundReadySupersedesOppositePostFlightTransition()
	{
		// A preserved post-flight transition targeting drawn=true must be
		// disarmed before a newer ground Ready edge targeting false is observed.
		const auto decision = DecideGroundReadySupersession(
			true, true, true, false);
		assert(decision.supersedePostFlightTransition);
		assert(decision.disarmNativeFallback);
		assert(decision.oppositeTarget);
	}

	void TestEquipmentSwapSessionResetClearsPin()
	{
		auto state = ArmWeaponEquipmentSwap();
		for (std::uint16_t i = 0; i < kWeaponEquipmentSwapStableGraceUpdates; ++i) {
			state = ObserveWeaponEquipmentSwap(
				state,
				WeaponEquipmentSwapObservation::kStableAtLogicalTarget);
		}
		assert(!IsWeaponEquipmentSwapPinned(state));
		assert(!state.reconciliationRequired);
		const auto reset = ResetWeaponEquipmentSwap();
		assert(!reset.pending && !reset.readinessPreserved);
	}

	void TestQueuedSequenceAllocatedAtExecutionOrder()
	{
		QueuedActionExecutionSequenceState state{};
		const auto first = AllocateQueuedActionExecutionSequence(state);
		const auto second = AllocateQueuedActionExecutionSequence(state);
		assert(first == 1);
		assert(second == 2);
		assert(IsQueuedActionSequenceNewer(first, second));
	}

	void TestWarhammerTypeTenIsTwoHanded()
	{
		assert(IsTwoHandedWeaponTypeValue(5));
		assert(IsTwoHandedWeaponTypeValue(6));
		assert(IsTwoHandedWeaponTypeValue(10));
		assert(!IsTwoHandedWeaponTypeValue(1));
		assert(ResolveWeaponEquipmentFamily(10, -1, false) == WeaponEquipmentFamily::kTwoHanded);
	}

	void TestDualWieldLandingCleanupIsIdempotent()
	{
		const FlightStopState input{
			true, true, true, true, true, true, true,
			WeaponTransitionState{ false, true, false, false, 0 } };
		const auto once = ReduceFlightStop(input);
		const auto twice = ReduceFlightStop(once);
		assert(!once.flying && !once.descending && !once.combatActive);
		assert(!once.blockRequested && !once.shoutHeld && !once.launchHeld && !once.boostHeld);
		assert(!once.weapon.pending && !once.weapon.postFlight);
		assert(once.flying == twice.flying);
		assert(once.combatActive == twice.combatActive);
		assert(once.weapon.pending == twice.weapon.pending);
	}

	void TestLandingHandsOffCurrentIdentityDuringRepeatedDrawingAndSheathing()
	{
		const auto drawing = DecideLandingWeaponHandoff(
			true, true, true, true, true, true);
		assert(drawing.armGroundObserver);
		assert(drawing.targetDrawn);
		assert(drawing.rebaseToCurrentIdentity);

		const auto sheathing = DecideLandingWeaponHandoff(
			true, true, false, true, true, true);
		assert(sheathing.armGroundObserver);
		assert(!sheathing.targetDrawn);

		GroundWeaponObserverState observer{};
		const auto readyAfterLanding = ReduceGroundWeaponObserver(
			observer,
			GroundWeaponObserverEvent::kReadyEdge);
		assert(readyAfterLanding.armed && readyAfterLanding.state.pending);

		// An explicit sheathe edge during/after landing replaces the handoff
		// transaction and targets the current identity, rather than reviving the
		// stale airborne draw request.
		const auto explicitSheathe = ReduceGroundWeaponObserver(
			readyAfterLanding.state,
			GroundWeaponObserverEvent::kReadyEdge);
		assert(explicitSheathe.armed && explicitSheathe.replaced);
		assert(explicitSheathe.state.pending);
		assert(explicitSheathe.state.edgeSequence == readyAfterLanding.state.edgeSequence + 1);
	}

	void TestSecondStopCannotRequestWorldCleanup()
	{
		assert(!OwnsWorldStateForStop(true, false));
		assert(OwnsWorldStateForStop(false, true));

		FlightStopState input{};
		input.flying = true;
		input.worldStateOwned = true;
		assert(OwnsWorldStateForStop(input.flying, input.worldStateOwned));

		const auto first = ReduceFlightStop(input);
		assert(!first.flying && !first.worldStateOwned);
		assert(!OwnsWorldStateForStop(first.flying, first.worldStateOwned));

		const auto second = ReduceFlightStop(first);
		assert(!second.flying && !second.worldStateOwned);
		assert(!OwnsWorldStateForStop(second.flying, second.worldStateOwned));
	}

	void TestShoutPressHeldReleaseEdges()
	{
		ShoutLatchState state{};
		const auto press = ReduceShoutEdge(state, ShoutEdge::kPress);
		assert(press.acceptedPress && press.state.held);

		const auto held = ReduceShoutEdge(press.state, ShoutEdge::kHeld);
		assert(held.ignoredHeld && !held.acceptedPress && !held.acceptedRelease);
		assert(held.state.held);

		const auto release = ReduceShoutEdge(held.state, ShoutEdge::kRelease);
		assert(release.acceptedRelease && !release.state.held);

		const auto duplicateRelease = ReduceShoutEdge(release.state, ShoutEdge::kRelease);
		assert(!duplicateRelease.acceptedRelease && !duplicateRelease.state.held);
	}

	void TestSemanticButtonEdgeKeepsReleaseAndDropsHeldOnlySamples()
	{
		const ButtonInputSnapshot heldOnly{
			"Shout", 0, 0, false, false, true, true, 0.25F };
		const ButtonInputSnapshot pressed{
			"Shout", 0, 0, false, false, true, false, 0.0F };
		const ButtonInputSnapshot down{
			"Shout", 0, 0, false, true, true, false, 0.0F };
		const ButtonInputSnapshot release{
			"Shout", 0, 0, true, false, false, false, 0.0F };
		const ButtonInputSnapshot other{
			"Shout", 0, 0, false, false, false, false, 0.0F };

		// Deferred input must retain the release: otherwise a shout/block/Ready
		// latch from the old dispatch can survive until a later unrelated action.
		assert(!IsButtonPressEdge(heldOnly));
		assert(IsButtonPressEdge(pressed));
		assert(IsButtonPressEdge(down));
		assert(!IsButtonPressEdge(release));
		assert(!IsButtonPressEdge(other));
		assert(!IsSemanticButtonEdge(heldOnly));
		assert(IsSemanticButtonEdge(pressed));
		assert(IsSemanticButtonEdge(down));
		assert(IsSemanticButtonEdge(release));
		assert(!IsSemanticButtonEdge(other));
	}

	void TestShoutDuplicateReleaseCannotQueueAnAcceptedEdge()
	{
		const auto firstRelease = ReduceShoutEdge(
			ShoutLatchState{}, ShoutEdge::kRelease);
		assert(!firstRelease.acceptedPress);
		assert(!firstRelease.acceptedRelease);
		assert(!firstRelease.state.held);

		const auto press = ReduceShoutEdge(firstRelease.state, ShoutEdge::kPress);
		const auto release = ReduceShoutEdge(press.state, ShoutEdge::kRelease);
		const auto duplicate = ReduceShoutEdge(release.state, ShoutEdge::kRelease);
		assert(press.acceptedPress);
		assert(release.acceptedRelease);
		assert(!duplicate.acceptedRelease);
		assert(!duplicate.state.held);
	}

	void TestStopClearsHeldInputLatches()
	{
		const auto reset = ResetInputLatches(
			InputLatchState{ true, true, true, true, true, true },
			true);
		assert(!reset.launchHeld && !reset.ascendHeld && !reset.descendHeld);
		assert(!reset.readyWeaponHeld && !reset.shoutHeld && !reset.boostHeld);

		// Descent's movement reset may preserve an in-progress vanilla shout;
		// flight stop and suppression use the clearing form above.
		const auto preserveShout = ResetInputLatches(
			InputLatchState{ true, true, true, true, true, true },
			false);
		assert(!preserveShout.readyWeaponHeld && preserveShout.shoutHeld);
	}

	void TestHeldDoesNotRearmWhirlwindLatch()
	{
		const auto press = ReduceShoutEdge(ShoutLatchState{ false, true }, ShoutEdge::kPress);
		assert(press.acceptedPress && press.state.whirlwindPending);
		const auto held = ReduceShoutEdge(press.state, ShoutEdge::kHeld);
		assert(held.ignoredHeld && held.state.whirlwindPending);
		const auto release = ReduceShoutEdge(held.state, ShoutEdge::kRelease);
		assert(release.acceptedRelease && !release.state.whirlwindPending);
	}

	void TestMagickaDrainOwnership()
	{
		const auto first = AccumulateMagickaDrain(0.0F, 0.016F);
		assert(first.chargeSeconds == 0.0F);
		const auto second = AccumulateMagickaDrain(first.carrySeconds, 0.084F);
		assert(std::fabs(second.chargeSeconds - 0.1F) < 0.0001F);
		assert(second.carrySeconds == 0.0F);
		const auto clamped = AccumulateMagickaDrain(0.0F, 2.0F);
		assert(std::fabs(clamped.chargeSeconds - kMagickaDrainMaxElapsedSeconds) < 0.0001F);

		// A stuck delay can be released only after three later-tick samples.
		MagickaRegenObservationState stuck{ true, 0.0F, 1.0F, 0 };
		auto step = ObserveMagickaRegenDelay(stuck, 1.0F);
		assert(step.result == MagickaRegenObservationResult::kWait);
		step = ObserveMagickaRegenDelay(step.state, 1.0F);
		assert(step.result == MagickaRegenObservationResult::kWait);
		step = ObserveMagickaRegenDelay(step.state, 1.0F);
		assert(step.result == MagickaRegenObservationResult::kStable);

		assert(ObserveMagickaRegenDelay(
			MagickaRegenObservationState{ true, 0.0F, 1.0F, 0 },
			0.98F).result == MagickaRegenObservationResult::kAbort);
		assert(ObserveMagickaRegenDelay(
			MagickaRegenObservationState{ true, 0.0F, 1.0F, 0 },
			1.02F).result == MagickaRegenObservationResult::kAbort);
		assert(ObserveMagickaRegenDelay(
			MagickaRegenObservationState{ false, 0.0F, 1.0F, 0 },
			1.0F).result == MagickaRegenObservationResult::kAbort);

		// A later increase without an immediate DAF-owned value is
		// indistinguishable from an external writer and must never be claimed.
		const MagickaRegenObservationState delayed{ false, 0.0F, 0.0F, 0, true };
		assert(ObserveMagickaRegenDelay(delayed, 1.0F).result == MagickaRegenObservationResult::kAbort);
		assert(!ObserveMagickaRegenDelay(delayed, 1.0F).state.dafOwned);
		assert(ObserveMagickaRegenDelay(delayed, 0.0F).result == MagickaRegenObservationResult::kWait);
	}

	void TestMagickaObservationIdentityAndNoWritePolicy()
	{
		const MagickaRegenObservationState finalDrain{
			false, 0.0F, 0.0F, 0, true, 41, 9, 0x01020304 };

		// A delayed final-drain publication is observable, but never becomes a
		// DAF-owned value merely because it is above the baseline.
		assert(ObserveMagickaRegenDelay(finalDrain, 1.0F).result == MagickaRegenObservationResult::kAbort);
		assert(!ObserveMagickaRegenDelay(finalDrain, 1.0F).state.dafOwned);

		// Samples from a previous session, a previous drain, or an unloaded actor
		// are all stale and cannot authorize a write.
		assert(IsMagickaRegenObservationCurrent(finalDrain, 41, 9, 0x01020304, true));
		assert(!IsMagickaRegenObservationCurrent(finalDrain, 42, 9, 0x01020304, true));
		assert(!IsMagickaRegenObservationCurrent(finalDrain, 41, 10, 0x01020304, true));
		assert(!IsMagickaRegenObservationCurrent(finalDrain, 41, 9, 0x05060708, true));
		assert(!IsMagickaRegenObservationCurrent(finalDrain, 41, 9, 0x01020304, false));
		assert(ShouldAdvanceMagickaDrainSequence(true));
		assert(!ShouldAdvanceMagickaDrainSequence(false));

		// A real final drain remains diagnostic through small later ticks and is
		// explicitly retained for the landing window while the actor is loaded.
		assert(ObserveMagickaRegenDelay(finalDrain, 0.0F).result == MagickaRegenObservationResult::kWait);
		assert(ShouldContinueMagickaObservationAfterAirborneTimeout(true, true));
		assert(!ShouldContinueMagickaObservationAfterAirborneTimeout(false, true));
		assert(!ShouldContinueMagickaObservationAfterAirborneTimeout(true, false));

		// A decaying or externally raised value aborts an existing observation;
		// there is no release/write decision in either case.
		const MagickaRegenObservationState owned{
			true, 0.0F, 1.0F, 0, false, 41, 9, 0x01020304 };
		assert(ObserveMagickaRegenDelay(owned, 0.98F).result == MagickaRegenObservationResult::kAbort);
		assert(ObserveMagickaRegenDelay(owned, 1.02F).result == MagickaRegenObservationResult::kAbort);
	}

	void TestQueuedActionCooperation()
	{
		assert(IsCooperativeFlightInputAction(FlightInputAction::kToggleCombatReady));
		assert(IsCooperativeFlightInputAction(FlightInputAction::kShoutPress));
		assert(IsCooperativeFlightInputAction(FlightInputAction::kBlockRelease));
		assert(!IsCooperativeFlightInputAction(FlightInputAction::kStartFlight));
		assert(!IsCooperativeFlightInputAction(FlightInputAction::kBeginDescent));
	}

	void TestQueuedActionOrderingAndSessionGuard()
	{
		assert(IsQueuedActionSequenceNewer(0, 1));
		assert(IsQueuedActionSequenceNewer(7, 8));
		assert(!IsQueuedActionSequenceNewer(8, 8));
		assert(!IsQueuedActionSequenceNewer(8, 7));

		assert(IsQueuedActionSessionCurrent(
			FlightInputAction::kStartFlight, 12, 12, false, 0, 4));
		assert(!IsQueuedActionSessionCurrent(
			FlightInputAction::kStartFlight, 12, 12, true, 0, 4));
		assert(IsQueuedActionSessionCurrent(
			FlightInputAction::kStopFlight, 13, 12, true, 4, 9));
		// Enqueued under session N, execution under N+1: ordinary actions are
		// stale and must not be applied to the new flight.
		assert(!IsQueuedActionSessionCurrent(
			FlightInputAction::kBeginCombat, 13, 12, true, 4, 9));
		assert(!IsQueuedActionSessionCurrent(
			FlightInputAction::kStopFlight, 14, 12, true, 4, 9));
		assert(!IsQueuedActionSessionCurrent(
			FlightInputAction::kStopFlight, 13, 12, true, 9, 9));

		// The task payload is a value snapshot: later changes to the source
		// variables cannot alter the queued action's copied intent.
		const FlightInputActionSnapshot action{
			FlightInputAction::kSetMovementInput, 0.75F, -0.25F, false };
		assert(action.action == FlightInputAction::kSetMovementInput);
		assert(std::fabs(action.valueA - 0.75F) < 0.0001F);
		assert(std::fabs(action.valueB + 0.25F) < 0.0001F);

		// The input callback stamps session N before its outer task is queued;
		// executing that envelope after N+1 must reject the ordinary action.
		const FlightInputActionSnapshot stamped{
			FlightInputAction::kBeginCombat, 0.0F, 0.0F, false, 12 };
		assert(stamped.sourceSession == 12);
		assert(!IsQueuedActionSessionCurrent(
			stamped.action, 13, stamped.sourceSession, true, 4, 9));
	}

	void TestInputActionCoalescing()
	{
		assert(IsCoalescibleInputAction(FlightInputAction::kSetMovementInput));
		assert(IsCoalescibleInputAction(FlightInputAction::kSetVerticalInput));
		assert(IsCoalescibleInputAction(FlightInputAction::kSetBoostHeld));
		assert(IsCoalescibleInputAction(FlightInputAction::kClearShout));
		assert(!IsCoalescibleInputAction(FlightInputAction::kStartFlight));
		assert(!IsCoalescibleInputAction(FlightInputAction::kStopFlight));
		assert(!IsCoalescibleInputAction(FlightInputAction::kBeginDescent));
		assert(!IsCoalescibleInputAction(FlightInputAction::kCancelDescent));
		assert(!IsCoalescibleInputAction(FlightInputAction::kToggleCombatReady));
		assert(!IsCoalescibleInputAction(FlightInputAction::kBeginCombat));
		assert(!IsCoalescibleInputAction(FlightInputAction::kBlockRequest));
		assert(!IsCoalescibleInputAction(FlightInputAction::kBlockRelease));
		assert(!IsCoalescibleInputAction(FlightInputAction::kShoutPress));
		assert(!IsCoalescibleInputAction(FlightInputAction::kShoutRelease));
		assert(!IsCoalescibleInputAction(FlightInputAction::kLaunchBoost));
		assert(!IsCoalescibleInputAction(FlightInputAction::kObserveGroundWeaponTransition));
		assert(!ShouldLogFlightActionQueueAcceptance(FlightInputAction::kSetVerticalInput));
		assert(!ShouldLogFlightActionQueueAcceptance(FlightInputAction::kClearShout));
		assert(ShouldLogFlightActionQueueAcceptance(FlightInputAction::kShoutPress));
		assert(AreInputActionAnalogValuesEquivalent(0.5F, 0.505F));
		assert(!AreInputActionAnalogValuesEquivalent(0.0F, 0.005F));
		assert(!AreInputActionAnalogValuesEquivalent(0.5F, -0.5F));

		FlightInputActionSnapshot vertical{
			FlightInputAction::kSetVerticalInput, 1.0F, 0.0F, false, 7 };
		const auto firstVertical = ReduceInputActionForQueue({}, vertical, 100, 3, false);
		assert(firstVertical.enqueue && !firstVertical.flushSummary);

		const auto heldVertical = ReduceInputActionForQueue(
			firstVertical.state, vertical, 125, 3, false);
		assert(!heldVertical.enqueue && !heldVertical.flushSummary);
		assert(heldVertical.state.suppressed.pending);
		assert(heldVertical.state.suppressed.count == 1);
		assert(heldVertical.state.suppressed.firstTimestampMs == 125);
		assert(heldVertical.state.suppressed.lastTimestampMs == 125);

		const auto heldVerticalAgain = ReduceInputActionForQueue(
			heldVertical.state, vertical, 150, 3, false);
		assert(!heldVerticalAgain.enqueue);
		assert(heldVerticalAgain.state.suppressed.count == 2);
		assert(heldVerticalAgain.state.suppressed.firstTimestampMs == 125);
		assert(heldVerticalAgain.state.suppressed.lastTimestampMs == 150);
		assert(heldVerticalAgain.state.suppressed.firstAction == FlightInputAction::kSetVerticalInput);
		assert(heldVerticalAgain.state.suppressed.lastAction == FlightInputAction::kSetVerticalInput);
		assert(heldVerticalAgain.state.suppressed.firstSourceSession == 7);
		assert(heldVerticalAgain.state.suppressed.lastSourceSession == 7);
		assert(heldVerticalAgain.state.suppressed.firstSequenceDomain == 3);
		assert(heldVerticalAgain.state.suppressed.lastSequenceDomain == 3);
		assert(std::fabs(heldVerticalAgain.state.suppressed.firstValueA - 1.0F) < 0.0001F);
		assert(std::fabs(heldVerticalAgain.state.suppressed.lastValueA - 1.0F) < 0.0001F);

		// Ordering-sensitive edges consume every pending summary before their
		// caller advances the sequence domain and enqueues the edge.  The generic
		// reducer contract applies equally to shout, Ready Weapon, and block.
		const auto assertControlEdgeFlush = [&](FlightInputAction a_action) {
			const FlightInputActionSnapshot edge{
				a_action, 0.0F, 0.0F, false, 7 };
			const auto decision = ReduceInputActionForQueue(
				heldVerticalAgain.state, edge, 160, 3, false);
			assert(decision.enqueue && decision.flushSummary);
			assert(decision.flushedSummary.count == 2);
			assert(!decision.state.suppressed.pending);
			assert(decision.state.sequenceDomain == 3);
		};
		assertControlEdgeFlush(FlightInputAction::kShoutPress);
		assertControlEdgeFlush(FlightInputAction::kToggleCombatReady);
		assertControlEdgeFlush(FlightInputAction::kBlockRequest);

		// A release is an edge even when its effective state is already zero.  It
		// must be emitted immediately and flush the preceding held summary.
		vertical.valueA = 0.0F;
		const auto verticalRelease = ReduceInputActionForQueue(
			heldVerticalAgain.state, vertical, 175, 3, true);
		assert(verticalRelease.enqueue && verticalRelease.flushSummary);
		assert(verticalRelease.flushedSummary.count == 2);
		assert(!verticalRelease.state.suppressed.pending);

		FlightInputActionSnapshot boost{
			FlightInputAction::kSetBoostHeld, 0.0F, 0.0F, true, 7 };
		const auto firstBoost = ReduceInputActionForQueue({}, boost, 200, 3, false);
		assert(firstBoost.enqueue);
		const auto heldBoost = ReduceInputActionForQueue(firstBoost.state, boost, 225, 3, false);
		assert(!heldBoost.enqueue && heldBoost.state.suppressed.count == 1);
		boost.flag = false;
		const auto boostRelease = ReduceInputActionForQueue(heldBoost.state, boost, 250, 3, true);
		assert(boostRelease.enqueue && boostRelease.flushSummary);
		assert(boostRelease.flushedSummary.firstFlag);
		assert(boostRelease.flushedSummary.lastFlag);

		// Session and sequence-domain boundaries re-arm the first state update;
		// old-session duplicates can never suppress a new-session action.
		FlightInputActionSnapshot movement{
			FlightInputAction::kSetMovementInput, 0.5F, -0.25F, false, 10 };
		const auto firstMovement = ReduceInputActionForQueue({}, movement, 300, 8, false);
		const auto duplicateMovement = ReduceInputActionForQueue(
			firstMovement.state, movement, 325, 8, false);
		assert(!duplicateMovement.enqueue);
		movement.sourceSession = 11;
		const auto newSession = ReduceInputActionForQueue(
			duplicateMovement.state, movement, 350, 8, false);
		assert(newSession.enqueue && newSession.flushSummary);
		const auto newDomain = ReduceInputActionForQueue(
			newSession.state, movement, 375, 9, false);
		assert(newDomain.enqueue && !newDomain.flushSummary);
		movement.valueA = 0.505F;
		const auto analogJitter = ReduceInputActionForQueue(
			newDomain.state, movement, 385, 9, false);
		assert(!analogJitter.enqueue);
		movement.valueA = 0.53F;
		const auto meaningfulAnalogChange = ReduceInputActionForQueue(
			analogJitter.state, movement, 390, 9, false);
		assert(meaningfulAnalogChange.enqueue && meaningfulAnalogChange.flushSummary);

		const auto duplicateInNewDomain = ReduceInputActionForQueue(
			meaningfulAnalogChange.state, movement, 400, 9, false);
		assert(!duplicateInNewDomain.enqueue);
		const auto flushed = FlushInputActionCoalescing(
			duplicateInNewDomain.state, true);
		assert(!flushed.enqueue && flushed.flushSummary);
		assert(flushed.flushedSummary.count == 1);
		assert(flushed.flushedSummary.firstTimestampMs == 400);
		assert(flushed.flushedSummary.lastTimestampMs == 400);
		assert(!flushed.state.initialized && !flushed.state.suppressed.pending);
		const auto emptyFlush = FlushInputActionCoalescing(flushed.state, true);
		assert(!emptyFlush.flushSummary);

		// A failed ClearShout callback can leave its accepted coalescing baseline in
		// place.  Recovery must force an equivalent state action back into the task
		// queue instead of allowing it to be suppressed again.
		const FlightInputActionSnapshot clearShout{
			FlightInputAction::kClearShout, 0.0F, 0.0F, false, 11 };
		const auto firstClear = ReduceInputActionForQueue(
			{}, clearShout, 450, 10, false);
		assert(firstClear.enqueue);
		const auto suppressedClear = ReduceInputActionForQueue(
			firstClear.state, clearShout, 475, 10, false);
		assert(!suppressedClear.enqueue);
		const auto forcedClear = ReduceInputActionForQueue(
			suppressedClear.state, clearShout, 500, 10, true);
		assert(forcedClear.enqueue);

		// Shout input remains an edge stream, not a coalescible state channel.
		const FlightInputActionSnapshot shout{
			FlightInputAction::kShoutPress, 0.0F, 0.0F, false, 11 };
		const auto shoutPress = ReduceInputActionForQueue({}, shout, 500, 10, false);
		const auto repeatedShoutPress = ReduceInputActionForQueue(
			shoutPress.state, shout, 525, 10, false);
		assert(shoutPress.enqueue && repeatedShoutPress.enqueue);
		assert(!shoutPress.state.initialized && !repeatedShoutPress.state.initialized);
		const FlightInputActionSnapshot shoutRelease{
			FlightInputAction::kShoutRelease, 0.0F, 0.0F, false, 11 };
		const auto shoutReleaseAfterPress = ReduceInputActionForQueue(
			repeatedShoutPress.state, shoutRelease, 550, 10, false);
		assert(shoutReleaseAfterPress.enqueue);
		assert(!shoutReleaseAfterPress.state.initialized);
	}

	void TestDiagnosticAggregatesAndBoundaries()
	{
		MagickaDrainDiagnosticAggregate drain{};
		assert(!HasMagickaDrainDiagnosticAggregate(drain));
		drain = AccumulateMagickaDrainDiagnostic(
			drain, 7, 10, 0x1234, 50.0F, 1.0F, 0.1F, 0.0F, 1.0F, false, true);
		drain = AccumulateMagickaDrainDiagnostic(
			drain, 7, 11, 0x1234, 49.0F, 1.0F, 0.1F, 1.0F, 2.0F, false, true);
		assert(drain.count == 2);
		assert(drain.firstDrainSequence == 10 && drain.lastDrainSequence == 11);
		assert(drain.actorFormId == 0x1234);
		assert(std::fabs(drain.totalAmount - 2.0F) < 0.0001F);
		assert(GetMagickaDrainDiagnosticFlushReason(
			drain, 7, 0x1234, true, false, 0.5F) == MagickaDiagnosticFlushReason::kNone);
		assert(GetMagickaDrainDiagnosticFlushReason(
			drain, 7, 0x1234, true, false, 1.0F) ==
			MagickaDiagnosticFlushReason::kOneSecondBoundary);
		assert(GetMagickaDrainDiagnosticFlushReason(
			drain, 8, 0x1234, true, false, 0.0F) ==
			MagickaDiagnosticFlushReason::kSessionBoundary);
		assert(GetMagickaDrainDiagnosticFlushReason(
			drain, 7, 0x9999, true, false, 0.0F) ==
			MagickaDiagnosticFlushReason::kActorBoundary);
		assert(GetMagickaDrainDiagnosticFlushReason(
			drain, 7, 0x1234, false, false, 0.0F) ==
			MagickaDiagnosticFlushReason::kPhaseBoundary);

		drain = AccumulateMagickaDrainDiagnostic(
			{}, 7, 0, 0x1234, 0.0F, 0.0F, 0.0F, 2.0F, 2.0F, true, true);
		assert(drain.depleted && drain.firstDrainSequence == 0 && drain.lastDrainSequence == 0);
		assert(GetMagickaDrainDiagnosticFlushReason(
			drain, 7, 0x1234, true, false, 0.0F) ==
			MagickaDiagnosticFlushReason::kDepletion);
		assert(std::string_view(MagickaDiagnosticFlushReasonName(
			MagickaDiagnosticFlushReason::kDepletion)) == "depletion");

		MagickaRegenDiagnosticAggregate regen{};
		assert(!HasMagickaRegenDiagnosticAggregate(regen));
		regen = AccumulateMagickaRegenDiagnostic(
			regen, 7, 11, 0x1234, 1.0F, 1.43F, 1.0F, 1.5F, true, false,
			MagickaRegenDiagnosticKind::kWait);
		regen = AccumulateMagickaRegenDiagnostic(
			regen, 7, 12, 0x1234, 2.0F, 1.43F, 1.0F, 1.5F, true, false,
			MagickaRegenDiagnosticKind::kObservationAbort);
		assert(regen.count == 2 && regen.waitCount == 1 && regen.observationCount == 1);
		assert(regen.firstDrainSequence == 11 && regen.lastDrainSequence == 12);
		assert(regen.drainSequence == 12);  // compatibility alias is always latest
		assert(regen.firstKind == MagickaRegenDiagnosticKind::kWait);
		assert(regen.lastKind == MagickaRegenDiagnosticKind::kObservationAbort);
		assert(std::fabs(regen.firstCurrent - 1.43F) < 0.0001F);
		assert(std::fabs(regen.lastSample - 2.0F) < 0.0001F);
		assert(GetMagickaRegenDiagnosticFlushReason(
			regen, 7, 0x1234, true, false, 1.43F, 1.0F, 1.5F, 0.5F) ==
			MagickaDiagnosticFlushReason::kNone);
		assert(GetMagickaRegenDiagnosticFlushReason(
			regen, 7, 0x1234, true, false, 1.45F, 1.0F, 1.5F, 0.0F) ==
			MagickaDiagnosticFlushReason::kValueBoundary);
		assert(GetMagickaRegenDiagnosticFlushReason(
			regen, 7, 0x1234, false, true, 1.43F, 1.0F, 1.5F, 0.0F) ==
			MagickaDiagnosticFlushReason::kPhaseBoundary);
		assert(GetMagickaRegenDiagnosticFlushReason(
			regen, 7, 0x9999, true, false, 1.43F, 1.0F, 1.5F, 0.0F) ==
			MagickaDiagnosticFlushReason::kActorBoundary);
		assert(GetMagickaRegenDiagnosticFlushReason(
			regen, 8, 0x1234, true, false, 1.43F, 1.0F, 1.5F, 0.0F) ==
			MagickaDiagnosticFlushReason::kSessionBoundary);
		assert(GetMagickaRegenDiagnosticFlushReason(
			regen, 7, 0x1234, true, false, 1.43F, 1.0F, 1.5F, 1.0F) ==
			MagickaDiagnosticFlushReason::kOneSecondBoundary);

		// Production pipeline regression: wait and final-abort observations use
		// the exact same timestamp-injected orchestration that native callers use.
		// At the old ~8.5 Hz paired cadence, three and a half seconds produce
		// one ordered stream, not two independent records per second.
		MagickaRegenDiagnosticPipeline pipeline{};
		pipeline.BeginObservation();
		MagickaRegenDiagnosticEmission emissions[8]{};
		std::uint32_t emittedSummaries = 0;
		const auto capture = [&](const MagickaRegenDiagnosticEmission& a_emission) {
			if (!a_emission.emitted) {
				return;
			}
			assert(emittedSummaries < 8);
			emissions[emittedSummaries++] = a_emission;
		};
		for (std::uint64_t frame = 0; frame < 210; ++frame) {
			const auto nowMs = frame * 1000 / 60;
			capture(pipeline.Record(
				MagickaRegenDiagnosticRecord{
					7, frame, 0x1234, 1.43F, 1.43F, 1.0F, 1.5F, false, true,
					MagickaRegenDiagnosticKind::kWait },
				nowMs));
			capture(pipeline.Record(
				MagickaRegenDiagnosticRecord{
					7, frame, 0x1234, 1.43F, 1.43F, 1.0F, 1.5F, false, true,
					MagickaRegenDiagnosticKind::kObservationAbort },
				nowMs));
		}
		assert(emittedSummaries == 3);
		assert(pipeline.HasAggregate());
		assert(pipeline.aggregate.waitCount != 0 && pipeline.aggregate.observationCount != 0);

		// A meaningful value edge emits the prior aggregate before the new
		// record is absorbed; the following terminal flush emits that final record.
		const auto valueBoundaryMs = 3501;
		capture(pipeline.Record(
			MagickaRegenDiagnosticRecord{
				7, 211, 0x1234, 1.46F, 1.46F, 1.0F, 1.5F, false, true,
				MagickaRegenDiagnosticKind::kObservationAbort },
			valueBoundaryMs));
		assert(emittedSummaries == 4);
		assert(emissions[3].reason == "value_boundary");
		assert(emissions[3].emittedAtMs == valueBoundaryMs);
		assert(emissions[3].aggregate.lastDrainSequence == 209);

		const auto terminalFlush = pipeline.TerminalFlush("terminal_observation_abort", 3502);
		capture(terminalFlush);
		assert(emittedSummaries == 5);
		assert(emissions[4].reason == "terminal_observation_abort");
		assert(emissions[4].emittedAtMs == 3502);
		assert(!pipeline.HasAggregate());
		assert(!pipeline.observationPending);
		assert(!pipeline.pumpRunning);
		pipeline.PumpStopped();
		assert(!pipeline.observationPending && !pipeline.pumpRunning);

		const auto duplicateFlush = pipeline.TerminalFlush("terminal_observation_abort", 3503);
		assert(!duplicateFlush.emitted);
		assert(!pipeline.HasAggregate());

		MagickaRegenDiagnosticPipeline emptyPipeline{};
		emptyPipeline.BeginObservation();
		const auto emptyFlush = emptyPipeline.TerminalFlush("explicit_abort", 3504);
		assert(!emptyFlush.emitted);
		assert(!emptyPipeline.HasAggregate());
		assert(!emptyPipeline.observationPending && !emptyPipeline.pumpRunning);
		assert(!emptyPipeline.TerminalFlush("explicit_abort", 3505).emitted);

		// Regression: the airborne extension is a 400 ms sliding window, not a
		// per-frame timeout branch.
		float now = 0.0F;
		float windowStarted = 0.0F;
		float nextDue = kMagickaRegenObservationWindowSeconds;
		std::uint32_t extensions = 0;
		for (std::uint32_t frame = 0; frame < 120; ++frame) {
			now += 1.0F / 60.0F;
			const float windowElapsed = now - windowStarted;
			const bool due = now >= nextDue;
			if (ShouldExtendMagickaObservationWindow(true, true, windowElapsed, due)) {
				++extensions;
				windowStarted = now;
				nextDue = now + kMagickaRegenObservationWindowSeconds;
			}
		}
		assert(extensions <= 4);
		assert(extensions >= 3);
		assert(!ShouldExtendMagickaObservationWindow(
			true, true, kMagickaRegenObservationWindowSeconds, false));
		assert(ShouldExtendMagickaObservationWindow(
			true, true, kMagickaRegenObservationWindowSeconds, true));

		const auto airborne = ResolveDiagnosticPhase(true, false);
		const auto postFlight = ResolveDiagnosticPhase(false, true);
		assert(airborne.flight && !airborne.postFlight);
		assert(!postFlight.flight && postFlight.postFlight);

		assert(ShouldFlushInputStateRefresh(false, false, false));
		assert(ShouldFlushInputStateRefresh(true, true, false));
		assert(ShouldFlushInputStateRefresh(true, false, true));
		assert(!ShouldFlushInputStateRefresh(true, false, false));

		// Both probe states have the same durable fields; observation_pending is
		// deliberately not an input to the signature.
		const auto signatureWithoutPending = ComputeDiagnosticStateSignature(
			2, false, true, false, true, false, true, false, false, false, false);
		const auto signatureWithObservationPending = ComputeDiagnosticStateSignature(
			2, false, true, false, true, false, true, false, false, false, false);
		assert(signatureWithoutPending == signatureWithObservationPending);
		const auto signatureWithGroundEdge = ComputeDiagnosticStateSignature(
			2, false, true, false, true, false, true, false, false, false, true);
		assert(signatureWithGroundEdge != signatureWithoutPending);
		const auto signatureWithSwapPending = ComputeDiagnosticStateSignature(
			2, false, true, false, true, false, true, false, false, false, false,
			true, false, false, true);
		assert(signatureWithSwapPending != signatureWithoutPending);
		const auto signatureWithSwapPin = ComputeDiagnosticStateSignature(
			2, false, true, false, true, false, true, false, false, false, false,
			false, true, true, true);
		assert(signatureWithSwapPin != signatureWithoutPending);
	}

	void TestInputDiagnosticThrottleReducer()
	{
		const auto first = ReduceInputDiagnostic({}, 11, false, false);
		assert(first.emit && first.flushedSuppressedCount == 0);
		assert(first.state.initialized && first.state.signature == 11);

		const auto suppressed = ReduceInputDiagnostic(first.state, 11, false, false);
		assert(!suppressed.emit && suppressed.state.suppressedCount == 1);

		const auto changed = ReduceInputDiagnostic(suppressed.state, 12, false, false);
		assert(changed.emit && changed.flushedSuppressedCount == 1);
		assert(changed.state.suppressedCount == 0 && changed.state.signature == 12);

		const auto edge = ReduceInputDiagnostic(changed.state, 12, true, false);
		assert(edge.emit && edge.flushedSuppressedCount == 0);

		const auto suppressedBeforeHeartbeat = ReduceInputDiagnostic(edge.state, 12, false, false);
		assert(!suppressedBeforeHeartbeat.emit && suppressedBeforeHeartbeat.state.suppressedCount == 1);
		const auto heartbeat = ReduceInputDiagnostic(suppressedBeforeHeartbeat.state, 12, false, true);
		assert(heartbeat.emit && heartbeat.flushedSuppressedCount == 1);
	}

	void TestGroundObserverProgressAndPhaseBoundaries()
	{
		assert(!ShouldPumpWeaponTransition(false, false, false, false, false));
		assert(ShouldPumpWeaponTransition(false, false, false, true));
		assert(ShouldPumpWeaponTransition(false, false, false, false, true));
		assert(ShouldPumpWeaponTransition(false, false, false, false, false, true));
		// A fallback is legal only when the same non-transition actor state was
		// observed for the complete window.
		assert(DecideWeaponFallbackObservation(
			false, true, WeaponFallbackMotion::kStable, false, false) ==
			WeaponFallbackDecision::kWait);
		assert(DecideWeaponFallbackObservation(
			true, true, WeaponFallbackMotion::kStable, false, false) ==
			WeaponFallbackDecision::kFallback);

		// Target-direction progress gets one bounded extension; a transition that
		// then stalls receives exactly one delayed directional fallback.
		assert(DecideWeaponFallbackObservation(
			false, false, WeaponFallbackMotion::kTowardTarget, false, false) ==
			WeaponFallbackDecision::kDeferForProgress);
		assert(DecideWeaponFallbackObservation(
			true, false, WeaponFallbackMotion::kTowardTarget, true, false) ==
			WeaponFallbackDecision::kFallback);
		assert(DecideWeaponFallbackObservation(
			false, true, WeaponFallbackMotion::kStable, true, false) ==
			WeaponFallbackDecision::kAbort);

		// Opposite transitions and arbitrary state changes abort immediately.
		assert(DecideWeaponFallbackObservation(
			false, true, WeaponFallbackMotion::kOppositeTarget, false, false) ==
			WeaponFallbackDecision::kAbort);
		assert(DecideWeaponFallbackObservation(
			false, false, WeaponFallbackMotion::kStable, false, false) ==
			WeaponFallbackDecision::kAbort);

		// Once the one-shot has been issued, the observer may only wait for its
		// result or stop; it can never issue another fallback.
		assert(DecideWeaponFallbackObservation(
			false, true, WeaponFallbackMotion::kStable, false, true) ==
			WeaponFallbackDecision::kWait);
		assert(DecideWeaponFallbackObservation(
			true, true, WeaponFallbackMotion::kStable, false, true) ==
			WeaponFallbackDecision::kAbort);

		const auto routing = MakeWeaponRoutingDiagnosticState(true, false);
		assert(routing.requestedTargetDrawn && !routing.actualDrawn);
		const auto airborne = ResolveDiagnosticPhase(true, false);
		assert(airborne.flight && !airborne.postFlight);
		const auto postFlight = ResolveDiagnosticPhase(false, true);
		assert(!postFlight.flight && postFlight.postFlight);
	}
}

int main()
{
	TestWeaponEquipmentEpochTrackerAdvancesAba();
	TestNativeBlockActionRequiresNonzeroIdentityEpoch();
	TestReadyAndGroundAbaActionsRejectRetiredIncarnations();
	TestBlockRequestSwapReleaseAndStopAreMetadataOnly();
	TestNativeBlockLeaseRequiresExactClaimedState();
	TestNativeBlockLeaseCannotReleaseAfterEquipmentSwap();
	TestQueuedEquipmentActionRequiresCurrentEpoch();
	TestStopClearsStaleWeaponTransition();
	TestStopClearsStaleSheatheTransition();
	TestWeaponToggleUsesLogicalState();
	TestEquipmentSwapMustNotToggleReadiness();
	TestEquipmentSwapRevokesOnlyDafBlockOwnership();
	TestQueuedBlockActionRequiresCurrentEquipmentIdentity();
	TestFlightBlockReleaseRequiresCurrentEpochAndOwnership();
	TestEquipmentSwapRebaseArmsCurrentIdentityFallback();
	TestStaleFallbackMustAbortAfterEquipmentSwap();
	TestTwoHandToOneHandSwapPreservesReadiness();
	TestGroundObserverIdentityMismatchCancels();
	TestEquipmentSwapSettleWaitsForEdgeBeforeStableTarget();
	TestEquipmentSwapStableGraceTimeoutIsBounded();
	TestEquipmentSwapRepeatedEdgesHitAbsoluteObservationBound();
	TestWeaponTransitionExpiryKeepsTargetUntilRecoveryFailure();
	TestNativeFallbackPostCallRequiresExactStableRawState();
	TestTerminalRecoveryPreservesRequestedTargetForTransitionalRawState();
	TestTerminalRecoveryStableOppositePreservesRequestedTarget();
	TestTerminalFailureQuarantineSequence();
	TestEquipmentSwapOrderingPreservesReadinessOnTimeout();
	TestEquipmentSwapExpirySchedulesCurrentIdentityReconciliation();
	TestEquipmentSwapTracksEitherHandAndCustomTwoHandedFallback();
	TestTypeMinusOneRequiresActualWeaponPredicate();
	TestGroundObserverCancellationAndReadyReplacement();
	TestGroundObserverBaselineUsesLiveIdentity();
	TestRepeatedStopPreservesGroundObserverAndAllowsOneFallback();
	TestReadyGenerationLeaseBelowAtAndAboveThreshold();
	TestStaleDirectionalRecoveryRebasesEquipmentReadyAndStopBoundaries();
	TestReadyGenerationBlocksOlderUpdateUntilApplied();
	TestReadyGenerationOutOfOrderAndRejectedRetirement();
	TestReadyGenerationRejectsOlderApplyAndPreservesLatestLease();
	TestReadyGenerationResetInvalidatesSessionAndBarrier();
	TestReadyGenerationSessionBindingAndExactRetirement();
	TestReadyGenerationStopStartAndLifecycleRejectDelayedTokens();
	TestReadyGenerationNativeFallbackGateLinearization();
	TestReadyGenerationFailureDispositionRetiresOnlyItsToken();
	TestInvalidInputSnapshotDefersAndRejectsStaleSession();
	TestReadyGenerationFailureCancelsOnlyItsProtectedState();
	TestNativeFallbackGateRejectsObsoleteGroundObserver();
	TestLifecycleInputResetAndOneSnapshotReplay();
	TestNativeFallbackGateReentrantReadyAnnouncement();
	TestNativeFallbackReservationRevalidatesIdentityEpoch();
	TestCombatInputPolicyKeepsNormalAttacksPassthrough();
	TestRuntimeBoundaryDefersAndRebindsSameDispatchEdges();
	TestDeferredRebindNeverCrossesActorOrUnloadBoundary();
	TestNativeFallbackGateRejectsPendingReadyBarrier();
	TestInvalidReadyTokenAndStaleDeferredDrain();
	TestBoundaryDeferralBoundedAndUnavailableQueueIsNonRefreshing();
	TestGroundReadySupersedesOppositePostFlightTransition();
	TestEquipmentSwapSessionResetClearsPin();
	TestQueuedSequenceAllocatedAtExecutionOrder();
	TestWarhammerTypeTenIsTwoHanded();
	TestDualWieldLandingCleanupIsIdempotent();
	TestLandingHandsOffCurrentIdentityDuringRepeatedDrawingAndSheathing();
	TestSecondStopCannotRequestWorldCleanup();
	TestShoutPressHeldReleaseEdges();
	TestSemanticButtonEdgeKeepsReleaseAndDropsHeldOnlySamples();
	TestShoutDuplicateReleaseCannotQueueAnAcceptedEdge();
	TestStopClearsHeldInputLatches();
	TestHeldDoesNotRearmWhirlwindLatch();
	TestMagickaDrainOwnership();
	TestMagickaObservationIdentityAndNoWritePolicy();
	TestQueuedActionCooperation();
	TestQueuedActionOrderingAndSessionGuard();
	TestInputActionCoalescing();
	TestDiagnosticAggregatesAndBoundaries();
	TestInputDiagnosticThrottleReducer();
	TestGroundObserverProgressAndPhaseBoundaries();
	std::cout << "flight_state_tests: PASS\n";
}
