#include "DragonAspectFlight/FlightShoutControlState.h"

#include <chrono>
#include <iostream>

namespace
{
	using namespace std::chrono_literals;
	using DragonAspectFlight::detail::BeginFlightShoutControl;
	using DragonAspectFlight::detail::FlightShoutControlState;
	using DragonAspectFlight::detail::PollFlightShoutControl;
	using DragonAspectFlight::detail::QueueFlightShoutControlClose;
	using DragonAspectFlight::detail::ResetFlightShoutControl;
	using DragonAspectFlight::detail::ShoutControlTransition;

	bool Require(bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << a_message << '\n';
		}
		return a_condition;
	}
}

int main()
{
	FlightShoutControlState state{};
	const auto origin = std::chrono::steady_clock::time_point{ 1s };

	if (!Require(BeginFlightShoutControl(state) == ShoutControlTransition::kOpen,
			"first press must open the control window")) {
		return 1;
	}

	QueueFlightShoutControlClose(state, origin, 150ms);
	if (!Require(state.closeAfter == origin + 150ms, "release must queue the close deadline")) {
		return 2;
	}

	if (!Require(BeginFlightShoutControl(state) == ShoutControlTransition::kNone,
			"second press must reuse the already-open window")) {
		return 3;
	}
	if (!Require(state.closeAfter == std::chrono::steady_clock::time_point{},
			"second press must cancel the previous release deadline")) {
		return 4;
	}
	if (!Require(PollFlightShoutControl(state, origin + 151ms) == ShoutControlTransition::kNone && state.open,
			"stale deadline must not close a newer shout press")) {
		return 5;
	}

	QueueFlightShoutControlClose(state, origin + 200ms, 150ms);
	if (!Require(PollFlightShoutControl(state, origin + 349ms) == ShoutControlTransition::kNone && state.open,
			"current deadline must not close early")) {
		return 6;
	}
	if (!Require(PollFlightShoutControl(state, origin + 350ms) == ShoutControlTransition::kClose && !state.open,
			"current deadline must close exactly once")) {
		return 7;
	}
	if (!Require(PollFlightShoutControl(state, origin + 500ms) == ShoutControlTransition::kNone,
			"closed window must remain idempotent")) {
		return 8;
	}

	(void)BeginFlightShoutControl(state);
	QueueFlightShoutControlClose(state, origin, 150ms);
	if (!Require(ResetFlightShoutControl(state) == ShoutControlTransition::kClose && !state.open &&
			state.closeAfter == std::chrono::steady_clock::time_point{},
			"flight stop must clear the open window and pending deadline")) {
		return 9;
	}

	return 0;
}
