#include "PCH.h"

#include "DragonAspectFlight/DragonAspectMonitor.h"
#include "DragonAspectFlight/Settings.h"
#include "DragonAspectFlight/UI.h"

#include "RE/M/MagicTarget.h"
#include "RE/T/TES.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/P/PlayerCharacter.h"

namespace
{
	// Dragon Aspect magic effect form IDs from Dragonborn.esm.
	// We check for active magic effects rather than HasSpell() because
	// shout-applied temporary ability spells may not register as "known" spells.
	constexpr RE::FormID DA_ArmsEffect = 0x021730;  // DLC2DragonAspectArmsEffect02 "Dragon Aspect - Arms"
	constexpr const char* DragonbornPlugin = "Dragonborn.esm";

	// More Draconic Aspect wings magic effect (form 0x804 in the ESL)
	constexpr RE::FormID DA_WingsEffect = 0x00804;
	constexpr const char* MoreDraconicPlugin = "More Draconic Aspect - Become The Dragonborn ESL.esp";

	RE::PlayerCharacter* GetPlayer()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	// Verbatim copy of FlightManager.cpp's anonymous-namespace HasDragonAspectActive.
	// Checks for the arms effect (vanilla full-form marker) OR the wings effect
	// (More Draconic's full-form marker) on the player's MagicTarget.
	bool HasDragonAspectActive(RE::PlayerCharacter* a_player)
	{
		if (!a_player || !a_player->Is3DLoaded()) return false;

		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) return false;

		auto* magicTarget = a_player->AsMagicTarget();
		if (!magicTarget) return false;

		auto* fullPowerArms = dh->LookupForm<RE::EffectSetting>(DA_ArmsEffect, DragonbornPlugin);
		if (fullPowerArms && magicTarget->HasMagicEffect(fullPowerArms)) {
			return true;
		}

		auto* wings = dh->LookupForm<RE::EffectSetting>(DA_WingsEffect, MoreDraconicPlugin);
		if (wings && magicTarget->HasMagicEffect(wings)) {
			return true;
		}

		return false;
	}

	void LogMonitorException(
		std::atomic_uint32_t& a_count,
		std::string_view a_phase,
		std::uint64_t a_generation,
		std::string_view a_error)
	{
		const auto count = a_count.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count == 1 || count % 8 == 0) {
			logger::error(
				"event=dragon_aspect_monitor_error phase={} generation={} count={} lease_released=true error={}",
				a_phase,
				a_generation,
				count,
				a_error);
		}
	}
}

namespace DragonAspectFlight
{
	struct DragonAspectMonitor::MonitorState
	{
		std::atomic_bool running{ false };
		std::atomic_bool wasActive{ false };
		std::atomic_uint64_t generation{ 1 };
		std::atomic_uint64_t queuedGeneration{ 0 };
		std::atomic_uint32_t taskUnavailablePolls{ 0 };
		std::atomic_uint32_t queueExceptionCount{ 0 };
		std::atomic_uint32_t pollExceptionCount{ 0 };
		std::atomic_uint32_t actorUnavailablePolls{ 0 };
	};

	DragonAspectMonitor& DragonAspectMonitor::GetSingleton()
	{
		static DragonAspectMonitor singleton;
		return singleton;
	}

	DragonAspectMonitor::DragonAspectMonitor() :
		_state(std::make_shared<MonitorState>())
	{}

	DragonAspectMonitor::~DragonAspectMonitor()
	{
		Stop();
	}

	std::uint64_t DragonAspectMonitor::AdvanceGeneration(
		const std::shared_ptr<MonitorState>& a_state) noexcept
	{
		if (!a_state) {
			return 0;
		}

		auto current = a_state->generation.load(std::memory_order_acquire);
		for (;;) {
			const auto next = current == std::numeric_limits<std::uint64_t>::max() ?
				std::uint64_t{ 1 } : current + 1;
			if (a_state->generation.compare_exchange_weak(
					current,
					next,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				// A lifecycle edge invalidates any callback from the previous
				// generation.  A stale callback can only clear its own token below,
				// so it cannot erase a lease reserved by a newer generation.
				a_state->queuedGeneration.store(0, std::memory_order_release);
				return next;
			}
		}
	}

	void DragonAspectMonitor::QueuePoll(const std::shared_ptr<MonitorState>& a_state)
	{
		if (!a_state || !a_state->running.load(std::memory_order_acquire)) {
			return;
		}

		const auto generation = a_state->generation.load(std::memory_order_acquire);
		std::uint64_t expectedGeneration = 0;
		if (!a_state->queuedGeneration.compare_exchange_strong(
				expectedGeneration,
				generation,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			// One game-thread task is enough for the whole 250 ms interval.  This
			// prevents a stalled game thread from accumulating duplicate polls.
			return;
		}

		const auto releaseQueueLease = [a_state, generation]() noexcept {
			std::uint64_t expected = generation;
			a_state->queuedGeneration.compare_exchange_strong(
				expected,
				std::uint64_t{ 0 },
				std::memory_order_acq_rel,
				std::memory_order_acquire);
		};

		// A lifecycle edge may have invalidated the reservation while the
		// worker was reading the task interface.  Drop it before returning.
		if (!a_state->running.load(std::memory_order_acquire) ||
			a_state->generation.load(std::memory_order_acquire) != generation) {
			releaseQueueLease();
			return;
		}

		const SKSE::TaskInterface* taskInterface = nullptr;
		try {
			taskInterface = SKSE::GetTaskInterface();
		} catch (const std::exception& e) {
			releaseQueueLease();
			LogMonitorException(a_state->queueExceptionCount, "get_task_interface", generation, e.what());
			return;
		} catch (...) {
			releaseQueueLease();
			LogMonitorException(a_state->queueExceptionCount, "get_task_interface", generation, "unknown");
			return;
		}

		if (!taskInterface) {
			releaseQueueLease();
			const auto count = a_state->taskUnavailablePolls.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count == 1 || count % 8 == 0) {
				logger::warn(
					"event=dragon_aspect_monitor_queue skipped=task_interface_unavailable generation={} "
					"polls={} lease_released=true",
					generation,
					count);
			}
			return;
		}
		a_state->taskUnavailablePolls.store(0, std::memory_order_relaxed);

		try {
			taskInterface->AddTask([a_state, generation]() {
				const auto releaseQueueLease = [a_state, generation]() noexcept {
					std::uint64_t expected = generation;
					a_state->queuedGeneration.compare_exchange_strong(
						expected,
						std::uint64_t{ 0 },
						std::memory_order_acq_rel,
						std::memory_order_acquire);
				};

				if (!a_state->running.load(std::memory_order_acquire) ||
					a_state->generation.load(std::memory_order_acquire) != generation) {
					releaseQueueLease();
					return;
				}

				try {
					DragonAspectMonitor::PollOnGameThread(a_state, generation);
				} catch (const std::exception& e) {
					releaseQueueLease();
					LogMonitorException(a_state->pollExceptionCount, "poll", generation, e.what());
					return;
				} catch (...) {
					releaseQueueLease();
					LogMonitorException(a_state->pollExceptionCount, "poll", generation, "unknown");
					return;
				}

				releaseQueueLease();
			});
		} catch (const std::exception& e) {
			releaseQueueLease();
			LogMonitorException(a_state->queueExceptionCount, "add_task", generation, e.what());
		} catch (...) {
			releaseQueueLease();
			LogMonitorException(a_state->queueExceptionCount, "add_task", generation, "unknown");
		}
	}

	void DragonAspectMonitor::PollOnGameThread(
		const std::shared_ptr<MonitorState>& a_state,
		std::uint64_t a_generation)
	{
		if (!a_state || !a_state->running.load(std::memory_order_acquire) ||
			a_state->generation.load(std::memory_order_acquire) != a_generation) {
			return;
		}

		auto* player = GetPlayer();
		if (!player || !player->Is3DLoaded()) {
			const auto count = a_state->actorUnavailablePolls.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count == 1 || count % 8 == 0) {
				logger::warn(
					"event=dragon_aspect_monitor_poll skipped=actor_unavailable generation={} polls={} "
					"transition_state_preserved=true",
					a_generation,
					count);
			}
			return;
		}

		const auto unavailablePolls = a_state->actorUnavailablePolls.exchange(0, std::memory_order_relaxed);
		if (unavailablePolls != 0) {
			logger::info(
				"event=dragon_aspect_monitor_poll actor_recovered generation={} skipped_polls={}",
				a_generation,
				unavailablePolls);
		}

		const bool nowActive = HasDragonAspectActive(player);
		// A stop/restart edge cannot normally interleave a game-thread task, but
		// this second check keeps the transition side effect safe if that changes.
		if (!a_state->running.load(std::memory_order_acquire) ||
			a_state->generation.load(std::memory_order_acquire) != a_generation) {
			return;
		}

		const bool wasActive = a_state->wasActive.exchange(nowActive, std::memory_order_acq_rel);
		if (nowActive && !wasActive) {
			bool showReady = true;
			InputBinding activation;
			{
				std::shared_lock lock(Settings::GetSingleton().mutex);
				showReady = Settings::GetSingleton().showReadyNotification;
				activation = Settings::GetSingleton().activation;
			}
			if (showReady) {
				const auto message =
					std::string("Dragon Aspect Flight ready: press ") + UI::DescribeBinding(activation) + " to fly";
				RE::SendHUDMessage::ShowHUDMessage(message.c_str());
				logger::info(
					"event=dragon_aspect_monitor_transition transition=ready generation={} notification=shown",
					a_generation);
			}
		} else if (!nowActive && wasActive) {
			bool showExpired = true;
			{
				std::shared_lock lock(Settings::GetSingleton().mutex);
				showExpired = Settings::GetSingleton().showExpiredNotification;
			}
			if (showExpired) {
				RE::SendHUDMessage::ShowHUDMessage("Dragon Aspect Flight exhausted");
				logger::info(
					"event=dragon_aspect_monitor_transition transition=exhausted generation={} notification=shown",
					a_generation);
			}
		}
	}

	void DragonAspectMonitor::Start()
	{
		std::lock_guard lifecycleLock(_lifecycleMutex);
		if (_state->running.load(std::memory_order_acquire)) {
			return;
		}

		if (_thread.joinable()) {
			_thread.request_stop();
			if (_thread.get_id() == std::this_thread::get_id()) {
				logger::error("event=dragon_aspect_monitor_start refused= self_join join_deferred=true");
				return;
			}
			try {
				_thread.join();
			} catch (const std::system_error& e) {
				logger::error(
					"event=dragon_aspect_monitor_start refused=stale_join_failed join_deferred=true error={}",
					e.what());
				return;
			}
		}

		const auto generation = AdvanceGeneration(_state);
		_state->wasActive.store(false, std::memory_order_release);
		_state->taskUnavailablePolls.store(0, std::memory_order_relaxed);
		_state->queueExceptionCount.store(0, std::memory_order_relaxed);
		_state->pollExceptionCount.store(0, std::memory_order_relaxed);
		_state->actorUnavailablePolls.store(0, std::memory_order_relaxed);
		_state->running.store(true, std::memory_order_release);

		try {
			auto state = _state;
			_thread = std::jthread([state](std::stop_token a_stopToken) {
				logger::info(
					"event=dragon_aspect_monitor_worker state=started generation={}",
					state->generation.load(std::memory_order_acquire));

				while (!a_stopToken.stop_requested() && state->running.load(std::memory_order_acquire)) {
					DragonAspectMonitor::QueuePoll(state);
					for (int i = 0; i < 25; ++i) {
						if (a_stopToken.stop_requested()) {
							break;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
				}

				logger::info(
					"event=dragon_aspect_monitor_worker state=stopped generation={}",
					state->generation.load(std::memory_order_acquire));
			});
			logger::info(
				"event=dragon_aspect_monitor lifecycle=start generation={} interval_ms=250",
				generation);
		} catch (const std::exception& e) {
			_state->running.store(false, std::memory_order_release);
			(void)AdvanceGeneration(_state);
			logger::error(
				"event=dragon_aspect_monitor lifecycle=start_failed generation={} error={}",
				generation,
				e.what());
		} catch (...) {
			_state->running.store(false, std::memory_order_release);
			(void)AdvanceGeneration(_state);
			logger::error(
				"event=dragon_aspect_monitor lifecycle=start_failed generation={} error=unknown",
				generation);
		}
	}

	void DragonAspectMonitor::Stop()
	{
		std::lock_guard lifecycleLock(_lifecycleMutex);
		const bool wasRunning = _state->running.exchange(false, std::memory_order_acq_rel);
		const bool hadThread = _thread.joinable();
		const auto generation = AdvanceGeneration(_state);

		if (hadThread) {
			_thread.request_stop();
			if (_thread.get_id() == std::this_thread::get_id()) {
				// No detach fallback: the worker must be joined while state and
				// dependent engine code are still alive by a later lifecycle edge.
				logger::error(
					"event=dragon_aspect_monitor lifecycle=stop self_join=true join_deferred=true generation={}",
					generation);
				return;
			}
			try {
				_thread.join();
			} catch (const std::system_error& e) {
				// Leave the jthread joinable so a later lifecycle edge can retry;
				// never detach a worker that holds shared lifecycle state.
				logger::error(
					"event=dragon_aspect_monitor lifecycle=stop join_deferred=true generation={} error={}",
					generation,
					e.what());
				return;
			}
		}

		if (wasRunning || hadThread) {
			logger::info(
				"event=dragon_aspect_monitor lifecycle=stopped generation={} joined=true",
				generation);
		}
	}

	void DragonAspectMonitor::ResetForLifecycle(std::string_view a_reason)
	{
		std::lock_guard lifecycleLock(_lifecycleMutex);
		const auto generation = AdvanceGeneration(_state);
		const bool running = _state->running.load(std::memory_order_acquire);
		_state->wasActive.store(false, std::memory_order_release);
		_state->taskUnavailablePolls.store(0, std::memory_order_relaxed);
		_state->queueExceptionCount.store(0, std::memory_order_relaxed);
		_state->pollExceptionCount.store(0, std::memory_order_relaxed);
		_state->actorUnavailablePolls.store(0, std::memory_order_relaxed);
		logger::info(
			"event=dragon_aspect_monitor lifecycle=reset reason={} generation={} running={} "
			"queued_invalidated=true actor_transition_reset=true counters_reset=true",
			a_reason,
			generation,
			running);
	}
}
