#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <shared_mutex>
#include <string_view>
#include <thread>

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
		[[nodiscard]] bool IsDragonAspectActive() const;
		[[nodiscard]] static bool ShouldSuppressInput();
		bool ToggleFlightCombatReady();
		bool BeginFlightCombat();

		void SetFlightSpeed(float a_speed);
		void SetVerticalSpeed(float a_speed);
		void SetLiftScale(float a_scale);
		void SetDetailedLogging(bool a_enabled, float a_snapshotIntervalSeconds);
		void SetMovementInput(float a_forwardInput, float a_strafeInput);
		void SetVerticalInput(float a_verticalInput);
		void TriggerLaunchBoost();
		void SetBoostHeld(bool a_boostHeld);
		void NotifyFlightShout(bool a_released);

		[[nodiscard]] float GetFlightSpeed() const;
		[[nodiscard]] float GetVerticalSpeed() const;
		[[nodiscard]] float GetLiftScale() const;
		void LogInputDiagnostic(
			std::string_view a_action,
			const RE::ButtonEvent* a_event,
			std::string_view a_outcome) const;

	private:
		FlightManager() = default;
		FlightManager(const FlightManager&) = delete;
		FlightManager(FlightManager&&) = delete;
		FlightManager& operator=(const FlightManager&) = delete;
		FlightManager& operator=(FlightManager&&) = delete;

		void StartUpdateThread();
		void StopUpdateThread();
		void QueueUpdate();
		void UpdateFlight();
		bool SetFlightCombatActive(bool a_active);
		void LogDiagnosticSnapshot(RE::PlayerCharacter* a_player);

		mutable std::shared_mutex _mutex;

		bool _isFlying{ false };
		bool _isDescending{ false };
		bool _flightCombatActive{ false };
		bool _useGeneratedCombatTopology{ false };
		bool _aerialCombatUnsupportedNotified{ false };
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
		std::uint64_t _lastDiagnosticEquipmentSignature{ ~std::uint64_t{ 0 } };
		std::uint64_t _lastDiagnosticStateSignature{ ~std::uint64_t{ 0 } };
		std::chrono::steady_clock::time_point _lastDiagnosticSnapshot{};
		std::chrono::steady_clock::time_point _shoutGraphOverrideUntil{};
		std::chrono::steady_clock::time_point _whirlwindSprintUntil{};
		bool _whirlwindSprintShoutPending{ false };

		float _originalGravity{ 0.0F };
		RE::hkVector4 _smoothedFlightVelocity{ 0.0F, 0.0F, 0.0F, 0.0F };

		std::atomic_bool _threadRunning{ false };
		std::jthread _updateThread;
	};
}
