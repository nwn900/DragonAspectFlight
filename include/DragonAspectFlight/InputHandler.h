#pragma once

namespace DragonAspectFlight
{
	class InputHandler final : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static InputHandler* GetSingleton();

		RE::BSEventNotifyControl ProcessEvent(
			RE::InputEvent* const* a_event,
			RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

		void Register();

	private:
		InputHandler() = default;
		InputHandler(const InputHandler&) = delete;
		InputHandler(InputHandler&&) = delete;

		InputHandler& operator=(const InputHandler&) = delete;
		InputHandler& operator=(InputHandler&&) = delete;

		bool HandleButtonEvent(const RE::ButtonEvent* a_event);
		void HandleThumbstickEvent(const RE::ThumbstickEvent* a_event);
		bool ProcessFlightShout(const RE::ButtonEvent* a_event);
		void ResetFlightInputState();
		void UpdateMovementInput();
		void UpdateVerticalInput();

		float _keyboardForwardInput{ 0.0F };
		float _keyboardStrafeInput{ 0.0F };
		float _thumbstickForwardInput{ 0.0F };
		float _thumbstickStrafeInput{ 0.0F };

		bool _leftCastHeld{ false };
		bool _rightCastHeld{ false };
		bool _dualCastHeld{ false };
		bool _launchHeld{ false };
		bool _ascendHeld{ false };
		bool _descendHeld{ false };
		bool _shoutHeld{ false };
		bool _boostHeld{ false };
		bool _registered{ false };
	};
}
