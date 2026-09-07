#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

// Pure state reducers used by the native controller.  Keeping these decisions
// free of CommonLib types makes the edge/cleanup invariants testable without a
// Skyrim process or a second runtime-specific binary.
namespace DragonAspectFlight::State
{
	// Stable, machine-readable diagnostic contract.  Increment only when a
	// consumer-visible field changes incompatibly.
	inline constexpr std::uint32_t StructuredDiagnosticSchemaVersion = 1;
	enum class WeaponEquipmentFamily : std::uint8_t
	{
		kUnknown,
		kUnarmed,
		kOneHanded,
		kDualWield,
		kTwoHanded,
		kQuarterstaff,
		kBow,
		kCrossbow,
		kMagic,
		kStaff
	};

	struct WeaponEquipmentIdentity
	{
		std::uint32_t rightFormId{ 0 };
		std::uint32_t leftFormId{ 0 };
		std::int32_t rightWeaponType{ -1 };
		std::int32_t leftWeaponType{ -1 };
		WeaponEquipmentFamily family{ WeaponEquipmentFamily::kUnknown };

		[[nodiscard]] constexpr bool operator==(const WeaponEquipmentIdentity&) const noexcept = default;
	};

	// Epoch zero means uncaptured.  The maximum value is terminal and is rejected
	// by captured-action validators; it is never wrapped and reused for an ABA
	// incarnation.
	[[nodiscard]] constexpr bool IsUsableWeaponEquipmentEpoch(
		std::uint64_t a_epoch) noexcept
	{
		return a_epoch != 0 && a_epoch != std::numeric_limits<std::uint64_t>::max();
	}

	[[nodiscard]] constexpr std::uint64_t AdvanceWeaponEquipmentEpoch(
		std::uint64_t a_epoch) noexcept
	{
		return a_epoch == std::numeric_limits<std::uint64_t>::max() ?
			a_epoch : (a_epoch == 0 ? std::uint64_t{ 1 } : a_epoch + 1);
	}

	enum class WeaponEquipmentSwapObservation : std::uint8_t
	{
		kStableAtLogicalTarget,
		kTransitionEdge,
		kStableAwayFromTarget
	};

	struct WeaponEquipmentSwapSettleState
	{
		bool pending{ false };
		bool transitionEdgeObserved{ false };
		std::uint16_t stableIdentityUpdates{ 0 };
		// Counts every observation, including repeated kDrawing/kSheathing edges.
		// This is deliberately independent from stableIdentityUpdates, which is
		// reset by a transition edge.
		std::uint16_t observationUpdates{ 0 };
		bool readinessPreserved{ false };
		// The old identity is protected only while pending.  Once the bounded
		// settle window expires, a caller must arm a fresh transition against the
		// current identity when the physical state still differs.
		bool reconciliationRequired{ false };
		std::uint64_t identityEpoch{ 0 };
	};

	// One frame can expose the replacement form before Skyrim publishes its
	// WantToDraw/WantToSheathe edge.  Keep the readiness hold through a bounded
	// stable grace period so that pre-edge state cannot reconcile the old item.
	inline constexpr std::uint16_t kWeaponEquipmentSwapStableGraceUpdates = 30;
	// Absolute bound: an actor that repeats a transition edge forever cannot keep
	// the old identity's readiness hold alive by resetting the stable counter.
	inline constexpr std::uint16_t kWeaponEquipmentSwapMaxObservationUpdates =
		kWeaponEquipmentSwapStableGraceUpdates;

	[[nodiscard]] constexpr WeaponEquipmentSwapSettleState ArmWeaponEquipmentSwap(
		std::uint64_t a_identityEpoch = 0) noexcept
	{
		return { true, false, 0, 0, true, false, a_identityEpoch };
	}

	[[nodiscard]] constexpr WeaponEquipmentSwapSettleState ResetWeaponEquipmentSwap() noexcept
	{
		return {};
	}

	[[nodiscard]] constexpr WeaponEquipmentSwapSettleState ObserveWeaponEquipmentSwap(
		WeaponEquipmentSwapSettleState a_state,
		WeaponEquipmentSwapObservation a_observation,
		bool a_explicitReadyCommand = false) noexcept
	{
		if (a_explicitReadyCommand) {
			return {};
		}
		if (!a_state.pending) {
			// A late edge belongs to the new/current identity observer, never to
			// the retired transaction.  Do not resurrect the old preservation hold.
			// A stable-at-target observation may, however, prove that the current
			// identity no longer needs the replacement reconciliation.
			if (a_state.reconciliationRequired &&
				a_observation == WeaponEquipmentSwapObservation::kStableAtLogicalTarget) {
				a_state.reconciliationRequired = false;
			}
			return a_state;
		}

		if (a_state.observationUpdates < kWeaponEquipmentSwapMaxObservationUpdates) {
			++a_state.observationUpdates;
		}
		const bool absoluteBoundReached =
			a_state.observationUpdates >= kWeaponEquipmentSwapMaxObservationUpdates;

		if (a_observation == WeaponEquipmentSwapObservation::kTransitionEdge) {
			a_state.transitionEdgeObserved = true;
			a_state.stableIdentityUpdates = 0;
			if (absoluteBoundReached) {
				return {
					false,
					true,
					0,
					a_state.observationUpdates,
					false,
					true,
					a_state.identityEpoch };
			}
			return a_state;
		}

		if (a_observation == WeaponEquipmentSwapObservation::kStableAtLogicalTarget) {
			if (a_state.transitionEdgeObserved) {
				// The current identity completed its own edge at the logical target.
				// There is no reason to retain a dormant readiness pin.
				return { false, true, 0, a_state.observationUpdates, false, false, a_state.identityEpoch };
			}
			if (a_state.stableIdentityUpdates < kWeaponEquipmentSwapStableGraceUpdates) {
				++a_state.stableIdentityUpdates;
			}
			if (a_state.stableIdentityUpdates >= kWeaponEquipmentSwapStableGraceUpdates ||
				absoluteBoundReached) {
				return { false, false, 0, a_state.observationUpdates, false, false, a_state.identityEpoch };
			}
			return a_state;
		}

		if (a_state.stableIdentityUpdates < kWeaponEquipmentSwapStableGraceUpdates) {
			++a_state.stableIdentityUpdates;
		}
		if (a_state.stableIdentityUpdates >= kWeaponEquipmentSwapStableGraceUpdates ||
			absoluteBoundReached) {
			return {
				false,
				false,
				0,
				a_state.observationUpdates,
				false,
				true,
				a_state.identityEpoch };
		}
		return a_state;
	}

	[[nodiscard]] constexpr bool IsWeaponEquipmentSwapPinned(
		const WeaponEquipmentSwapSettleState& a_state) noexcept
	{
		// Compatibility query: preservation is active only during the bounded
		// settle transaction.  It is never a post-expiry dormant pin.
		return a_state.pending && a_state.readinessPreserved;
	}

	[[nodiscard]] constexpr bool IsWeaponEquipmentSwapReconciliationRequired(
		const WeaponEquipmentSwapSettleState& a_state) noexcept
	{
		return !a_state.pending && a_state.reconciliationRequired;
	}

	// Ground Ready is a passthrough observer, but its reducer still needs an
	// explicit replacement/reset contract so a stale observer cannot swallow a
	// later edge or leave flight-only state behind.
	struct GroundWeaponObserverState
	{
		bool pending{ false };
		bool fallbackIssued{ false };
		std::uint64_t edgeSequence{ 0 };
	};

	enum class GroundWeaponObserverEvent : std::uint8_t
	{
		kReadyEdge,
		kIdentityChanged,
		kSessionReset
	};

	struct GroundWeaponObserverDecision
	{
		GroundWeaponObserverState state{};
		bool armed{ false };
		bool cancelled{ false };
		bool replaced{ false };
	};

	[[nodiscard]] constexpr GroundWeaponObserverDecision ReduceGroundWeaponObserver(
		GroundWeaponObserverState a_state,
		GroundWeaponObserverEvent a_event) noexcept
	{
		GroundWeaponObserverDecision result{ a_state };
		switch (a_event) {
		case GroundWeaponObserverEvent::kReadyEdge:
			result.replaced = result.state.pending;
			result.state.pending = true;
			result.state.fallbackIssued = false;
			++result.state.edgeSequence;
			result.armed = true;
			break;
		case GroundWeaponObserverEvent::kIdentityChanged:
			result.cancelled = result.state.pending;
			result.state.pending = false;
			result.state.fallbackIssued = false;
			break;
		case GroundWeaponObserverEvent::kSessionReset:
			result.cancelled = result.state.pending;
			result.state = {};
			break;
		}
		return result;
	}

	struct GroundWeaponObserverStopDecision
	{
		GroundWeaponObserverState state{};
		bool preserve{ false };
		bool rebase{ false };
		bool clear{ false };
		bool keepPump{ false };
	};

	// StopFlight is idempotent, but a post-flight ground observer is still a live
	// actor-owned transaction.  Preserve it when its identity/session/epoch are
	// current; otherwise rebase it once to the currently loaded actor instead of
	// allowing a repeated stop to erase the only observer pump.
	[[nodiscard]] constexpr GroundWeaponObserverStopDecision ReduceGroundObserverAfterStop(
		GroundWeaponObserverState a_state,
		bool a_wasFlying,
		bool a_actorLoaded,
		bool a_identityMatches,
		bool a_sessionMatches,
		bool a_epochMatches,
		bool a_actorMatches = true) noexcept
	{
		GroundWeaponObserverStopDecision result{ a_state };
		if (a_wasFlying || !a_state.pending) {
			return result;
		}
		const bool current = a_actorLoaded && a_identityMatches &&
			a_sessionMatches && a_epochMatches && a_actorMatches;
		if (current) {
			result.preserve = true;
			result.keepPump = true;
			return result;
		}
		if (a_actorLoaded) {
			result.rebase = true;
			result.keepPump = true;
			result.state.pending = true;
			result.state.fallbackIssued = false;
			++result.state.edgeSequence;
			return result;
		}
		result.clear = true;
		result.state = {};
		return result;
	}

	struct GroundWeaponObserverBaselineDecision
	{
		WeaponEquipmentIdentity identity{};
		bool captured{ false };
	};

	[[nodiscard]] constexpr GroundWeaponObserverBaselineDecision DecideGroundWeaponObserverBaseline(
		const WeaponEquipmentIdentity& a_actionIdentity,
		bool a_actionIdentityCaptured,
		const WeaponEquipmentIdentity& a_currentIdentity,
		bool a_currentIdentityCaptured) noexcept
	{
		return a_currentIdentityCaptured ?
			GroundWeaponObserverBaselineDecision{ a_currentIdentity, true } :
			GroundWeaponObserverBaselineDecision{ a_actionIdentity, a_actionIdentityCaptured };
	}

	struct LandingWeaponHandoffDecision
	{
		bool armGroundObserver{ false };
		bool targetDrawn{ false };
		bool rebaseToCurrentIdentity{ false };
	};

	[[nodiscard]] constexpr LandingWeaponHandoffDecision DecideLandingWeaponHandoff(
		bool a_wasFlying,
		bool a_transitionPending,
		bool a_logicalTargetDrawn,
		bool a_actorInTransition,
		bool a_actorLoaded,
		bool a_currentIdentityCaptured) noexcept
	{
		const bool handoff = a_wasFlying && a_actorLoaded && a_currentIdentityCaptured &&
			(a_transitionPending || a_actorInTransition);
		return { handoff, a_logicalTargetDrawn, handoff };
	}

	struct GroundReadySupersessionDecision
	{
		bool supersedePostFlightTransition{ false };
		bool disarmNativeFallback{ false };
		bool oppositeTarget{ false };
	};

	[[nodiscard]] constexpr GroundReadySupersessionDecision DecideGroundReadySupersession(
		bool a_postFlightTransitionPending,
		bool a_nativeFallbackArmed,
		bool a_oldTargetDrawn,
		bool a_newTargetDrawn) noexcept
	{
		return {
			a_postFlightTransitionPending,
			a_postFlightTransitionPending && a_nativeFallbackArmed,
			 a_postFlightTransitionPending && a_oldTargetDrawn != a_newTargetDrawn };
	}

	struct ReadyGenerationToken
	{
		std::uint64_t session{ 0 };
		std::uint64_t generation{ 0 };

		[[nodiscard]] constexpr bool operator==(const ReadyGenerationToken&) const noexcept = default;
	};

	[[nodiscard]] constexpr bool IsReadyGenerationTokenValid(
		const ReadyGenerationToken& a_token) noexcept
	{
		return a_token.generation != 0;
	}

	struct ReadyGenerationBarrierState
	{
		std::uint64_t session{ 0 };
		std::uint64_t announced{ 0 };
		std::uint64_t applied{ 0 };
		std::uint64_t retired{ 0 };
	};

	enum class ReadyGenerationBarrierEvent : std::uint8_t
	{
		kAnnounce,
		kApply,
		kRetire
	};

	struct ReadyGenerationBarrierDecision
	{
		ReadyGenerationBarrierState state{};
		bool accepted{ false };
		bool stale{ false };
		bool blocked{ false };
	};

	[[nodiscard]] constexpr ReadyGenerationBarrierDecision ReduceReadyGenerationBarrier(
		ReadyGenerationBarrierState a_state,
		ReadyGenerationBarrierEvent a_event,
		const ReadyGenerationToken& a_token) noexcept
	{
		ReadyGenerationBarrierDecision result{ a_state };
		if (!IsReadyGenerationTokenValid(a_token)) {
			result.blocked = true;
			return result;
		}
		if (a_token.session != result.state.session) {
			// The lifecycle/session owner rebases the barrier before accepting a
			// token.  A delayed task from another session is retire-only evidence;
			// it must never rewrite the current session's state.
			result.stale = true;
			return result;
		}

		switch (a_event) {
		case ReadyGenerationBarrierEvent::kAnnounce:
			if (a_token.generation <= result.state.announced) {
				result.stale = true;
				break;
			}
			result.state.announced = a_token.generation;
			result.accepted = true;
			break;
		case ReadyGenerationBarrierEvent::kApply:
			// Apply owns the newest protected state and may only consume the exact
			// generation currently announced.  An older Apply can arrive after a
			// newer Ready announcement; accepting it would let the caller clear the
			// newer generation's lease and transition state.
			if (a_token.generation != result.state.announced) {
				if (a_token.generation > result.state.announced) {
					result.blocked = true;
				} else {
					result.stale = true;
				}
				break;
			}
			if (a_token.generation <= result.state.retired) {
				result.stale = true;
				break;
			}
			result.state.applied = a_token.generation;
			result.state.retired = a_token.generation;
			result.accepted = true;
			break;
		case ReadyGenerationBarrierEvent::kRetire:
			if (a_token.generation > result.state.announced) {
				result.blocked = true;
				break;
			}
			if (a_token.generation <= result.state.retired) {
				// Retire intentionally permits an older task to advance the retired
				// watermark while a newer generation remains pending.
				result.stale = true;
				break;
			}
			result.state.retired = a_token.generation;
			result.accepted = true;
			break;
		}
		return result;
	}

	enum class ReadyGenerationTaskDisposition : std::uint8_t
	{
		kApplyCurrentSession,
		kRetireOnlyStaleSession,
		kIgnoreInvalid
	};

	[[nodiscard]] constexpr ReadyGenerationTaskDisposition DecideReadyGenerationTaskDisposition(
		const ReadyGenerationToken& a_token,
		std::uint64_t a_currentSession) noexcept
	{
		if (!IsReadyGenerationTokenValid(a_token)) {
			return ReadyGenerationTaskDisposition::kIgnoreInvalid;
		}
		return a_token.session == a_currentSession ?
			ReadyGenerationTaskDisposition::kApplyCurrentSession :
			ReadyGenerationTaskDisposition::kRetireOnlyStaleSession;
	}

	struct ReadyGenerationEnvelopeDecision
	{
		bool valid{ false };
		bool sourceSessionMismatch{ false };
		bool barrierSessionMismatch{ false };
		bool mayPrepareOrClear{ false };
	};

	[[nodiscard]] constexpr ReadyGenerationEnvelopeDecision DecideReadyGenerationEnvelope(
		const ReadyGenerationToken& a_token,
		std::uint64_t a_sourceSession,
		std::uint64_t a_currentSession,
		std::uint64_t a_barrierSession) noexcept
	{
		ReadyGenerationEnvelopeDecision result;
		result.sourceSessionMismatch = a_sourceSession != a_token.session;
		result.barrierSessionMismatch =
			a_currentSession != a_token.session || a_barrierSession != a_token.session;
		result.valid = IsReadyGenerationTokenValid(a_token) &&
			!result.sourceSessionMismatch && !result.barrierSessionMismatch;
		result.mayPrepareOrClear = result.valid;
		return result;
	}

	struct ReadyGenerationFailureDecision
	{
		ReadyGenerationBarrierDecision barrier{};
		bool envelopeValid{ false };
		bool retireAccepted{ false };
		bool cancelProtectedState{ false };
	};

	[[nodiscard]] constexpr ReadyGenerationFailureDecision DecideReadyGenerationFailure(
		ReadyGenerationBarrierState a_state,
		const ReadyGenerationToken& a_token,
		std::uint64_t a_sourceSession,
		std::uint64_t a_currentSession) noexcept
	{
		ReadyGenerationFailureDecision result;
		result.envelopeValid = DecideReadyGenerationEnvelope(
			a_token,
			a_sourceSession,
			a_currentSession,
			a_state.session).valid;
		if (!result.envelopeValid) {
			// Do not even evaluate the reducer for a foreign envelope.  The barrier
			// reducer is intentionally session-local and can otherwise produce an
			// apparently accepted watermark that callers must not consume.
			result.barrier.state = a_state;
			result.barrier.stale = IsReadyGenerationTokenValid(a_token);
			result.barrier.blocked = !result.barrier.stale;
			return result;
		}
		result.barrier = ReduceReadyGenerationBarrier(
			a_state,
			ReadyGenerationBarrierEvent::kRetire,
			a_token);
		// Retirement is an ownership operation, not merely a watermark update.
		// A token from another source/current session may still look acceptable to
		// the pure barrier reducer, but it must not advance this session's barrier.
		result.retireAccepted = result.envelopeValid && result.barrier.accepted;
		// Only the latest, same-session, correctly enveloped token may cancel the
		// state it was protecting.  An older token may retire its own barrier entry,
		// but it must never clear a newer generation's transition or pin.
		result.cancelProtectedState = result.envelopeValid &&
			result.retireAccepted && a_token.generation == a_state.announced;
		return result;
	}

	[[nodiscard]] constexpr ReadyGenerationBarrierState ResetReadyGenerationBarrier(
		ReadyGenerationBarrierState,
		std::uint64_t a_session,
		std::uint64_t a_generationFloor) noexcept
	{
		return { a_session, a_generationFloor, a_generationFloor, a_generationFloor };
	}

	[[nodiscard]] constexpr bool ReadyGenerationBarrierPending(
		const ReadyGenerationBarrierState& a_state) noexcept
	{
		return a_state.announced > a_state.retired;
	}

	// A queued Ready action owns a monotonic five-second lease.  This is a
	// liveness guard, not a frame/update watchdog: at 60 Hz the old eight-update
	// heuristic could expire in roughly 133 ms while the engine was still doing a
	// legitimate equipment transition.  The token identity is part of the
	// decision, so a late task for an older generation cannot retire a newer one.
	inline constexpr std::uint64_t kReadyGenerationLeaseMilliseconds = 5000;

	struct ReadyGenerationLeaseDecision
	{
		bool pending{ false };
		bool exactToken{ false };
		bool expired{ false };
	};

	[[nodiscard]] constexpr ReadyGenerationLeaseDecision DecideReadyGenerationLease(
		const ReadyGenerationBarrierState& a_state,
		const ReadyGenerationToken& a_leaseToken,
		std::uint64_t a_elapsedMilliseconds,
		std::uint64_t a_leaseMilliseconds = kReadyGenerationLeaseMilliseconds) noexcept
	{
		const bool exactToken = IsReadyGenerationTokenValid(a_leaseToken) &&
			a_leaseToken.session == a_state.session &&
			a_leaseToken.generation == a_state.announced;
		const bool pending = exactToken && ReadyGenerationBarrierPending(a_state);
		return {
			pending,
			exactToken,
			pending && a_elapsedMilliseconds >= a_leaseMilliseconds };
	}

	struct ReadyGenerationUpdateDecision
	{
		bool barrierPending{ false };
		bool preserveReadiness{ false };
		bool suppressNativeFallback{ false };
		bool allowEquipmentMutation{ true };
	};

	[[nodiscard]] constexpr ReadyGenerationUpdateDecision DecideReadyGenerationUpdate(
		const ReadyGenerationBarrierState& a_state,
		bool a_transitionPending,
		bool a_nativeFallbackDue,
		bool a_groundObserverPending) noexcept
	{
		const bool pending = ReadyGenerationBarrierPending(a_state);
		return {
			pending,
			pending && (a_transitionPending || a_nativeFallbackDue || a_groundObserverPending),
			pending && a_nativeFallbackDue,
			!pending };
	}

	struct LifecycleInvalidationDecision
	{
		std::uint64_t oldSession{ 0 };
		std::uint64_t newSession{ 0 };
		std::uint64_t readyGenerationFloor{ 0 };
	};

	[[nodiscard]] constexpr LifecycleInvalidationDecision DecideLifecycleInvalidation(
		std::uint64_t a_oldSession,
		std::uint64_t a_readyGenerationCounter) noexcept
	{
		const auto nextSession = a_oldSession == std::numeric_limits<std::uint64_t>::max() ?
			std::uint64_t{ 1 } : a_oldSession + 1;
		return { a_oldSession, nextSession, a_readyGenerationCounter };
	}

	// The native fallback must validate the raw ActorState weapon state rather
	// than ActorState::IsWeaponDrawn(), whose compatibility projection treats
	// kWantToSheathe/kSheathing as drawn.  Keep this mirror CommonLib-free so the
	// post-call contract is unit-testable without a Skyrim process.
	enum class NativeFallbackWeaponState : std::int8_t
	{
		kUnknown = -1,
		kSheathed = 0,
		kWantToDraw = 1,
		kDrawing = 2,
		kDrawn = 3,
		kWantToSheathe = 4,
		kSheathing = 5
	};

	[[nodiscard]] constexpr NativeFallbackWeaponState NormalizeNativeFallbackWeaponState(
		std::int32_t a_state) noexcept
	{
		switch (a_state) {
		case 0: return NativeFallbackWeaponState::kSheathed;
		case 1: return NativeFallbackWeaponState::kWantToDraw;
		case 2: return NativeFallbackWeaponState::kDrawing;
		case 3: return NativeFallbackWeaponState::kDrawn;
		case 4: return NativeFallbackWeaponState::kWantToSheathe;
		case 5: return NativeFallbackWeaponState::kSheathing;
		default: return NativeFallbackWeaponState::kUnknown;
		}
	}

	[[nodiscard]] constexpr bool IsNativeFallbackWeaponStateTransitional(
		NativeFallbackWeaponState a_state) noexcept
	{
		return a_state == NativeFallbackWeaponState::kWantToDraw ||
			a_state == NativeFallbackWeaponState::kDrawing ||
			a_state == NativeFallbackWeaponState::kWantToSheathe ||
			a_state == NativeFallbackWeaponState::kSheathing;
	}

	[[nodiscard]] constexpr bool IsNativeFallbackWeaponStateTowardTarget(
		NativeFallbackWeaponState a_state,
		bool a_targetDrawn) noexcept
	{
		return a_targetDrawn ?
			a_state == NativeFallbackWeaponState::kWantToDraw ||
			a_state == NativeFallbackWeaponState::kDrawing :
			a_state == NativeFallbackWeaponState::kWantToSheathe ||
			a_state == NativeFallbackWeaponState::kSheathing;
	}

	[[nodiscard]] constexpr bool IsNativeFallbackWeaponStateAtTarget(
		NativeFallbackWeaponState a_state,
		bool a_targetDrawn) noexcept
	{
		return a_targetDrawn ?
			a_state == NativeFallbackWeaponState::kDrawn :
			a_state == NativeFallbackWeaponState::kSheathed;
	}

	struct NativeFallbackGateRequest
	{
		std::uint64_t session{ 0 };
		std::uint64_t sequence{ 0 };
		std::uint64_t readyGeneration{ 0 };
		std::uint64_t identityEpoch{ 0 };
		std::uint64_t actionId{ 0 };
	};

	struct NativeFallbackGateDecision
	{
		bool execute{ false };
		bool sessionMismatch{ false };
		bool readyGenerationChanged{ false };
		bool newerReady{ false };
		bool identityMismatch{ false };
		bool identityEpochMismatch{ false };
		bool transitionMismatch{ false };
		bool barrierPending{ false };
		bool definitiveRejection{ false };
		bool retryableEngineUnavailable{ false };
		bool abortGroundObserver{ false };
	};

	[[nodiscard]] constexpr NativeFallbackGateDecision DecideNativeFallbackGate(
		std::uint64_t a_currentSession,
		std::uint64_t a_currentReadyGeneration,
		const NativeFallbackGateRequest& a_request,
		bool a_identityMatches,
		bool a_transitionMatches,
		bool a_engineAvailable = true,
		bool a_groundObserver = false,
		bool a_barrierPending = false,
		std::uint64_t a_currentIdentityEpoch = 0) noexcept
	{
		NativeFallbackGateDecision result;
		result.sessionMismatch = a_currentSession != a_request.session;
		result.readyGenerationChanged =
			a_currentReadyGeneration != a_request.readyGeneration;
		result.newerReady = a_currentReadyGeneration > a_request.readyGeneration;
		result.identityMismatch = !a_identityMatches;
		result.identityEpochMismatch = !IsUsableWeaponEquipmentEpoch(a_request.identityEpoch) ||
			!IsUsableWeaponEquipmentEpoch(a_currentIdentityEpoch) ||
			a_request.identityEpoch != a_currentIdentityEpoch;
		result.transitionMismatch = !a_transitionMatches;
		result.barrierPending = a_barrierPending;
		const bool requestMismatch = result.sessionMismatch ||
			result.readyGenerationChanged || result.identityMismatch ||
			result.identityEpochMismatch || result.transitionMismatch;
		result.definitiveRejection = result.barrierPending ||
			(a_engineAvailable && requestMismatch);
		result.retryableEngineUnavailable = !a_engineAvailable && !result.barrierPending;
		result.abortGroundObserver = a_groundObserver && result.definitiveRejection;
		result.execute = a_engineAvailable && !result.sessionMismatch &&
			!result.barrierPending &&
		!result.readyGenerationChanged &&
			!result.identityMismatch &&
			!result.identityEpochMismatch &&
			!result.transitionMismatch;
		return result;
	}

	struct NativeFallbackRevalidationDecision
	{
		bool commit{ false };
		bool sessionMismatch{ false };
		bool readyGenerationChanged{ false };
		bool identityMismatch{ false };
		bool identityEpochMismatch{ false };
		bool transitionMismatch{ false };
		bool weaponStateMismatch{ false };
		bool weaponStatePending{ false };
	};

	[[nodiscard]] constexpr NativeFallbackRevalidationDecision DecideNativeFallbackRevalidation(
		std::uint64_t a_currentSession,
		std::uint64_t a_currentReadyGeneration,
		const NativeFallbackGateRequest& a_request,
		bool a_identityMatches,
		bool a_transitionMatches,
		std::uint64_t a_currentIdentityEpoch = 0,
		NativeFallbackWeaponState a_postWeaponState = NativeFallbackWeaponState::kUnknown,
		bool a_targetDrawn = false) noexcept
	{
		NativeFallbackRevalidationDecision result;
		result.sessionMismatch = a_currentSession != a_request.session;
		result.readyGenerationChanged =
			a_currentReadyGeneration != a_request.readyGeneration;
		result.identityMismatch = !a_identityMatches;
		result.identityEpochMismatch = !IsUsableWeaponEquipmentEpoch(a_request.identityEpoch) ||
			!IsUsableWeaponEquipmentEpoch(a_currentIdentityEpoch) ||
			a_request.identityEpoch != a_currentIdentityEpoch;
		result.transitionMismatch = !a_transitionMatches;
		result.weaponStatePending = IsNativeFallbackWeaponStateTransitional(a_postWeaponState);
		result.weaponStateMismatch = !IsNativeFallbackWeaponStateAtTarget(
			a_postWeaponState,
			a_targetDrawn);
		result.commit = !result.sessionMismatch &&
			!result.readyGenerationChanged && !result.identityMismatch &&
			!result.identityEpochMismatch && !result.transitionMismatch &&
			!result.weaponStateMismatch;
		return result;
	}

	enum class NativeFallbackStaleRecoveryDisposition : std::uint8_t
	{
		kNoStale,
		kPreserveNewerObserver,
		kRebaseFlightTransition,
		kRebaseGroundObserver,
		kFailure
	};

	struct NativeFallbackStaleRecoveryDecision
	{
		NativeFallbackStaleRecoveryDisposition disposition{
			NativeFallbackStaleRecoveryDisposition::kNoStale };
		bool stale{ false };
		bool preserveNewerObserver{ false };
		bool rebased{ false };
		bool failed{ false };
	};

	// DrawWeaponMagicHands is directional and may re-enter the game while its
	// reservation is outside DAF locks.  If the post-call identity/epoch/session
	// changed, this reducer chooses a serialized state repair without issuing a
	// recursive second engine call.  Newer Ready/observer work always wins.
	[[nodiscard]] constexpr NativeFallbackStaleRecoveryDecision
		DecideNativeFallbackStaleRecovery(
			bool a_sessionMismatch,
			bool a_readyGenerationChanged,
			bool a_identityMismatch,
			bool a_identityEpochMismatch,
			bool a_transitionMismatch,
			bool a_groundObserver,
			bool a_flightActive,
			bool a_actorLoaded,
			bool a_actorInTransition,
			bool a_newerObserver,
			bool a_newerReady = false) noexcept
	{
		const bool stale = a_sessionMismatch || a_readyGenerationChanged ||
			a_identityMismatch || a_identityEpochMismatch || a_transitionMismatch;
		if (!stale) {
			return {};
		}
		static_cast<void>(a_actorInTransition);
		if (a_newerObserver || a_newerReady) {
			return {
				NativeFallbackStaleRecoveryDisposition::kPreserveNewerObserver,
				true,
				true,
				false,
				false };
		}
		if (!a_actorLoaded) {
			return {
				NativeFallbackStaleRecoveryDisposition::kFailure,
				true,
				false,
				false,
				true };
		}
		// A flight start can clear the old ground observer while the directional
		// engine call is still in flight.  Once the current state is airborne, the
		// replacement must therefore be a flight transition even when the stale
		// request originated from that ground observer.
		if (a_flightActive) {
			return {
				NativeFallbackStaleRecoveryDisposition::kRebaseFlightTransition,
				true,
				false,
				true,
				false };
		}
		// Once flight has ended, the ground observer is the authoritative repair
		// path even when the actor has already reached a stable state.  The
		// observer records that state and prevents the stale flight identity from
		// being reused on a later delayed edge.
		if (!a_flightActive) {
			return {
				NativeFallbackStaleRecoveryDisposition::kRebaseGroundObserver,
				true,
				false,
				true,
				false };
		}
		return {
			NativeFallbackStaleRecoveryDisposition::kFailure,
			true,
			false,
			false,
			true };
	}

	 enum class InputSnapshotDispatchDisposition : std::uint8_t
	 {
		kDispatch,
		kDeferUntilRefresh,
		kRejectSession,
		kSuppress
	};

	struct InputSnapshotDispatchDecision
	{
		InputSnapshotDispatchDisposition disposition{
			InputSnapshotDispatchDisposition::kRejectSession };
	};

	[[nodiscard]] constexpr InputSnapshotDispatchDecision DecideInputSnapshotDispatch(
		bool a_snapshotValid,
		bool a_semanticEdge,
		std::uint64_t a_sourceSession,
		std::uint64_t a_currentSession,
		bool a_suppressInput = false) noexcept
	{
		if (!a_snapshotValid) {
			return { a_semanticEdge ?
				InputSnapshotDispatchDisposition::kDeferUntilRefresh :
				InputSnapshotDispatchDisposition::kRejectSession };
		}
		if (a_sourceSession != a_currentSession) {
			return { InputSnapshotDispatchDisposition::kRejectSession };
		}
		if (a_suppressInput) {
			return { InputSnapshotDispatchDisposition::kSuppress };
		}
		return { InputSnapshotDispatchDisposition::kDispatch };
	}

	struct InputLifecycleResetDecision
	{
		std::uint64_t publishedSession{ 0 };
		bool snapshotValid{ false };
		bool deferredCleared{ true };
		bool requestRefresh{ true };
	};

	[[nodiscard]] constexpr InputLifecycleResetDecision DecideInputLifecycleReset(
		std::uint64_t a_session) noexcept
	{
		return { a_session, false, true, true };
	}

	struct DeferredInputReplayDecision
	{
		InputSnapshotDispatchDisposition disposition{
			InputSnapshotDispatchDisposition::kRejectSession };
		bool consumeOne{ false };
		bool scheduleNextRefresh{ false };
		bool terminalDiscard{ false };
		std::uint8_t nextBoundaryDeferralAttempts{ 0 };
	};

	inline constexpr std::uint8_t MaxRuntimeBoundaryDeferralAttempts = 2;

	[[nodiscard]] constexpr DeferredInputReplayDecision DecideDeferredInputReplay(
		std::uint32_t a_pendingCount,
		bool a_snapshotValid,
		bool a_suppressInput,
		std::uint64_t a_sourceSession,
		std::uint64_t a_currentSession,
		bool a_rebindSession = false,
		bool a_boundaryFresh = true,
		std::uint8_t a_boundaryDeferralAttempts = 0) noexcept
	{
		if (a_pendingCount == 0) {
			return {};
		}
		const auto effectiveSourceSession = a_rebindSession ?
			a_currentSession : a_sourceSession;
		if (a_rebindSession && !a_boundaryFresh) {
			if (a_boundaryDeferralAttempts >= MaxRuntimeBoundaryDeferralAttempts) {
				return {
					InputSnapshotDispatchDisposition::kRejectSession,
					true,
					a_pendingCount > 1,
					true,
					a_boundaryDeferralAttempts };
			}
			return {
				InputSnapshotDispatchDisposition::kDeferUntilRefresh,
				false,
				true,
				false,
				static_cast<std::uint8_t>(a_boundaryDeferralAttempts + 1) };
		}
		const auto dispatch = DecideInputSnapshotDispatch(
			a_snapshotValid,
			true,
			effectiveSourceSession,
			a_currentSession,
			a_suppressInput);
		return {
			dispatch.disposition,
			dispatch.disposition != InputSnapshotDispatchDisposition::kDeferUntilRefresh,
			dispatch.disposition != InputSnapshotDispatchDisposition::kSuppress &&
			dispatch.disposition != InputSnapshotDispatchDisposition::kDeferUntilRefresh &&
			a_pendingCount > 1 };
	}

	struct TaskInterfaceUnavailableDecision
	{
		bool retireReadyToken{ false };
		bool refreshGameThreadState{ false };
	};

	[[nodiscard]] constexpr TaskInterfaceUnavailableDecision DecideTaskInterfaceUnavailable(
		bool a_readyTokenValid) noexcept
	{
		// This branch may be reached by an off-thread producer.  Refreshing the
		// runtime snapshot is never valid here; only the token terminal action is.
		return { a_readyTokenValid, false };
	}

	struct DeferredInputQueueState
	{
		std::uint32_t count{ 0 };
		bool readyPending{ false };
		std::uint32_t readyCount{ 0 };
	};

	struct DeferredInputQueueDecision
	{
		DeferredInputQueueState state{};
		bool enqueue{ false };
		bool replaceOldest{ false };
	};

	[[nodiscard]] constexpr DeferredInputQueueDecision DecideDeferredInputQueue(
		DeferredInputQueueState a_state,
		bool a_semanticEdge,
		bool a_readyEdge,
		std::uint32_t a_capacity) noexcept
	{
		DeferredInputQueueDecision result{ a_state };
		if (!a_semanticEdge || a_capacity == 0) {
			return result;
		}
		result.state.readyCount = std::min(result.state.readyCount, result.state.count);
		if (result.state.count < a_capacity) {
			result.enqueue = true;
			++result.state.count;
			if (a_readyEdge) {
				++result.state.readyCount;
			}
		} else {
			// The production queue replaces an older non-Ready semantic event when
			// possible.  Ready edges remain protected; a non-Ready edge is rejected
			// when every slot is already a Ready edge.
			const bool hasReplaceableNonReady = result.state.readyCount < result.state.count;
			if (a_readyEdge || hasReplaceableNonReady) {
				result.enqueue = true;
				result.replaceOldest = true;
				if (a_readyEdge && hasReplaceableNonReady) {
					++result.state.readyCount;
				}
			}
		}
		result.state.readyPending = result.state.readyCount != 0;
		return result;
	}

	[[nodiscard]] constexpr bool ShouldReconcileFlightCombatReady(
		bool a_flightActive,
		bool a_logicalCombatReady,
		bool a_actorWeaponsDrawn,
		bool a_swapPending,
		bool a_swapTimedOut,
		bool a_swapPinned = false,
		bool a_swapReconciliationRequired = false) noexcept
	{
		// A normal update may reconcile only when no transaction is active.  An
		// expired transaction explicitly authorizes a fresh current-identity
		// reconciliation; it never authorizes use of the retired identity.
		return a_flightActive &&
			a_logicalCombatReady != a_actorWeaponsDrawn &&
			!a_swapPending &&
			!a_swapPinned &&
			(a_swapReconciliationRequired || !a_swapTimedOut);
	}

	enum class WeaponTransitionExpiryDisposition : std::uint8_t
	{
		kNoAction,
		kRequestCurrentIdentityRecovery,
		kTerminalFailure
	};

	struct WeaponTransitionExpiryDecision
	{
		WeaponTransitionExpiryDisposition disposition{
			WeaponTransitionExpiryDisposition::kNoAction };
		// The logical Ready target remains authoritative while a recovery request is
		// pending; terminal policy must preserve it rather than infer a new target
		// from the actor projection.
		bool targetAuthoritative{ false };
	};

	[[nodiscard]] constexpr WeaponTransitionExpiryDecision DecideWeaponTransitionExpiry(
		bool a_transitionPending,
		bool a_targetReached,
		bool a_actorLoaded,
		bool a_identityMatches,
		bool a_recoveryAttempted) noexcept
	{
		if (!a_transitionPending || a_targetReached) {
			return { WeaponTransitionExpiryDisposition::kNoAction, true };
		}
		if (!a_actorLoaded || !a_identityMatches || a_recoveryAttempted) {
			return { WeaponTransitionExpiryDisposition::kTerminalFailure, false };
		}
		return {
			WeaponTransitionExpiryDisposition::kRequestCurrentIdentityRecovery,
			true };
	}

	struct WeaponTransitionTerminalRecoveryDecision
	{
		bool targetDrawn{ false };
		bool retryNativeFallback{ false };
		bool terminalFailure{ false };
	};

	// A failed recovery can leave Skyrim in a transitional or stable-opposite raw
	// state.  Do not collapse that state through ActorState::IsWeaponDrawn():
	// preserve the requested target and allow one explicitly bounded,
	// identity-gated retry when the raw transition is moving toward that target.
	// Opposite-direction calls have no safe cancel primitive and therefore
	// terminate fail-closed.  Once the retry is consumed, retain the logical
	// target; never claim that kDrawing/kSheathing reached a stable target.
	[[nodiscard]] constexpr WeaponTransitionTerminalRecoveryDecision
		DecideWeaponTransitionTerminalRecovery(
			bool a_targetDrawn,
			bool a_actorLoaded,
			bool a_identityMatches,
			NativeFallbackWeaponState a_weaponState,
			bool a_retryAlreadyUsed) noexcept
	{
		const bool targetReached = a_actorLoaded && a_identityMatches &&
			IsNativeFallbackWeaponStateAtTarget(a_weaponState, a_targetDrawn);
		if (targetReached) {
			return { a_targetDrawn, false, false };
		}
		// The requested target remains authoritative for every terminal failure,
		// including a stable state on the opposite side or an unavailable identity.
		// Physical state is never a substitute for the logical Ready target.
		const bool towardTarget = IsNativeFallbackWeaponStateTowardTarget(
			a_weaponState,
			a_targetDrawn);
		return {
			a_targetDrawn,
			a_actorLoaded && a_identityMatches && towardTarget && !a_retryAlreadyUsed,
			!a_actorLoaded || !a_identityMatches || !towardTarget || a_retryAlreadyUsed };
	}

	// A terminal recovery failure is a bounded, metadata-only quarantine.  The
	// actor projection may remain on the opposite stable state, but passive
	// observations must not overwrite the requested Ready target and immediately
	// re-arm the native fallback.  Only an explicit input, a new equipment/session
	// incarnation, or exact stable convergence releases the hold.
	struct WeaponTransitionTerminalFailureHoldState
	{
		bool active{ false };
		bool targetDrawn{ false };
		std::uint64_t sessionId{ 0 };
		std::uint64_t identityEpoch{ 0 };
	};

	enum class WeaponTransitionTerminalFailureHoldEvent : std::uint8_t
	{
		kTerminalFailure,
		kPassiveObservation,
		kExplicitReady,
		kEquipmentIdentityChange,
		kSessionReset,
		kStableConvergence,
		kReadyGenerationCleanup
	};

	struct WeaponTransitionTerminalFailureHoldDecision
	{
		WeaponTransitionTerminalFailureHoldState state{};
		bool preserveTarget{ false };
		bool allowPassiveReconciliation{ true };
		bool rearmNativeFallback{ false };
	};

	[[nodiscard]] constexpr WeaponTransitionTerminalFailureHoldDecision
		ReduceWeaponTransitionTerminalFailureHold(
			const WeaponTransitionTerminalFailureHoldState& a_state,
			WeaponTransitionTerminalFailureHoldEvent a_event,
			bool a_targetDrawn = false,
			std::uint64_t a_sessionId = 0,
			std::uint64_t a_identityEpoch = 0,
			bool a_stableAtTarget = false) noexcept
	{
		switch (a_event) {
		case WeaponTransitionTerminalFailureHoldEvent::kTerminalFailure:
			return {
				{ true, a_targetDrawn, a_sessionId, a_identityEpoch },
				true,
				false,
				false };
		case WeaponTransitionTerminalFailureHoldEvent::kPassiveObservation:
			if (!a_state.active) {
				return { a_state, false, true, false };
			}
			if (a_stableAtTarget || a_state.sessionId != a_sessionId ||
				a_state.identityEpoch != a_identityEpoch) {
				return { {}, false, true, false };
			}
			return { a_state, true, false, false };
		case WeaponTransitionTerminalFailureHoldEvent::kExplicitReady:
		case WeaponTransitionTerminalFailureHoldEvent::kEquipmentIdentityChange:
		case WeaponTransitionTerminalFailureHoldEvent::kSessionReset:
		case WeaponTransitionTerminalFailureHoldEvent::kStableConvergence:
			return { {}, false, true, false };
		case WeaponTransitionTerminalFailureHoldEvent::kReadyGenerationCleanup:
			return a_state.active ?
				WeaponTransitionTerminalFailureHoldDecision{ a_state, true, false, false } :
				WeaponTransitionTerminalFailureHoldDecision{ a_state, false, true, false };
		}
		return { {}, false, true, false };
	}

	[[nodiscard]] constexpr bool SameWeaponEquipmentIdentity(
		const WeaponEquipmentIdentity& a_left,
		const WeaponEquipmentIdentity& a_right) noexcept
	{
		const bool familyMatches = a_left.family == a_right.family ||
			a_left.family == WeaponEquipmentFamily::kUnknown ||
			a_right.family == WeaponEquipmentFamily::kUnknown;
		return a_left.rightFormId == a_right.rightFormId &&
			a_left.leftFormId == a_right.leftFormId &&
			a_left.rightWeaponType == a_right.rightWeaponType &&
			a_left.leftWeaponType == a_right.leftWeaponType &&
			familyMatches;
	}

	// Epoch-bound actions use the captured token literally.  The broader
	// SameWeaponEquipmentIdentity predicate intentionally treats an unknown family
	// as a diagnostic wildcard for routing decisions; that tolerance is not safe
	// for a queued state mutation.
	[[nodiscard]] constexpr bool ExactWeaponEquipmentIdentity(
		const WeaponEquipmentIdentity& a_left,
		const WeaponEquipmentIdentity& a_right) noexcept
	{
		return a_left == a_right;
	}

	// The producer-side identity authority.  Every live identity observation must
	// pass through this reducer before a snapshot is published.  The epoch is
	// nonzero once an identity has been captured and advances for every observed
	// incarnation change, including A -> B -> A.  An uncaptured observation does
	// not manufacture a wildcard epoch or discard the last known incarnation.  The
	// maximum counter value is terminal: it is never reused and all captured
	// action validators reject it, so counter exhaustion fails closed rather than
	// wrapping into an ABA collision.
	struct WeaponEquipmentEpochTrackerState
	{
		WeaponEquipmentIdentity identity{};
		std::uint64_t epoch{ 0 };
		bool captured{ false };
	};

	struct WeaponEquipmentEpochObservation
	{
		WeaponEquipmentEpochTrackerState state{};
		WeaponEquipmentIdentity previousIdentity{};
		std::uint64_t previousEpoch{ 0 };
		bool identityCaptured{ false };
		bool initialized{ false };
		bool identityChanged{ false };
	};

	// One immutable producer snapshot for input/action envelopes.  The identity,
	// capture bit, and epoch are returned from the same serialized observation so
	// a snapshot cannot pair form A with the epoch later assigned to form B.
	struct WeaponEquipmentEpochSnapshot
	{
		WeaponEquipmentIdentity identity{};
		std::uint64_t epoch{ 0 };
		bool captured{ false };
	};

	[[nodiscard]] constexpr WeaponEquipmentEpochObservation ObserveWeaponEquipmentEpoch(
		WeaponEquipmentEpochTrackerState a_state,
		const WeaponEquipmentIdentity& a_identity,
		bool a_identityCaptured) noexcept
	{
		WeaponEquipmentEpochObservation result{};
		result.state = a_state;
		result.previousEpoch = a_state.epoch;
		if (!a_identityCaptured) {
			// Keep the last captured baseline across an unload/reload gap.  A later
			// loaded observation can then prove whether the incarnation changed.
			return result;
		}

		result.identityCaptured = true;
		if (!a_state.captured) {
			result.state.identity = a_identity;
			result.state.captured = true;
			if (result.state.epoch == 0) {
				result.state.epoch = 1;
			}
			result.initialized = true;
			return result;
		}

		if (ExactWeaponEquipmentIdentity(a_state.identity, a_identity)) {
			result.state.captured = true;
			if (result.state.epoch == 0) {
				result.state.epoch = 1;
			}
			return result;
		}

		result.previousIdentity = a_state.identity;
		result.state.identity = a_identity;
		result.state.captured = true;
		result.state.epoch = AdvanceWeaponEquipmentEpoch(result.state.epoch);
		result.identityChanged = true;
		return result;
	}

	// A queued action that can change readiness or block state must identify one
	// exact equipment incarnation.  Epoch zero is the pre-token/unknown value,
	// so it is never a valid match for a captured action.  Uncaptured snapshots
	// remain available only for lifecycle cleanup paths that deliberately do not
	// target a weapon incarnation.
	[[nodiscard]] constexpr bool ShouldApplyCurrentEquipmentAction(
		bool a_actionIdentityCaptured,
		bool a_currentIdentityCaptured,
		const WeaponEquipmentIdentity& a_actionIdentity,
		const WeaponEquipmentIdentity& a_currentIdentity,
		std::uint64_t a_actionIdentityEpoch,
		std::uint64_t a_currentIdentityEpoch) noexcept
	{
		return a_actionIdentityCaptured && a_currentIdentityCaptured &&
			IsUsableWeaponEquipmentEpoch(a_actionIdentityEpoch) &&
			IsUsableWeaponEquipmentEpoch(a_currentIdentityEpoch) &&
			a_actionIdentityEpoch == a_currentIdentityEpoch &&
			ExactWeaponEquipmentIdentity(a_actionIdentity, a_currentIdentity);
	}

	// A queued block edge may outlive the equipment identity that produced it.
	// Uncaptured lifecycle cleanup is still allowed, but a captured edge must
	// execute only against the same currently-observed equipment identity.
	[[nodiscard]] constexpr bool ShouldApplyFlightBlockAction(
		bool a_actionIdentityCaptured,
		bool a_currentIdentityCaptured,
		const WeaponEquipmentIdentity& a_actionIdentity,
		const WeaponEquipmentIdentity& a_currentIdentity,
		std::uint64_t a_actionIdentityEpoch = 0,
		std::uint64_t a_currentIdentityEpoch = 0) noexcept
	{
		if (!a_actionIdentityCaptured) {
			return true;
		}
		return ShouldApplyCurrentEquipmentAction(
			a_actionIdentityCaptured,
			a_currentIdentityCaptured,
			a_actionIdentity,
			a_currentIdentity,
			a_actionIdentityEpoch,
			a_currentIdentityEpoch);
	}

	struct NativeBlockStateSnapshot
	{
		bool wantBlocking{ false };
		bool isBlocking{ false };
		bool graphBlocking{ false };

		[[nodiscard]] constexpr bool operator==(const NativeBlockStateSnapshot&) const noexcept = default;
	};

	// DAF's local lease is the only authority that permits a native release.  A
	// generation makes each claim distinct even when an actor/equipment pair is
	// revisited later; identity+epoch binds that claim to one incarnation.
	struct NativeBlockLeaseToken
	{
		WeaponEquipmentIdentity identity{};
		std::uint64_t identityEpoch{ 0 };
		std::uint64_t claimGeneration{ 0 };
		NativeBlockStateSnapshot expectedState{};
		bool active{ false };
	};

	struct NativeBlockLeaseDecision
	{
		bool tokenCurrent{ false };
		bool nativeStateMatches{ false };
		bool externalConflict{ false };
		bool stale{ false };
		bool clearNative{ false };
	};

	// Without an engine producer token, an exact snapshot can detect a changed
	// state (or an independently observed conflict) but cannot prove that a
	// same-valued vanilla/MCO write did not happen.  This reducer is therefore a
	// guard for a future cooperative/native path; the current runtime keeps the
	// shared block path metadata-only and performs no unprovable release.
	[[nodiscard]] constexpr NativeBlockLeaseDecision DecideNativeBlockLeaseRelease(
		const NativeBlockLeaseToken& a_lease,
		bool a_currentIdentityCaptured,
		const WeaponEquipmentIdentity& a_currentIdentity,
		std::uint64_t a_currentIdentityEpoch,
		const NativeBlockStateSnapshot& a_currentState,
		bool a_externalMutationObserved = false) noexcept
	{
		const bool tokenCurrent = a_lease.active &&
			a_lease.claimGeneration != 0 &&
			a_currentIdentityCaptured &&
			IsUsableWeaponEquipmentEpoch(a_lease.identityEpoch) &&
			IsUsableWeaponEquipmentEpoch(a_currentIdentityEpoch) &&
			a_lease.identityEpoch == a_currentIdentityEpoch &&
			ExactWeaponEquipmentIdentity(a_lease.identity, a_currentIdentity);
		const bool nativeStateMatches = tokenCurrent &&
			a_currentState == a_lease.expectedState;
		const bool externalConflict = a_externalMutationObserved ||
			(tokenCurrent && !nativeStateMatches);
		return {
			tokenCurrent,
			nativeStateMatches,
			externalConflict,
			!tokenCurrent,
			nativeStateMatches && !externalConflict
		};
	}

	struct FlightBlockActionDecision
	{
		bool accepted{ false };
		bool stale{ false };
		bool clearNative{ false };
	};

	// Logical release remains accepted for an unowned state so lifecycle/input
	// latches can converge without touching vanilla or another combat plugin's
	// actor/graph state.  A native release would additionally require the lease
	// reducer above and a cooperative producer signal unavailable in this runtime.
	[[nodiscard]] constexpr FlightBlockActionDecision DecideFlightBlockAction(
		bool a_requested,
		bool a_actionIdentityCaptured,
		bool a_currentIdentityCaptured,
		const WeaponEquipmentIdentity& a_actionIdentity,
		const WeaponEquipmentIdentity& a_currentIdentity,
		bool a_nativeStateOwned,
		std::uint64_t a_actionIdentityEpoch = 0,
		std::uint64_t a_currentIdentityEpoch = 0) noexcept
	{
		// A request is a state claim and therefore must carry the current
		// identity+epoch.  An uncaptured release remains an idempotent lifecycle
		// cleanup edge, but it never authorizes a native write.
		const bool current = a_requested && !a_actionIdentityCaptured ?
			false : (!a_requested && !a_actionIdentityCaptured ?
				true :
				ShouldApplyFlightBlockAction(
				a_actionIdentityCaptured,
				a_currentIdentityCaptured,
				a_actionIdentity,
				a_currentIdentity,
				a_actionIdentityEpoch,
				a_currentIdentityEpoch));
		return {
			current,
			!current,
			current && a_actionIdentityCaptured && a_currentIdentityCaptured &&
				!a_requested && a_nativeStateOwned };
	}

	enum class WeaponEquipmentChange : std::uint8_t
	{
		kStable,
		kEquipmentSwap
	};

	struct WeaponEquipmentChangeDecision
	{
		WeaponEquipmentChange change{ WeaponEquipmentChange::kStable };
		bool cancelPending{ false };
		bool disarmNativeFallback{ false };
		bool preserveCombatReady{ false };
	};

	[[nodiscard]] constexpr WeaponEquipmentChangeDecision DecideWeaponEquipmentChange(
		const WeaponEquipmentIdentity& a_captured,
		const WeaponEquipmentIdentity& a_current) noexcept
	{
		if (SameWeaponEquipmentIdentity(a_captured, a_current)) {
			return {};
		}
		return {
			WeaponEquipmentChange::kEquipmentSwap,
			true,
			true,
			true
		};
	}

	// Block is a DAF-owned latch, not an equipment property.  A held block edge
	// cannot safely be carried across an item identity change because the input
	// queue has no identity token.  Revoke only that DAF-owned latch; vanilla and
	// MCO attack/block ownership remains untouched.
	[[nodiscard]] constexpr bool ShouldRevokeFlightBlockOnEquipmentSwap(
		bool a_equipmentChanged,
		bool a_dafBlockRequested,
		bool a_dafNativeStateOwned = false) noexcept
	{
		return a_equipmentChanged && (a_dafBlockRequested || a_dafNativeStateOwned);
	}

	// Rebased transitions are bound to the current equipment identity.  If the
	// engine is still on the opposite stable state after the settle hold, arm the
	// one-shot native fallback immediately (subject to its normal delay) instead
	// of waiting through another full transition timeout.
	[[nodiscard]] constexpr bool ShouldArmNativeFallbackAfterEquipmentRebase(
		bool a_flightActive,
		bool a_actorLoaded,
		bool a_identityCaptured,
		bool a_logicalTargetDrawn,
		bool a_actualDrawn) noexcept
	{
		return a_flightActive && a_actorLoaded && a_identityCaptured &&
			a_logicalTargetDrawn != a_actualDrawn;
	}

	[[nodiscard]] constexpr bool IsOneHandedWeaponTypeValue(std::int32_t a_type) noexcept
	{
		return a_type >= 1 && a_type <= 4;
	}

	[[nodiscard]] constexpr bool IsTwoHandedWeaponTypeValue(std::int32_t a_type) noexcept
	{
		// Skyrim's kTwoHandSword/kTwoHandAxe/kTwoHandMace values are 5/6/10.
		return a_type == 5 || a_type == 6 || a_type == 10;
	}

	[[nodiscard]] constexpr bool IsCustomTwoHandedWeaponTypeValue(
		std::int32_t a_type,
		bool a_isWeapon) noexcept
	{
		// OAR's Greatsword route deliberately accepts unknown/custom weapon type
		// -1.  The predicate must come from TESObjectWEAP::As(), not from a
		// non-null equipped-form check: shields and unrelated forms also occupy a
		// hand but are not weapons.
		return a_isWeapon && a_type == -1;
	}

	[[nodiscard]] constexpr WeaponEquipmentFamily ResolveWeaponEquipmentFamily(
		std::int32_t a_rightWeaponType,
		std::int32_t a_leftWeaponType,
		bool a_quarterstaff,
		bool a_rightMagic = false,
		bool a_leftMagic = false,
		bool a_rightIsWeapon = false,
		bool a_leftIsWeapon = false) noexcept
	{
		if (a_quarterstaff) {
			return WeaponEquipmentFamily::kQuarterstaff;
		}
		if (a_rightMagic || a_leftMagic) {
			return WeaponEquipmentFamily::kMagic;
		}
		if (a_rightWeaponType == 7 || a_leftWeaponType == 7) {
			return WeaponEquipmentFamily::kBow;
		}
		if (a_rightWeaponType == 9 || a_leftWeaponType == 9) {
			return WeaponEquipmentFamily::kCrossbow;
		}
		if (a_rightWeaponType == 8 || a_leftWeaponType == 8) {
			return WeaponEquipmentFamily::kStaff;
		}
		if (IsTwoHandedWeaponTypeValue(a_rightWeaponType) ||
			IsTwoHandedWeaponTypeValue(a_leftWeaponType) ||
			IsCustomTwoHandedWeaponTypeValue(a_rightWeaponType, a_rightIsWeapon) ||
			IsCustomTwoHandedWeaponTypeValue(a_leftWeaponType, a_leftIsWeapon)) {
			return WeaponEquipmentFamily::kTwoHanded;
		}
		if (IsOneHandedWeaponTypeValue(a_rightWeaponType) &&
			IsOneHandedWeaponTypeValue(a_leftWeaponType)) {
			return WeaponEquipmentFamily::kDualWield;
		}
		if (IsOneHandedWeaponTypeValue(a_rightWeaponType) ||
			IsOneHandedWeaponTypeValue(a_leftWeaponType)) {
			return WeaponEquipmentFamily::kOneHanded;
		}
		return WeaponEquipmentFamily::kUnarmed;
	}

	[[nodiscard]] constexpr bool IsBlockCapableWeaponFamily(
		WeaponEquipmentFamily a_family) noexcept
	{
		return a_family == WeaponEquipmentFamily::kTwoHanded ||
			a_family == WeaponEquipmentFamily::kQuarterstaff;
	}

	// Left/Right Attack/Block events are shared vanilla combat channels.  The
	// initial down edge is a normal attack/combat edge; only a subsequent held
	// sample is a block intent.  This keeps DAF from priming its block latch for
	// every ordinary attack while leaving vanilla/MCO as the owner of combat.
	enum class FlightCombatInputChannel : std::uint8_t
	{
		kLeftAttackBlock,
		kRightAttackBlock,
		kDualAttack
	};

	struct FlightCombatInputPolicyDecision
	{
		bool consume{ false };
		bool queueBlockRequest{ false };
		bool queueBlockRelease{ false };
		bool queueBeginCombat{ false };
	};

	[[nodiscard]] constexpr FlightCombatInputPolicyDecision DecideFlightCombatInputPolicy(
		bool a_flightActive,
		FlightCombatInputChannel a_channel,
		bool a_blockRequested,
		bool a_down,
		bool a_up,
		bool a_held) noexcept
	{
		FlightCombatInputPolicyDecision result;
		if (!a_flightActive) {
			return result;
		}
		result.queueBeginCombat = a_down;
		if (a_channel == FlightCombatInputChannel::kLeftAttackBlock) {
			result.queueBlockRequest = a_held && !a_blockRequested;
			result.queueBlockRelease = a_up && a_blockRequested;
		}
		return result;
	}

	[[nodiscard]] constexpr bool ShouldReleaseFlightBlockForReadyEdge(
		bool a_readyPressEdge,
		bool a_weaponsDrawn,
		bool a_blockHeld) noexcept
	{
		return a_readyPressEdge && a_weaponsDrawn && a_blockHeld;
	}

	struct QueuedActionExecutionSequenceState
	{
		std::uint64_t next{ 0 };
	};

	[[nodiscard]] constexpr std::uint64_t AllocateQueuedActionExecutionSequence(
		QueuedActionExecutionSequenceState& a_state) noexcept
	{
		return ++a_state.next;
	}

	[[nodiscard]] constexpr std::string_view WeaponEquipmentFamilyName(
		WeaponEquipmentFamily a_family) noexcept
	{
		switch (a_family) {
		case WeaponEquipmentFamily::kUnarmed: return "unarmed";
		case WeaponEquipmentFamily::kOneHanded: return "one_handed";
		case WeaponEquipmentFamily::kDualWield: return "dual_wield";
		case WeaponEquipmentFamily::kTwoHanded: return "two_handed";
		case WeaponEquipmentFamily::kQuarterstaff: return "quarterstaff";
		case WeaponEquipmentFamily::kBow: return "bow";
		case WeaponEquipmentFamily::kCrossbow: return "crossbow";
		case WeaponEquipmentFamily::kMagic: return "magic";
		case WeaponEquipmentFamily::kStaff: return "staff";
		default: return "unknown";
		}
	}

	struct WeaponTransitionState
	{
		bool pending{ false };
		bool targetDrawn{ false };
		bool nativeFallbackArmed{ false };
		bool postFlight{ false };
		std::uint64_t sequence{ 0 };
	};

	enum class ObservedWeaponEdge : std::uint8_t
	{
		kSheathed,
		kDrawing,
		kDrawn,
		kSheathing
	};

	// Ready Weapon toggles the logical DAF state.  Vanilla's transient actor
	// state is diagnostic only: a draw/sheath edge may be observed before or
	// after the actor publishes its state, so it must never decide this target.
	[[nodiscard]] constexpr bool ResolveWeaponToggleTarget(
		bool a_dafCombatActive,
		bool a_transitionPending,
		bool a_transitionTargetDrawn,
		ObservedWeaponEdge = ObservedWeaponEdge::kSheathed,
		bool a_equipmentIdentityChanged = false) noexcept
	{
		if (a_equipmentIdentityChanged) {
			return a_dafCombatActive;
		}
		const bool logicalCurrent = a_transitionPending ? a_transitionTargetDrawn : a_dafCombatActive;
		return !logicalCurrent;
	}

	[[nodiscard]] constexpr WeaponTransitionState PreserveWeaponTransitionAfterStop(
		WeaponTransitionState a_state) noexcept
	{
		// A flight transition is identity- and session-bound.  Carrying it through
		// landing lets a delayed callback or fallback act on a stale item.  Ground
		// Ready edges may start a new bounded observer against the live identity.
		a_state.pending = false;
		a_state.targetDrawn = false;
		a_state.nativeFallbackArmed = false;
		a_state.postFlight = false;
		return a_state;
	}

	[[nodiscard]] constexpr bool ShouldPumpWeaponTransition(
		bool a_flightActive,
		bool a_transitionPending,
		bool a_regenObservationPending = false,
		bool a_groundWeaponObservationPending = false,
		bool a_equipmentSwapPending = false,
		bool a_readyGenerationPending = false) noexcept
	{
		return a_flightActive || a_transitionPending || a_regenObservationPending ||
			a_groundWeaponObservationPending || a_equipmentSwapPending || a_readyGenerationPending;
	}

	inline constexpr float kDiagnosticAggregateWindowSeconds = 1.0F;
	inline constexpr float kMagickaRegenDiagnosticValueTolerance = 0.01F;
	inline constexpr float kMagickaRegenObservationWindowSeconds = 0.40F;

	enum class MagickaDiagnosticFlushReason : std::uint8_t
	{
		kNone,
		kOneSecondBoundary,
		kDepletion,
		kSessionBoundary,
		kActorBoundary,
		kPhaseBoundary,
		kValueBoundary
	};

	[[nodiscard]] constexpr const char* MagickaDiagnosticFlushReasonName(
		MagickaDiagnosticFlushReason a_reason) noexcept
	{
		switch (a_reason) {
		case MagickaDiagnosticFlushReason::kOneSecondBoundary: return "one_second_boundary";
		case MagickaDiagnosticFlushReason::kDepletion: return "depletion";
		case MagickaDiagnosticFlushReason::kSessionBoundary: return "session_boundary";
		case MagickaDiagnosticFlushReason::kActorBoundary: return "actor_boundary";
		case MagickaDiagnosticFlushReason::kPhaseBoundary: return "phase_boundary";
		case MagickaDiagnosticFlushReason::kValueBoundary: return "value_boundary";
		case MagickaDiagnosticFlushReason::kNone: break;
		}
		return "none";
	}

	struct MagickaDrainDiagnosticAggregate
	{
		std::uint64_t sessionId{ 0 };
		std::uint64_t firstDrainSequence{ 0 };
		std::uint64_t lastDrainSequence{ 0 };
		std::uint32_t count{ 0 };
		std::uint32_t actorFormId{ 0 };
		float firstCurrent{ 0.0F };
		float lastCurrent{ 0.0F };
		float totalAmount{ 0.0F };
		float totalChargeSeconds{ 0.0F };
		float firstRegenBefore{ 0.0F };
		float lastRegenAfter{ 0.0F };
		bool depleted{ false };
		bool firstFlight{ false };
		bool lastFlight{ false };
	};

	[[nodiscard]] constexpr bool HasMagickaDrainDiagnosticAggregate(
		const MagickaDrainDiagnosticAggregate& a_aggregate) noexcept
	{
		return a_aggregate.count != 0;
	}

	[[nodiscard]] constexpr bool ShouldFlushMagickaDrainDiagnostic(
		const MagickaDrainDiagnosticAggregate& a_aggregate,
		std::uint64_t a_sessionId,
		std::uint32_t a_actorFormId,
		bool a_flight,
		bool a_depleted,
		float a_elapsedSeconds) noexcept
	{
		if (!HasMagickaDrainDiagnosticAggregate(a_aggregate)) {
			return false;
		}
		return a_aggregate.sessionId != a_sessionId ||
			a_aggregate.actorFormId != a_actorFormId ||
			a_aggregate.lastFlight != a_flight ||
			a_aggregate.depleted || a_depleted ||
			a_elapsedSeconds >= kDiagnosticAggregateWindowSeconds;
	}

	[[nodiscard]] constexpr MagickaDiagnosticFlushReason GetMagickaDrainDiagnosticFlushReason(
		const MagickaDrainDiagnosticAggregate& a_aggregate,
		std::uint64_t a_sessionId,
		std::uint32_t a_actorFormId,
		bool a_flight,
		bool a_depleted,
		float a_elapsedSeconds) noexcept
	{
		if (!HasMagickaDrainDiagnosticAggregate(a_aggregate)) {
			return MagickaDiagnosticFlushReason::kNone;
		}
		if (a_aggregate.sessionId != a_sessionId) {
			return MagickaDiagnosticFlushReason::kSessionBoundary;
		}
		if (a_aggregate.actorFormId != a_actorFormId) {
			return MagickaDiagnosticFlushReason::kActorBoundary;
		}
		if (a_aggregate.lastFlight != a_flight) {
			return MagickaDiagnosticFlushReason::kPhaseBoundary;
		}
		if (a_aggregate.depleted || a_depleted) {
			return MagickaDiagnosticFlushReason::kDepletion;
		}
		if (a_elapsedSeconds >= kDiagnosticAggregateWindowSeconds) {
			return MagickaDiagnosticFlushReason::kOneSecondBoundary;
		}
		return MagickaDiagnosticFlushReason::kNone;
	}

	[[nodiscard]] constexpr MagickaDrainDiagnosticAggregate AccumulateMagickaDrainDiagnostic(
		MagickaDrainDiagnosticAggregate a_aggregate,
		std::uint64_t a_sessionId,
		std::uint64_t a_drainSequence,
		std::uint32_t a_actorFormId,
		float a_current,
		float a_amount,
		float a_chargeSeconds,
		float a_regenBefore,
		float a_regenAfter,
		bool a_depleted,
		bool a_flight) noexcept
	{
		if (a_aggregate.count == 0) {
			a_aggregate.sessionId = a_sessionId;
			a_aggregate.firstDrainSequence = a_drainSequence;
			a_aggregate.actorFormId = a_actorFormId;
			a_aggregate.firstCurrent = a_current;
			a_aggregate.firstRegenBefore = a_regenBefore;
			a_aggregate.firstFlight = a_flight;
		}
		a_aggregate.lastDrainSequence = a_drainSequence;
		a_aggregate.count += 1;
		a_aggregate.lastCurrent = a_current;
		a_aggregate.totalAmount += std::max(0.0F, a_amount);
		a_aggregate.totalChargeSeconds += std::max(0.0F, a_chargeSeconds);
		a_aggregate.lastRegenAfter = a_regenAfter;
		a_aggregate.depleted = a_aggregate.depleted || a_depleted;
		a_aggregate.lastFlight = a_flight;
		return a_aggregate;
	}

	enum class MagickaRegenDiagnosticKind : std::uint8_t
	{
		kWait,
		kObservationAbort
	};

	struct MagickaRegenDiagnosticAggregate
	{
		std::uint64_t sessionId{ 0 };
		std::uint64_t firstDrainSequence{ 0 };
		std::uint64_t lastDrainSequence{ 0 };
		// Compatibility alias retained for existing log readers; always latest.
		std::uint64_t drainSequence{ 0 };
		std::uint32_t actorFormId{ 0 };
		std::uint32_t count{ 0 };
		std::uint32_t waitCount{ 0 };
		std::uint32_t observationCount{ 0 };
		float firstSample{ 0.0F };
		float lastSample{ 0.0F };
		float firstCurrent{ 0.0F };
		float lastCurrent{ 0.0F };
		float firstBaseline{ 0.0F };
		float lastBaseline{ 0.0F };
		float firstAfterDrain{ 0.0F };
		float lastAfterDrain{ 0.0F };
		bool firstFlight{ false };
		bool lastFlight{ false };
		bool firstPostFlight{ false };
		bool lastPostFlight{ false };
		MagickaRegenDiagnosticKind firstKind{ MagickaRegenDiagnosticKind::kWait };
		MagickaRegenDiagnosticKind lastKind{ MagickaRegenDiagnosticKind::kWait };
	};

	[[nodiscard]] constexpr bool HasMagickaRegenDiagnosticAggregate(
		const MagickaRegenDiagnosticAggregate& a_aggregate) noexcept
	{
		return a_aggregate.count != 0;
	}

	[[nodiscard]] constexpr MagickaDiagnosticFlushReason GetMagickaRegenDiagnosticFlushReason(
		const MagickaRegenDiagnosticAggregate& a_aggregate,
		std::uint64_t a_sessionId,
		std::uint32_t a_actorFormId,
		bool a_flight,
		bool a_postFlight,
		float a_current,
		float a_baseline,
		float a_afterDrain,
		float a_elapsedSeconds) noexcept
	{
		if (!HasMagickaRegenDiagnosticAggregate(a_aggregate)) {
			return MagickaDiagnosticFlushReason::kNone;
		}
		if (a_aggregate.sessionId != a_sessionId) {
			return MagickaDiagnosticFlushReason::kSessionBoundary;
		}
		if (a_aggregate.actorFormId != a_actorFormId) {
			return MagickaDiagnosticFlushReason::kActorBoundary;
		}
		if (a_aggregate.lastFlight != a_flight || a_aggregate.lastPostFlight != a_postFlight) {
			return MagickaDiagnosticFlushReason::kPhaseBoundary;
		}
		if (std::fabs(a_aggregate.lastCurrent - a_current) > kMagickaRegenDiagnosticValueTolerance ||
			std::fabs(a_aggregate.lastBaseline - a_baseline) > kMagickaRegenDiagnosticValueTolerance ||
			std::fabs(a_aggregate.lastAfterDrain - a_afterDrain) > kMagickaRegenDiagnosticValueTolerance) {
			return MagickaDiagnosticFlushReason::kValueBoundary;
		}
		if (a_elapsedSeconds >= kDiagnosticAggregateWindowSeconds) {
			return MagickaDiagnosticFlushReason::kOneSecondBoundary;
		}
		return MagickaDiagnosticFlushReason::kNone;
	}

	[[nodiscard]] constexpr bool ShouldFlushMagickaRegenDiagnostic(
		const MagickaRegenDiagnosticAggregate& a_aggregate,
		std::uint64_t a_sessionId,
		std::uint32_t a_actorFormId,
		bool a_flight,
		bool a_postFlight,
		float a_current,
		float a_baseline,
		float a_afterDrain,
		float a_elapsedSeconds) noexcept
	{
		return GetMagickaRegenDiagnosticFlushReason(
			a_aggregate,
			a_sessionId,
			a_actorFormId,
			a_flight,
			a_postFlight,
			a_current,
			a_baseline,
			a_afterDrain,
			a_elapsedSeconds) != MagickaDiagnosticFlushReason::kNone;
	}

	[[nodiscard]] constexpr MagickaRegenDiagnosticAggregate AccumulateMagickaRegenDiagnostic(
		MagickaRegenDiagnosticAggregate a_aggregate,
		std::uint64_t a_sessionId,
		std::uint64_t a_drainSequence,
		std::uint32_t a_actorFormId,
		float a_sample,
		float a_current,
		float a_baseline,
		float a_afterDrain,
		bool a_flight,
		bool a_postFlight,
		MagickaRegenDiagnosticKind a_kind) noexcept
	{
		if (a_aggregate.count == 0) {
			a_aggregate.sessionId = a_sessionId;
			a_aggregate.firstDrainSequence = a_drainSequence;
			a_aggregate.actorFormId = a_actorFormId;
			a_aggregate.firstSample = a_sample;
			a_aggregate.firstCurrent = a_current;
			a_aggregate.firstBaseline = a_baseline;
			a_aggregate.firstAfterDrain = a_afterDrain;
			a_aggregate.firstFlight = a_flight;
			a_aggregate.firstPostFlight = a_postFlight;
			a_aggregate.firstKind = a_kind;
		}
		a_aggregate.lastDrainSequence = a_drainSequence;
		a_aggregate.drainSequence = a_drainSequence;
		a_aggregate.count += 1;
		if (a_kind == MagickaRegenDiagnosticKind::kWait) {
			a_aggregate.waitCount += 1;
		} else {
			a_aggregate.observationCount += 1;
		}
		a_aggregate.lastSample = a_sample;
		a_aggregate.lastCurrent = a_current;
		a_aggregate.lastBaseline = a_baseline;
		a_aggregate.lastAfterDrain = a_afterDrain;
		a_aggregate.lastFlight = a_flight;
		a_aggregate.lastPostFlight = a_postFlight;
		a_aggregate.lastKind = a_kind;
		return a_aggregate;
	}

	struct MagickaRegenDiagnosticRecord
	{
		std::uint64_t sessionId{ 0 };
		std::uint64_t drainSequence{ 0 };
		std::uint32_t actorFormId{ 0 };
		float sample{ 0.0F };
		float current{ 0.0F };
		float baseline{ 0.0F };
		float afterDrain{ 0.0F };
		bool flight{ false };
		bool postFlight{ false };
		MagickaRegenDiagnosticKind kind{ MagickaRegenDiagnosticKind::kWait };
	};

	struct MagickaRegenDiagnosticEmission
	{
		bool emitted{ false };
		MagickaRegenDiagnosticAggregate aggregate{};
		std::uint64_t emittedAtMs{ 0 };
		std::string_view reason{};
	};

	// Production-used, deterministic orchestration for the regen diagnostic
	// stream.  The native layer injects a steady-clock millisecond timestamp;
	// tests inject fixed ticks and observe the exact same ordered emissions.
	struct MagickaRegenDiagnosticPipeline
	{
		MagickaRegenDiagnosticAggregate aggregate{};
		std::uint64_t startedAtMs{ 0 };
		bool started{ false };
		bool observationPending{ false };
		bool pumpRunning{ false };

		constexpr void BeginObservation() noexcept
		{
			observationPending = true;
			pumpRunning = true;
		}

		constexpr void PumpStopped() noexcept
		{
			observationPending = false;
			pumpRunning = false;
		}

		[[nodiscard]] constexpr bool HasAggregate() const noexcept
		{
			return HasMagickaRegenDiagnosticAggregate(aggregate);
		}

		[[nodiscard]] constexpr MagickaRegenDiagnosticEmission Flush(
			std::string_view a_reason,
			std::uint64_t a_nowMs,
			bool a_terminal = false) noexcept
		{
			MagickaRegenDiagnosticEmission emission{};
			if (HasAggregate()) {
				emission = { true, aggregate, a_nowMs, a_reason };
				aggregate = {};
				started = false;
				startedAtMs = 0;
			}
			if (a_terminal) {
				observationPending = false;
				pumpRunning = false;
			}
			return emission;
		}

		[[nodiscard]] constexpr MagickaRegenDiagnosticEmission TerminalFlush(
			std::string_view a_reason,
			std::uint64_t a_nowMs) noexcept
		{
			return Flush(a_reason, a_nowMs, true);
		}

		[[nodiscard]] constexpr MagickaRegenDiagnosticEmission Record(
			const MagickaRegenDiagnosticRecord& a_record,
			std::uint64_t a_nowMs) noexcept
		{
			MagickaRegenDiagnosticEmission emission{};
			const float elapsed = started && a_nowMs >= startedAtMs ?
				static_cast<float>(a_nowMs - startedAtMs) / 1000.0F : 0.0F;
			const auto reason = GetMagickaRegenDiagnosticFlushReason(
				aggregate,
				a_record.sessionId,
				a_record.actorFormId,
				a_record.flight,
				a_record.postFlight,
				a_record.current,
				a_record.baseline,
				a_record.afterDrain,
				 elapsed);
			if (reason != MagickaDiagnosticFlushReason::kNone) {
				emission = Flush(MagickaDiagnosticFlushReasonName(reason), a_nowMs);
			}
			if (!HasAggregate()) {
				started = true;
				startedAtMs = a_nowMs;
			}
			aggregate = AccumulateMagickaRegenDiagnostic(
				aggregate,
				a_record.sessionId,
				a_record.drainSequence,
				a_record.actorFormId,
				a_record.sample,
				a_record.current,
				a_record.baseline,
				a_record.afterDrain,
				a_record.flight,
				a_record.postFlight,
				a_record.kind);
			observationPending = true;
			pumpRunning = true;
			return emission;
		}
	};

	[[nodiscard]] constexpr bool ShouldExtendMagickaObservationWindow(
		bool a_actorLoaded,
		bool a_flightActive,
		float a_elapsedSeconds,
		bool a_extensionDue) noexcept
	{
		return a_actorLoaded && a_flightActive &&
			a_elapsedSeconds >= kMagickaRegenObservationWindowSeconds && a_extensionDue;
	}

	[[nodiscard]] constexpr bool ShouldFlushInputStateRefresh(
		bool a_initialized,
		bool a_changed,
		bool a_heartbeatDue) noexcept
	{
		return !a_initialized || a_changed || a_heartbeatDue;
	}

	struct InputDiagnosticThrottleState
	{
		bool initialized{ false };
		std::uint64_t signature{ ~std::uint64_t{ 0 } };
		std::uint32_t suppressedCount{ 0 };
	};

	struct InputDiagnosticThrottleDecision
	{
		InputDiagnosticThrottleState state{};
		bool emit{ false };
		std::uint32_t flushedSuppressedCount{ 0 };
	};

	// Input callbacks can deliver the same held event every frame.  Keep the
	// first event, meaningful changes, and a bounded heartbeat, while counting
	// suppressed repeats so the next emitted record explains what was folded.
	[[nodiscard]] constexpr InputDiagnosticThrottleDecision ReduceInputDiagnostic(
		InputDiagnosticThrottleState a_state,
		std::uint64_t a_signature,
		bool a_edgeDue,
		bool a_heartbeatDue) noexcept
	{
		const bool changed = !a_state.initialized || a_state.signature != a_signature;
		const bool emit = !a_state.initialized || changed || a_edgeDue || a_heartbeatDue;
		InputDiagnosticThrottleDecision result{ a_state, emit, 0 };
		if (emit) {
			result.flushedSuppressedCount = a_state.suppressedCount;
			result.state.initialized = true;
			result.state.signature = a_signature;
			result.state.suppressedCount = 0;
		} else {
			++result.state.suppressedCount;
		}
		return result;
	}

	// Regen observation probes are intentionally excluded from this signature.
	// Their transient ownership/sample fields are emitted as snapshot values, but
	// changing them alone must not turn every 50 ms probe into a full
	// state_snapshot log line.  A ground weapon edge remains included because it
	// is a durable passthrough boundary that can explain a transition result.
	[[nodiscard]] constexpr std::uint64_t ComputeDiagnosticStateSignature(
		std::int32_t a_graphState,
		bool a_descending,
		bool a_combatActive,
		bool a_boostHeld,
		bool a_blockRequested,
		bool a_weaponTransitionPending,
		bool a_weaponTransitionTargetDrawn,
		bool a_weaponTransitionNativeFallbackArmed,
		bool a_weaponTransitionPostFlight,
		bool a_flightShoutHeld,
		bool a_groundWeaponObservationPending,
		bool a_equipmentSwapPending = false,
		bool a_equipmentSwapPinned = false,
		bool a_equipmentSwapEdgeObserved = false,
		bool a_equipmentSwapTargetDrawn = false) noexcept
	{
		std::uint64_t signature = static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_graphState));
		signature |= a_descending ? (std::uint64_t{ 1 } << 32U) : 0;
		signature |= a_combatActive ? (std::uint64_t{ 1 } << 33U) : 0;
		signature |= a_boostHeld ? (std::uint64_t{ 1 } << 34U) : 0;
		signature |= a_blockRequested ? (std::uint64_t{ 1 } << 35U) : 0;
		signature |= a_weaponTransitionPending ? (std::uint64_t{ 1 } << 36U) : 0;
		signature |= a_weaponTransitionTargetDrawn ? (std::uint64_t{ 1 } << 37U) : 0;
		signature |= a_weaponTransitionNativeFallbackArmed ? (std::uint64_t{ 1 } << 38U) : 0;
		signature |= a_weaponTransitionPostFlight ? (std::uint64_t{ 1 } << 39U) : 0;
		signature |= a_flightShoutHeld ? (std::uint64_t{ 1 } << 40U) : 0;
		signature |= a_groundWeaponObservationPending ? (std::uint64_t{ 1 } << 41U) : 0;
		signature |= a_equipmentSwapPending ? (std::uint64_t{ 1 } << 42U) : 0;
		signature |= a_equipmentSwapPinned ? (std::uint64_t{ 1 } << 43U) : 0;
		signature |= a_equipmentSwapEdgeObserved ? (std::uint64_t{ 1 } << 44U) : 0;
		signature |= a_equipmentSwapTargetDrawn ? (std::uint64_t{ 1 } << 45U) : 0;
		return signature;
	}

	enum class WeaponFallbackMotion : std::uint8_t
	{
		kStable,
		kTowardTarget,
		kOppositeTarget
	};

	enum class WeaponFallbackDecision : std::uint8_t
	{
		kWait,
		kDeferForProgress,
		kFallback,
		kAbort
	};

	// A native draw/sheath request is a last-resort one-shot.  It is eligible
	// only if the actor remained in the exact same stable state for the whole
	// observation window.  Target-direction progress gets one bounded extension;
	// if it then stalls, one delayed directional fallback is allowed.  An opposite
	// transition or any unrelated state change aborts immediately.
	[[nodiscard]] constexpr WeaponFallbackDecision DecideWeaponFallbackObservation(
		bool a_timedOut,
		bool a_stateUnchanged,
		WeaponFallbackMotion a_motion,
		bool a_progressExtensionUsed,
		bool a_fallbackIssued,
		bool a_equipmentIdentityChanged = false) noexcept
	{
		if (a_equipmentIdentityChanged) {
			return WeaponFallbackDecision::kAbort;
		}
		if (a_motion == WeaponFallbackMotion::kOppositeTarget) {
			return WeaponFallbackDecision::kAbort;
		}
		if (a_motion == WeaponFallbackMotion::kTowardTarget) {
			if (a_fallbackIssued) {
				return a_timedOut ? WeaponFallbackDecision::kAbort : WeaponFallbackDecision::kWait;
			}
			if (!a_progressExtensionUsed) {
				return WeaponFallbackDecision::kDeferForProgress;
			}
			return a_timedOut ? WeaponFallbackDecision::kFallback : WeaponFallbackDecision::kWait;
		}
		if (!a_stateUnchanged || a_progressExtensionUsed) {
			return WeaponFallbackDecision::kAbort;
		}
		if (!a_timedOut) {
			return WeaponFallbackDecision::kWait;
		}
		return a_fallbackIssued ? WeaponFallbackDecision::kAbort : WeaponFallbackDecision::kFallback;
	}

	struct WeaponRoutingDiagnosticState
	{
		bool requestedTargetDrawn{ false };
		bool actualDrawn{ false };
	};

	[[nodiscard]] constexpr WeaponRoutingDiagnosticState MakeWeaponRoutingDiagnosticState(
		bool a_requestedTargetDrawn,
		bool a_actualDrawn) noexcept
	{
		return { a_requestedTargetDrawn, a_actualDrawn };
	}

	struct DiagnosticPhase
	{
		bool flight{ false };
		bool postFlight{ false };
	};

	[[nodiscard]] constexpr DiagnosticPhase ResolveDiagnosticPhase(
		bool a_flying,
		bool a_postFlightTransition) noexcept
	{
		return { a_flying, !a_flying && a_postFlightTransition };
	}

	// The world-state token is consumed by the first stop.  Flying alone is not
	// proof that DAF captured a controller/gravity state; a later stop may clear
	// DAF-only bookkeeping but must not rewrite a controller without ownership.
	[[nodiscard]] constexpr bool OwnsWorldStateForStop(
		bool,
		bool a_worldStateOwned) noexcept
	{
		return a_worldStateOwned;
	}

	struct FlightStopState
	{
		bool flying{ false };
		bool descending{ false };
		bool combatActive{ false };
		bool blockRequested{ false };
		bool shoutHeld{ false };
		bool launchHeld{ false };
		bool boostHeld{ false };
		WeaponTransitionState weapon{};
		bool worldStateOwned{ false };
	};

	[[nodiscard]] constexpr FlightStopState ReduceFlightStop(FlightStopState a_state) noexcept
	{
		a_state.flying = false;
		a_state.descending = false;
		a_state.combatActive = false;
		a_state.blockRequested = false;
		a_state.shoutHeld = false;
		a_state.launchHeld = false;
		a_state.boostHeld = false;
		a_state.worldStateOwned = false;
		a_state.weapon = PreserveWeaponTransitionAfterStop(a_state.weapon);
		return a_state;
	}

	struct InputLatchState
	{
		bool launchHeld{ false };
		bool ascendHeld{ false };
		bool descendHeld{ false };
		bool readyWeaponHeld{ false };
		bool shoutHeld{ false };
		bool boostHeld{ false };
	};

	[[nodiscard]] constexpr InputLatchState ResetInputLatches(
		InputLatchState a_state,
		bool a_clearShout) noexcept
	{
		a_state.launchHeld = false;
		a_state.ascendHeld = false;
		a_state.descendHeld = false;
		a_state.readyWeaponHeld = false;
		a_state.boostHeld = false;
		if (a_clearShout) {
			a_state.shoutHeld = false;
		}
		return a_state;
	}

	enum class ShoutEdge : std::uint8_t
	{
		kPress,
		kHeld,
		kRelease
	};

	// Immutable actions captured by the input/Papyrus boundary.  The native
	// manager applies these only from a queued SKSE task; no InputEvent pointer
	// crosses that boundary.
	enum class FlightInputAction : std::uint8_t
	{
		kStartFlight,
		kStopFlight,
		kBeginDescent,
		kCancelDescent,
		kToggleCombatReady,
		kBeginCombat,
		kBlockRequest,
		kBlockRelease,
		kShoutPress,
		kShoutRelease,
		kClearShout,
		kLaunchBoost,
		kSetMovementInput,
		kSetVerticalInput,
		kSetBoostHeld,
		kObserveGroundWeaponTransition
	};

	struct RuntimeDispatchBoundaryState
	{
		bool boundaryQueued{ false };
		bool expectedFlying{ false };
	};

	struct RuntimeDispatchBoundaryDecision
	{
		RuntimeDispatchBoundaryState state{};
		bool boundaryQueued{ false };
		bool deferSemanticEdge{ false };
		bool rebindSession{ false };
		bool expectedFlying{ false };
	};

	[[nodiscard]] constexpr bool IsRuntimeSessionBoundaryAction(
		FlightInputAction a_action) noexcept
	{
		return a_action == FlightInputAction::kStartFlight ||
			a_action == FlightInputAction::kStopFlight;
	}

	[[nodiscard]] constexpr RuntimeDispatchBoundaryDecision ReduceRuntimeDispatchBoundary(
		RuntimeDispatchBoundaryState a_state,
		FlightInputAction a_action,
		bool a_semanticEdge) noexcept
	{
		RuntimeDispatchBoundaryDecision result{ a_state };
		result.boundaryQueued = a_state.boundaryQueued;
		if (a_state.boundaryQueued && a_semanticEdge) {
			result.deferSemanticEdge = true;
			result.rebindSession = true;
			result.expectedFlying = a_state.expectedFlying;
			return result;
		}
		if (IsRuntimeSessionBoundaryAction(a_action)) {
			result.state.boundaryQueued = true;
			result.state.expectedFlying = a_action == FlightInputAction::kStartFlight;
			result.boundaryQueued = true;
		}
		return result;
	}

	// Same-dispatch rebind edges may survive a start/stop/session transition,
	// but never an actor or player unload boundary.  The latter can reuse the
	// same physical input code for a different owner, so retaining its snapshot
	// would apply a stale release/Ready edge to the new actor.
	[[nodiscard]] constexpr bool ShouldPreserveDeferredInputRebind(
		bool a_flightStopped,
		bool a_playerUnloaded,
		bool a_actorChanged,
		bool a_sessionChanged) noexcept
	{
		return (a_flightStopped || a_sessionChanged) &&
			!a_playerUnloaded && !a_actorChanged;
	}

	struct FlightInputActionSnapshot
	{
		inline static constexpr auto UncapturedSession = std::numeric_limits<std::uint64_t>::max();
		FlightInputAction action{ FlightInputAction::kClearShout };
		float valueA{ 0.0F };
		float valueB{ 0.0F };
		bool flag{ false };
		std::uint64_t sourceSession{ UncapturedSession };
		ReadyGenerationToken readyToken{};
		std::uint32_t actorFormId{ 0 };
		std::int32_t actorWeaponState{ -1 };
		WeaponEquipmentIdentity equipmentIdentity{};
		bool equipmentIdentityCaptured{ false };
		std::uint64_t equipmentIdentityEpoch{ 0 };
		// Immutable producer ordering domain.  It is assigned at the input boundary
		// and bridges that boundary to the game-thread manager sequence.
		std::uint64_t inputSequenceDomain{ 0 };
		// Allocated once at the input/Papyrus boundary and never reused within a
		// process.  It is diagnostic metadata only; state admission remains based
		// on the existing session/ready/identity gates.
		std::uint64_t actionId{ 0 };
	};

	inline constexpr float InputActionAnalogEpsilon = 0.01F;
	inline constexpr std::size_t InputActionCoalescingChannelCount = 4;
	inline constexpr std::size_t InvalidInputActionCoalescingChannel =
		InputActionCoalescingChannelCount;

	[[nodiscard]] constexpr std::size_t GetInputActionCoalescingChannel(
		FlightInputAction a_action) noexcept
	{
		switch (a_action) {
		case FlightInputAction::kSetMovementInput:
			return 0;
		case FlightInputAction::kSetVerticalInput:
			return 1;
		case FlightInputAction::kSetBoostHeld:
			return 2;
		case FlightInputAction::kClearShout:
			// Reset may be requested repeatedly while input is suppressed.  Clear is
			// idempotent cleanup; physical shout press/release edges remain outside
			// every coalescing channel.
			return 3;
		default:
			return InvalidInputActionCoalescingChannel;
		}
	}

	[[nodiscard]] constexpr bool IsCoalescibleInputAction(
		FlightInputAction a_action) noexcept
	{
		return GetInputActionCoalescingChannel(a_action) !=
			InvalidInputActionCoalescingChannel;
	}

	[[nodiscard]] constexpr bool ShouldLogFlightActionQueueAcceptance(
		FlightInputAction a_action) noexcept
	{
		// State-channel acceptance is intentionally represented by change and
		// suppression-summary diagnostics at the input boundary.  Failures and
		// stale drops remain logged independently.
		return !IsCoalescibleInputAction(a_action);
	}

	struct InputActionSuppressionSummary
	{
		bool pending{ false };
		std::uint32_t count{ 0 };
		std::uint64_t firstTimestampMs{ 0 };
		std::uint64_t lastTimestampMs{ 0 };
		std::uint64_t firstActionId{ 0 };
		std::uint64_t lastActionId{ 0 };
		FlightInputAction firstAction{ FlightInputAction::kClearShout };
		FlightInputAction lastAction{ FlightInputAction::kClearShout };
		std::uint64_t firstSourceSession{ FlightInputActionSnapshot::UncapturedSession };
		std::uint64_t lastSourceSession{ FlightInputActionSnapshot::UncapturedSession };
		std::uint64_t firstSequenceDomain{ 0 };
		std::uint64_t lastSequenceDomain{ 0 };
		float firstValueA{ 0.0F };
		float firstValueB{ 0.0F };
		float lastValueA{ 0.0F };
		float lastValueB{ 0.0F };
		bool firstFlag{ false };
		bool lastFlag{ false };
	};

	struct InputActionCoalescingState
	{
		bool initialized{ false };
		std::uint64_t sequenceDomain{ 0 };
		FlightInputActionSnapshot lastAccepted{};
		InputActionSuppressionSummary suppressed{};
	};

	struct InputActionCoalescingDecision
	{
		InputActionCoalescingState state{};
		bool enqueue{ false };
		bool flushSummary{ false };
		InputActionSuppressionSummary flushedSummary{};
	};

	[[nodiscard]] constexpr float InputActionAbsolute(float a_value) noexcept
	{
		return a_value < 0.0F ? -a_value : a_value;
	}

	[[nodiscard]] constexpr std::int8_t InputActionDirection(float a_value) noexcept
	{
		return a_value > 0.0F ? 1 : (a_value < 0.0F ? -1 : 0);
	}

	[[nodiscard]] constexpr bool AreInputActionAnalogValuesEquivalent(
		float a_left,
		float a_right) noexcept
	{
		// NaN is never a stable input state.  A direction/zero boundary is always
		// meaningful even when its magnitude is smaller than the analog epsilon.
		if (a_left != a_left || a_right != a_right) {
			return false;
		}
		return InputActionDirection(a_left) == InputActionDirection(a_right) &&
			InputActionAbsolute(a_left - a_right) <= InputActionAnalogEpsilon;
	}

	[[nodiscard]] constexpr bool AreInputActionStatesEquivalent(
		const FlightInputActionSnapshot& a_left,
		const FlightInputActionSnapshot& a_right) noexcept
	{
		if (a_left.action != a_right.action) {
			return false;
		}
		switch (a_left.action) {
		case FlightInputAction::kSetMovementInput:
			return AreInputActionAnalogValuesEquivalent(a_left.valueA, a_right.valueA) &&
				AreInputActionAnalogValuesEquivalent(a_left.valueB, a_right.valueB);
		case FlightInputAction::kSetVerticalInput:
			return AreInputActionAnalogValuesEquivalent(a_left.valueA, a_right.valueA);
		case FlightInputAction::kSetBoostHeld:
			return a_left.flag == a_right.flag;
		case FlightInputAction::kClearShout:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] constexpr InputActionCoalescingDecision FlushInputActionCoalescing(
		InputActionCoalescingState a_state,
		bool a_resetBaseline) noexcept
	{
		InputActionCoalescingDecision result;
		result.state = a_state;
		if (result.state.suppressed.pending) {
			result.flushSummary = true;
			result.flushedSummary = result.state.suppressed;
			result.state.suppressed = {};
		}
		if (a_resetBaseline) {
			result.state.initialized = false;
			result.state.sequenceDomain = 0;
			result.state.lastAccepted = {};
		}
		return result;
	}

	[[nodiscard]] constexpr InputActionCoalescingDecision ReduceInputActionForQueue(
		InputActionCoalescingState a_state,
		const FlightInputActionSnapshot& a_action,
		std::uint64_t a_timestampMs,
		std::uint64_t a_sequenceDomain,
		bool a_forceEmission) noexcept
	{
		InputActionCoalescingDecision result;
		result.state = a_state;
		if (!IsCoalescibleInputAction(a_action.action)) {
			// Physical/control edges are never folded into state channels.
			if (result.state.suppressed.pending) {
				result.flushSummary = true;
				result.flushedSummary = result.state.suppressed;
				result.state.suppressed = {};
			}
			result.enqueue = true;
			return result;
		}

		const bool sameDomain = result.state.initialized &&
			result.state.sequenceDomain == a_sequenceDomain &&
			result.state.lastAccepted.sourceSession == a_action.sourceSession;
		const bool sameState = sameDomain &&
			AreInputActionStatesEquivalent(result.state.lastAccepted, a_action);
		if (a_forceEmission || !sameState) {
			if (result.state.suppressed.pending) {
				result.flushSummary = true;
				result.flushedSummary = result.state.suppressed;
				result.state.suppressed = {};
			}
			result.enqueue = true;
			result.state.initialized = true;
			result.state.sequenceDomain = a_sequenceDomain;
			result.state.lastAccepted = a_action;
			return result;
		}

		result.enqueue = false;
		auto& summary = result.state.suppressed;
		if (!summary.pending) {
			summary.pending = true;
			summary.count = 1;
			summary.firstTimestampMs = a_timestampMs;
			summary.lastTimestampMs = a_timestampMs;
			summary.firstActionId = a_action.actionId;
			summary.lastActionId = a_action.actionId;
			summary.firstAction = a_action.action;
			summary.lastAction = a_action.action;
			summary.firstSourceSession = a_action.sourceSession;
			summary.lastSourceSession = a_action.sourceSession;
			summary.firstSequenceDomain = a_sequenceDomain;
			summary.lastSequenceDomain = a_sequenceDomain;
			summary.firstValueA = a_action.valueA;
			summary.firstValueB = a_action.valueB;
			summary.lastValueA = a_action.valueA;
			summary.lastValueB = a_action.valueB;
			summary.firstFlag = a_action.flag;
			summary.lastFlag = a_action.flag;
		} else {
			if (summary.count < std::numeric_limits<std::uint32_t>::max()) {
				++summary.count;
			}
			summary.lastTimestampMs = a_timestampMs;
			summary.lastActionId = a_action.actionId;
			summary.lastAction = a_action.action;
			summary.lastSourceSession = a_action.sourceSession;
			summary.lastSequenceDomain = a_sequenceDomain;
			summary.lastValueA = a_action.valueA;
			summary.lastValueB = a_action.valueB;
			summary.lastFlag = a_action.flag;
		}
		return result;
	}

	[[nodiscard]] constexpr bool IsQueuedActionSequenceNewer(
		std::uint64_t a_lastSequence,
		std::uint64_t a_candidateSequence) noexcept
	{
		return a_candidateSequence > a_lastSequence;
	}

	// Input/Papyrus tasks capture the source session before they are queued.  A
	// stop immediately following a queued start is the sole successor allowed to
	// cross a session boundary; all other stale tasks are rejected on the game
	// thread before touching engine state.
	[[nodiscard]] constexpr bool IsQueuedActionSessionCurrent(
		FlightInputAction a_action,
		std::uint64_t a_currentSession,
		std::uint64_t a_sourceSession,
		bool a_flying,
		std::uint64_t a_startActionSequence,
		std::uint64_t a_actionSequence) noexcept
	{
		bool valid = a_currentSession == a_sourceSession;
		if (a_action == FlightInputAction::kStartFlight) {
			valid = valid && !a_flying;
		} else if (a_action == FlightInputAction::kStopFlight && !valid) {
			valid = a_sourceSession != std::numeric_limits<std::uint64_t>::max() &&
				a_currentSession == a_sourceSession + 1 &&
				a_flying &&
				a_startActionSequence > 0 &&
				a_startActionSequence < a_actionSequence;
		}
		return valid;
	}

	enum class InputButtonPhase : std::uint8_t
	{
		kOther,
		kUp,
		kDown,
		kHeld,
		kPressed
	};

	// A value-only copy of the fields the input sink needs.  The RE event is
	// valid only during ProcessEvent; no pointer or engine-owned string crosses
	// into a handler or queued task.
	struct ButtonInputSnapshot
	{
		std::string userEvent{};
		std::uint32_t device{ 0 };
		std::uint32_t code{ 0 };
		bool up{ false };
		bool down{ false };
		bool pressed{ false };
		bool held{ false };
		float heldDuration{ 0.0F };

		[[nodiscard]] bool IsUp() const noexcept { return up; }
		[[nodiscard]] bool IsDown() const noexcept { return down; }
		[[nodiscard]] bool IsPressed() const noexcept { return pressed; }
		[[nodiscard]] bool IsHeld() const noexcept { return held; }
	};

	// CommonLib reports IsPressed() for both the initial press and subsequent
	// held samples.  Treat only the initial sample as a press edge; held-only
	// observations are level samples and must never be replayed as a new action.
	[[nodiscard]] inline bool IsButtonPressEdge(
		const ButtonInputSnapshot& a_event) noexcept
	{
		return a_event.IsDown() || (a_event.IsPressed() && !a_event.IsHeld());
	}

	// A semantic input edge includes both halves of a button transition.  The
	// release is just as important as the press when a snapshot is deferred
	// across a game/session boundary: dropping it leaves a shout, block, or
	// Ready Weapon latch owned by the old dispatch.
	[[nodiscard]] inline bool IsSemanticButtonEdge(
		const ButtonInputSnapshot& a_event) noexcept
	{
		return IsButtonPressEdge(a_event) || a_event.IsUp();
	}

	struct ThumbstickInputSnapshot
	{
		bool left{ false };
		float x{ 0.0F };
		float y{ 0.0F };

		[[nodiscard]] bool IsLeft() const noexcept { return left; }
	};

	// Copied diagnostic data is safe to move from an input callback to the game
	// thread.  It deliberately contains no RE event pointer or RE object.
	struct InputDiagnosticSnapshot
	{
		std::string action;
		std::string userEvent;
		std::string outcome;
		std::uint32_t device{ 0 };
		std::uint32_t code{ 0 };
		InputButtonPhase phase{ InputButtonPhase::kOther };
		float heldDuration{ 0.0F };
		std::uint64_t actionId{ 0 };
		std::uint64_t inputSequenceDomain{ 0 };
	};

	[[nodiscard]] constexpr bool IsCooperativeFlightInputAction(
		FlightInputAction a_action) noexcept
	{
		switch (a_action) {
		case FlightInputAction::kToggleCombatReady:
		case FlightInputAction::kBeginCombat:
		case FlightInputAction::kBlockRequest:
		case FlightInputAction::kBlockRelease:
		case FlightInputAction::kShoutPress:
		case FlightInputAction::kShoutRelease:
			return true;
		default:
			return false;
		}
	}

	struct ShoutLatchState
	{
		bool held{ false };
		bool whirlwindPending{ false };
	};

	struct ShoutEdgeResult
	{
		ShoutLatchState state{};
		bool acceptedPress{ false };
		bool acceptedRelease{ false };
		bool ignoredHeld{ false };
	};

	[[nodiscard]] constexpr ShoutEdgeResult ReduceShoutEdge(
		ShoutLatchState a_state,
		ShoutEdge a_edge) noexcept
	{
		ShoutEdgeResult result{ a_state };
		switch (a_edge) {
		case ShoutEdge::kPress:
			if (result.state.held) {
				result.ignoredHeld = true;
			} else {
				result.state.held = true;
				result.acceptedPress = true;
			}
			break;
		case ShoutEdge::kHeld:
			// Held is an observation, never a new shout edge.  In particular it
			// must not extend the graph override or queue another shout release.
			result.ignoredHeld = result.state.held;
			break;
		case ShoutEdge::kRelease:
			result.acceptedRelease = result.state.held || result.state.whirlwindPending;
			result.state.held = false;
			result.state.whirlwindPending = false;
			break;
		}
		return result;
	}

	inline constexpr float kMagickaDrainBatchSeconds = 0.10F;
	inline constexpr float kMagickaDrainMaxElapsedSeconds = 0.25F;

	struct MagickaDrainSlice
	{
		float chargeSeconds{ 0.0F };
		float carrySeconds{ 0.0F };
	};

	[[nodiscard]] constexpr MagickaDrainSlice AccumulateMagickaDrain(
		float a_carrySeconds,
		float a_elapsedSeconds) noexcept
	{
		const auto elapsed = std::clamp(a_elapsedSeconds, 0.0F, kMagickaDrainMaxElapsedSeconds);
		const auto total = std::max(0.0F, a_carrySeconds) + elapsed;
		if (total < kMagickaDrainBatchSeconds) {
			return { 0.0F, total };
		}
		return { total, 0.0F };
	}

	enum class MagickaRegenObservationResult : std::uint8_t
	{
		kWait,
		kStable,
		kAbort
	};

	struct MagickaRegenObservationState
	{
		bool dafOwned{ false };
		float baseline{ 0.0F };
		float observedAfterDrain{ 0.0F };
		std::uint32_t unchangedSamples{ 0 };
		// DamageActorValue can publish the process regen delay on a later game
		// tick.  Keep that candidate window explicit, but do not infer ownership
		// from a later increase: without an immediate DAF-owned value, that
		// increase is indistinguishable from an external writer.
		bool awaitingDafIncrease{ false };
		// Identity is kept with the observation so a late sample can never be
		// applied to a different flight session, drain, or actor instance.
		std::uint64_t sessionId{ 0 };
		std::uint64_t drainSequence{ 0 };
		std::uint32_t actorFormId{ 0 };
	};

	struct MagickaRegenObservationStep
	{
		MagickaRegenObservationState state{};
		MagickaRegenObservationResult result{ MagickaRegenObservationResult::kAbort };
	};

	// Observation identity is deliberately stricter than value equality.  A
	// process sample is diagnostic only and is valid solely for the same loaded
	// actor, session, and drain that produced it.  This is also the pure guard
	// used by native code before recording a sample; there is no corresponding
	// write operation because the engine exposes no DAF writer provenance.
	[[nodiscard]] constexpr bool IsMagickaRegenObservationCurrent(
		const MagickaRegenObservationState& a_state,
		std::uint64_t a_currentSession,
		std::uint64_t a_currentDrainSequence,
		std::uint32_t a_currentActorFormId,
		bool a_actorLoaded) noexcept
	{
		return a_actorLoaded &&
			a_state.sessionId != 0 &&
			a_state.drainSequence != 0 &&
			a_state.actorFormId != 0 &&
			a_state.sessionId == a_currentSession &&
			a_state.drainSequence == a_currentDrainSequence &&
			a_state.actorFormId == a_currentActorFormId;
	}

	[[nodiscard]] constexpr bool ShouldContinueMagickaObservationAfterAirborneTimeout(
		bool a_actorLoaded,
		bool a_flightActive) noexcept
	{
		// In no-write mode a loaded actor's unresolved observation must survive the
		// airborne window so StopFlight can re-arm it for landing diagnostics.
		return a_actorLoaded && a_flightActive;
	}

	[[nodiscard]] constexpr bool ShouldAdvanceMagickaDrainSequence(
		bool a_damageActorValueIssued) noexcept
	{
		return a_damageActorValueIssued;
	}

	// Several later-tick samples can establish a stable diagnostic observation.
	// Any decay or increase after an immediate candidate means regeneration or
	// another system owns the value.  The native layer never writes this field:
	// when the engine publishes DamageActorValue's delay on a later tick, the
	// candidate window observes that change but cannot identify its writer.
	[[nodiscard]] constexpr MagickaRegenObservationStep ObserveMagickaRegenDelay(
		MagickaRegenObservationState a_state,
		float a_current,
		std::uint32_t a_requiredSamples = 3,
		float a_tolerance = 0.01F) noexcept
	{
		if (a_requiredSamples == 0) {
			return { a_state, MagickaRegenObservationResult::kAbort };
		}

		const auto tolerance = std::max(0.0F, a_tolerance);
		if (!a_state.dafOwned) {
			if (!a_state.awaitingDafIncrease ||
				a_current < a_state.baseline - tolerance ||
				a_current > a_state.baseline + tolerance) {
				return { a_state, MagickaRegenObservationResult::kAbort };
			}
			return { a_state, MagickaRegenObservationResult::kWait };
		}

		if (a_current < a_state.observedAfterDrain - tolerance ||
			a_current > a_state.observedAfterDrain + tolerance ||
			a_current <= a_state.baseline + tolerance) {
			return { a_state, MagickaRegenObservationResult::kAbort };
		}

		++a_state.unchangedSamples;
		if (a_state.unchangedSamples >= a_requiredSamples) {
			return { a_state, MagickaRegenObservationResult::kStable };
		}
		return { a_state, MagickaRegenObservationResult::kWait };
	}
}
