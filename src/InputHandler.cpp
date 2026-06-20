#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/InputHandler.h"

#include "RE/C/ControlMap.h"
#include "RE/M/MemoryManager.h"
#include "RE/P/PlayerControls.h"
#include "RE/S/ShoutHandler.h"

namespace
{
	constexpr const char* SprintUserEvent = "Sprint";
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
	constexpr std::uint32_t DefaultReadyWeaponKeyboardScanCode = 0x13;  // DIK_R
	constexpr float ThumbstickDeadzone = 0.25F;
	constexpr const char* FlightBuildVersion = "v0.3.2-dragon-aspect";
	constexpr auto FlightDoubleTapWindow = std::chrono::milliseconds(450);

	bool IsKeyboardSprint(const RE::ButtonEvent* a_event)
	{
		if (!a_event) return false;
		if (a_event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) return false;
		return a_event->QUserEvent() == SprintUserEvent;
	}

	bool IsLaunchAction(const RE::ButtonEvent* a_event)
	{
		return a_event && a_event->QUserEvent() == JumpUserEvent;
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
		return IsKeyboardSprint(a_event);
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

	bool ProcessFlightShout(const RE::ButtonEvent* a_event)
	{
		auto* controlMap = RE::ControlMap::GetSingleton();
		auto* controls = RE::PlayerControls::GetSingleton();

		if (!controls || !controls->shoutHandler) {
			logger::warn("Dragon Aspect Flight: shout blocked because PlayerControls/ShoutHandler is unavailable");
			return true;
		}

		const bool fightingWasEnabled = controlMap && controlMap->IsFightingControlsEnabled();
		auto* shoutEvent = RE::ButtonEvent::Create(
			a_event->GetDevice(),
			RE::BSFixedString(ShoutUserEvent),
			a_event->GetIDCode(),
			a_event->Value(),
			a_event->HeldDuration());

		if (!shoutEvent) {
			logger::warn("Dragon Aspect Flight: shout blocked because synthetic ButtonEvent allocation failed");
			return true;
		}

		if (controlMap && !fightingWasEnabled) {
			controlMap->ToggleControls(RE::ControlMap::UEFlag::kFighting, true);
		}

		controls->shoutHandler->ProcessButton(shoutEvent, std::addressof(controls->data));

		if (controlMap && !fightingWasEnabled) {
			controlMap->ToggleControls(RE::ControlMap::UEFlag::kFighting, false);
		}

		RE::free(shoutEvent);
		logger::info("Dragon Aspect Flight: routed shout through vanilla ShoutHandler while flying, device={}, id={}", static_cast<std::uint32_t>(a_event->GetDevice()), a_event->GetIDCode());
		return true;
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

		if (!dragonAspectActive) {
			if (fm.IsFlying()) {
				fm.StopFlight();
			}
			ResetFlightInputState();

			if (a_event->IsPressed()) {
				ShowMessage("Dragon Aspect Flight: Dragon Aspect required");
			}

			return false;
		}

		if (a_event->IsPressed()) {
			const auto now = std::chrono::steady_clock::now();

			if (_flightToggleTapArmed && (now - _flightToggleLastTap) <= FlightDoubleTapWindow) {
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
			} else {
				_flightToggleTapArmed = true;
				_flightToggleLastTap = now;

				if (fm.IsFlying()) {
					ShowMessage("Dragon Aspect Flight: double tap Sprint/Alt to descend");
					return true;
				} else {
					ShowMessage("Dragon Aspect Flight: double tap Sprint/Alt to toggle flight");
					return true;
				}
			}
		}

		if (a_event->IsUp()) {
			return fm.IsFlying() || _flightToggleTapArmed;
		}

		return fm.IsFlying();
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
		_flightToggleTapArmed = false;
		_leftCastHeld = false;
		_rightCastHeld = false;
		_dualCastHeld = false;
		_launchHeld = false;
		_boostHeld = false;
		FlightManager::GetSingleton().SetBoostHeld(false);
		UpdateMovementInput();
	}

	void InputHandler::UpdateMovementInput()
	{
		FlightManager::GetSingleton().SetMovementInput(
			std::clamp(_keyboardForwardInput + _thumbstickForwardInput, -1.0F, 1.0F),
			std::clamp(_keyboardStrafeInput + _thumbstickStrafeInput, -1.0F, 1.0F));
	}
}
