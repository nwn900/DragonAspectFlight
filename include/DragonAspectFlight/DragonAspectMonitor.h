#pragma once

#include <atomic>
#include <thread>

namespace DragonAspectFlight
{
	// Polls the player's Dragon Aspect magic-effect state and fires a
	// DebugNotification the moment the full shout is applied (and again when
	// it expires). Runs on its own jthread with a 250 ms tick; all RE:: calls
	// are marshalled onto the SKSE game thread via the Task interface.
	class DragonAspectMonitor
	{
	public:
		static DragonAspectMonitor& GetSingleton();

		void Start();
		void Stop();

	private:
		DragonAspectMonitor() = default;
		DragonAspectMonitor(const DragonAspectMonitor&) = delete;
		DragonAspectMonitor(DragonAspectMonitor&&) = delete;
		DragonAspectMonitor& operator=(const DragonAspectMonitor&) = delete;
		DragonAspectMonitor& operator=(DragonAspectMonitor&&) = delete;

		std::jthread _thread;
		std::atomic_bool _running{ false };
		std::atomic_bool _wasActive{ false };
	};
}
