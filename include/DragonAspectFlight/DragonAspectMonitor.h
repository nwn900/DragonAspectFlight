#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace DragonAspectFlight
{
	// Polls the player's Dragon Aspect magic-effect state and fires a HUD
	// notification when the full shout is applied (and again when it expires).
	// The worker only schedules a coalesced game-thread poll; every RE/UI/HUD
	// access is performed by that poll.
	class DragonAspectMonitor
	{
	public:
		static DragonAspectMonitor& GetSingleton();

		void Start();
		void Stop();
		void ResetForLifecycle(std::string_view a_reason = "lifecycle_reset");
		~DragonAspectMonitor();

	private:
		struct MonitorState;

		DragonAspectMonitor();
		DragonAspectMonitor(const DragonAspectMonitor&) = delete;
		DragonAspectMonitor(DragonAspectMonitor&&) = delete;
		DragonAspectMonitor& operator=(const DragonAspectMonitor&) = delete;
		DragonAspectMonitor& operator=(DragonAspectMonitor&&) = delete;

		[[nodiscard]] static std::uint64_t AdvanceGeneration(
			const std::shared_ptr<MonitorState>& a_state) noexcept;
		static void QueuePoll(const std::shared_ptr<MonitorState>& a_state);
		static void PollOnGameThread(
			const std::shared_ptr<MonitorState>& a_state,
			std::uint64_t a_generation);

		// Keep lifecycle/state members before the jthread.  Destruction is in
		// reverse declaration order, so the worker joins while state is alive.
		mutable std::mutex _lifecycleMutex;
		std::shared_ptr<MonitorState> _state;
		std::jthread _thread;
	};
}
