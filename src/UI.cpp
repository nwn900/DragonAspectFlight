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

	std::string BindingName(const DragonAspectFlight::InputBinding& a_binding)
	{
		const auto unknownName = [&a_binding](const char* a_device) {
			char label[32]{};
			std::snprintf(label, sizeof(label), "%s 0x%X", a_device, a_binding.code);
			return std::string(label);
		};

		if (a_binding.device == DragonAspectFlight::BindingDevice::Keyboard) {
			if (const char* name = ScanCodeToName(a_binding.code)) return name;
			return unknownName("Keyboard");
		}

		using GamepadKey = RE::BSWin32GamepadDevice::Key;
		switch (a_binding.code) {
		case GamepadKey::kUp: return "D-Pad Up";
		case GamepadKey::kDown: return "D-Pad Down";
		case GamepadKey::kLeft: return "D-Pad Left";
		case GamepadKey::kRight: return "D-Pad Right";
		case GamepadKey::kStart: return "Start / Menu";
		case GamepadKey::kBack: return "Back / View";
		case GamepadKey::kLeftThumb: return "Left Stick";
		case GamepadKey::kRightThumb: return "Right Stick";
		case GamepadKey::kLeftShoulder: return "Left Bumper";
		case GamepadKey::kRightShoulder: return "Right Bumper";
		case GamepadKey::kA: return "A / Cross";
		case GamepadKey::kB: return "B / Circle";
		case GamepadKey::kX: return "X / Square";
		case GamepadKey::kY: return "Y / Triangle";
		case GamepadKey::kLeftTrigger: return "Left Trigger";
		case GamepadKey::kRightTrigger: return "Right Trigger";
		default: return unknownName("Gamepad");
		}
	}

	// Which hotkey row is waiting for a key press. 0 = none.
	enum class BindSlot : int
	{
		None = 0,
		Activation,
		Ascend,
		Descend,
	};

	std::atomic<BindSlot> g_listening = BindSlot::None;
	std::atomic_int g_listenSkipFrames = 0;
	SKSEMenuFramework::Model::InputEvent* g_inputEvent = nullptr;
	std::mutex g_captureMutex;
	std::optional<std::pair<BindSlot, DragonAspectFlight::InputBinding>> g_capturedBinding;
	std::optional<BindSlot> g_cancelledBinding;
	std::string g_rebindStatus;

	// Raw SMF3 input capture accepts only button-down keyboard/gamepad events.
	bool __stdcall CaptureRebindInput(RE::InputEvent* a_event)
	{
		const auto listening = g_listening.load();
		if (!a_event || listening == BindSlot::None || g_listenSkipFrames.load() > 0 ||
			a_event->eventType != RE::INPUT_EVENT_TYPE::kButton) return false;

		auto* button = a_event->AsButtonEvent();
		if (!button || !button->IsDown()) return false;

		const auto device = button->GetDevice();
		if (device != RE::INPUT_DEVICE::kKeyboard && device != RE::INPUT_DEVICE::kGamepad) return false;

		std::scoped_lock lock(g_captureMutex);
		if (device == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == 0x01) {
			g_cancelledBinding = listening;
		} else {
			const auto bindingDevice = device == RE::INPUT_DEVICE::kKeyboard ?
				DragonAspectFlight::BindingDevice::Keyboard : DragonAspectFlight::BindingDevice::Gamepad;
			g_capturedBinding = std::pair{ listening, DragonAspectFlight::InputBinding{ bindingDevice, button->GetIDCode() } };
		}
		return true;
	}

	bool ConsumeCapturedBinding(BindSlot a_slot, DragonAspectFlight::InputBinding& a_outBinding, bool& a_outCancelled)
	{
		std::scoped_lock lock(g_captureMutex);
		a_outCancelled = g_cancelledBinding == a_slot;
		if (a_outCancelled) g_cancelledBinding.reset();
		if (!g_capturedBinding || g_capturedBinding->first != a_slot) return false;
		a_outBinding = g_capturedBinding->second;
		g_capturedBinding.reset();
		return true;
	}

	bool PollRebind(std::uint32_t& a_outScan, bool& a_outCancelled)
	{
		a_outCancelled = false;
		(void)a_outScan;
		return false;

	#if 0
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
	#endif
	}

	bool SameBinding(const DragonAspectFlight::InputBinding& a_lhs, const DragonAspectFlight::InputBinding& a_rhs)
	{
		return a_lhs.device == a_rhs.device && a_lhs.code == a_rhs.code;
	}

	const char* SlotName(BindSlot a_slot)
	{
		switch (a_slot) {
		case BindSlot::Activation: return "Activation";
		case BindSlot::Ascend: return "Ascend";
		case BindSlot::Descend: return "Descend";
		default: return "binding";
		}
	}

	DragonAspectFlight::InputBinding* BindingForSlot(DragonAspectFlight::Settings& a_settings, BindSlot a_slot)
	{
		switch (a_slot) {
		case BindSlot::Activation: return std::addressof(a_settings.activation);
		case BindSlot::Ascend: return std::addressof(a_settings.ascend);
		case BindSlot::Descend: return std::addressof(a_settings.descend);
		default: return nullptr;
		}
	}

	bool ApplyBinding(DragonAspectFlight::Settings& a_settings, BindSlot a_slot, const DragonAspectFlight::InputBinding& a_binding)
	{
		for (const auto other : { BindSlot::Activation, BindSlot::Ascend, BindSlot::Descend }) {
			if (other != a_slot && SameBinding(*BindingForSlot(a_settings, other), a_binding)) {
				g_rebindStatus = DragonAspectFlight::UI::DescribeBinding(a_binding) +
					" is already assigned to " + SlotName(other) + ". Choose another binding.";
				return false;
			}
		}

		*BindingForSlot(a_settings, a_slot) = a_binding;
		g_rebindStatus = std::string(SlotName(a_slot)) + " set to " + DragonAspectFlight::UI::DescribeBinding(a_binding) + ".";
		return true;
	}

	void DrawHotkeyRow(const char* a_label, DragonAspectFlight::InputBinding& a_binding, BindSlot a_slot, DragonAspectFlight::Settings& a_settings)
	{
		ImGuiMCP::AlignTextToFramePadding();
		ImGuiMCP::TextUnformatted(a_label);
		ImGuiMCP::SameLine(160.0F);

		char buttonLabel[96]{};
		const bool listening = g_listening.load() == a_slot;
		if (listening) {
			std::snprintf(buttonLabel, sizeof(buttonLabel), "Press a key or controller button... (Esc cancel)##%s", a_label);
		} else {
			std::snprintf(buttonLabel, sizeof(buttonLabel), "%s##%s", DragonAspectFlight::UI::DescribeBinding(a_binding).c_str(), a_label);
		}

		if (ImGuiMCP::Button(buttonLabel, ImGuiMCP::ImVec2(220.0F, 0.0F))) {
			g_listening = a_slot;
			g_listenSkipFrames = 2;  // Arm after the UI click has cleared.
			std::scoped_lock lock(g_captureMutex);
			g_capturedBinding.reset();
			g_cancelledBinding.reset();
		}

		if (listening) {
			if (g_listenSkipFrames > 0) {
				--g_listenSkipFrames;
				return;
			}

			DragonAspectFlight::InputBinding captured;
			bool cancelled = false;
			if (ConsumeCapturedBinding(a_slot, captured, cancelled)) {
				ApplyBinding(a_settings, a_slot, captured);
				g_listening = BindSlot::None;
			} else if (cancelled) {
				g_rebindStatus = std::string(SlotName(a_slot)) + " rebinding cancelled.";
				g_listening = BindSlot::None;
			}
		}
	}
}

namespace DragonAspectFlight::UI
{
	std::string DescribeBinding(const InputBinding& a_binding)
	{
		return BindingName(a_binding);
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::info("Dragon Aspect Flight: SKSE Menu Framework not installed; UI disabled");
			return;
		}

		SKSEMenuFramework::SetSection("Dragon Aspect Flight");
		SKSEMenuFramework::AddSectionItem("Settings", Render);
		g_inputEvent = SKSEMenuFramework::AddInputEvent(CaptureRebindInput);
		logger::info("Dragon Aspect Flight: SMF Settings page registered");
	}

	void __stdcall Render()
	{
		auto& s = Settings::GetSingleton();

		// Hold exclusive lock for the render; InputHandler copies each binding
		// under a shared lock, then releases it before game-state processing.
		std::unique_lock lock(s.mutex);

		ImGuiMCP::SeparatorText("Dragon Aspect Flight v1.3.0");

		if (ImGuiMCP::CollapsingHeader("Hotkeys", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGuiMCP::TextWrapped("Click a binding, then press a key or controller button. Esc cancels keyboard rebinding.");
			DrawHotkeyRow("Activation", s.activation, BindSlot::Activation, s);
			DrawHotkeyRow("Ascend", s.ascend, BindSlot::Ascend, s);
			DrawHotkeyRow("Descend", s.descend, BindSlot::Descend, s);
			if (!g_rebindStatus.empty()) ImGuiMCP::TextWrapped(g_rebindStatus.c_str());
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
