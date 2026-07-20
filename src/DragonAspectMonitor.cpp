#include "PCH.h"

#include "DragonAspectFlight/DragonAspectMonitor.h"
#include "DragonAspectFlight/Settings.h"

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

	void NotifyOnGameThread(const char* a_message)
	{
		auto* taskInterface = SKSE::GetTaskInterface();
		if (!taskInterface) {
			logger::warn("Dragon Aspect Flight: TaskInterface unavailable; notification skipped");
			return;
		}
		taskInterface->AddTask([a_message]() {
			RE::DebugNotification(a_message);
		});
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

		_thread = std::jthread([this](std::stop_token stopToken) {
			logger::info("Dragon Aspect Flight: DA monitor thread started");

			while (!stopToken.stop_requested() && _running.load()) {
				const bool nowActive = HasDragonAspectActive();
				const bool wasActive = _wasActive.exchange(nowActive);

				if (nowActive && !wasActive) {
					// false -> true: full Dragon Aspect shout just applied
					bool showReady = true;
					{
						std::shared_lock lock(Settings::GetSingleton().mutex);
						showReady = Settings::GetSingleton().showReadyNotification;
					}
					if (showReady) {
						NotifyOnGameThread("Dragon Aspect Flight ready: press B to fly");
						logger::info("Dragon Aspect Flight: 'ready' notification queued");
					}
				} else if (!nowActive && wasActive) {
					// true -> false: shout expired
					bool showExpired = true;
					{
						std::shared_lock lock(Settings::GetSingleton().mutex);
						showExpired = Settings::GetSingleton().showExpiredNotification;
					}
					if (showExpired) {
						NotifyOnGameThread("Dragon Aspect Flight exhausted");
						logger::info("Dragon Aspect Flight: 'exhausted' notification queued");
					}
				}

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
		_running = false;
		if (_thread.joinable()) {
			_thread.request_stop();
			_thread.join();
		}
	}
}
