#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/Settings.h"
#include "DragonAspectFlight/UI.h"
#include "SKSEMenuFramework.h"

#include <cstdio>

namespace
{
	// DirectInput keyboard scan codes used by InputHandler / Skyrim.
	struct ScanName
	{
		std::uint32_t code;
		const char* name;
	};

	// Keep in DIK order; names match common key labels.
	constexpr ScanName kScanNames[] = {
		{ 0x01, "Escape" },
		{ 0x02, "1" }, { 0x03, "2" }, { 0x04, "3" }, { 0x05, "4" },
		{ 0x06, "5" }, { 0x07, "6" }, { 0x08, "7" }, { 0x09, "8" },
		{ 0x0A, "9" }, { 0x0B, "0" },
		{ 0x0C, "Minus" }, { 0x0D, "Equals" }, { 0x0E, "Backspace" }, { 0x0F, "Tab" },
		{ 0x10, "Q" }, { 0x11, "W" }, { 0x12, "E" }, { 0x13, "R" },
		{ 0x14, "T" }, { 0x15, "Y" }, { 0x16, "U" }, { 0x17, "I" },
		{ 0x18, "O" }, { 0x19, "P" },
		{ 0x1A, "[" }, { 0x1B, "]" }, { 0x1C, "Enter" }, { 0x1D, "Left Ctrl" },
		{ 0x1E, "A" }, { 0x1F, "S" }, { 0x20, "D" }, { 0x21, "F" },
		{ 0x22, "G" }, { 0x23, "H" }, { 0x24, "J" }, { 0x25, "K" },
		{ 0x26, "L" }, { 0x27, ";" }, { 0x28, "'" }, { 0x29, "`" },
		{ 0x2A, "Left Shift" }, { 0x2B, "\\" },
		{ 0x2C, "Z" }, { 0x2D, "X" }, { 0x2E, "C" }, { 0x2F, "V" },
		{ 0x30, "B" }, { 0x31, "N" }, { 0x32, "M" },
		{ 0x33, "," }, { 0x34, "." }, { 0x35, "/" },
		{ 0x36, "Right Shift" }, { 0x37, "Numpad *" }, { 0x38, "Left Alt" },
		{ 0x39, "Space" }, { 0x3A, "Caps Lock" },
		{ 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" },
		{ 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" }, { 0x42, "F8" },
		{ 0x43, "F9" }, { 0x44, "F10" }, { 0x57, "F11" }, { 0x58, "F12" },
		{ 0x45, "Num Lock" }, { 0x46, "Scroll Lock" },
		{ 0x47, "Numpad 7" }, { 0x48, "Numpad 8" }, { 0x49, "Numpad 9" },
		{ 0x4A, "Numpad -" }, { 0x4B, "Numpad 4" }, { 0x4C, "Numpad 5" },
		{ 0x4D, "Numpad 6" }, { 0x4E, "Numpad +" }, { 0x4F, "Numpad 1" },
		{ 0x50, "Numpad 2" }, { 0x51, "Numpad 3" }, { 0x52, "Numpad 0" },
		{ 0x53, "Numpad ." },
		{ 0x9C, "Numpad Enter" }, { 0x9D, "Right Ctrl" },
		{ 0xB5, "Numpad /" }, { 0xB8, "Right Alt" },
		{ 0xC7, "Home" }, { 0xC8, "Up" }, { 0xC9, "Page Up" },
		{ 0xCB, "Left" }, { 0xCD, "Right" },
		{ 0xCF, "End" }, { 0xD0, "Down" }, { 0xD1, "Page Down" },
		{ 0xD2, "Insert" }, { 0xD3, "Delete" },
	};

	// ImGui key → DI scan code pairs used while listening for a rebind.
	struct ImGuiToScan
	{
		ImGuiMCP::ImGuiKey imgui;
		std::uint32_t scan;
	};

	constexpr ImGuiToScan kImGuiToScan[] = {
		{ ImGuiMCP::ImGuiKey_Escape, 0x01 },
		{ ImGuiMCP::ImGuiKey_1, 0x02 }, { ImGuiMCP::ImGuiKey_2, 0x03 },
		{ ImGuiMCP::ImGuiKey_3, 0x04 }, { ImGuiMCP::ImGuiKey_4, 0x05 },
		{ ImGuiMCP::ImGuiKey_5, 0x06 }, { ImGuiMCP::ImGuiKey_6, 0x07 },
		{ ImGuiMCP::ImGuiKey_7, 0x08 }, { ImGuiMCP::ImGuiKey_8, 0x09 },
		{ ImGuiMCP::ImGuiKey_9, 0x0A }, { ImGuiMCP::ImGuiKey_0, 0x0B },
		{ ImGuiMCP::ImGuiKey_Minus, 0x0C }, { ImGuiMCP::ImGuiKey_Equal, 0x0D },
		{ ImGuiMCP::ImGuiKey_Backspace, 0x0E }, { ImGuiMCP::ImGuiKey_Tab, 0x0F },
		{ ImGuiMCP::ImGuiKey_Q, 0x10 }, { ImGuiMCP::ImGuiKey_W, 0x11 },
		{ ImGuiMCP::ImGuiKey_E, 0x12 }, { ImGuiMCP::ImGuiKey_R, 0x13 },
		{ ImGuiMCP::ImGuiKey_T, 0x14 }, { ImGuiMCP::ImGuiKey_Y, 0x15 },
		{ ImGuiMCP::ImGuiKey_U, 0x16 }, { ImGuiMCP::ImGuiKey_I, 0x17 },
		{ ImGuiMCP::ImGuiKey_O, 0x18 }, { ImGuiMCP::ImGuiKey_P, 0x19 },
		{ ImGuiMCP::ImGuiKey_LeftBracket, 0x1A }, { ImGuiMCP::ImGuiKey_RightBracket, 0x1B },
		{ ImGuiMCP::ImGuiKey_Enter, 0x1C },
		{ ImGuiMCP::ImGuiKey_LeftCtrl, 0x1D }, { ImGuiMCP::ImGuiKey_RightCtrl, 0x9D },
		{ ImGuiMCP::ImGuiKey_A, 0x1E }, { ImGuiMCP::ImGuiKey_S, 0x1F },
		{ ImGuiMCP::ImGuiKey_D, 0x20 }, { ImGuiMCP::ImGuiKey_F, 0x21 },
		{ ImGuiMCP::ImGuiKey_G, 0x22 }, { ImGuiMCP::ImGuiKey_H, 0x23 },
		{ ImGuiMCP::ImGuiKey_J, 0x24 }, { ImGuiMCP::ImGuiKey_K, 0x25 },
		{ ImGuiMCP::ImGuiKey_L, 0x26 },
		{ ImGuiMCP::ImGuiKey_Semicolon, 0x27 }, { ImGuiMCP::ImGuiKey_Apostrophe, 0x28 },
		{ ImGuiMCP::ImGuiKey_GraveAccent, 0x29 },
		{ ImGuiMCP::ImGuiKey_LeftShift, 0x2A }, { ImGuiMCP::ImGuiKey_RightShift, 0x36 },
		{ ImGuiMCP::ImGuiKey_Backslash, 0x2B },
		{ ImGuiMCP::ImGuiKey_Z, 0x2C }, { ImGuiMCP::ImGuiKey_X, 0x2D },
		{ ImGuiMCP::ImGuiKey_C, 0x2E }, { ImGuiMCP::ImGuiKey_V, 0x2F },
		{ ImGuiMCP::ImGuiKey_B, 0x30 }, { ImGuiMCP::ImGuiKey_N, 0x31 },
		{ ImGuiMCP::ImGuiKey_M, 0x32 },
		{ ImGuiMCP::ImGuiKey_Comma, 0x33 }, { ImGuiMCP::ImGuiKey_Period, 0x34 },
		{ ImGuiMCP::ImGuiKey_Slash, 0x35 },
		{ ImGuiMCP::ImGuiKey_LeftAlt, 0x38 }, { ImGuiMCP::ImGuiKey_RightAlt, 0xB8 },
		{ ImGuiMCP::ImGuiKey_Space, 0x39 }, { ImGuiMCP::ImGuiKey_CapsLock, 0x3A },
		{ ImGuiMCP::ImGuiKey_F1, 0x3B }, { ImGuiMCP::ImGuiKey_F2, 0x3C },
		{ ImGuiMCP::ImGuiKey_F3, 0x3D }, { ImGuiMCP::ImGuiKey_F4, 0x3E },
		{ ImGuiMCP::ImGuiKey_F5, 0x3F }, { ImGuiMCP::ImGuiKey_F6, 0x40 },
		{ ImGuiMCP::ImGuiKey_F7, 0x41 }, { ImGuiMCP::ImGuiKey_F8, 0x42 },
		{ ImGuiMCP::ImGuiKey_F9, 0x43 }, { ImGuiMCP::ImGuiKey_F10, 0x44 },
		{ ImGuiMCP::ImGuiKey_F11, 0x57 }, { ImGuiMCP::ImGuiKey_F12, 0x58 },
		{ ImGuiMCP::ImGuiKey_Keypad0, 0x52 }, { ImGuiMCP::ImGuiKey_Keypad1, 0x4F },
		{ ImGuiMCP::ImGuiKey_Keypad2, 0x50 }, { ImGuiMCP::ImGuiKey_Keypad3, 0x51 },
		{ ImGuiMCP::ImGuiKey_Keypad4, 0x4B }, { ImGuiMCP::ImGuiKey_Keypad5, 0x4C },
		{ ImGuiMCP::ImGuiKey_Keypad6, 0x4D }, { ImGuiMCP::ImGuiKey_Keypad7, 0x47 },
		{ ImGuiMCP::ImGuiKey_Keypad8, 0x48 }, { ImGuiMCP::ImGuiKey_Keypad9, 0x49 },
		{ ImGuiMCP::ImGuiKey_KeypadDecimal, 0x53 }, { ImGuiMCP::ImGuiKey_KeypadDivide, 0xB5 },
		{ ImGuiMCP::ImGuiKey_KeypadMultiply, 0x37 }, { ImGuiMCP::ImGuiKey_KeypadSubtract, 0x4A },
		{ ImGuiMCP::ImGuiKey_KeypadAdd, 0x4E }, { ImGuiMCP::ImGuiKey_KeypadEnter, 0x9C },
		{ ImGuiMCP::ImGuiKey_Home, 0xC7 }, { ImGuiMCP::ImGuiKey_UpArrow, 0xC8 },
		{ ImGuiMCP::ImGuiKey_PageUp, 0xC9 }, { ImGuiMCP::ImGuiKey_LeftArrow, 0xCB },
		{ ImGuiMCP::ImGuiKey_RightArrow, 0xCD }, { ImGuiMCP::ImGuiKey_End, 0xCF },
		{ ImGuiMCP::ImGuiKey_DownArrow, 0xD0 }, { ImGuiMCP::ImGuiKey_PageDown, 0xD1 },
		{ ImGuiMCP::ImGuiKey_Insert, 0xD2 }, { ImGuiMCP::ImGuiKey_Delete, 0xD3 },
	};

	const char* ScanCodeToName(std::uint32_t a_code)
	{
		for (const auto& entry : kScanNames) {
			if (entry.code == a_code) {
				return entry.name;
			}
		}
		return nullptr;
	}

	// Which hotkey row is waiting for a key press. 0 = none.
	enum class BindSlot : int
	{
		None = 0,
		Activation,
		Ascend,
		Descend,
	};

	BindSlot g_listening = BindSlot::None;
	int g_listenSkipFrames = 0;

	// Returns true and writes DI scan code when a bindable key was pressed this frame.
	// Escape cancels (returns false, sets outCancelled).
	bool PollRebind(std::uint32_t& a_outScan, bool& a_outCancelled)
	{
		a_outCancelled = false;

		if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false)) {
			a_outCancelled = true;
			return false;
		}

		for (const auto& map : kImGuiToScan) {
			// Escape handled above as cancel — skip assigning Escape as a hotkey.
			if (map.imgui == ImGuiMCP::ImGuiKey_Escape) {
				continue;
			}
			if (ImGuiMCP::IsKeyPressed(map.imgui, false)) {
				a_outScan = map.scan;
				return true;
			}
		}
		return false;
	}

	void DrawHotkeyRow(const char* a_label, std::uint32_t& a_scan, BindSlot a_slot)
	{
		ImGuiMCP::AlignTextToFramePadding();
		ImGuiMCP::TextUnformatted(a_label);
		ImGuiMCP::SameLine(160.0F);

		char buttonLabel[96]{};
		const bool listening = g_listening == a_slot;
		if (listening) {
			std::snprintf(buttonLabel, sizeof(buttonLabel), "Press a key... (Esc cancel)##%s", a_label);
		} else if (const char* name = ScanCodeToName(a_scan)) {
			std::snprintf(buttonLabel, sizeof(buttonLabel), "%s##%s", name, a_label);
		} else {
			std::snprintf(buttonLabel, sizeof(buttonLabel), "0x%02X (unknown)##%s", a_scan, a_label);
		}

		if (ImGuiMCP::Button(buttonLabel, ImGuiMCP::ImVec2(220.0F, 0.0F))) {
			g_listening = a_slot;
			g_listenSkipFrames = 2;  // ponytail: ignore click residual
		}

		if (listening) {
			if (g_listenSkipFrames > 0) {
				--g_listenSkipFrames;
				return;
			}

			std::uint32_t captured = 0;
			bool cancelled = false;
			if (PollRebind(captured, cancelled)) {
				a_scan = captured;
				g_listening = BindSlot::None;
			} else if (cancelled) {
				g_listening = BindSlot::None;
			}
		}
	}
}

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

		ImGuiMCP::SeparatorText("Dragon Aspect Flight v1.2.0");

		if (ImGuiMCP::CollapsingHeader("Hotkeys", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGuiMCP::TextWrapped("Click a button, then press the key you want. Esc cancels.");
			DrawHotkeyRow("Activation", s.activation, BindSlot::Activation);
			DrawHotkeyRow("Ascend", s.ascend, BindSlot::Ascend);
			DrawHotkeyRow("Descend", s.descend, BindSlot::Descend);
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

		if (ImGuiMCP::CollapsingHeader("Magicka Cost", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
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
