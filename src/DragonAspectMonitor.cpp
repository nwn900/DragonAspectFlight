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
	bool HasDragonAspectActive()
	{
		auto* player = GetPlayer();
		if (!player) return false;

		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) return false;

		auto* magicTarget = player->AsMagicTarget();
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

}

namespace DragonAspectFlight
{
	DragonAspectMonitor& DragonAspectMonitor::GetSingleton()
	{
		static DragonAspectMonitor singleton;
		return singleton;
	}

	void DragonAspectMonitor::Start()
	{
		if (_running.exchange(true)) {
			return;  // already running
		}
		_generation.fetch_add(1, std::memory_order_acq_rel);

		_thread = std::jthread([this](std::stop_token stopToken) {
			logger::info("Dragon Aspect Flight: DA monitor thread started");

			while (!stopToken.stop_requested() && _running.load()) {
				QueuePoll();

				// 250 ms tick with stop-token-friendly sleep (25 * 10 ms)
				for (int i = 0; i < 25; ++i) {
					if (stopToken.stop_requested()) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			}

			logger::info("Dragon Aspect Flight: DA monitor thread stopped");
		});
	}

	void DragonAspectMonitor::Stop()
	{
		_running.store(false, std::memory_order_release);
		_generation.fetch_add(1, std::memory_order_acq_rel);
		if (_thread.joinable()) {
			_thread.request_stop();
			_thread.join();
		}
	}

	void DragonAspectMonitor::QueuePoll()
	{
		auto* taskInterface = SKSE::GetTaskInterface();
		if (!taskInterface) {
			return;
		}

		bool expected = false;
		if (!_pollQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			return;
		}

		const auto generation = _generation.load(std::memory_order_acquire);
		try {
			taskInterface->AddTask([this, generation]() {
				try {
					if (_running.load(std::memory_order_acquire) &&
						_generation.load(std::memory_order_acquire) == generation) {
						PollOnGameThread();
					}
				} catch (const std::exception& e) {
					logger::error("Dragon Aspect Flight: monitor poll failed: {}", e.what());
				} catch (...) {
					logger::error("Dragon Aspect Flight: monitor poll failed with an unknown exception");
				}
				_pollQueued.store(false, std::memory_order_release);
			});
		} catch (const std::exception& e) {
			_pollQueued.store(false, std::memory_order_release);
			logger::error("Dragon Aspect Flight: failed to queue monitor poll: {}", e.what());
		} catch (...) {
			_pollQueued.store(false, std::memory_order_release);
			logger::error("Dragon Aspect Flight: failed to queue monitor poll with an unknown exception");
		}
	}

	void DragonAspectMonitor::PollOnGameThread()
	{
		const bool nowActive = HasDragonAspectActive();
		const bool wasActive = _wasActive.exchange(nowActive);

		if (nowActive && !wasActive) {
			bool showReady = true;
			InputBinding activation;
			{
				std::shared_lock lock(Settings::GetSingleton().mutex);
				showReady = Settings::GetSingleton().showReadyNotification;
				activation = Settings::GetSingleton().activation;
			}
			if (showReady) {
				const auto message = "Dragon Aspect Flight ready: press " + UI::DescribeBinding(activation) + " to fly";
				RE::SendHUDMessage::ShowHUDMessage(message.c_str());
				logger::info("Dragon Aspect Flight: 'ready' notification shown");
			}
		} else if (!nowActive && wasActive) {
			bool showExpired = true;
			{
				std::shared_lock lock(Settings::GetSingleton().mutex);
				showExpired = Settings::GetSingleton().showExpiredNotification;
			}
			if (showExpired) {
				RE::SendHUDMessage::ShowHUDMessage("Dragon Aspect Flight exhausted");
				logger::info("Dragon Aspect Flight: 'exhausted' notification shown");
			}
		}
	}
}
