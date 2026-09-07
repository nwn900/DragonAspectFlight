#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string_view>

#include "DragonAspectFlight/FlightStateHelpers.h"
#include "DragonAspectFlight/Settings.h"

namespace DragonAspectFlight
{
	class InputHandler final : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static InputHandler* GetSingleton();

		RE::BSEventNotifyControl ProcessEvent(
			RE::InputEvent* const* a_event,
			RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

		void Register();
		void ResetFlightInputState(std::string_view a_reason = "external", bool a_clearShout = true);
		void ResetForLifecycle(
			std::uint64_t a_session,
			std::string_view a_reason = "lifecycle_reset");
		// Called only from an SKSE game-thread task.  It refreshes all RE/UI/
		// ControlMap/FlightManager state consumed by the input sink.  A non-zero
		// generation identifies the queued refresh lease; direct game-thread
		// callers leave it at zero and do not own a queue lease.
		void RefreshGameThreadState(std::uint64_t a_refreshGeneration = 0);
		void QueueGameThreadStateRefresh();
		// Called by a game-thread manager task when applying a release action fails.
		// The input-owned pending release is retried from the next serialized input
		// dispatch, never from this callback, so no engine work occurs here.
		void NotifyFlightActionQueueFailure(
			const State::FlightInputActionSnapshot& a_action,
			std::string_view a_reason) noexcept;

	public:
		struct RuntimeState
		{
			bool valid{ false };
			InputBinding activation{ BindingDevice::Keyboard, 0x30 };
			InputBinding ascend{ BindingDevice::Keyboard, 0x39 };
			InputBinding descend{ BindingDevice::Keyboard, 0x2A };
			std::uint32_t readyWeaponKeyboard{ 0xFFFFFFFFU };
			std::uint32_t readyWeaponGamepad{ 0xFFFFFFFFU };
			std::uint32_t leftAttackBlockKeyboard{ 0xFFFFFFFFU };
			std::uint32_t leftAttackBlockGamepad{ 0xFFFFFFFFU };
			std::uint32_t rightAttackBlockKeyboard{ 0xFFFFFFFFU };
			std::uint32_t rightAttackBlockGamepad{ 0xFFFFFFFFU };
			std::uint32_t dualAttackKeyboard{ 0xFFFFFFFFU };
			std::uint32_t dualAttackGamepad{ 0xFFFFFFFFU };
			std::uint32_t shoutKeyboard{ 0xFFFFFFFFU };
			std::uint32_t shoutGamepad{ 0xFFFFFFFFU };
			std::uint32_t kinectShoutKeyboard{ 0xFFFFFFFFU };
			std::uint32_t kinectShoutGamepad{ 0xFFFFFFFFU };
			bool playerLoaded{ false };
			bool playerMounted{ false };
			std::uint32_t playerFormId{ 0 };
			std::int32_t playerWeaponState{ -1 };
			bool playerWeaponsDrawn{ false };
			State::WeaponEquipmentIdentity playerEquipmentIdentity{};
			bool playerEquipmentIdentityCaptured{ false };
			std::uint64_t playerEquipmentIdentityEpoch{ 0 };
			std::uint64_t publishedFlightSession{ 0 };
			bool dragonAspectActive{ false };
			bool flying{ false };
			bool descending{ false };
			bool blockRequested{ false };
			bool suppressInput{ false };
			bool showShoutRequiredNotification{ true };
		};

	private:
		InputHandler() = default;
		InputHandler(const InputHandler&) = delete;
		InputHandler(InputHandler&&) = delete;

		InputHandler& operator=(const InputHandler&) = delete;
		InputHandler& operator=(InputHandler&&) = delete;

		bool HandleButtonEvent(const State::ButtonInputSnapshot& a_event, const RuntimeState& a_runtime);
		bool HandleFlightActivation(const State::ButtonInputSnapshot& a_event, const RuntimeState& a_runtime);
		void HandleThumbstickEvent(const State::ThumbstickInputSnapshot& a_event);
		bool ProcessFlightShout(const State::ButtonInputSnapshot& a_event);
		bool QueueFlightAction(State::FlightInputAction a_action, bool a_forceStateEmission = false);
		bool QueueFlightAction(
			State::FlightInputActionSnapshot a_action,
			bool a_forceStateEmission = false);
		bool RecoverFlightActionQueueFailure(
			const State::FlightInputActionSnapshot& a_action,
			std::uint64_t a_sequenceDomain,
			std::string_view a_reason) noexcept;
		void ArmPendingRelease(
			const State::FlightInputActionSnapshot& a_action,
			std::string_view a_reason) noexcept;
		void ClearPendingReleaseForNewInput(State::FlightInputAction a_action) noexcept;
		void RetryPendingReleasesLocked() noexcept;
		bool QueueInputDiagnostic(
			std::string_view a_action,
			const State::ButtonInputSnapshot& a_event,
			std::string_view a_outcome,
			std::uint64_t a_actionId = 0);
		[[nodiscard]] std::uint64_t AllocateInputCorrelationId() noexcept;
		void FlushInputActionCoalescing(std::string_view a_reason, bool a_resetBaseline);
		void FlushDeferredButtonEventsLocked(
			std::string_view a_reason,
			bool a_preserveBoundaryRebind);
		void LogInputActionSuppressionSummary(
			const State::InputActionSuppressionSummary& a_summary,
			std::string_view a_reason) const;
		[[nodiscard]] RuntimeState GetRuntimeState() const;
		void UpdateMovementInput(bool a_forceEmission = false);
		void UpdateVerticalInput(bool a_forceEmission = false);
		void DeferButtonEvent(
			const State::ButtonInputSnapshot& a_event,
			std::uint64_t a_sourceSession,
			std::uint64_t a_actionId,
			bool a_rebindSession = false,
			bool a_expectedFlying = false);
		void ReplayDeferredButtonEvents(
			const RuntimeState& a_runtime,
			std::uint64_t a_refreshGeneration);
		[[nodiscard]] static bool IsSemanticButtonEdge(const State::ButtonInputSnapshot& a_event);
		[[nodiscard]] static bool IsReadyButtonEdge(const State::ButtonInputSnapshot& a_event);

		struct DeferredButtonEvent
		{
			State::ButtonInputSnapshot event{};
			std::uint64_t sourceSession{ 0 };
			std::uint64_t actionId{ 0 };
			bool rebindSession{ false };
			bool expectedFlying{ false };
			std::uint8_t boundaryDeferralAttempts{ 0 };
		};

		struct PendingRelease
		{
			State::FlightInputActionSnapshot action{};
			bool pending{ false };
			std::uint32_t failureCount{ 0 };
		};

		static constexpr std::size_t DeferredButtonCapacity = 8;
		static constexpr std::size_t PendingReleaseChannelCount = 2;

		float _keyboardForwardInput{ 0.0F };
		float _keyboardStrafeInput{ 0.0F };
		float _thumbstickForwardInput{ 0.0F };
		float _thumbstickStrafeInput{ 0.0F };

		bool _launchHeld{ false };
		bool _ascendHeld{ false };
		bool _descendHeld{ false };
		bool _readyWeaponHeld{ false };
		bool _shoutHeld{ false };
		bool _shoutHeldDiagnosticLogged{ false };
		bool _blockHeld{ false };
		bool _boostHeld{ false };
		bool _registered{ false };
		mutable std::recursive_mutex _inputMutex;
		mutable std::mutex _inputActionCoalescingMutex;
		std::array<State::InputActionCoalescingState, State::InputActionCoalescingChannelCount>
			_inputActionCoalescingStates{};
		std::uint64_t _inputActionSequenceDomain{ 0 };
		mutable std::shared_mutex _runtimeMutex;
		RuntimeState _runtimeState{};
		RuntimeState _lastLoggedRuntimeState{};
		bool _runtimeStateLogInitialized{ false };
		std::chrono::steady_clock::time_point _lastRuntimeStateLog{};
		// Non-zero value is the generation owning the one queued refresh task.  A
		// stale callback can only release its own token, so a lifecycle reset or a
		// later session cannot have its refresh lease cleared by old work.
		std::atomic_uint64_t _runtimeRefreshQueued{ 0 };
		std::atomic_uint64_t _runtimeRefreshQueueGeneration{ 0 };
		std::atomic_uint32_t _runtimeRefreshExceptionCount{ 0 };
		std::atomic_uint32_t _flightActionQueueExceptionCount{ 0 };
		std::atomic_uint32_t _flightActionTaskExceptionCount{ 0 };
		std::atomic_uint64_t _suppressedInputCount{ 0 };
		// Published by RefreshGameThreadState on the game thread.  Input callbacks
		// use this plugin-owned POD/atomic snapshot rather than touching the
		// FlightManager singleton while the engine is dispatching an event.
		std::atomic_uint64_t _publishedFlightSession{ 0 };
		std::atomic_uint64_t _nextDiagnosticActionId{ 0 };
		// Scoped by ProcessEvent / deferred replay while one physical input edge is
		// being interpreted.  QueueFlightAction and QueueInputDiagnostic must reuse
		// this value rather than independently minting diagnostic ids.
		std::uint64_t _activeInputCorrelationId{ 0 };
		std::uint64_t _activeInputSequenceDomain{ 0 };
		// One pending release per locally-owned latch (block and shout).  A release
		// is retained only for its source session and is retried at the next input
		// boundary; lifecycle reset clears these entries before publishing a new
		// session.
		std::array<PendingRelease, PendingReleaseChannelCount> _pendingReleases{};
		std::array<DeferredButtonEvent, DeferredButtonCapacity> _deferredButtonEvents{};
		std::uint32_t _deferredButtonEventCount{ 0 };
		bool _dispatchBoundaryActive{ false };
		State::RuntimeDispatchBoundaryState _dispatchBoundaryState{};
	};
}
