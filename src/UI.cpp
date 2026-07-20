#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/Settings.h"
#include "DragonAspectFlight/UI.h"
#include "SKSEMenuFramework.h"

namespace DragonAspectFlight::UI
{
	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::info("Dragon Aspect Flight: SKSE Menu Framework not installed; UI disabled");
			return;
		}

		SKSEMenuFramework::SetSection("Dragon Aspect Flight");
		SKSEMenuFramework::AddSectionItem("Settings", Render);
		logger::info("Dragon Aspect Flight: SMF Settings page registered");
	}

	void __stdcall Render()
	{
		auto& s = Settings::GetSingleton();

		// Hold exclusive lock for the duration of the render so InputHandler
		// (game thread) never reads a half-edited value. Both threads are
		// effectively the game thread during a menu open, so contention is nil.
		std::unique_lock lock(s.mutex);

		ImGuiMCP::SeparatorText("Dragon Aspect Flight v1.1.0");

		if (ImGuiMCP::CollapsingHeader("Hotkeys", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGuiMCP::InputScalar("Activation##hk", ImGuiMCP::ImGuiDataType_U32, &s.activation, nullptr, nullptr, "%08X");
			ImGuiMCP::InputScalar("Ascend##hk", ImGuiMCP::ImGuiDataType_U32, &s.ascend, nullptr, nullptr, "%08X");
			ImGuiMCP::InputScalar("Descend##hk", ImGuiMCP::ImGuiDataType_U32, &s.descend, nullptr, nullptr, "%08X");
			ImGuiMCP::TextWrapped("DirectInput scan codes (hex). B=0x30, Space=0x39, LShift=0x2A.");
		}

		if (ImGuiMCP::CollapsingHeader("Flight Physics", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGuiMCP::SliderFloat("Flight Speed", &s.flightSpeed, 1.0F, 50.0F, "%.1f")) {
				FlightManager::GetSingleton().SetFlightSpeed(s.flightSpeed);
			}
			if (ImGuiMCP::SliderFloat("Vertical Speed", &s.verticalSpeed, 1.0F, 50.0F, "%.1f")) {
				FlightManager::GetSingleton().SetVerticalSpeed(s.verticalSpeed);
			}
			if (ImGuiMCP::SliderFloat("Lift Scale", &s.liftScale, 0.25F, 2.50F, "%.2f")) {
				FlightManager::GetSingleton().SetLiftScale(s.liftScale);
			}
		}

		if (ImGuiMCP::CollapsingHeader("Notifications", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGuiMCP::Checkbox("Show 'Flight Ready' on Dragon Aspect Shout", &s.showReadyNotification);
			ImGuiMCP::Checkbox("Show 'Flight Expired' when shout ends", &s.showExpiredNotification);
			ImGuiMCP::Checkbox("Suppress flight hotkeys while typing in menus", &s.suppressInMenus);
		}

		if (ImGuiMCP::CollapsingHeader("Magicka Cost")) {
			ImGuiMCP::Checkbox("Drain magicka while flying", &s.magickaCostEnabled);
			if (ImGuiMCP::SliderFloat("Magicka per second", &s.magickaCostPerSecond, 0.0F, 100.0F, "%.1f")) {
				if (s.magickaCostPerSecond < 0.0F) s.magickaCostPerSecond = 0.0F;
			}
			ImGuiMCP::TextWrapped("When magicka runs out, flight descends safely to the ground.");
		}

		ImGuiMCP::Separator();

		if (ImGuiMCP::Button("Save to INI")) {
			s.Save();
			ImGuiMCP::OpenPopup("SavedPopup##daf");
		}

		if (ImGuiMCP::BeginPopupModal("SavedPopup##daf", nullptr, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGuiMCP::Text("Settings saved to Data\\SKSE\\Plugins\\DragonAspectFlight.ini");
			if (ImGuiMCP::Button("OK")) {
				ImGuiMCP::CloseCurrentPopup();
			}
			ImGuiMCP::EndPopup();
		}
	}
}
