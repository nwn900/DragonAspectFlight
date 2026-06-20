#pragma once

#include <atomic>
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
		void TriggerLaunchBoost();
		void SetBoostHeld(bool a_boostHeld);

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
		void SuppressFightingControls();
		void RestoreFightingControls();
		void EnforceFightingControlsSuppressed();

		mutable std::shared_mutex _mutex;

		bool _isFlying{ false };
		bool _isDescending{ false };
		bool _fightingControlsSuppressed{ false };
		bool _restoreFightingControls{ false };
		float _flightSpeed{ 14.0F };
		float _verticalSpeed{ 24.0F };
		float _liftScale{ 1.0F };
		float _forwardInput{ 0.0F };
		float _strafeInput{ 0.0F };
		float _pendingLaunchBoost{ 0.0F };
		bool _boostHeld{ false };
		std::int32_t _lastGraphState{ 0 };

		float _originalGravity{ 0.0F };

		std::atomic_bool _threadRunning{ false };
		std::jthread _updateThread;
	};
}
