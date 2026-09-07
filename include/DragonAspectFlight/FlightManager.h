#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <thread>

#include "DragonAspectFlight/FlightStateHelpers.h"
#include "RE/H/hkVector4.h"

namespace DragonAspectFlight
{
	class FlightManager
	{
	public:
		static FlightManager& GetSingleton();

		void StartFlight();
		void BeginDescent();
		void CancelDescent();
		void StopFlight();
		[[nodiscard]] bool IsFlying() const;
		[[nodiscard]] bool IsDescending() const;
		[[nodiscard]] bool IsFlightCombatActive() const;
		[[nodiscard]] bool IsFlightBlockRequested() const;
		[[nodiscard]] bool IsDragonAspectActive() const;
		[[nodiscard]] static bool ShouldSuppressInput();
		[[nodiscard]] State::WeaponEquipmentEpochSnapshot GetCurrentWeaponEquipmentSnapshot();
		[[nodiscard]] std::uint64_t GetCurrentWeaponEquipmentIdentityEpoch();
		bool ToggleFlightCombatReady();
		bool BeginFlightCombat();
		bool SetFlightBlockRequested(
			bool a_requested,
			std::optional<State::WeaponEquipmentIdentity> a_actionIdentity = std::nullopt,
			std::uint64_t a_actionIdentityEpoch = 0);
		bool QueueFlightAction(State::FlightInputActionSnapshot a_action);

		void SetFlightSpeed(float a_speed);
		void SetVerticalSpeed(float a_speed);
		void SetLiftScale(float a_scale);
		void SetDetailedLogging(bool a_enabled, float a_snapshotIntervalSeconds);
		void FlushDiagnosticAggregates(std::string_view a_reason = "manual");
		void SetMovementInput(float a_forwardInput, float a_strafeInput);
		void SetVerticalInput(float a_verticalInput);
		void TriggerLaunchBoost();
		void SetBoostHeld(bool a_boostHeld);
		void NotifyFlightShout(bool a_released);
		void ClearFlightShoutState();

		[[nodiscard]] float GetFlightSpeed() const;
		[[nodiscard]] float GetVerticalSpeed() const;
		[[nodiscard]] float GetLiftScale() const;
		[[nodiscard]] std::uint64_t GetPublishedFlightSession() const noexcept;
		[[nodiscard]] State::ReadyGenerationToken AnnounceReadyGeneration();
		void RetireReadyGeneration(
			State::ReadyGenerationToken a_token,
			std::string_view a_reason = "retired",
			std::uint64_t a_sourceSession = State::FlightInputActionSnapshot::UncapturedSession);
		[[nodiscard]] std::uint64_t ResetForLifecycle(std::string_view a_reason);
		void LogInputDiagnostic(const State::InputDiagnosticSnapshot& a_snapshot) const;

	private:
		FlightManager() = default;
		FlightManager(const FlightManager&) = delete;
		FlightManager(FlightManager&&) = delete;
		FlightManager& operator=(const FlightManager&) = delete;
		FlightManager& operator=(FlightManager&&) = delete;

		void StartUpdateThread();
		void StopUpdateThread();
		[[nodiscard]] std::uint64_t AdvanceUpdateTaskGeneration() noexcept;
		void QueueUpdate();
		void UpdateFlight();
		void ApplyQueuedFlightAction(State::FlightInputActionSnapshot a_action);
		void StartGroundWeaponObservation(State::FlightInputActionSnapshot a_action);
		void ObserveGroundWeaponTransition(
			RE::PlayerCharacter* a_player,
			std::chrono::steady_clock::time_point a_now);
		void LogWeaponRoutingDiagnostic(
			RE::PlayerCharacter* a_player,
			std::string_view a_reason,
			std::string_view a_fallbackReason,
			bool a_force,
			std::optional<bool> a_targetDrawn = std::nullopt);
		bool HandleEquipmentIdentityChange(
			const State::WeaponEquipmentIdentity& a_current,
			std::string_view a_reason);
		[[nodiscard]] State::WeaponEquipmentEpochObservation ObserveWeaponEquipmentIdentityLocked(
			const State::WeaponEquipmentIdentity& a_identity,
			bool a_identityCaptured);
		void SetWeaponEquipmentEpochBaselineLocked(
			const State::WeaponEquipmentIdentity& a_identity,
			bool a_identityCaptured);
		bool ToggleFlightCombatReadyWithIdentity(
			std::optional<State::WeaponEquipmentIdentity> a_observedIdentity,
			std::uint64_t a_observedIdentityEpoch);
		[[nodiscard]] bool AcceptQueuedFlightAction(
			const State::FlightInputActionSnapshot& a_action,
			std::uint64_t a_sequence,
			std::uint64_t a_sourceSession);
		[[nodiscard]] bool PrepareReadyGeneration(State::ReadyGenerationToken a_token);
		[[nodiscard]] std::uint64_t GetReadyGenerationSession() const;
		void ClearReadyGenerationLeaseLocked();
		void ClearReadyProtectedStateLocked();
		[[nodiscard]] bool TryExecuteNativeWeaponFallback(
			RE::PlayerCharacter* a_player,
			bool a_groundObserver,
			bool a_targetDrawn,
			std::uint32_t a_actorFormId,
			State::WeaponEquipmentIdentity a_observedIdentity,
			bool a_observedIdentityCaptured,
			State::NativeFallbackGateRequest a_request);
		bool SetFlightCombatActive(bool a_active);
		bool DrainFlightMagicka(RE::PlayerCharacter* a_player, std::chrono::steady_clock::time_point a_now);
		void ArmDafMagickaRegenObservation(std::chrono::steady_clock::time_point a_now);
		void ObserveDafMagickaRegen(
			RE::PlayerCharacter* a_player,
			std::chrono::steady_clock::time_point a_now,
			bool a_allowRelease);
		void RecordMagickaDrainDiagnostic(
			std::uint64_t a_session,
			std::uint64_t a_drainSequence,
			std::uint32_t a_actorFormId,
			float a_current,
			float a_amount,
			float a_chargeSeconds,
			float a_regenBefore,
			float a_regenAfter,
			bool a_depleted,
			bool a_flight);
		void FlushMagickaDrainDiagnostic(std::string_view a_reason);
		void RecordMagickaRegenDiagnostic(
			std::uint64_t a_session,
			std::uint64_t a_drainSequence,
			std::uint32_t a_actorFormId,
			float a_sample,
			float a_current,
			float a_baseline,
			float a_afterDrain,
			bool a_flight,
			bool a_postFlight,
			State::MagickaRegenDiagnosticKind a_kind);
		void FlushMagickaRegenDiagnostic(std::string_view a_reason, bool a_terminal = false);
		void LogDiagnosticSnapshot(RE::PlayerCharacter* a_player);

		mutable std::shared_mutex _mutex;

		bool _isFlying{ false };
		bool _isDescending{ false };
		bool _flightCombatActive{ false };
		bool _flightBlockRequested{ false };
		// True only while DAF owns the shared native actor/graph block state.  A
		// logical request alone is not proof that DAF may clear vanilla/MCO state.
		// The lease below is the authority; this bool is retained as a diagnostic
		// mirror for existing state snapshots and is never sufficient to authorize
		// a native release.  The current metadata-only runtime never sets it true.
		bool _flightBlockNativeStateOwned{ false };
		State::NativeBlockLeaseToken _flightBlockLease{};
		// Metadata-only block intent is still bound to the exact producer snapshot,
		// allowing an old pending A request to be revoked without discarding a newer
		// B request observed before the update callback runs.
		State::WeaponEquipmentIdentity _flightBlockRequestedIdentity{};
		std::uint64_t _flightBlockRequestedIdentityEpoch{ 0 };
		bool _flightBlockRequestedIdentityCaptured{ false };
		bool _weaponTransitionPending{ false };
		bool _weaponTransitionTargetDrawn{ false };
		bool _weaponTransitionNativeFallbackArmed{ false };
		bool _weaponTransitionExpiryRecoveryAttempted{ false };
		bool _weaponTransitionNativeFallbackRetryUsed{ false };
		bool _weaponTransitionProgressExtensionUsed{ false };
		bool _weaponTransitionPostFlight{ false };
		State::WeaponTransitionTerminalFailureHoldState _weaponTransitionTerminalFailureHold{};
		bool _useGeneratedCombatTopology{ false };
		bool _aerialCombatUnsupportedNotified{ false };
		// One-shot ownership token for controller/gravity/fall-state cleanup.
		// It is consumed by StopFlight so repeated stops cannot overwrite later
		// vanilla or third-party controller changes.
		bool _flightWorldStateOwned{ false };
		float _flightSpeed{ 14.0F };
		float _verticalSpeed{ 24.0F };
		float _liftScale{ 1.0F };
		float _forwardInput{ 0.0F };
		float _strafeInput{ 0.0F };
		float _verticalInput{ 0.0F };
		float _pendingLaunchBoost{ 0.0F };
		bool _boostHeld{ false };
		std::int32_t _lastGraphState{ 0 };
		std::uint32_t _landingContactTicks{ 0 };
		bool _detailedLogging{ true };
		float _diagnosticSnapshotIntervalSeconds{ 2.0F };
		std::uint64_t _flightSessionId{ 0 };
		// Published on the game thread when a session starts; queue callers use
		// this immutable snapshot without taking engine or manager state locks.
		std::atomic_uint64_t _publishedFlightSessionId{ 0 };
		std::atomic_uint64_t _readyGenerationCounter{ 0 };
		State::ReadyGenerationBarrierState _readyGenerationBarrier{};
		State::ReadyGenerationToken _readyGenerationLeaseToken{};
		std::chrono::steady_clock::time_point _readyGenerationLeaseStartedAt{};
		bool _readyGenerationBlockedDiagnosticLogged{ false };
		// Lock order: the native-action gate is acquired before _mutex whenever
		// announcing/rebasing a Ready token or reserving/validating a
		// DrawWeaponMagicHands fallback.  The directional engine call itself runs
		// after both locks are released; the recursive type permits documented
		// same-thread lifecycle re-entry during reservation/revalidation.
		mutable std::recursive_mutex _readyNativeActionMutex;
		std::uint64_t _flightSessionStartActionSequence{ 0 };
		State::QueuedActionExecutionSequenceState _nextQueuedActionSequence{};
		std::uint64_t _lastQueuedActionSequence{ 0 };
		std::uint64_t _activeQueuedActionSequence{ 0 };
		std::uint64_t _applyingActionId{ 0 };
		std::uint64_t _applyingInputSequenceDomain{ 0 };
		std::atomic_uint32_t _flightActionQueueExceptionCount{ 0 };
		std::atomic_uint32_t _flightActionTaskExceptionCount{ 0 };
		std::uint64_t _lastDiagnosticEquipmentSignature{ ~std::uint64_t{ 0 } };
		std::uint64_t _lastWeaponRoutingSignature{ ~std::uint64_t{ 0 } };
		std::uint64_t _lastDiagnosticStateSignature{ ~std::uint64_t{ 0 } };
		mutable std::uint64_t _lastInputDiagnosticSignature{ ~std::uint64_t{ 0 } };
		mutable std::chrono::steady_clock::time_point _lastInputDiagnosticAt{};
		mutable std::uint32_t _suppressedInputDiagnosticCount{ 0 };
		std::chrono::steady_clock::time_point _lastDiagnosticSnapshot{};
		std::chrono::steady_clock::time_point _weaponTransitionDeadline{};
		std::chrono::steady_clock::time_point _weaponTransitionNativeFallbackAt{};
		std::int32_t _weaponTransitionPreRequestState{ -1 };
		std::uint32_t _weaponTransitionActorFormId{ 0 };
		std::uint64_t _weaponTransitionSessionId{ 0 };
		std::uint64_t _weaponTransitionIdentityEpoch{ 0 };
		State::WeaponEquipmentIdentity _weaponTransitionEquipmentIdentity{};
		bool _weaponTransitionEquipmentIdentityCaptured{ false };
		bool _groundWeaponObservationPending{ false };
		bool _groundWeaponFallbackIssued{ false };
		bool _groundWeaponFallbackRetryUsed{ false };
		bool _groundWeaponProgressExtensionUsed{ false };
		bool _groundWeaponTargetDrawn{ false };
		std::uint32_t _groundWeaponActorFormId{ 0 };
		std::int32_t _groundWeaponPreEdgeState{ -1 };
		std::uint64_t _groundWeaponSessionId{ 0 };
		std::uint64_t _groundWeaponEdgeSequence{ 0 };
		std::uint64_t _groundWeaponIdentityEpoch{ 0 };
		std::chrono::steady_clock::time_point _groundWeaponDeadline{};
		State::WeaponEquipmentIdentity _groundWeaponEquipmentIdentity{};
		bool _groundWeaponEquipmentIdentityCaptured{ false };
		State::WeaponEquipmentIdentity _lastEquipmentIdentity{};
		bool _lastEquipmentIdentityCaptured{ false };
		State::WeaponEquipmentEpochTrackerState _weaponEquipmentEpochTracker{};
		bool _weaponEquipmentIdentityChangePending{ false };
		State::WeaponEquipmentIdentity _weaponEquipmentPreviousIdentityPending{};
		std::uint64_t _weaponEquipmentIdentityPendingFromEpoch{ 0 };
		std::uint64_t _weaponEquipmentIdentityPendingToEpoch{ 0 };
		State::WeaponEquipmentSwapSettleState _weaponEquipmentSwap{};
		// Monotonic identity epoch binding every transition/fallback to the
		// equipment snapshot that armed it.  A swap invalidates the old epoch
		// before any replacement transaction can be armed.  Counter exhaustion is a
		// terminal fail-closed state; it is never wrapped and reused.
		std::uint64_t _weaponEquipmentIdentityEpoch{ 0 };
		State::WeaponEquipmentIdentity _weaponEquipmentSwapPreviousIdentity{};
		State::WeaponEquipmentIdentity _weaponEquipmentSwapCurrentIdentity{};
		bool _weaponEquipmentSwapIdentityCaptured{ false };
		bool _weaponEquipmentSwapPinnedDiagnosticLogged{ false };
		std::chrono::steady_clock::time_point _shoutGraphOverrideUntil{};
		std::chrono::steady_clock::time_point _whirlwindSprintUntil{};
		std::uint64_t _weaponTransitionSequence{ 0 };
		std::uint64_t _weaponTransitionActionId{ 0 };
		std::uint64_t _groundWeaponActionId{ 0 };
		bool _whirlwindSprintShoutPending{ false };
		bool _flightShoutHeld{ false };

		// DAF owns only this elapsed-time carry and diagnostic observations around
		// its DamageActorValue call.  The regen-delay field has no writer
		// provenance in the engine, so DAF never writes or restores it.
		float _magickaDrainCarrySeconds{ 0.0F };
		std::chrono::steady_clock::time_point _lastMagickaDrainAt{};
		std::uint64_t _magickaDrainSequence{ 0 };
		std::uint64_t _magickaRegenObservationSessionId{ 0 };
		std::uint64_t _magickaRegenObservationDrainSequence{ 0 };
		std::uint32_t _magickaRegenObservationActorFormId{ 0 };
		float _magickaRegenDelayBaseline{ 0.0F };
		float _magickaRegenDelayAfterDrain{ 0.0F };
		bool _magickaRegenDelayOwned{ false };
		bool _magickaRegenAwaitingDelayedIncrease{ false };
		bool _magickaRegenObservationStableLogged{ false };
		bool _magickaRegenObservationPending{ false };
		std::uint32_t _magickaRegenObservationSamples{ 0 };
		std::chrono::steady_clock::time_point _magickaRegenObservationStarted{};
		std::chrono::steady_clock::time_point _magickaRegenObservationNextSample{};
		State::MagickaDrainDiagnosticAggregate _magickaDrainDiagnosticAggregate{};
		std::chrono::steady_clock::time_point _magickaDrainDiagnosticStarted{};
		State::MagickaRegenDiagnosticPipeline _magickaRegenDiagnosticPipeline{};

		float _originalGravity{ 0.0F };
		bool _originalNoFriction{ false };
		RE::hkVector4 _smoothedFlightVelocity{ 0.0F, 0.0F, 0.0F, 0.0F };

		// Only the game/lifecycle callers mutate the jthread object.  Serialize
		// start/stop/rejoin so a lifecycle reset cannot race a landing cleanup or
		// replace a still-joinable worker.
		std::mutex _threadLifecycleMutex;
		std::atomic_bool _threadRunning{ false };
		// At most one game-thread UpdateFlight task may be queued for a given
		// session/generation.  Lifecycle/session edges invalidate old callbacks;
		// stale callbacks compare their captured generation before touching state.
		std::atomic_uint64_t _updateTaskGeneration{ 1 };
		std::atomic_uint64_t _queuedUpdateGeneration{ 0 };
		std::jthread _updateThread;
	};
}
