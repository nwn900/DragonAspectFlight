#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/InputHandler.h"

#include "RE/C/ControlMap.h"

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
	constexpr std::uint32_t FlightActivationKeyboardScanCode = 0x30;     // DIK_B
	constexpr std::uint32_t DefaultReadyWeaponKeyboardScanCode = 0x13;  // DIK_R
	constexpr std::uint32_t SpaceKeyboardScanCode = 0x39;               // DIK_SPACE
	constexpr std::uint32_t LeftShiftKeyboardScanCode = 0x2A;           // DIK_LSHIFT
	constexpr float ThumbstickDeadzone = 0.25F;
	constexpr const char* FlightBuildVersion = "v0.8.7-dragon-aspect";

	bool IsLaunchAction(const RE::ButtonEvent* a_event)
	{
		return a_event && a_event->QUserEvent() == JumpUserEvent;
	}

	bool IsKeyboardSpace(const RE::ButtonEvent* a_event)
	{
		return a_event &&
			a_event->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
			a_event->GetIDCode() == SpaceKeyboardScanCode;
	}

	bool IsKeyboardLeftShift(const RE::ButtonEvent* a_event)
	{
		return a_event &&
			a_event->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
			a_event->GetIDCode() == LeftShiftKeyboardScanCode;
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
			a_event->GetIDCode() == FlightActivationKeyboardScanCode;
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

		if (fm.IsFlying() && (IsKeyboardSpace(a_event) || IsKeyboardLeftShift(a_event) || IsLaunchAction(a_event))) {
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
				if (IsKeyboardSpace(a_event) || IsLaunchAction(a_event)) {
					_ascendHeld = false;
				}
				if (IsKeyboardLeftShift(a_event)) {
					_descendHeld = false;
				}
			} else if (a_event->IsPressed() || a_event->IsHeld()) {
				if (IsKeyboardSpace(a_event) || IsLaunchAction(a_event)) {
					_ascendHeld = true;
				}
				if (IsKeyboardLeftShift(a_event)) {
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
			if (IsFlightActivationInput(a_event) || IsLaunchAction(a_event) || IsSpellCastAction(a_event) || IsShoutAction(a_event)) {
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
				ShowMessage("Dragon Aspect Flight: Dragon Aspect required");
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
