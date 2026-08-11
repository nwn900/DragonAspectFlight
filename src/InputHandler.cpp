#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/InputHandler.h"
#include "DragonAspectFlight/Settings.h"
#include "DragonAspectFlight/Version.h"
#include "SKSEMenuFramework.h"

#include "RE/C/ControlMap.h"
#include "RE/U/UI.h"

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
	constexpr float ThumbstickDeadzone = 0.25F;
	bool MatchesBinding(const RE::ButtonEvent* a_event, const DragonAspectFlight::InputBinding& a_binding)
	{
		if (!a_event) return false;

		const auto device = a_binding.device == DragonAspectFlight::BindingDevice::Keyboard ?
			RE::INPUT_DEVICE::kKeyboard : RE::INPUT_DEVICE::kGamepad;
		return a_event->GetDevice() == device && a_event->GetIDCode() == a_binding.code;
	}

	bool IsLaunchAction(const RE::ButtonEvent* a_event)
	{
		return a_event && a_event->QUserEvent() == JumpUserEvent;
	}

	bool IsConfiguredAscendInput(const RE::ButtonEvent* a_event)
	{
		auto& settings = DragonAspectFlight::Settings::GetSingleton();
		DragonAspectFlight::InputBinding binding;
		{
			std::shared_lock lock(settings.mutex);
			binding = settings.ascend;
		}
		return MatchesBinding(a_event, binding);
	}

	bool IsConfiguredDescendInput(const RE::ButtonEvent* a_event)
	{
		auto& settings = DragonAspectFlight::Settings::GetSingleton();
		DragonAspectFlight::InputBinding binding;
		{
			std::shared_lock lock(settings.mutex);
			binding = settings.descend;
		}
		return MatchesBinding(a_event, binding);
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

		auto* controlMap = RE::ControlMap::GetSingleton();
		if (!controlMap) {
			return false;
		}
		const auto mappedKey = controlMap->GetMappedKey(ReadyWeaponUserEvent, a_event->GetDevice());
		return mappedKey != RE::ControlMap::kInvalid && a_event->GetIDCode() == mappedKey;
	}

	bool IsCombatAction(const RE::ButtonEvent* a_event)
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
		auto& settings = DragonAspectFlight::Settings::GetSingleton();
		DragonAspectFlight::InputBinding binding;
		{
			std::shared_lock lock(settings.mutex);
			binding = settings.activation;
		}
		return MatchesBinding(a_event, binding);
	}

	RE::PlayerCharacter* GetPlayer() { return RE::PlayerCharacter::GetSingleton(); }

	bool IsPlayerLoaded()
	{
		auto* p = GetPlayer();
		return p && p->Is3DLoaded();
	}

	void ShowMessage(const char* a_msg)
	{
		RE::SendHUDMessage::ShowHUDMessage(a_msg);
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
		logger::info("Input handler registered - {}", BuildVersion);
	}

	RE::BSEventNotifyControl InputHandler::ProcessEvent(
		RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*)
	{
		if (!a_event) return RE::BSEventNotifyControl::kContinue;

		if (FlightManager::ShouldSuppressInput()) {
			ResetFlightInputState();
			return RE::BSEventNotifyControl::kContinue;
		}

		bool consumeInput = false;
		for (auto* e = *a_event; e; e = e->next) {
			if (e->eventType == RE::INPUT_EVENT_TYPE::kButton) {
				if (auto* btn = e->AsButtonEvent()) {
					consumeInput = HandleButtonEvent(btn) || consumeInput;
				}
			} else if (e->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
				HandleThumbstickEvent(static_cast<RE::ThumbstickEvent*>(e));
			}
		}

		return consumeInput ? RE::BSEventNotifyControl::kStop : RE::BSEventNotifyControl::kContinue;
	}

	bool InputHandler::ProcessFlightShout(const RE::ButtonEvent* a_event)
	{
		auto& fm = FlightManager::GetSingleton();

		if (a_event->IsUp()) {
			_shoutHeld = false;
			fm.NotifyFlightShout(true);
			logger::info("Dragon Aspect Flight: released vanilla flight shout input after {:.2f}s", a_event->HeldDuration());
			return false;
		}

		if (a_event->IsDown() || (a_event->IsPressed() && !_shoutHeld)) {
			_shoutHeld = true;
			fm.NotifyFlightShout(false);
			logger::info("Dragon Aspect Flight: passing vanilla flight shout input through");
			return false;
		}

		if (a_event->IsHeld()) {
			fm.NotifyFlightShout(false);
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

		// Bindings are deliberately evaluated before vanilla semantic actions.
		// Gamepad A/Y/bumpers/triggers can be mapped to flight without their
		// vanilla action pre-empting the configured flight response.
		if (IsFlightActivationInput(a_event)) {
			const bool consumed = HandleFlightActivation(a_event);
			fm.LogInputDiagnostic("flight_activation", a_event, consumed ? "consumed" : "passthrough");
			return consumed;
		}

		const bool isConfiguredAscendInput = IsConfiguredAscendInput(a_event);
		const bool isConfiguredDescendInput = IsConfiguredDescendInput(a_event);

		if (fm.IsFlying() && (isConfiguredAscendInput || isConfiguredDescendInput)) {
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

		if (isKb) {
			if (ue == ForwardUserEvent) { _keyboardForwardInput = pv; UpdateMovementInput(); return false; }
			if (ue == BackUserEvent) { _keyboardForwardInput = -pv; UpdateMovementInput(); return false; }
			if (ue == StrafeLeftUserEvent) { _keyboardStrafeInput = -pv; UpdateMovementInput(); return false; }
			if (ue == StrafeRightUserEvent) { _keyboardStrafeInput = pv; UpdateMovementInput(); return false; }
		}

		if (IsLaunchAction(a_event)) {
			if (!a_event->IsHeld()) {
				fm.LogInputDiagnostic("launch", a_event, fm.IsDescending() ? "blocked_descent" : "observed");
			}
			if (fm.IsDescending()) return true;
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

		if (IsShoutAction(a_event) && fm.IsFlying()) {
			fm.LogInputDiagnostic("shout", a_event, fm.IsDescending() ? "observed_descent" : "observed");
			if (!fm.IsDragonAspectActive()) {
				fm.StopFlight();
				ResetFlightInputState();
				return true;
			}

			const bool consumed = ProcessFlightShout(a_event);
			fm.LogInputDiagnostic("shout", a_event, consumed ? "handled" : "passthrough");
			return consumed;
		}

		if (IsReadyWeaponAction(a_event) && fm.IsFlying()) {
			if (a_event->IsUp()) {
				_readyWeaponHeld = false;
				fm.LogInputDiagnostic("ready_weapon", a_event, "released_passthrough");
				return false;
			}
			if ((a_event->IsDown() || a_event->IsPressed()) &&
				!_readyWeaponHeld) {
				_readyWeaponHeld = true;
				const bool synchronized = fm.ToggleFlightCombatReady();
				fm.LogInputDiagnostic(
					"ready_weapon",
					a_event,
					synchronized ? "observer_armed_passthrough" : "observer_arm_failed_passthrough");
			}
			// Vanilla and installed equipment-state/input mods keep first ownership.
			// FlightManager only invokes the relocated native fallback later if the
			// actor state does not begin moving toward the requested target.
			return false;
		}

		if (IsCombatAction(a_event)) {
			const bool blockInput = ue == LeftCastUserEvent;
			const bool bashInput = ue == RightCastUserEvent && fm.IsFlightBlockRequested();
			if (fm.IsFlying() && blockInput) {
				if (a_event->IsUp()) {
					const bool synchronized = fm.SetFlightBlockRequested(false);
					fm.LogInputDiagnostic(
						"block",
						a_event,
						synchronized ? "released_passthrough" : "release_sync_failed_passthrough");
				} else if (a_event->IsDown()) {
					const bool synchronized = fm.SetFlightBlockRequested(true);
					fm.LogInputDiagnostic(
						"block",
						a_event,
						synchronized ? "requested_passthrough" : "unsupported_passthrough");
				}
			}
			if (fm.IsFlying() && bashInput && a_event->IsDown()) {
				fm.LogInputDiagnostic("bash", a_event, "blocking_state_primed_passthrough");
			}
			if (!a_event->IsHeld()) {
				fm.LogInputDiagnostic(
					"combat",
					a_event,
					fm.IsFlying() ?
						(fm.IsDescending() ? "descent_passthrough_to_animation_graph" : "passthrough_to_animation_graph") :
						"ground_passthrough");
			}
			if (fm.IsFlying() && a_event->IsDown()) {
				if (!fm.IsDragonAspectActive()) {
					fm.StopFlight();
					ResetFlightInputState();
					return true;
				}

				if (!fm.BeginFlightCombat()) {
					// Do not swallow combat if DAF could not synchronize its graph state.
					// Vanilla, MCO, and other input handlers still need the original event.
					fm.LogInputDiagnostic("combat", a_event, "graph_sync_failed_passthrough");
					return false;
				}
			}
			// Jumping Attack owns the topology when present. Otherwise the normal
			// vanilla/MCO event continues into DAF's flight-scoped OAR fallback.
			return false;
		}

		return false;
	}

	bool InputHandler::HandleFlightActivation(const RE::ButtonEvent* a_event)
	{
		if (!IsPlayerLoaded()) return false;

		auto& fm = FlightManager::GetSingleton();

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
				bool showShoutRequired = true;
				auto& settings = Settings::GetSingleton();
				{
					std::shared_lock lock(settings.mutex);
					showShoutRequired = settings.showShoutRequiredNotification;
				}
				if (showShoutRequired) {
					ShowMessage("Dragon Aspect Flight: full Dragon Aspect required");
				}
			}

			return true;
		}

		if (fm.IsDescending()) {
			if (a_event->IsDown()) {
				ResetFlightInputState();
				fm.CancelDescent();
				UpdateMovementInput();
				ShowMessage("Dragon Aspect Flight: descent cancelled");
			}
			return true;
		}

		if (a_event->IsDown()) {
			if (fm.IsFlying()) {
				fm.BeginDescent();
				ResetFlightInputState();
				ShowMessage("Dragon Aspect Flight: descending");
			} else {
				if (auto* player = GetPlayer(); player && player->IsOnMount()) {
					ShowMessage("Dragon Aspect Flight: unavailable while mounted");
					return true;
				}
				ResetFlightInputState();
				fm.StartFlight();
				UpdateMovementInput();
				if (fm.IsFlying()) {
					ShowMessage("Dragon Aspect Flight: flight toggled on");
				}
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
		_launchHeld = false;
		_ascendHeld = false;
		_descendHeld = false;
		_readyWeaponHeld = false;
		_shoutHeld = false;
		_boostHeld = false;
		FlightManager::GetSingleton().SetBoostHeld(false);
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
