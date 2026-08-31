#pragma once

#include <chrono>

namespace DragonAspectFlight::detail
{
	enum class ShoutControlTransition
	{
		kNone,
		kOpen,
		kClose
	};

	struct FlightShoutControlState
	{
		bool open{ false };
		std::chrono::steady_clock::time_point closeAfter{};
	};

	[[nodiscard]] inline ShoutControlTransition BeginFlightShoutControl(
		FlightShoutControlState& a_state) noexcept
	{
		const bool wasOpen = a_state.open;
		a_state.open = true;
		// A fresh press owns the window. Cancel any delayed close queued by the
		// previous press so rapid shout sequences cannot close the new one.
		a_state.closeAfter = {};
		return wasOpen ? ShoutControlTransition::kNone : ShoutControlTransition::kOpen;
	}

	inline void QueueFlightShoutControlClose(
		FlightShoutControlState& a_state,
		std::chrono::steady_clock::time_point a_now,
		std::chrono::steady_clock::duration a_delay) noexcept
	{
		if (a_state.open) {
			a_state.closeAfter = a_now + a_delay;
		}
	}

	[[nodiscard]] inline ShoutControlTransition PollFlightShoutControl(
		FlightShoutControlState& a_state,
		std::chrono::steady_clock::time_point a_now) noexcept
	{
		if (!a_state.open || a_state.closeAfter == std::chrono::steady_clock::time_point{} ||
			a_now < a_state.closeAfter) {
			return ShoutControlTransition::kNone;
		}

		a_state.open = false;
		a_state.closeAfter = {};
		return ShoutControlTransition::kClose;
	}

	[[nodiscard]] inline ShoutControlTransition ResetFlightShoutControl(
		FlightShoutControlState& a_state) noexcept
	{
		const bool wasOpen = a_state.open;
		a_state.open = false;
		a_state.closeAfter = {};
		return wasOpen ? ShoutControlTransition::kClose : ShoutControlTransition::kNone;
	}
}
