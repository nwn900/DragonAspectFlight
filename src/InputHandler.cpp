#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/InputHandler.h"

#include "RE/C/ControlMap.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace
{
	constexpr const char* ForwardUserEvent = "Forward";
	constexpr const char* BackUserEvent = "Back";
	constexpr const char* StrafeLeftUserEvent = "Strafe Left";
	constexpr const char* StrafeRightUserEvent = "Strafe Right";
	constexpr const char* JumpUserEvent = "Jump";
	constexpr const char* ReadyWeaponUserEvent = "Ready Weapon";
	constexpr const char* ReadyWeaponCompactUserEvent = "ReadyWeapon";
	constexpr const char* DrawWeaponUserEvent = "Draw Weapon";
	constexpr const char* SheatheWeaponUserEvent = "Sheathe Weapon";
	constexpr const char* WeaponDrawUserEvent = "Weapon Draw";
	constexpr const char* WeaponSheatheUserEvent = "Weapon Sheathe";
	constexpr const char* LeftCastUserEvent = "Left Attack/Block";
	constexpr const char* RightCastUserEvent = "Right Attack/Block";
	constexpr const char* DualCastUserEvent = "Dual Attack";
	constexpr const char* ShoutUserEvent = "Shout";
	constexpr const char* KinectShoutUserEvent = "KinectShout";
	constexpr std::uint32_t DefaultFlightActivationKeyboardScanCode = 0x30;  // DIK_B
	constexpr std::uint32_t DefaultReadyWeaponKeyboardScanCode = 0x13;       // DIK_R
	constexpr std::uint32_t DefaultAscendKeyboardScanCode = 0x39;            // DIK_SPACE
	constexpr std::uint32_t DefaultDescendKeyboardScanCode = 0x2A;           // DIK_LSHIFT
	constexpr float ThumbstickDeadzone = 0.25F;
	constexpr const char* FlightBuildVersion = "v1.0.0-dragon-aspect";
	constexpr const char* HotkeyIniPath = "Data\\SKSE\\Plugins\\DragonAspectFlight.ini";

	struct HotkeyConfig
	{
		std::uint32_t activation = DefaultFlightActivationKeyboardScanCode;
		std::uint32_t ascend = DefaultAscendKeyboardScanCode;
		std::uint32_t descend = DefaultDescendKeyboardScanCode;
	};

	std::string_view Trim(std::string_view a_value)
	{
		while (!a_value.empty() && std::isspace(static_cast<unsigned char>(a_value.front())) != 0) {
			a_value.remove_prefix(1);
		}

		while (!a_value.empty() && std::isspace(static_cast<unsigned char>(a_value.back())) != 0) {
			a_value.remove_suffix(1);
		}

		return a_value;
	}

	bool EqualsIgnoreCase(std::string_view a_lhs, std::string_view a_rhs)
	{
		if (a_lhs.size() != a_rhs.size()) {
			return false;
		}

		for (std::size_t i = 0; i < a_lhs.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a_lhs[i])) !=
				std::tolower(static_cast<unsigned char>(a_rhs[i]))) {
				return false;
			}
		}

		return true;
	}

	std::optional<std::uint32_t> ParseScanCode(std::string_view a_value)
	{
		a_value = Trim(a_value);

		if (a_value.empty()) {
			return std::nullopt;
		}

		std::string value{ a_value };
		const char* begin = value.c_str();
		int base = 10;

		if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
			begin += 2;
			base = 16;
		}

		errno = 0;
		char* end = nullptr;
		const auto parsed = std::strtoul(begin, std::addressof(end), base);

		if (begin == end || errno != 0 || *end != '\0' ||
			parsed > std::numeric_limits<std::uint32_t>::max()) {
			return std::nullopt;
		}

		return static_cast<std::uint32_t>(parsed);
	}

	HotkeyConfig LoadHotkeyConfig()
	{
		HotkeyConfig config;
		std::ifstream file(HotkeyIniPath);

		if (!file) {
			logger::info(
				"Dragon Aspect Flight hotkey INI not found at {}; using defaults Activation=0x{:X}, Ascend=0x{:X}, Descend=0x{:X}",
				HotkeyIniPath,
				config.activation,
				config.ascend,
				config.descend);
			return config;
		}

		bool inHotkeysSection = false;
		std::string line;

		while (std::getline(file, line)) {
			if (const auto comment = line.find_first_of(";#"); comment != std::string::npos) {
				line.erase(comment);
			}

			auto trimmed = Trim(line);
			if (trimmed.empty()) {
				continue;
			}

			if (trimmed.front() == '[' && trimmed.back() == ']') {
				trimmed.remove_prefix(1);
				trimmed.remove_suffix(1);
				inHotkeysSection = EqualsIgnoreCase(Trim(trimmed), "Hotkeys");
				continue;
			}

			if (!inHotkeysSection) {
				continue;
			}

			const auto delimiter = trimmed.find('=');
			if (delimiter == std::string_view::npos) {
				continue;
			}

			const auto key = Trim(trimmed.substr(0, delimiter));
			const auto value = Trim(trimmed.substr(delimiter + 1));
			const auto parsed = ParseScanCode(value);

			if (!parsed) {
				logger::warn("Dragon Aspect Flight: ignored invalid hotkey value '{}={}'", key, value);
				continue;
			}

			if (EqualsIgnoreCase(key, "Activation")) {
				config.activation = *parsed;
			} else if (EqualsIgnoreCase(key, "Ascend")) {
				config.ascend = *parsed;
			} else if (EqualsIgnoreCase(key, "Descend")) {
				config.descend = *parsed;
			}
		}

		logger::info(
			"Dragon Aspect Flight hotkeys loaded from {}: Activation=0x{:X}, Ascend=0x{:X}, Descend=0x{:X}",
			HotkeyIniPath,
			config.activation,
			config.ascend,
			config.descend);

		return config;
	}

	const HotkeyConfig& GetHotkeys()
	{
		static const HotkeyConfig config = LoadHotkeyConfig();
		return config;
	}

	bool IsLaunchAction(const RE::ButtonEvent* a_event)
	{
		return a_event && a_event->QUserEvent() == JumpUserEvent;
	}

	bool IsConfiguredAscendInput(const RE::ButtonEvent* a_event)
	{
		return a_event &&
			a_event->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
			a_event->GetIDCode() == GetHotkeys().ascend;
	}

	bool IsConfiguredDescendInput(const RE::ButtonEvent* a_event)
	{
		return a_event &&
			a_event->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
			a_event->GetIDCode() == GetHotkeys().descend;
	}

	bool IsReadyWeaponAction(const RE::ButtonEvent* a_event)
	{
		if (!a_event) return false;

		const auto ue = a_event->QUserEvent();
		if (ue == ReadyWeaponUserEvent ||
			ue == ReadyWeaponCompactUserEvent ||
			ue == DrawWeaponUserEvent ||
			ue == SheatheWeaponUserEvent ||
			ue == WeaponDrawUserEvent ||
			ue == WeaponSheatheUserEvent) {
			return true;
		}

		return a_event->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
			a_event->GetIDCode() == DefaultReadyWeaponKeyboardScanCode;
	}

	bool IsSpellCastAction(const RE::ButtonEvent* a_event)
	{
		if (!a_event) return false;
		const auto ue = a_event->QUserEvent();
		return ue == LeftCastUserEvent || ue == RightCastUserEvent || ue == DualCastUserEvent;
	}

	bool IsShoutAction(const RE::ButtonEvent* a_event)
	{
		if (!a_event) return false;

		const auto ue = a_event->QUserEvent();

		if (ue == ShoutUserEvent || ue == KinectShoutUserEvent) {
			return true;
		}

		auto* controlMap = RE::ControlMap::GetSingleton();

		if (!controlMap) {
			return false;
		}

		const auto device = a_event->GetDevice();
		const auto idCode = a_event->GetIDCode();
		const auto shoutKey = controlMap->GetMappedKey(ShoutUserEvent, device);
		const auto kinectShoutKey = controlMap->GetMappedKey(KinectShoutUserEvent, device);

		return (shoutKey != RE::ControlMap::kInvalid && idCode == shoutKey) ||
			(kinectShoutKey != RE::ControlMap::kInvalid && idCode == kinectShoutKey);
	}

	bool IsFlightActivationInput(const RE::ButtonEvent* a_event)
	{
		return a_event &&
			a_event->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
			a_event->GetIDCode() == GetHotkeys().activation;
	}

	RE::PlayerCharacter* GetPlayer() { return RE::PlayerCharacter::GetSingleton(); }

	bool IsPlayerLoaded()
	{
		auto* p = GetPlayer();
		return p && p->Is3DLoaded();
	}

	void ShowMessage(const char* a_msg)
	{
		RE::DebugNotification(a_msg);
		logger::info("{}", a_msg);
	}

	void ApplyRadialThumbstickDeadzone(float a_rawX, float a_rawY, float& a_outX, float& a_outY)
	{
		const float x = std::clamp(a_rawX, -1.0F, 1.0F);
		const float y = std::clamp(a_rawY, -1.0F, 1.0F);
		const float magnitude = std::sqrt((x * x) + (y * y));

		if (magnitude < ThumbstickDeadzone) {
			a_outX = 0.0F; a_outY = 0.0F;
			return;
		}

		const float n = std::clamp((magnitude - ThumbstickDeadzone) / (1.0F - ThumbstickDeadzone), 0.0F, 1.0F);
		a_outX = (x / magnitude) * n;
		a_outY = (y / magnitude) * n;
	}
}

namespace DragonAspectFlight
{
	InputHandler* InputHandler::GetSingleton()
	{
		static InputHandler s;
		return std::addressof(s);
	}

	void InputHandler::Register()
	{
		if (_registered) {
			logger::info("Input handler was already registered");
			return;
		}

		auto* mgr = RE::BSInputDeviceManager::GetSingleton();
		if (!mgr) {
			logger::error("Failed to get BSInputDeviceManager");
			return;
		}

		mgr->AddEventSink(this);
		_registered = true;
		logger::info("Input handler registered - {}", FlightBuildVersion);
	}

	RE::BSEventNotifyControl InputHandler::ProcessEvent(
		RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*)
	{
		if (!a_event) return RE::BSEventNotifyControl::kContinue;

		for (auto* e = *a_event; e; e = e->next) {
			if (e->eventType == RE::INPUT_EVENT_TYPE::kButton) {
				if (auto* btn = e->AsButtonEvent()) {
					if (HandleButtonEvent(btn))
						return RE::BSEventNotifyControl::kStop;
				}
			} else if (e->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
				HandleThumbstickEvent(static_cast<RE::ThumbstickEvent*>(e));
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	bool InputHandler::ProcessFlightShout(const RE::ButtonEvent* a_event)
	{
		auto& fm = FlightManager::GetSingleton();

		if (a_event->IsUp()) {
			_shoutHeld = false;
			fm.NotifyFlightShout();
			fm.QueueEndFlightShoutInput();
			logger::info("Dragon Aspect Flight: released vanilla flight shout input after {:.2f}s", a_event->HeldDuration());
			return false;
		}

		if (a_event->IsDown() || (a_event->IsPressed() && !_shoutHeld)) {
			_shoutHeld = true;
			fm.BeginFlightShoutInput();
			fm.NotifyFlightShout();
			logger::info("Dragon Aspect Flight: passing vanilla flight shout input through");
			return false;
		}

		if (a_event->IsHeld()) {
			fm.NotifyFlightShout();
			return false;
		}

		return false;
	}

	bool InputHandler::HandleButtonEvent(const RE::ButtonEvent* a_event)
	{
		if (!a_event) return false;

		const auto ue = a_event->QUserEvent();
		const bool isKb = a_event->GetDevice() == RE::INPUT_DEVICE::kKeyboard;
		const float pv = a_event->IsPressed() ? 1.0F : 0.0F;
		auto& fm = FlightManager::GetSingleton();

		if (isKb) {
			if (ue == ForwardUserEvent) { _keyboardForwardInput = pv; UpdateMovementInput(); return false; }
			if (ue == BackUserEvent) { _keyboardForwardInput = -pv; UpdateMovementInput(); return false; }
			if (ue == StrafeLeftUserEvent) { _keyboardStrafeInput = -pv; UpdateMovementInput(); return false; }
			if (ue == StrafeRightUserEvent) { _keyboardStrafeInput = pv; UpdateMovementInput(); return false; }
		}

		const bool isConfiguredAscendInput = IsConfiguredAscendInput(a_event);
		const bool isConfiguredDescendInput = IsConfiguredDescendInput(a_event);
		const bool isJumpInput = IsLaunchAction(a_event);

		if (fm.IsFlying() && (isConfiguredAscendInput || isConfiguredDescendInput || isJumpInput)) {
			if (!fm.IsDragonAspectActive()) {
				fm.StopFlight();
				ResetFlightInputState();
				return true;
			}

			if (fm.IsDescending()) {
				_ascendHeld = false;
				_descendHeld = false;
				UpdateVerticalInput();
				return true;
			}

			if (a_event->IsUp()) {
				if (isConfiguredAscendInput) {
					_ascendHeld = false;
				}
				if (isConfiguredDescendInput) {
					_descendHeld = false;
				}
			} else if (a_event->IsPressed() || a_event->IsHeld()) {
				if (isConfiguredAscendInput) {
					_ascendHeld = true;
				}
				if (isConfiguredDescendInput) {
					_descendHeld = true;
				}
			}

			UpdateVerticalInput();
			return true;
		}

		if (IsShoutAction(a_event) && fm.IsFlying()) {
			if (!fm.IsDragonAspectActive()) {
				fm.StopFlight();
				ResetFlightInputState();
				return true;
			}

			if (fm.IsDescending()) {
				return true;
			}

			return ProcessFlightShout(a_event);
		}

		if (IsReadyWeaponAction(a_event) && fm.IsFlying()) {
			if ((a_event->IsPressed() || a_event->IsHeld()) && !fm.IsDescending()) {
				fm.BeginDescent();
				ResetFlightInputState();
				ShowMessage("Dragon Aspect Flight: descending");
			}
			return true;
		}

		if (fm.IsDescending()) {
			if (IsFlightActivationInput(a_event)) {
				if (!fm.IsDragonAspectActive()) {
					fm.StopFlight();
					ResetFlightInputState();
					return true;
				}

				if (a_event->IsDown()) {
					ResetFlightInputState();
					fm.CancelDescent();
					UpdateMovementInput();
					ShowMessage("Dragon Aspect Flight: descent cancelled");
				}
				return true;
			}

			if (IsLaunchAction(a_event) || IsSpellCastAction(a_event) || IsShoutAction(a_event)) {
				return true;
			}
		}

		if (IsSpellCastAction(a_event)) {
			bool* ch = nullptr;

			if (ue == LeftCastUserEvent) { ch = &_leftCastHeld; }
			else if (ue == RightCastUserEvent) { ch = &_rightCastHeld; }
			else if (ue == DualCastUserEvent) { ch = &_dualCastHeld; }

			if (ch && a_event->IsUp()) {
				*ch = false;
				return fm.IsFlying();
			}

			if (ch && fm.IsFlying() && (a_event->IsPressed() || a_event->IsHeld())) {
				if (!fm.IsDragonAspectActive()) {
					fm.StopFlight();
					ResetFlightInputState();
					return true;
				}

				*ch = true;
				return true;
			}
			return false;
		}

		if (IsLaunchAction(a_event)) {
			if (a_event->IsUp()) { _launchHeld = false; return fm.IsFlying() && fm.IsDragonAspectActive(); }
			if ((a_event->IsPressed() || a_event->IsHeld()) && fm.IsFlying()) {
				if (!fm.IsDragonAspectActive()) {
					fm.StopFlight();
					ResetFlightInputState();
					return false;
				}

				if (!_launchHeld) { _launchHeld = true; fm.TriggerLaunchBoost(); }
				return true;
			}
			return false;
		}

		if (!IsFlightActivationInput(a_event)) return false;
		if (!IsPlayerLoaded()) return false;

		const bool dragonAspectActive = fm.IsDragonAspectActive();

		if (!a_event->IsDown() && !a_event->IsUp()) {
			return true;
		}

		if (!dragonAspectActive) {
			if (fm.IsFlying()) {
				fm.StopFlight();
			}
			ResetFlightInputState();

			if (a_event->IsDown()) {
				ShowMessage("Dragon Aspect Flight: full Dragon Aspect required");
			}

			return true;
		}

		if (a_event->IsDown()) {
			if (fm.IsFlying()) {
				fm.BeginDescent();
				ResetFlightInputState();
				ShowMessage("Dragon Aspect Flight: descending");
			} else {
				ResetFlightInputState();
				fm.StartFlight();
				UpdateMovementInput();
				ShowMessage("Dragon Aspect Flight: flight toggled on");
			}
			return true;
		}

		if (a_event->IsUp()) {
			return true;
		}

		return true;
	}

	void InputHandler::HandleThumbstickEvent(const RE::ThumbstickEvent* a_event)
	{
		if (!a_event || !a_event->IsLeft()) return;
		ApplyRadialThumbstickDeadzone(a_event->xValue, a_event->yValue, _thumbstickStrafeInput, _thumbstickForwardInput);
		UpdateMovementInput();
	}

	void InputHandler::ResetFlightInputState()
	{
		_keyboardForwardInput = 0.0F;
		_keyboardStrafeInput = 0.0F;
		_thumbstickForwardInput = 0.0F;
		_thumbstickStrafeInput = 0.0F;
		_leftCastHeld = false;
		_rightCastHeld = false;
		_dualCastHeld = false;
		_launchHeld = false;
		_ascendHeld = false;
		_descendHeld = false;
		_shoutHeld = false;
		_boostHeld = false;
		FlightManager::GetSingleton().SetBoostHeld(false);
		FlightManager::GetSingleton().EndFlightShoutInput();
		UpdateVerticalInput();
		UpdateMovementInput();
	}

	void InputHandler::UpdateMovementInput()
	{
		FlightManager::GetSingleton().SetMovementInput(
			std::clamp(_keyboardForwardInput + _thumbstickForwardInput, -1.0F, 1.0F),
			std::clamp(_keyboardStrafeInput + _thumbstickStrafeInput, -1.0F, 1.0F));
	}

	void InputHandler::UpdateVerticalInput()
	{
		const float verticalInput =
			(_ascendHeld ? 1.0F : 0.0F) +
			(_descendHeld ? -1.0F : 0.0F);

		FlightManager::GetSingleton().SetVerticalInput(std::clamp(verticalInput, -1.0F, 1.0F));
	}
}
