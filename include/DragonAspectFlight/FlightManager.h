#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <shared_mutex>
#include <thread>

namespace DragonAspectFlight
{
	class FlightManager
	{
	public:
		static FlightManager& GetSingleton();

		void StartFlight();
		void BeginDescent();
		void StopFlight();
		[[nodiscard]] bool IsFlying() const;
		[[nodiscard]] bool IsDescending() const;
		[[nodiscard]] bool IsDragonAspectActive() const;

		void SetFlightSpeed(float a_speed);
		void SetVerticalSpeed(float a_speed);
		void SetLiftScale(float a_scale);
		void SetMovementInput(float a_forwardInput, float a_strafeInput);
		void SetVerticalInput(float a_verticalInput);
		void TriggerLaunchBoost();
		void SetBoostHeld(bool a_boostHeld);
		void NotifyFlightShout();
		void BeginFlightShoutInput();
		void QueueEndFlightShoutInput();
		void EndFlightShoutInput();

		[[nodiscard]] float GetFlightSpeed() const;
		[[nodiscard]] float GetVerticalSpeed() const;
		[[nodiscard]] float GetLiftScale() const;

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
		void QueueStartAfterSheathe();
		void SuppressFightingControls();
		void RestoreFightingControls();
		void EnforceFightingControlsSuppressed();

		mutable std::shared_mutex _mutex;

		bool _isFlying{ false };
		bool _isDescending{ false };
		bool _fightingControlsSuppressed{ false };
		bool _restoreFightingControls{ false };
		bool _flightShoutControlsOpen{ false };
		std::chrono::steady_clock::time_point _flightShoutControlsCloseAfter{};
		float _flightSpeed{ 14.0F };
		float _verticalSpeed{ 24.0F };
		float _liftScale{ 1.0F };
		float _forwardInput{ 0.0F };
		float _strafeInput{ 0.0F };
		float _verticalInput{ 0.0F };
		float _pendingLaunchBoost{ 0.0F };
		bool _boostHeld{ false };
		std::int32_t _lastGraphState{ 0 };
		std::int32_t _lastAnimationPulseState{ 0 };
		std::uint32_t _landingContactTicks{ 0 };
		std::chrono::steady_clock::time_point _lastAnimationPulseAt{};
		std::chrono::steady_clock::time_point _shoutGraphOverrideUntil{};

		float _originalGravity{ 0.0F };

		std::atomic_bool _startAfterSheathePending{ false };
		std::atomic_uint32_t _startAfterSheatheAttempts{ 0 };
		std::atomic_bool _threadRunning{ false };
		std::jthread _updateThread;
	};
}
