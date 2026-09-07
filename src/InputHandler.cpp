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
#include <optional>

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
	bool MatchesBinding(
		const DragonAspectFlight::State::ButtonInputSnapshot& a_event,
		const DragonAspectFlight::InputBinding& a_binding)
	{
		const auto device = a_binding.device == DragonAspectFlight::BindingDevice::Keyboard ?
			RE::INPUT_DEVICE::kKeyboard : RE::INPUT_DEVICE::kGamepad;
		return a_event.device == static_cast<std::uint32_t>(device) && a_event.code == a_binding.code;
	}

	bool IsLaunchAction(const DragonAspectFlight::State::ButtonInputSnapshot& a_event)
	{
		return a_event.userEvent == JumpUserEvent;
	}

	bool IsReadyWeaponAction(
		const DragonAspectFlight::State::ButtonInputSnapshot& a_event,
		const DragonAspectFlight::InputHandler::RuntimeState& a_runtime)
	{
		const auto& ue = a_event.userEvent;
		if (ue == ReadyWeaponUserEvent ||
			ue == ReadyWeaponCompactUserEvent ||
			ue == DrawWeaponUserEvent ||
			ue == SheatheWeaponUserEvent ||
			ue == WeaponDrawUserEvent ||
			ue == WeaponSheatheUserEvent) {
			return true;
		}

		const auto mappedKey = a_event.device == static_cast<std::uint32_t>(RE::INPUT_DEVICE::kKeyboard) ?
			a_runtime.readyWeaponKeyboard : a_runtime.readyWeaponGamepad;
		return mappedKey != 0xFFFFFFFFU && a_event.code == mappedKey;
	}

	std::optional<DragonAspectFlight::State::FlightCombatInputChannel> ResolveCombatInputChannel(
		const DragonAspectFlight::State::ButtonInputSnapshot& a_event,
		const DragonAspectFlight::InputHandler::RuntimeState& a_runtime)
	{
		const auto& ue = a_event.userEvent;
		if (ue == LeftCastUserEvent) {
			return DragonAspectFlight::State::FlightCombatInputChannel::kLeftAttackBlock;
		}
		if (ue == RightCastUserEvent) {
			return DragonAspectFlight::State::FlightCombatInputChannel::kRightAttackBlock;
		}
		if (ue == DualCastUserEvent) {
			return DragonAspectFlight::State::FlightCombatInputChannel::kDualAttack;
		}

		const bool keyboard = a_event.device == static_cast<std::uint32_t>(RE::INPUT_DEVICE::kKeyboard);
		const auto matches = [code = a_event.code](std::uint32_t a_mappedCode) {
			return a_mappedCode != 0xFFFFFFFFU && code == a_mappedCode;
		};
		if (matches(keyboard ? a_runtime.leftAttackBlockKeyboard : a_runtime.leftAttackBlockGamepad)) {
			return DragonAspectFlight::State::FlightCombatInputChannel::kLeftAttackBlock;
		}
		if (matches(keyboard ? a_runtime.rightAttackBlockKeyboard : a_runtime.rightAttackBlockGamepad)) {
			return DragonAspectFlight::State::FlightCombatInputChannel::kRightAttackBlock;
		}
		if (matches(keyboard ? a_runtime.dualAttackKeyboard : a_runtime.dualAttackGamepad)) {
			return DragonAspectFlight::State::FlightCombatInputChannel::kDualAttack;
		}
		return std::nullopt;
	}

	bool IsShoutAction(
		const DragonAspectFlight::State::ButtonInputSnapshot& a_event,
		const DragonAspectFlight::InputHandler::RuntimeState& a_runtime)
	{
		const auto& ue = a_event.userEvent;

		if (ue == ShoutUserEvent || ue == KinectShoutUserEvent) {
			return true;
		}

		const auto device = a_event.device;
		const auto idCode = a_event.code;
		const auto shoutKey = device == static_cast<std::uint32_t>(RE::INPUT_DEVICE::kKeyboard) ?
			a_runtime.shoutKeyboard : a_runtime.shoutGamepad;
		const auto kinectShoutKey = device == static_cast<std::uint32_t>(RE::INPUT_DEVICE::kKeyboard) ?
			a_runtime.kinectShoutKeyboard : a_runtime.kinectShoutGamepad;

		return (shoutKey != 0xFFFFFFFFU && idCode == shoutKey) ||
			(kinectShoutKey != 0xFFFFFFFFU && idCode == kinectShoutKey);
	}

	bool IsFlightActivationInput(
		const DragonAspectFlight::State::ButtonInputSnapshot& a_event,
		const DragonAspectFlight::InputBinding& a_binding)
	{
		return MatchesBinding(a_event, a_binding);
	}

	// These are the only statements that dereference RE input events.  Every
	// downstream helper receives the resulting value snapshot instead.
	DragonAspectFlight::State::ButtonInputSnapshot CopyButtonInput(const RE::ButtonEvent& a_event)
	{
		return DragonAspectFlight::State::ButtonInputSnapshot{
			std::string(a_event.QUserEvent().c_str()),
			static_cast<std::uint32_t>(a_event.GetDevice()),
			a_event.GetIDCode(),
			a_event.IsUp(),
			a_event.IsDown(),
			a_event.IsPressed(),
			a_event.IsHeld(),
			a_event.HeldDuration() };
	}

	DragonAspectFlight::State::ThumbstickInputSnapshot CopyThumbstickInput(const RE::ThumbstickEvent& a_event)
	{
		return DragonAspectFlight::State::ThumbstickInputSnapshot{
			a_event.IsLeft(),
			a_event.xValue,
			a_event.yValue };
	}

	bool SameRuntimeState(
		const DragonAspectFlight::InputHandler::RuntimeState& a_left,
		const DragonAspectFlight::InputHandler::RuntimeState& a_right)
	{
		return a_left.valid == a_right.valid &&
			a_left.activation.device == a_right.activation.device &&
			a_left.activation.code == a_right.activation.code &&
			a_left.ascend.device == a_right.ascend.device &&
			a_left.ascend.code == a_right.ascend.code &&
			a_left.descend.device == a_right.descend.device &&
			a_left.descend.code == a_right.descend.code &&
			a_left.readyWeaponKeyboard == a_right.readyWeaponKeyboard &&
			a_left.readyWeaponGamepad == a_right.readyWeaponGamepad &&
			a_left.leftAttackBlockKeyboard == a_right.leftAttackBlockKeyboard &&
			a_left.leftAttackBlockGamepad == a_right.leftAttackBlockGamepad &&
			a_left.rightAttackBlockKeyboard == a_right.rightAttackBlockKeyboard &&
			a_left.rightAttackBlockGamepad == a_right.rightAttackBlockGamepad &&
			a_left.dualAttackKeyboard == a_right.dualAttackKeyboard &&
			a_left.dualAttackGamepad == a_right.dualAttackGamepad &&
			a_left.shoutKeyboard == a_right.shoutKeyboard &&
			a_left.shoutGamepad == a_right.shoutGamepad &&
			a_left.kinectShoutKeyboard == a_right.kinectShoutKeyboard &&
			a_left.kinectShoutGamepad == a_right.kinectShoutGamepad &&
			a_left.playerLoaded == a_right.playerLoaded &&
			a_left.playerMounted == a_right.playerMounted &&
			a_left.playerFormId == a_right.playerFormId &&
			a_left.playerWeaponState == a_right.playerWeaponState &&
			a_left.playerWeaponsDrawn == a_right.playerWeaponsDrawn &&
			a_left.playerEquipmentIdentityCaptured == a_right.playerEquipmentIdentityCaptured &&
			(!a_left.playerEquipmentIdentityCaptured ||
				DragonAspectFlight::State::SameWeaponEquipmentIdentity(
					a_left.playerEquipmentIdentity,
					a_right.playerEquipmentIdentity)) &&
			a_left.playerEquipmentIdentityEpoch == a_right.playerEquipmentIdentityEpoch &&
			a_left.publishedFlightSession == a_right.publishedFlightSession &&
			a_left.dragonAspectActive == a_right.dragonAspectActive &&
			a_left.flying == a_right.flying &&
			a_left.descending == a_right.descending &&
			a_left.blockRequested == a_right.blockRequested &&
			a_left.suppressInput == a_right.suppressInput &&
			a_left.showShoutRequiredNotification == a_right.showShoutRequiredNotification;
	}

	RE::PlayerCharacter* GetPlayer() { return RE::PlayerCharacter::GetSingleton(); }

	std::atomic_uint32_t HudMessageQueueExceptionCount{ 0 };
	std::atomic_uint32_t HudMessageTaskExceptionCount{ 0 };
	std::atomic_uint32_t InputDiagnosticQueueExceptionCount{ 0 };
	std::atomic_uint32_t InputDiagnosticTaskExceptionCount{ 0 };

	void LogInputBoundaryException(
		std::atomic_uint32_t& a_counter,
		std::string_view a_event,
		std::string_view a_phase,
		std::string_view a_action,
		const char* a_error) noexcept;

	void ShowMessage(const char* a_msg)
	{
		if (!a_msg) {
			return;
		}

		// Input events are an immutable intent boundary.  Do not mutate HUD/UI
		// state from the sink callback; copy the text and let the SKSE task queue
		// execute it on Skyrim's game thread.
		try {
			const std::string message{ a_msg };
			const SKSE::TaskInterface* taskInterface = SKSE::GetTaskInterface();
			if (!taskInterface) {
				try {
					logger::warn(
						"event=hud_message_queue queued=false reason=task_interface_unavailable text=\"{}\"",
						message);
				} catch (...) {
					// Diagnostics must not escape the input boundary.
				}
				return;
			}

			taskInterface->AddTask([message]() {
				try {
					RE::SendHUDMessage::ShowHUDMessage(message.c_str());
					logger::info("event=hud_message text=\"{}\"", message);
				} catch (const std::exception& e) {
					LogInputBoundaryException(
						HudMessageTaskExceptionCount,
						"hud_message",
						"callback",
						"show",
						e.what());
				} catch (...) {
					LogInputBoundaryException(
						HudMessageTaskExceptionCount,
						"hud_message",
						"callback",
						"show",
						"unknown");
				}
			});
			try {
				logger::info("event=hud_message_queue queued=true text=\"{}\"", message);
			} catch (...) {
				// Diagnostics must not escape the input boundary.
			}
		} catch (const std::exception& e) {
			LogInputBoundaryException(
				HudMessageQueueExceptionCount,
				"hud_message",
				"queue",
				"add_task",
				e.what());
		} catch (...) {
			LogInputBoundaryException(
				HudMessageQueueExceptionCount,
				"hud_message",
				"queue",
				"add_task",
				"unknown");
		}
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

	const char* InputActionName(DragonAspectFlight::State::FlightInputAction a_action)
	{
		using DragonAspectFlight::State::FlightInputAction;
		switch (a_action) {
		case FlightInputAction::kStartFlight: return "start_flight";
		case FlightInputAction::kStopFlight: return "stop_flight";
		case FlightInputAction::kBeginDescent: return "begin_descent";
		case FlightInputAction::kCancelDescent: return "cancel_descent";
		case FlightInputAction::kToggleCombatReady: return "toggle_combat_ready";
		case FlightInputAction::kBeginCombat: return "begin_combat";
		case FlightInputAction::kBlockRequest: return "block_request";
		case FlightInputAction::kBlockRelease: return "block_release";
		case FlightInputAction::kShoutPress: return "shout_press";
		case FlightInputAction::kShoutRelease: return "shout_release";
		case FlightInputAction::kClearShout: return "clear_shout";
		case FlightInputAction::kLaunchBoost: return "launch_boost";
		case FlightInputAction::kSetMovementInput: return "set_movement_input";
		case FlightInputAction::kSetVerticalInput: return "set_vertical_input";
		case FlightInputAction::kSetBoostHeld: return "set_boost_held";
		case FlightInputAction::kObserveGroundWeaponTransition: return "observe_ground_weapon_transition";
		default: return "unknown";
		}
	}

	constexpr std::size_t InvalidPendingReleaseChannel = 2;

	[[nodiscard]] constexpr std::size_t GetPendingReleaseChannel(
		DragonAspectFlight::State::FlightInputAction a_action) noexcept
	{
		using DragonAspectFlight::State::FlightInputAction;
		switch (a_action) {
		case FlightInputAction::kBlockRequest:
		case FlightInputAction::kBlockRelease:
			return 0;
		case FlightInputAction::kShoutPress:
		case FlightInputAction::kShoutRelease:
		case FlightInputAction::kClearShout:
			return 1;
		default:
			return InvalidPendingReleaseChannel;
		}
	}

	[[nodiscard]] constexpr bool IsPendingReleaseAction(
		DragonAspectFlight::State::FlightInputAction a_action) noexcept
	{
		using DragonAspectFlight::State::FlightInputAction;
		return a_action == FlightInputAction::kBlockRelease ||
			a_action == FlightInputAction::kShoutRelease ||
			a_action == FlightInputAction::kClearShout;
	}

	[[nodiscard]] constexpr bool IsPendingReleaseReplacement(
		DragonAspectFlight::State::FlightInputAction a_action) noexcept
	{
		using DragonAspectFlight::State::FlightInputAction;
		return a_action == FlightInputAction::kBlockRequest ||
			a_action == FlightInputAction::kShoutPress ||
			IsPendingReleaseAction(a_action);
	}

	struct ExceptionLogDecision
	{
		std::uint32_t count{ 0 };
		bool emit{ false };
	};

	ExceptionLogDecision NextExceptionLog(std::atomic_uint32_t& a_counter) noexcept
	{
		const auto count = a_counter.fetch_add(1, std::memory_order_relaxed) + 1;
		return { count, count == 1 || count % 8 == 0 };
	}

	void LogInputBoundaryException(
		std::atomic_uint32_t& a_counter,
		std::string_view a_event,
		std::string_view a_phase,
		std::string_view a_action,
		const char* a_error) noexcept
	{
		const auto failure = NextExceptionLog(a_counter);
		if (!failure.emit) {
			return;
		}
		try {
			logger::error(
				"event={} exception={} action={} count={} error={}",
				a_event,
				a_phase,
				a_action,
				failure.count,
				a_error ? a_error : "unknown");
		} catch (...) {
			// Diagnostics must not reopen the exception boundary.
		}
	}

	std::uint64_t InputActionTimestampMilliseconds()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	std::uint64_t NextQueueGeneration(std::atomic_uint64_t& a_counter) noexcept
	{
		auto current = a_counter.load(std::memory_order_relaxed);
		for (;;) {
			const auto next = current == std::numeric_limits<std::uint64_t>::max() ?
				std::uint64_t{ 1 } : current + 1;
			if (a_counter.compare_exchange_weak(
					current,
					next,
					std::memory_order_acq_rel,
					std::memory_order_relaxed)) {
				return next;
			}
		}
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
		QueueGameThreadStateRefresh();
		logger::info("Input handler registered - {}", BuildVersion);
	}

	InputHandler::RuntimeState InputHandler::GetRuntimeState() const
	{
		std::shared_lock lock(_runtimeMutex);
		return _runtimeState;
	}

	void InputHandler::QueueGameThreadStateRefresh()
	{
		const auto generation = NextQueueGeneration(_runtimeRefreshQueueGeneration);
		std::uint64_t expectedGeneration = 0;
		if (!_runtimeRefreshQueued.compare_exchange_strong(
				expectedGeneration,
				generation,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			// One refresh task is enough for all producers until the task reaches the
			// game thread.  The token, rather than a plain bool, prevents stale work
			// from releasing a lease reserved by a later lifecycle/session edge.
			return;
		}

		const auto releaseQueueLease = [this, generation]() noexcept {
			std::uint64_t expected = generation;
			_runtimeRefreshQueued.compare_exchange_strong(
				expected,
				std::uint64_t{ 0 },
				std::memory_order_acq_rel,
				std::memory_order_acquire);
		};

		const SKSE::TaskInterface* taskInterface = nullptr;
		try {
			taskInterface = SKSE::GetTaskInterface();
		} catch (const std::exception& e) {
			releaseQueueLease();
			const auto failure = NextExceptionLog(_runtimeRefreshExceptionCount);
			if (failure.emit) {
				logger::error(
					"event=input_state_refresh schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
					"first_session=0 last_session=0 first_input_sequence_domain=0 last_input_sequence_domain=0 "
					"manager_sequence=0 outcome=queue_failed reason=get_task_interface_exception "
					"exception=get_task_interface generation={} count={} lease_released=true error={}",
					State::StructuredDiagnosticSchemaVersion,
					generation,
					failure.count,
					e.what());
			}
			return;
		} catch (...) {
			releaseQueueLease();
			const auto failure = NextExceptionLog(_runtimeRefreshExceptionCount);
			if (failure.emit) {
				logger::error(
					"event=input_state_refresh schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
					"first_session=0 last_session=0 first_input_sequence_domain=0 last_input_sequence_domain=0 "
					"manager_sequence=0 outcome=queue_failed reason=get_task_interface_exception "
					"exception=get_task_interface generation={} count={} lease_released=true error=unknown",
					State::StructuredDiagnosticSchemaVersion,
					generation,
					failure.count);
			}
			return;
		}

		if (!taskInterface) {
			releaseQueueLease();
			logger::warn(
				"event=input_state_refresh schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
				"first_session=0 last_session=0 first_input_sequence_domain=0 last_input_sequence_domain=0 "
				"manager_sequence=0 outcome=queue_failed queued=false reason=task_interface_unavailable "
				"generation={} lease_released=true",
				State::StructuredDiagnosticSchemaVersion,
				generation);
			return;
		}

		// InputHandler is a process-lifetime singleton.  Capturing this is therefore
		// intentional; the generation check keeps stale callbacks from touching a
		// later session's refresh lease, and all RE reads remain on the game thread.
		try {
			taskInterface->AddTask([this, generation]() {
				const auto releaseQueueLease = [this, generation]() noexcept {
					std::uint64_t expected = generation;
					_runtimeRefreshQueued.compare_exchange_strong(
						expected,
						std::uint64_t{ 0 },
						std::memory_order_acq_rel,
						std::memory_order_acquire);
				};

				if (_runtimeRefreshQueued.load(std::memory_order_acquire) != generation) {
					releaseQueueLease();
					return;
				}

				try {
					RefreshGameThreadState(generation);
				} catch (const std::exception& e) {
					releaseQueueLease();
					const auto failure = NextExceptionLog(_runtimeRefreshExceptionCount);
					if (failure.emit) {
						logger::error(
							"event=input_state_refresh schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
							"first_session=0 last_session=0 first_input_sequence_domain=0 last_input_sequence_domain=0 "
							"manager_sequence=0 outcome=queue_failed reason=refresh_callback_exception exception=callback "
							"generation={} count={} lease_released=true error={}",
							State::StructuredDiagnosticSchemaVersion,
							generation,
							failure.count,
							e.what());
					}
					return;
				} catch (...) {
					releaseQueueLease();
					const auto failure = NextExceptionLog(_runtimeRefreshExceptionCount);
					if (failure.emit) {
						logger::error(
							"event=input_state_refresh schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
							"first_session=0 last_session=0 first_input_sequence_domain=0 last_input_sequence_domain=0 "
							"manager_sequence=0 outcome=queue_failed reason=refresh_callback_exception exception=callback "
							"generation={} count={} lease_released=true error=unknown",
							State::StructuredDiagnosticSchemaVersion,
							generation,
							failure.count);
					}
					return;
				}

				releaseQueueLease();
			});
		} catch (const std::exception& e) {
			releaseQueueLease();
			const auto failure = NextExceptionLog(_runtimeRefreshExceptionCount);
			if (failure.emit) {
				logger::error(
					"event=input_state_refresh schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
					"first_session=0 last_session=0 first_input_sequence_domain=0 last_input_sequence_domain=0 "
					"manager_sequence=0 outcome=queue_failed reason=add_task_exception exception=add_task "
					"generation={} count={} lease_released=true error={}",
					State::StructuredDiagnosticSchemaVersion,
					generation,
					failure.count,
					e.what());
			}
		} catch (...) {
			releaseQueueLease();
			const auto failure = NextExceptionLog(_runtimeRefreshExceptionCount);
			if (failure.emit) {
				logger::error(
					"event=input_state_refresh schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
					"first_session=0 last_session=0 first_input_sequence_domain=0 last_input_sequence_domain=0 "
					"manager_sequence=0 outcome=queue_failed reason=add_task_exception exception=add_task "
					"generation={} count={} lease_released=true error=unknown",
					State::StructuredDiagnosticSchemaVersion,
					generation,
					failure.count);
			}
		}
	}

	void InputHandler::RefreshGameThreadState(std::uint64_t a_refreshGeneration)
	{
		// This function is called only by SKSE's game-thread task queue.  Keep all
		// RE singleton/player/UI/ControlMap/FlightManager reads here; the input
		// sink consumes only the copied RuntimeState below.
		RuntimeState next;
		auto& settings = Settings::GetSingleton();
		{
			std::shared_lock lock(settings.mutex);
			next.activation = settings.activation;
			next.ascend = settings.ascend;
			next.descend = settings.descend;
			next.showShoutRequiredNotification = settings.showShoutRequiredNotification;
		}

		if (auto* controlMap = RE::ControlMap::GetSingleton()) {
			const auto mapped = [controlMap](const char* a_event, RE::INPUT_DEVICE a_device) {
				const auto key = controlMap->GetMappedKey(a_event, a_device);
				return key == RE::ControlMap::kInvalid ? 0xFFFFFFFFU : static_cast<std::uint32_t>(key);
			};
			next.readyWeaponKeyboard = mapped(ReadyWeaponUserEvent, RE::INPUT_DEVICE::kKeyboard);
			next.readyWeaponGamepad = mapped(ReadyWeaponUserEvent, RE::INPUT_DEVICE::kGamepad);
			next.leftAttackBlockKeyboard = mapped(LeftCastUserEvent, RE::INPUT_DEVICE::kKeyboard);
			next.leftAttackBlockGamepad = mapped(LeftCastUserEvent, RE::INPUT_DEVICE::kGamepad);
			next.rightAttackBlockKeyboard = mapped(RightCastUserEvent, RE::INPUT_DEVICE::kKeyboard);
			next.rightAttackBlockGamepad = mapped(RightCastUserEvent, RE::INPUT_DEVICE::kGamepad);
			next.dualAttackKeyboard = mapped(DualCastUserEvent, RE::INPUT_DEVICE::kKeyboard);
			next.dualAttackGamepad = mapped(DualCastUserEvent, RE::INPUT_DEVICE::kGamepad);
			next.shoutKeyboard = mapped(ShoutUserEvent, RE::INPUT_DEVICE::kKeyboard);
			next.shoutGamepad = mapped(ShoutUserEvent, RE::INPUT_DEVICE::kGamepad);
			next.kinectShoutKeyboard = mapped(KinectShoutUserEvent, RE::INPUT_DEVICE::kKeyboard);
			next.kinectShoutGamepad = mapped(KinectShoutUserEvent, RE::INPUT_DEVICE::kGamepad);
		}

		if (auto* player = GetPlayer()) {
			next.playerLoaded = player->Is3DLoaded();
			next.playerMounted = player->IsOnMount();
			next.playerFormId = static_cast<std::uint32_t>(player->GetFormID());
			if (const auto* actorState = player->AsActorState()) {
				next.playerWeaponState = static_cast<std::int32_t>(actorState->GetWeaponState());
				next.playerWeaponsDrawn = actorState->IsWeaponDrawn();
			}
		}
		auto& flightManager = FlightManager::GetSingleton();
		if (next.playerLoaded) {
			// Capture the identity and its producer epoch as one immutable observation.
			// A separate identity read followed by an epoch read could publish A with
			// the epoch advanced for B when equipment changes between those calls.
			const auto equipmentSnapshot = flightManager.GetCurrentWeaponEquipmentSnapshot();
			next.playerEquipmentIdentity = equipmentSnapshot.identity;
			next.playerEquipmentIdentityCaptured = equipmentSnapshot.captured;
			next.playerEquipmentIdentityEpoch = equipmentSnapshot.epoch;
		}
		next.publishedFlightSession = flightManager.GetPublishedFlightSession();
		next.dragonAspectActive = flightManager.IsDragonAspectActive();
		next.flying = flightManager.IsFlying();
		next.descending = flightManager.IsDescending();
		next.blockRequested = flightManager.IsFlightBlockRequested();
		next.suppressInput = FlightManager::ShouldSuppressInput();
		next.valid = true;

		const auto now = std::chrono::steady_clock::now();
		bool initialized = false;
		bool changed = false;
		bool heartbeatDue = false;
		bool emit = false;
		std::uint64_t suppressedCount = 0;
		RuntimeState previous;
		{
			std::unique_lock lock(_runtimeMutex);
			previous = _runtimeState;
			initialized = _runtimeStateLogInitialized;
			changed = !initialized || !SameRuntimeState(_lastLoggedRuntimeState, next);
			heartbeatDue = initialized &&
				(_lastRuntimeStateLog.time_since_epoch().count() == 0 ||
					std::chrono::duration<float>(now - _lastRuntimeStateLog).count() >= 2.0F);
			emit = State::ShouldFlushInputStateRefresh(initialized, changed, heartbeatDue);
			_runtimeState = next;
			if (emit) {
				suppressedCount = _suppressedInputCount.exchange(0, std::memory_order_acq_rel);
				_lastLoggedRuntimeState = next;
				_runtimeStateLogInitialized = true;
				_lastRuntimeStateLog = now;
			}
		}
		_publishedFlightSession.store(next.publishedFlightSession, std::memory_order_release);
		const bool playerUnloaded = previous.playerLoaded && !next.playerLoaded;
		const bool actorChanged = previous.playerFormId != 0 && next.playerFormId != 0 &&
			previous.playerFormId != next.playerFormId;
		const bool flightStopped = previous.flying && !next.flying;
		const bool sessionChanged = previous.publishedFlightSession != next.publishedFlightSession;
		if (flightStopped) {
			FlushInputActionCoalescing("flight_stop", true);
		} else if (playerUnloaded) {
			FlushInputActionCoalescing("player_unload", true);
		} else if (actorChanged) {
			FlushInputActionCoalescing("actor_change", true);
		} else if (sessionChanged) {
			FlushInputActionCoalescing("session_change", true);
		}
		// Deferred button snapshots are valid only for the actor/session that
		// captured them.  A matching start/stop may have intentionally deferred
		// edges from the same input dispatch; those are explicitly marked for
		// session rebind and remain eligible after that boundary task.  Every other
		// boundary discards the queue before replay so an old release cannot mutate
		// a new actor.
		if (flightStopped || playerUnloaded || actorChanged || sessionChanged) {
			const bool preserveBoundaryRebind = State::ShouldPreserveDeferredInputRebind(
				flightStopped,
				playerUnloaded,
				actorChanged,
				sessionChanged);
			std::scoped_lock lock(_inputMutex);
			FlushDeferredButtonEventsLocked(
				flightStopped ? "flight_stop" :
					(playerUnloaded ? "player_unload" :
						(actorChanged ? "actor_change" : "session_change")),
				preserveBoundaryRebind);
		}
		ReplayDeferredButtonEvents(next, a_refreshGeneration);
		if (!emit) {
			return;
		}
		const char* boundary = !initialized ? "first" : (changed ? "change" : "heartbeat");
		logger::info(
			"event=input_state_refresh schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
			"first_session={} last_session={} first_input_sequence_domain=0 last_input_sequence_domain=0 "
			"manager_sequence=0 outcome=refreshed reason=runtime_snapshot queued=true "
			"player_loaded={} mounted={} player_form=0x{:08X} weapon_state={} "
			"weapons_drawn={} equipment_right_form=0x{:08X} equipment_left_form=0x{:08X} "
			"equipment_right_type={} equipment_left_type={} equipment_family={} published_session={} dragon_aspect={} flying={} descending={} "
			"block_requested={} suppress_input={} suppressed_count={} boundary={} "
			"activation_device={} activation_code=0x{:X}",
			State::StructuredDiagnosticSchemaVersion,
			next.publishedFlightSession,
			next.publishedFlightSession,
			next.playerLoaded,
			next.playerMounted,
			next.playerFormId,
			next.playerWeaponState,
			next.playerWeaponsDrawn,
			next.playerEquipmentIdentity.rightFormId,
			next.playerEquipmentIdentity.leftFormId,
			next.playerEquipmentIdentity.rightWeaponType,
			next.playerEquipmentIdentity.leftWeaponType,
			State::WeaponEquipmentFamilyName(next.playerEquipmentIdentity.family),
			next.publishedFlightSession,
			next.dragonAspectActive,
			next.flying,
			next.descending,
			next.blockRequested,
			next.suppressInput,
			suppressedCount,
			boundary,
			static_cast<std::uint32_t>(next.activation.device),
			next.activation.code);
	}

	RE::BSEventNotifyControl InputHandler::ProcessEvent(
		RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*)
	{
		std::scoped_lock inputLock(_inputMutex);
		if (!a_event) return RE::BSEventNotifyControl::kContinue;
		// Retry failed release edges before inspecting this dispatch.  This keeps a
		// prior release ahead of a same-session press while input serialization is
		// held; failures simply remain armed for the next event.
		RetryPendingReleasesLocked();

		// A session boundary queued by an earlier edge in this same dispatch must
		// run before the refresh task.  The refresh is therefore requested after
		// this loop, making the task-interface FIFO ordering explicit.
		_dispatchBoundaryActive = true;
		_dispatchBoundaryState = {};
		const auto runtime = GetRuntimeState();
		if (!runtime.valid) {
			// Refresh asynchronously; no engine object, singleton, manager, UI, or
			// ControlMap read is allowed on this callback path.
			QueueGameThreadStateRefresh();
			const auto sourceSession = _publishedFlightSession.load(std::memory_order_acquire);
			std::uint64_t firstDeferredActionId = 0;
			std::uint64_t lastDeferredActionId = 0;
			for (auto* e = *a_event; e; e = e->next) {
				if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
					continue;
				}
				if (const auto* button = e->AsButtonEvent()) {
					const auto snapshot = CopyButtonInput(*button);
					if (IsSemanticButtonEdge(snapshot)) {
						const auto actionId = AllocateInputCorrelationId();
						if (firstDeferredActionId == 0) {
							firstDeferredActionId = actionId;
						}
						lastDeferredActionId = actionId;
						DeferButtonEvent(snapshot, sourceSession, actionId);
					}
				}
			}
			_dispatchBoundaryActive = false;
			_dispatchBoundaryState = {};
			logger::info(
				"event=input_deferred schema={} correlation=aggregate first_action_id={} last_action_id={} "
				"first_session={} last_session={} first_input_sequence_domain=0 last_input_sequence_domain=0 "
				"manager_sequence=0 outcome=deferred reason=runtime_snapshot_invalid refresh_pending=true",
				State::StructuredDiagnosticSchemaVersion,
				firstDeferredActionId,
				lastDeferredActionId,
				sourceSession,
				sourceSession);
			return RE::BSEventNotifyControl::kContinue;
		}
		if (runtime.suppressInput) {
			_suppressedInputCount.fetch_add(1, std::memory_order_relaxed);
			ResetFlightInputState();
			QueueGameThreadStateRefresh();
			_dispatchBoundaryActive = false;
			_dispatchBoundaryState = {};
			return RE::BSEventNotifyControl::kContinue;
		}

		bool consumeInput = false;
		for (auto* e = *a_event; e; e = e->next) {
			if (e->eventType == RE::INPUT_EVENT_TYPE::kButton) {
				if (const auto* btn = e->AsButtonEvent()) {
					const auto snapshot = CopyButtonInput(*btn);
					const auto boundaryDecision = State::ReduceRuntimeDispatchBoundary(
						_dispatchBoundaryState,
						State::FlightInputAction::kClearShout,
						IsSemanticButtonEdge(snapshot));
					if (boundaryDecision.deferSemanticEdge) {
						DeferButtonEvent(
							snapshot,
							runtime.publishedFlightSession,
							AllocateInputCorrelationId(),
							boundaryDecision.rebindSession,
							boundaryDecision.expectedFlying);
						continue;
					}
					struct ScopedInputCorrelation final {
						std::uint64_t& id;
						std::uint64_t& domain;
						std::uint64_t priorId;
						std::uint64_t priorDomain;
						~ScopedInputCorrelation() noexcept { id = priorId; domain = priorDomain; }
					} scope{ _activeInputCorrelationId, _activeInputSequenceDomain, _activeInputCorrelationId, _activeInputSequenceDomain };
					_activeInputCorrelationId = IsSemanticButtonEdge(snapshot) ? AllocateInputCorrelationId() : 0;
					_activeInputSequenceDomain = 0;
					consumeInput = HandleButtonEvent(snapshot, runtime) || consumeInput;
				}
			} else if (e->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
				if (const auto* stick = static_cast<const RE::ThumbstickEvent*>(e)) {
					const auto snapshot = CopyThumbstickInput(*stick);
					HandleThumbstickEvent(snapshot);
				}
			}
		}

		const auto boundaryQueued = _dispatchBoundaryState.boundaryQueued;
		if (boundaryQueued) {
			logger::info(
				"event=input_runtime_boundary schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
				"first_session={} last_session={} first_input_sequence_domain=0 last_input_sequence_domain=0 "
				"manager_sequence=0 outcome=queued reason=session_boundary queued=true deferred_edges_rebind=true "
				"refresh_order=boundary_task_then_refresh",
				State::StructuredDiagnosticSchemaVersion,
				runtime.publishedFlightSession,
				runtime.publishedFlightSession);
		}
		QueueGameThreadStateRefresh();
		_dispatchBoundaryActive = false;
		_dispatchBoundaryState = {};

		return consumeInput ? RE::BSEventNotifyControl::kStop : RE::BSEventNotifyControl::kContinue;
	}

	bool InputHandler::ProcessFlightShout(const State::ButtonInputSnapshot& a_event)
	{
		const auto edge = a_event.IsUp() ? DragonAspectFlight::State::ShoutEdge::kRelease :
			(State::IsButtonPressEdge(a_event) && !_shoutHeld) ?
				DragonAspectFlight::State::ShoutEdge::kPress :
				DragonAspectFlight::State::ShoutEdge::kHeld;
		const auto result = DragonAspectFlight::State::ReduceShoutEdge(
			DragonAspectFlight::State::ShoutLatchState{ _shoutHeld, false },
			edge);
		_shoutHeld = result.state.held;
		const auto actionId = _activeInputCorrelationId != 0 ?
			_activeInputCorrelationId : AllocateInputCorrelationId();
		const auto sourceSession = _publishedFlightSession.load(std::memory_order_acquire);
		const auto inputSequenceDomain = _activeInputSequenceDomain;

		if (result.acceptedPress) {
			_shoutHeldDiagnosticLogged = false;
			const bool queued = QueueFlightAction(State::FlightInputAction::kShoutPress);
			logger::info(
				"event=shout_edge schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
				"action=shout outcome={} reason=press edge=press passthrough=true",
				State::StructuredDiagnosticSchemaVersion,
				actionId,
				sourceSession,
				inputSequenceDomain,
				queued ? "queued" : "queue_failed");
		} else if (edge == DragonAspectFlight::State::ShoutEdge::kRelease) {
			// A release without a locally-owned press is stale input.  Queueing it
			// anyway lets a duplicate release race a later press at the manager
			// boundary and was the source of the old "shout wakes on next attack"
			// symptom.  The manager receives exactly one release for each accepted
			// press; vanilla still owns the physical event because this method returns
			// false below.
			const bool queued = result.acceptedRelease &&
				QueueFlightAction(State::FlightInputAction::kShoutRelease);
			_shoutHeldDiagnosticLogged = false;
			logger::info(
				"event=shout_edge schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
				"action=shout edge=release outcome={} reason=release accepted={} queued={} held_duration={:.2f} passthrough=true",
				State::StructuredDiagnosticSchemaVersion,
				actionId,
				sourceSession,
				inputSequenceDomain,
				result.acceptedRelease ? (queued ? "queued" : "queue_failed") : "stale_ignored",
				result.acceptedRelease,
				queued,
				a_event.heldDuration);
		} else if (result.ignoredHeld && !_shoutHeldDiagnosticLogged) {
			_shoutHeldDiagnosticLogged = true;
			logger::info(
				"event=shout_edge schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
				"action=shout edge=held outcome=ignored reason=held_rearm_false passthrough=true",
				State::StructuredDiagnosticSchemaVersion,
				actionId,
				sourceSession,
				inputSequenceDomain);
		}

		return false;
	}

	bool InputHandler::HandleButtonEvent(
		const State::ButtonInputSnapshot& a_event,
		const RuntimeState& a_runtime)
	{
		const auto& ue = a_event.userEvent;
		const bool isKb = a_event.device == static_cast<std::uint32_t>(RE::INPUT_DEVICE::kKeyboard);
		const float pv = a_event.IsPressed() ? 1.0F : 0.0F;

		// Bindings are deliberately evaluated before vanilla semantic actions.
		// Gamepad A/Y/bumpers/triggers can be mapped to flight without their
		// vanilla action pre-empting the configured flight response.
		if (IsFlightActivationInput(a_event, a_runtime.activation)) {
			const bool consumed = HandleFlightActivation(a_event, a_runtime);
			QueueInputDiagnostic("flight_activation", a_event, consumed ? "consumed" : "passthrough");
			return consumed;
		}

		const bool isConfiguredAscendInput = MatchesBinding(a_event, a_runtime.ascend);
		const bool isConfiguredDescendInput = MatchesBinding(a_event, a_runtime.descend);

		if (a_runtime.flying && (isConfiguredAscendInput || isConfiguredDescendInput)) {
			if (!a_runtime.dragonAspectActive) {
				QueueFlightAction(State::FlightInputAction::kStopFlight);
				ResetFlightInputState();
				return true;
			}

			if (a_runtime.descending) {
				_ascendHeld = false;
				_descendHeld = false;
				UpdateVerticalInput();
				return true;
			}

			if (a_event.IsUp()) {
				if (isConfiguredAscendInput) {
					_ascendHeld = false;
				}
				if (isConfiguredDescendInput) {
					_descendHeld = false;
				}
			} else if (a_event.IsPressed() || a_event.IsHeld()) {
				if (isConfiguredAscendInput) {
					_ascendHeld = true;
				}
				if (isConfiguredDescendInput) {
					_descendHeld = true;
				}
			}

			UpdateVerticalInput(a_event.IsDown() || a_event.IsUp());
			return true;
		}

		if (isKb) {
			const bool inputEdge = a_event.IsDown() || a_event.IsUp();
			if (ue == ForwardUserEvent) { _keyboardForwardInput = pv; UpdateMovementInput(inputEdge); return false; }
			if (ue == BackUserEvent) { _keyboardForwardInput = -pv; UpdateMovementInput(inputEdge); return false; }
			if (ue == StrafeLeftUserEvent) { _keyboardStrafeInput = -pv; UpdateMovementInput(inputEdge); return false; }
			if (ue == StrafeRightUserEvent) { _keyboardStrafeInput = pv; UpdateMovementInput(inputEdge); return false; }
		}

		if (IsLaunchAction(a_event)) {
			if (!a_event.IsHeld()) {
				QueueInputDiagnostic("launch", a_event, a_runtime.descending ? "blocked_descent" : "observed");
			}
			if (a_runtime.descending) return true;
			if (a_event.IsUp()) { _launchHeld = false; return a_runtime.flying && a_runtime.dragonAspectActive; }
			if (State::IsButtonPressEdge(a_event) && a_runtime.flying) {
				if (!a_runtime.dragonAspectActive) {
					QueueFlightAction(State::FlightInputAction::kStopFlight);
					ResetFlightInputState();
					return false;
				}

				if (!_launchHeld) {
					_launchHeld = true;
					QueueFlightAction(State::FlightInputAction::kLaunchBoost);
				}
				return true;
			}
			return false;
		}

		if (IsShoutAction(a_event, a_runtime) && a_runtime.flying) {
			QueueInputDiagnostic("shout", a_event, a_runtime.descending ? "observed_descent" : "observed");
			if (!a_runtime.dragonAspectActive) {
				QueueFlightAction(State::FlightInputAction::kStopFlight);
				ResetFlightInputState();
				// Flight cleanup must not steal the physical shout from vanilla when
				// the cached flight snapshot has already lost Dragon Aspect.
				return false;
			}

			const bool consumed = ProcessFlightShout(a_event);
			QueueInputDiagnostic("shout", a_event, consumed ? "handled" : "passthrough");
			return consumed;
		}

		if (IsReadyWeaponAction(a_event, a_runtime)) {
			if (a_event.IsUp()) {
				_readyWeaponHeld = false;
				QueueInputDiagnostic("ready_weapon", a_event, "released_passthrough");
				return false;
			}
			if (State::IsButtonPressEdge(a_event) &&
				!_readyWeaponHeld) {
				_readyWeaponHeld = true;
				const bool explicitSheathe =
					a_event.userEvent == SheatheWeaponUserEvent ||
					a_event.userEvent == WeaponSheatheUserEvent;
				if (State::ShouldReleaseFlightBlockForReadyEdge(
					State::IsButtonPressEdge(a_event),
					a_runtime.playerWeaponsDrawn || explicitSheathe,
					_blockHeld)) {
					// A sheathe edge must release DAF's locally-owned block intent
					// before vanilla/equipment-state code changes the weapon state.
					_blockHeld = false;
					QueueFlightAction(State::FlightInputAction::kBlockRelease);
				}
				if (a_runtime.flying) {
					const auto readyToken = FlightManager::GetSingleton().AnnounceReadyGeneration();
					State::FlightInputActionSnapshot readyAction{
						State::FlightInputAction::kToggleCombatReady };
					readyAction.readyToken = readyToken;
					readyAction.equipmentIdentity = a_runtime.playerEquipmentIdentity;
					readyAction.equipmentIdentityCaptured = a_runtime.playerEquipmentIdentityCaptured;
					readyAction.equipmentIdentityEpoch = a_runtime.playerEquipmentIdentityEpoch;
					const bool queued = QueueFlightAction(readyAction);
					QueueInputDiagnostic(
						"ready_weapon",
						a_event,
						queued ? "observer_queued_passthrough" : "observer_queue_failed_passthrough");
				} else if (!a_runtime.playerLoaded) {
					QueueInputDiagnostic("ready_weapon", a_event, "ground_observer_aborted_player_unloaded_passthrough");
				} else {
					// Capture the pre-edge actor snapshot before vanilla processes the
					// Ready Weapon event.  The action is always passed through.
					const auto readyToken = FlightManager::GetSingleton().AnnounceReadyGeneration();
					State::FlightInputActionSnapshot observer{
						State::FlightInputAction::kObserveGroundWeaponTransition };
					observer.readyToken = readyToken;
					observer.flag = !a_runtime.playerWeaponsDrawn;
					observer.actorFormId = a_runtime.playerFormId;
					observer.actorWeaponState = a_runtime.playerWeaponState;
					observer.equipmentIdentity = a_runtime.playerEquipmentIdentity;
					observer.equipmentIdentityCaptured = a_runtime.playerEquipmentIdentityCaptured;
					observer.equipmentIdentityEpoch = a_runtime.playerEquipmentIdentityEpoch;
					const bool queued = QueueFlightAction(observer);
					QueueInputDiagnostic(
						"ready_weapon",
						a_event,
						queued ? "ground_observer_queued_passthrough" :
							"ground_observer_queue_failed_passthrough");
				}
			}
			// Vanilla and installed equipment-state/input mods keep first ownership.
			// FlightManager only invokes the relocated native fallback later if the
			// actor state does not begin moving toward the requested target.
			return false;
		}

		if (const auto combatChannel = ResolveCombatInputChannel(a_event, a_runtime)) {
			// Keep vanilla/MCO ownership of the shared attack channels.  The pure
			// policy only primes DAF's block latch after a left-channel held sample;
			// an initial down edge remains an ordinary attack/combat edge.  Right
			// attack/block and dual cast remain passthrough and never become block
			// requests.
			const auto combatPolicy = State::DecideFlightCombatInputPolicy(
				a_runtime.flying,
				*combatChannel,
				_blockHeld,
				a_event.IsDown(),
				a_event.IsUp(),
				a_event.IsHeld());
			const bool bashInput =
				*combatChannel == State::FlightCombatInputChannel::kRightAttackBlock &&
				a_runtime.blockRequested;
			if (!a_event.IsHeld()) {
				// Skyrim's input event does not expose a reliable normal-versus-power
				// discriminator here.  Preserve that boundary honestly instead of
				// inferring an animation consumer outcome.
				const auto policyActionId = _activeInputCorrelationId != 0 ?
					_activeInputCorrelationId : AllocateInputCorrelationId();
				logger::info(
					"event=input_policy schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
					"action=combat outcome=passthrough reason=attack_classification attack_classification={} "
					"policy=passthrough_to_vanilla_mco_oar rendered_pose_visible=false",
					State::StructuredDiagnosticSchemaVersion,
					policyActionId,
					a_runtime.publishedFlightSession,
					_activeInputSequenceDomain,
					bashInput ? "bash" : "normal_or_power_unclassified");
			}
			if (combatPolicy.queueBlockRequest || combatPolicy.queueBlockRelease) {
				if (combatPolicy.queueBlockRelease) {
					_blockHeld = false;
					const bool queued = QueueFlightAction(State::FlightInputAction::kBlockRelease);
					QueueInputDiagnostic(
						"block",
						a_event,
						queued ? "release_queued_passthrough" : "release_queue_failed_passthrough");
				} else if (combatPolicy.queueBlockRequest) {
					_blockHeld = true;
					const bool queued = QueueFlightAction(State::FlightInputAction::kBlockRequest);
					QueueInputDiagnostic(
						"block",
						a_event,
						queued ? "request_queued_passthrough" : "request_queue_failed_passthrough");
				}
			}
			if (a_runtime.flying && bashInput && a_event.IsDown()) {
				QueueInputDiagnostic("bash", a_event, "blocking_state_primed_passthrough");
			}
			if (!a_event.IsHeld()) {
				QueueInputDiagnostic(
					"combat",
					a_event,
					a_runtime.flying ?
						(a_runtime.descending ? "descent_passthrough_to_animation_graph" : "passthrough_to_animation_graph") :
						"ground_passthrough");
			}
			if (combatPolicy.queueBeginCombat) {
				if (!a_runtime.dragonAspectActive) {
					QueueFlightAction(State::FlightInputAction::kStopFlight);
					ResetFlightInputState();
					return combatPolicy.consume;
				}

				const bool queued = QueueFlightAction(State::FlightInputAction::kBeginCombat);
				QueueInputDiagnostic(
					"combat",
					a_event,
					queued ? "graph_sync_queued_passthrough" : "graph_sync_queue_failed_passthrough");
			}
			// Jumping Attack owns the topology when present. Otherwise the normal
			// vanilla/MCO event continues into DAF's flight-scoped OAR fallback.
			return combatPolicy.consume;
		}

		return false;
	}

	bool InputHandler::HandleFlightActivation(
		const State::ButtonInputSnapshot& a_event,
		const RuntimeState& a_runtime)
	{
		if (!a_event.IsDown() && !a_event.IsUp()) {
			return true;
		}

		if (!a_runtime.playerLoaded) return false;

		if (!a_runtime.dragonAspectActive) {
			if (a_runtime.flying) {
				QueueFlightAction(State::FlightInputAction::kStopFlight);
			}
			ResetFlightInputState();

			if (a_event.IsDown()) {
				if (a_runtime.showShoutRequiredNotification) {
					ShowMessage("Dragon Aspect Flight: full Dragon Aspect required");
				}
			}

			return true;
		}

		if (a_runtime.descending) {
			if (a_event.IsDown()) {
				ResetFlightInputState("cancel_descent", false);
				const bool queued = QueueFlightAction(State::FlightInputAction::kCancelDescent);
				UpdateMovementInput();
				if (queued) {
					ShowMessage("Dragon Aspect Flight: descent cancelled");
				}
			}
			return true;
		}

		if (a_event.IsDown()) {
			if (a_runtime.flying) {
				const bool queued = QueueFlightAction(State::FlightInputAction::kBeginDescent);
				ResetFlightInputState("begin_descent", false);
				if (queued) {
					ShowMessage("Dragon Aspect Flight: descending");
				}
			} else {
				if (a_runtime.playerMounted) {
					ShowMessage("Dragon Aspect Flight: unavailable while mounted");
					return true;
				}
				ResetFlightInputState();
				const bool queued = QueueFlightAction(State::FlightInputAction::kStartFlight);
				UpdateMovementInput();
				if (queued) {
					ShowMessage("Dragon Aspect Flight: flight toggled on");
				}
			}
			return true;
		}

		if (a_event.IsUp()) {
			return true;
		}

		return true;
	}

	void InputHandler::HandleThumbstickEvent(const State::ThumbstickInputSnapshot& a_event)
	{
		if (!a_event.IsLeft()) return;
		ApplyRadialThumbstickDeadzone(a_event.x, a_event.y, _thumbstickStrafeInput, _thumbstickForwardInput);
		UpdateMovementInput();
	}

	void InputHandler::ResetForLifecycle(
		std::uint64_t a_session,
		std::string_view a_reason)
	{
		const auto lifecycle = State::DecideInputLifecycleReset(a_session);
		std::uint32_t pendingChannels = 0;
		std::uint32_t pendingReleaseChannels = 0;
		std::uint64_t oldPublishedSession = 0;
		std::uint64_t sequenceDomain = 0;
		{
			std::scoped_lock lock(_inputMutex, _inputActionCoalescingMutex, _runtimeMutex);
			oldPublishedSession = _publishedFlightSession.load(std::memory_order_acquire);
			for (const auto& state : _inputActionCoalescingStates) {
				pendingChannels += state.suppressed.pending ? 1U : 0U;
			}
			for (auto& pending : _pendingReleases) {
				pendingReleaseChannels += pending.pending ? 1U : 0U;
				pending = {};
			}
			_keyboardForwardInput = 0.0F;
			_keyboardStrafeInput = 0.0F;
			_thumbstickForwardInput = 0.0F;
			_thumbstickStrafeInput = 0.0F;
			_launchHeld = false;
			_ascendHeld = false;
			_descendHeld = false;
			_readyWeaponHeld = false;
			_shoutHeld = false;
			_shoutHeldDiagnosticLogged = false;
			_blockHeld = false;
			_boostHeld = false;
			for (auto& state : _inputActionCoalescingStates) {
				state = {};
			}
			++_inputActionSequenceDomain;
			sequenceDomain = _inputActionSequenceDomain;
			_runtimeState = {};
			_runtimeState.publishedFlightSession = lifecycle.publishedSession;
			_lastLoggedRuntimeState = {};
			_runtimeStateLogInitialized = false;
			_lastRuntimeStateLog = {};
			_deferredButtonEvents = {};
			_deferredButtonEventCount = 0;
			_dispatchBoundaryActive = false;
			_dispatchBoundaryState = {};
			_runtimeRefreshQueued.store(0, std::memory_order_release);
			_publishedFlightSession.store(lifecycle.publishedSession, std::memory_order_release);
			_suppressedInputCount.store(0, std::memory_order_release);
		}
		logger::info(
			"event=input_lifecycle_reset schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
			"first_session={} last_session={} first_input_sequence_domain={} last_input_sequence_domain={} "
			"manager_sequence=0 outcome=reset reason={} old_published_session={} new_published_session={} "
			"snapshot_valid={} deferred_cleared={} pending_channels={} pending_release_channels={} "
			"sequence_domain={} refresh_requested={}",
			State::StructuredDiagnosticSchemaVersion,
			oldPublishedSession,
			lifecycle.publishedSession,
			sequenceDomain,
			sequenceDomain,
			a_reason,
			oldPublishedSession,
			lifecycle.publishedSession,
			lifecycle.snapshotValid,
			lifecycle.deferredCleared,
			pendingChannels,
			pendingReleaseChannels,
			sequenceDomain,
			lifecycle.requestRefresh);
		if (lifecycle.requestRefresh) {
			QueueGameThreadStateRefresh();
		}
	}

	bool InputHandler::IsSemanticButtonEdge(const State::ButtonInputSnapshot& a_event)
	{
		return State::IsSemanticButtonEdge(a_event);
	}

	bool InputHandler::IsReadyButtonEdge(const State::ButtonInputSnapshot& a_event)
	{
		if (!IsSemanticButtonEdge(a_event)) {
			return false;
		}
		return a_event.userEvent == ReadyWeaponUserEvent ||
			a_event.userEvent == ReadyWeaponCompactUserEvent ||
			a_event.userEvent == WeaponDrawUserEvent ||
			a_event.userEvent == DrawWeaponUserEvent ||
			a_event.userEvent == WeaponSheatheUserEvent ||
			a_event.userEvent == SheatheWeaponUserEvent;
	}

	void InputHandler::DeferButtonEvent(
		const State::ButtonInputSnapshot& a_event,
		std::uint64_t a_sourceSession,
		std::uint64_t a_actionId,
		bool a_rebindSession,
		bool a_expectedFlying)
	{
		std::uint32_t readyCount = 0;
		for (std::uint32_t i = 0; i < _deferredButtonEventCount; ++i) {
			readyCount += IsReadyButtonEdge(_deferredButtonEvents[i].event) ? 1U : 0U;
		}
		const auto decision = State::DecideDeferredInputQueue(
			State::DeferredInputQueueState{
				_deferredButtonEventCount,
				readyCount != 0,
				readyCount },
			IsSemanticButtonEdge(a_event),
			IsReadyButtonEdge(a_event),
			static_cast<std::uint32_t>(DeferredButtonCapacity));
		if (!decision.enqueue) {
			logger::warn(
				"event=input_deferred_drop schema={} action_id={} session={} input_sequence_domain=0 manager_sequence=0 "
				"action={} outcome=dropped reason=ready_priority_queue_full semantic_edge=true ready_edge={} count={} ready_count={}",
				State::StructuredDiagnosticSchemaVersion,
				a_actionId,
				a_sourceSession,
				a_event.userEvent,
				IsReadyButtonEdge(a_event),
				_deferredButtonEventCount,
				readyCount);
			return;
		}

		std::uint32_t index = _deferredButtonEventCount;
		if (decision.replaceOldest) {
			index = 0;
			bool foundNonReady = false;
			for (std::uint32_t i = 0; i < _deferredButtonEventCount; ++i) {
				if (!IsReadyButtonEdge(_deferredButtonEvents[i].event)) {
					index = i;
					foundNonReady = true;
					break;
				}
			}
			if (!foundNonReady && !IsReadyButtonEdge(a_event)) {
				logger::warn(
					"event=input_deferred_drop schema={} action_id={} session={} input_sequence_domain=0 manager_sequence=0 "
					"action={} outcome=dropped reason=ready_priority_queue_full semantic_edge=true ready_edge=false count={} ready_count={}",
					State::StructuredDiagnosticSchemaVersion,
					a_actionId,
					a_sourceSession,
					a_event.userEvent,
					_deferredButtonEventCount,
					readyCount);
				return;
			}
		}
		_deferredButtonEvents[index] = DeferredButtonEvent{
			a_event,
			a_sourceSession,
			a_actionId,
			a_rebindSession,
			a_expectedFlying };
		if (_deferredButtonEventCount < DeferredButtonCapacity) {
			++_deferredButtonEventCount;
		}
		logger::info(
			"event=input_deferred_enqueue schema={} action_id={} session={} input_sequence_domain=0 manager_sequence=0 "
			"action={} outcome=queued reason={} count={} replaced_oldest={} ready_edge={} source_session={} rebind_session={} expected_flying={}",
			State::StructuredDiagnosticSchemaVersion, a_actionId, a_sourceSession, a_event.userEvent,
			a_rebindSession ? "runtime_boundary" : "runtime_snapshot_invalid",
			_deferredButtonEventCount,
			decision.replaceOldest,
			IsReadyButtonEdge(a_event),
			a_sourceSession,
			a_rebindSession,
			a_expectedFlying);
	}

	void InputHandler::FlushDeferredButtonEventsLocked(
		std::string_view a_reason,
		bool a_preserveBoundaryRebind)
	{
		const auto before = _deferredButtonEventCount;
		if (before == 0) {
			return;
		}
		const auto firstDeferredEvent = _deferredButtonEvents[0];
		const auto lastDeferredEvent = _deferredButtonEvents[before - 1];

		std::uint32_t kept = 0;
		if (a_preserveBoundaryRebind) {
			for (std::uint32_t i = 0; i < before; ++i) {
				if (_deferredButtonEvents[i].rebindSession) {
					if (kept != i) {
						_deferredButtonEvents[kept] = std::move(_deferredButtonEvents[i]);
					}
					++kept;
				}
			}
		}
		for (std::uint32_t i = kept; i < before; ++i) {
			_deferredButtonEvents[i] = {};
		}
		_deferredButtonEventCount = kept;

		logger::info(
			"event=input_deferred_flush schema={} correlation=aggregate first_action_id={} last_action_id={} "
			"first_session={} last_session={} first_input_sequence_domain=0 last_input_sequence_domain=0 "
			"manager_sequence=0 outcome=flushed reason={} before={} kept_boundary_rebind={} dropped={}",
			State::StructuredDiagnosticSchemaVersion,
			firstDeferredEvent.actionId,
			lastDeferredEvent.actionId,
			firstDeferredEvent.sourceSession,
			lastDeferredEvent.sourceSession,
			a_reason,
			before,
			kept,
			before - kept);
	}

	void InputHandler::ReplayDeferredButtonEvents(
		const RuntimeState& a_runtime,
		std::uint64_t a_refreshGeneration)
	{
		DeferredButtonEvent item{};
		std::uint32_t pendingCount = 0;
		{
			std::scoped_lock lock(_inputMutex);
			pendingCount = _deferredButtonEventCount;
			if (pendingCount == 0) {
				return;
			}
			item = std::move(_deferredButtonEvents[0]);
			for (std::uint32_t i = 1; i < pendingCount; ++i) {
				_deferredButtonEvents[i - 1] = std::move(_deferredButtonEvents[i]);
			}
			_deferredButtonEvents[pendingCount - 1] = {};
			--_deferredButtonEventCount;
		}

		const auto replaySourceSession = item.rebindSession ?
			a_runtime.publishedFlightSession : item.sourceSession;
		const bool boundaryFresh = !item.rebindSession ||
			(a_runtime.publishedFlightSession != item.sourceSession) ||
			(a_runtime.flying == item.expectedFlying);
		const auto decision = State::DecideDeferredInputReplay(
			pendingCount,
			a_runtime.valid,
			a_runtime.suppressInput,
			replaySourceSession,
			a_runtime.publishedFlightSession,
			item.rebindSession,
			boundaryFresh,
			item.boundaryDeferralAttempts);
		switch (decision.disposition) {
		case State::InputSnapshotDispatchDisposition::kDispatch:
			{
				std::scoped_lock lock(_inputMutex);
				struct ScopedDeferredCorrelation final {
					std::uint64_t& id;
					std::uint64_t& domain;
					std::uint64_t priorId;
					std::uint64_t priorDomain;
					~ScopedDeferredCorrelation() noexcept { id = priorId; domain = priorDomain; }
				} scope{ _activeInputCorrelationId, _activeInputSequenceDomain, _activeInputCorrelationId, _activeInputSequenceDomain };
				_activeInputCorrelationId = item.actionId;
				_activeInputSequenceDomain = 0;
				HandleButtonEvent(item.event, a_runtime);
			}
			logger::info(
				"event=input_deferred_replay schema={} action_id={} session={} input_sequence_domain=0 manager_sequence=0 "
				"action={} outcome=dispatched reason=handler_invoked count_before={} source_session={} current_session={} "
				"schedule_next_refresh={}",
				State::StructuredDiagnosticSchemaVersion,
				item.actionId,
				item.sourceSession,
				item.event.userEvent,
				pendingCount,
				item.sourceSession,
				a_runtime.publishedFlightSession,
				decision.scheduleNextRefresh);
			break;
		case State::InputSnapshotDispatchDisposition::kSuppress:
			_suppressedInputCount.fetch_add(1, std::memory_order_relaxed);
			{
				std::scoped_lock lock(_inputMutex);
				// Suppression is a terminal input boundary.  Do not replay the
				// remaining stale edges after the boundary has consumed the first one.
				_deferredButtonEvents = {};
				_deferredButtonEventCount = 0;
			}
			logger::info(
				"event=input_deferred_replay schema={} action_id={} session={} input_sequence_domain=0 manager_sequence=0 "
				"action={} outcome=suppressed reason=runtime_suppressed count_before={} source_session={} current_session={}",
				State::StructuredDiagnosticSchemaVersion,
				item.actionId,
				item.sourceSession,
				item.event.userEvent,
				pendingCount,
				item.sourceSession,
				a_runtime.publishedFlightSession);
			break;
		case State::InputSnapshotDispatchDisposition::kRejectSession:
			logger::info(
				"event=input_deferred_replay schema={} action_id={} session={} input_sequence_domain=0 manager_sequence=0 "
				"action={} outcome=rejected reason={} count_before={} source_session={} current_session={} "
				"boundary_attempts={} terminal_discard={}",
				State::StructuredDiagnosticSchemaVersion,
				item.actionId,
				item.sourceSession,
				item.event.userEvent,
				decision.terminalDiscard ? "boundary_deferral_limit" : "session_or_invalid",
				pendingCount,
				item.sourceSession,
				a_runtime.publishedFlightSession,
				item.boundaryDeferralAttempts,
				decision.terminalDiscard);
			break;
		case State::InputSnapshotDispatchDisposition::kDeferUntilRefresh:
			{
				item.boundaryDeferralAttempts = decision.nextBoundaryDeferralAttempts;
				std::scoped_lock lock(_inputMutex);
				// The item was removed above, so the current count—not the captured
				// pre-pop count—bounds this shift.  Using pendingCount here writes one
				// slot past the fixed-capacity array when a full queue defers again.
				for (std::uint32_t i = _deferredButtonEventCount; i > 0; --i) {
					_deferredButtonEvents[i] = std::move(_deferredButtonEvents[i - 1]);
				}
				_deferredButtonEvents[0] = std::move(item);
				++_deferredButtonEventCount;
			}
			logger::info(
				"event=input_deferred_replay schema={} action_id={} session={} input_sequence_domain=0 manager_sequence=0 "
				"action={} outcome=deferred reason=snapshot_invalid source_session={} current_session={}",
				State::StructuredDiagnosticSchemaVersion,
				item.actionId,
				item.sourceSession,
				item.event.userEvent,
				item.sourceSession,
				a_runtime.publishedFlightSession);
			break;
		}
		if (decision.scheduleNextRefresh) {
			// Only the callback's own generation may release its lease.  A direct
			// RefreshGameThreadState caller owns no lease and must leave any newer
			// queued refresh untouched; QueueGameThreadStateRefresh will coalesce
			// against it below.
			if (a_refreshGeneration != 0) {
				std::uint64_t expectedGeneration = a_refreshGeneration;
				_runtimeRefreshQueued.compare_exchange_strong(
					expectedGeneration,
					std::uint64_t{ 0 },
					std::memory_order_acq_rel,
					std::memory_order_acquire);
			}
			QueueGameThreadStateRefresh();
		}
	}

	void InputHandler::ResetFlightInputState(std::string_view a_reason, bool a_clearShout)
	{
		std::scoped_lock inputLock(_inputMutex);
		FlushDeferredButtonEventsLocked(
			a_reason,
			a_reason == "flight_stop");
		const bool shoutHeldBeforeReset = _shoutHeld;
		const bool blockHeldBeforeReset = _blockHeld;
		const bool hadLatch = _keyboardForwardInput != 0.0F ||
			_keyboardStrafeInput != 0.0F ||
			_thumbstickForwardInput != 0.0F ||
			_thumbstickStrafeInput != 0.0F ||
			_launchHeld || _ascendHeld || _descendHeld || _readyWeaponHeld ||
			_shoutHeld || _blockHeld || _boostHeld;
		const auto resetLatches = DragonAspectFlight::State::ResetInputLatches(
			DragonAspectFlight::State::InputLatchState{
				_launchHeld,
				_ascendHeld,
				_descendHeld,
				_readyWeaponHeld,
				_shoutHeld,
				_boostHeld },
			a_clearShout);
		_keyboardForwardInput = 0.0F;
		_keyboardStrafeInput = 0.0F;
		_thumbstickForwardInput = 0.0F;
		_thumbstickStrafeInput = 0.0F;
		_launchHeld = resetLatches.launchHeld;
		_ascendHeld = resetLatches.ascendHeld;
		_descendHeld = resetLatches.descendHeld;
		_readyWeaponHeld = resetLatches.readyWeaponHeld;
		_shoutHeld = resetLatches.shoutHeld;
		_blockHeld = false;
		_boostHeld = resetLatches.boostHeld;
		if (a_clearShout) {
			_shoutHeldDiagnosticLogged = false;
		}
		if (blockHeldBeforeReset) {
			QueueFlightAction(State::FlightInputAction::kBlockRelease);
		}
		if (a_clearShout) {
			QueueFlightAction(State::FlightInputAction::kClearShout);
		}
		QueueFlightAction(State::FlightInputActionSnapshot{
			State::FlightInputAction::kSetBoostHeld,
			0.0F,
			0.0F,
			false });
		if (hadLatch) {
			logger::info(
				"event=input_latches_reset schema={} correlation=aggregate first_action_id=0 last_action_id=0 "
				"first_session={} last_session={} first_input_sequence_domain={} last_input_sequence_domain={} "
				"manager_sequence=0 outcome=reset reason={} clear_shout={} shout_held_before_reset={}",
				State::StructuredDiagnosticSchemaVersion,
				_publishedFlightSession.load(std::memory_order_acquire),
				_publishedFlightSession.load(std::memory_order_acquire),
				_activeInputSequenceDomain,
				_activeInputSequenceDomain,
				a_reason,
				a_clearShout,
				shoutHeldBeforeReset);
		}
		UpdateVerticalInput();
		UpdateMovementInput();
	}

	bool InputHandler::QueueFlightAction(
		State::FlightInputAction a_action,
		bool a_forceStateEmission)
	{
		State::FlightInputActionSnapshot action{ a_action };
		if (a_action == State::FlightInputAction::kBeginCombat ||
			a_action == State::FlightInputAction::kBlockRequest ||
			a_action == State::FlightInputAction::kBlockRelease) {
			// Combat/block edges are captured on the input side before the queued task
			// can observe a replacement weapon.  Lifecycle cleanup may remain
			// uncaptured; FlightManager rejects a missing epoch for equipment-bound
			// actions before changing transition state.
			RuntimeState runtime;
			{
				std::shared_lock lock(_runtimeMutex);
				runtime = _runtimeState;
			}
			if (runtime.playerEquipmentIdentityCaptured) {
				action.equipmentIdentity = runtime.playerEquipmentIdentity;
				action.equipmentIdentityCaptured = true;
				action.equipmentIdentityEpoch = runtime.playerEquipmentIdentityEpoch;
			}
		}
		return QueueFlightAction(action, a_forceStateEmission);
	}

	bool InputHandler::QueueFlightAction(
		State::FlightInputActionSnapshot a_action,
		bool a_forceStateEmission)
	{
		if (IsPendingReleaseReplacement(a_action.action)) {
			// A new locally-owned request/press, or a newer release cleanup, replaces
			// an older pending edge before it can be replayed out of order.
			ClearPendingReleaseForNewInput(a_action.action);
		}
		// Stamp the origin before the outer task is queued.  The task may execute
		// after another flight session has started; FlightManager must validate
		// this immutable envelope rather than recapture the then-current session.
		a_action.sourceSession = _publishedFlightSession.load(std::memory_order_acquire);
		if (a_action.actionId == 0) {
			a_action.actionId = _activeInputCorrelationId != 0 ?
				_activeInputCorrelationId : AllocateInputCorrelationId();
		}
		logger::info(
			"event=input_received schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
			"action={} outcome=observed reason=input_boundary classification=normal_or_power_or_bash_boundary",
			State::StructuredDiagnosticSchemaVersion,
			a_action.actionId,
			a_action.sourceSession,
			a_action.inputSequenceDomain,
			InputActionName(a_action.action));
		const auto dispatchBoundaryStateBefore = _dispatchBoundaryState;
		const bool dispatchBoundaryWasActive = _dispatchBoundaryActive;
		const auto retireReadyToken = [a_action](std::string_view a_reason) noexcept {
			if (!State::IsReadyGenerationTokenValid(a_action.readyToken)) {
				return;
			}
			try {
				FlightManager::GetSingleton().RetireReadyGeneration(
					a_action.readyToken,
					a_reason,
					a_action.sourceSession);
			} catch (...) {
				// Ready-token cleanup is best effort on a failed outer queue path.  Do
				// not let diagnostics or state retirement escape the input callback.
			}
		};
		const auto resetCoalescingAfterQueueFailure = [this, &a_action](std::string_view a_reason) noexcept {
			if (a_action.action != State::FlightInputAction::kStopFlight) {
				return;
			}
			try {
				FlushInputActionCoalescing(a_reason, true);
			} catch (...) {
				// The queue failure has already been contained; logging must not reopen it.
			}
		};

		const SKSE::TaskInterface* taskInterface = nullptr;
		try {
			taskInterface = SKSE::GetTaskInterface();
		} catch (const std::exception& e) {
			retireReadyToken("input_get_task_interface_exception");
			resetCoalescingAfterQueueFailure("flight_stop_queue_exception");
			const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
			if (failure.emit) {
				logger::error(
					"event=input_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
						"action={} outcome=queue_failed reason=get_task_interface_exception exception=get_task_interface source_session={} "
						"ready_generation={} count={} lease_released=true error={}",
					State::StructuredDiagnosticSchemaVersion, a_action.actionId, a_action.sourceSession,
					a_action.inputSequenceDomain, InputActionName(a_action.action),
					a_action.sourceSession,
					a_action.readyToken.generation,
					failure.count,
					e.what());
			}
			ArmPendingRelease(a_action, "input_get_task_interface_exception");
			return false;
		} catch (...) {
			retireReadyToken("input_get_task_interface_exception");
			resetCoalescingAfterQueueFailure("flight_stop_queue_exception");
			const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
			if (failure.emit) {
				logger::error(
					"event=input_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
						"action={} outcome=queue_failed reason=get_task_interface_exception exception=get_task_interface source_session={} "
						"ready_generation={} count={} lease_released=true error=unknown",
					State::StructuredDiagnosticSchemaVersion, a_action.actionId, a_action.sourceSession,
					a_action.inputSequenceDomain, InputActionName(a_action.action),
					a_action.sourceSession,
					a_action.readyToken.generation,
					failure.count);
			}
			ArmPendingRelease(a_action, "input_get_task_interface_exception");
			return false;
		}
		if (!taskInterface) {
			retireReadyToken("input_task_interface_unavailable");
			resetCoalescingAfterQueueFailure("flight_stop_queue_unavailable");
			logger::warn(
				"event=input_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
				"action={} outcome=queue_failed queued=false reason=task_interface_unavailable",
				State::StructuredDiagnosticSchemaVersion,
				a_action.actionId,
				a_action.sourceSession,
				a_action.inputSequenceDomain,
				InputActionName(a_action.action));
			ArmPendingRelease(a_action, "input_task_interface_unavailable");
			return false;
		}

		if (a_action.action == State::FlightInputAction::kStopFlight) {
			FlushInputActionCoalescing("flight_stop", true);
		}

		bool enqueue = true;
		std::uint64_t sequenceDomain = 0;
		std::array<
			State::InputActionSuppressionSummary,
			State::InputActionCoalescingChannelCount> flushedSummaries{};
		std::size_t flushedSummaryCount = 0;
		std::string_view suppressionReason =
			a_forceStateEmission ? "input_edge" : "state_change";
		if (State::IsCoalescibleInputAction(a_action.action)) {
			const auto channel = State::GetInputActionCoalescingChannel(a_action.action);
			{
				std::scoped_lock lock(_inputActionCoalescingMutex);
				sequenceDomain = _inputActionSequenceDomain;
				const auto decision = State::ReduceInputActionForQueue(
					_inputActionCoalescingStates[channel],
					a_action,
					InputActionTimestampMilliseconds(),
					_inputActionSequenceDomain,
					a_forceStateEmission);
				_inputActionCoalescingStates[channel] = decision.state;
				enqueue = decision.enqueue;
				if (decision.flushSummary) {
					flushedSummaries[flushedSummaryCount++] = decision.flushedSummary;
				}
			}
		} else {
			suppressionReason = "control_edge";
			{
				std::scoped_lock lock(_inputActionCoalescingMutex);
				// Flush every channel while the current sequence domain still owns
				// its summaries.  Only after the flush state is captured may this
				// ordering-sensitive edge advance the domain.
				for (auto& state : _inputActionCoalescingStates) {
					const auto decision = State::FlushInputActionCoalescing(state, false);
					state = decision.state;
					if (decision.flushSummary) {
						flushedSummaries[flushedSummaryCount++] = decision.flushedSummary;
					}
				}
				++_inputActionSequenceDomain;
				sequenceDomain = _inputActionSequenceDomain;
			}
		}
		a_action.inputSequenceDomain = sequenceDomain;
		_activeInputSequenceDomain = sequenceDomain;

		// Logging occurs outside the coalescing mutex.  The summary state has
		// already been consumed and the edge's sequence domain is now exact.
		for (std::size_t i = 0; i < flushedSummaryCount; ++i) {
			LogInputActionSuppressionSummary(flushedSummaries[i], suppressionReason);
		}
		if (!enqueue) {
			logger::info(
				"event=input_suppressed schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
				"action={} outcome=suppressed reason=coalesced",
				State::StructuredDiagnosticSchemaVersion,
				a_action.actionId,
				a_action.sourceSession,
				a_action.inputSequenceDomain,
				InputActionName(a_action.action));
			return true;
		}
		if (_dispatchBoundaryActive && State::IsRuntimeSessionBoundaryAction(a_action.action)) {
			_dispatchBoundaryState = State::ReduceRuntimeDispatchBoundary(
				_dispatchBoundaryState,
				a_action.action,
				true).state;
		}

		// InputHandler is a process-lifetime singleton.  Capturing this is
		// intentional; the callback carries only immutable action/session/domain
		// state, and FlightManager performs session validation before engine work.
		try {
			logger::info(
				"event=action_enqueued schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
				"action={} outcome=queued reason=task_submission",
				State::StructuredDiagnosticSchemaVersion,
				a_action.actionId,
				a_action.sourceSession,
				a_action.inputSequenceDomain,
				InputActionName(a_action.action));
			taskInterface->AddTask([this, a_action, sequenceDomain]() {
				try {
					const bool managerQueued = FlightManager::GetSingleton().QueueFlightAction(a_action);
					if (!managerQueued) {
						ArmPendingRelease(a_action, "manager_queue_failed");
						const bool recovered = RecoverFlightActionQueueFailure(
							a_action,
							sequenceDomain,
							"manager_queue_failed");
						const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
						if (failure.emit) {
							logger::warn(
								"event=input_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
									"action={} outcome=queue_failed queued=false reason=manager_queue_failed "
									"source_session={} ready_generation={} count={} recovered={}",
								State::StructuredDiagnosticSchemaVersion,
								a_action.actionId,
								a_action.sourceSession,
								a_action.inputSequenceDomain,
								InputActionName(a_action.action),
								a_action.sourceSession,
								a_action.readyToken.generation,
								failure.count,
								recovered);
						}
					}
				} catch (const std::exception& e) {
					ArmPendingRelease(a_action, "input_action_callback_exception");
					const bool recovered = RecoverFlightActionQueueFailure(
						a_action,
						sequenceDomain,
						"input_action_callback_exception");
					try {
						if (State::IsReadyGenerationTokenValid(a_action.readyToken)) {
							FlightManager::GetSingleton().RetireReadyGeneration(
								a_action.readyToken,
								"input_action_callback_exception",
								a_action.sourceSession);
						}
					} catch (...) {
						// The callback must not escape if token retirement itself fails.
					}
					const auto failure = NextExceptionLog(_flightActionTaskExceptionCount);
					if (failure.emit) {
						logger::error(
							"event=input_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
								"action={} outcome=queue_failed reason=input_action_callback_exception exception=callback "
								"source_session={} ready_generation={} count={} recovered={} error={}",
							State::StructuredDiagnosticSchemaVersion,
							a_action.actionId,
							a_action.sourceSession,
							a_action.inputSequenceDomain,
							InputActionName(a_action.action),
							a_action.sourceSession,
							a_action.readyToken.generation,
							failure.count,
							recovered,
							e.what());
					}
				} catch (...) {
					ArmPendingRelease(a_action, "input_action_callback_exception");
					const bool recovered = RecoverFlightActionQueueFailure(
						a_action,
						sequenceDomain,
						"input_action_callback_exception");
					try {
						if (State::IsReadyGenerationTokenValid(a_action.readyToken)) {
							FlightManager::GetSingleton().RetireReadyGeneration(
								a_action.readyToken,
								"input_action_callback_exception",
								a_action.sourceSession);
						}
					} catch (...) {
						// The callback must not escape if token retirement itself fails.
					}
					const auto failure = NextExceptionLog(_flightActionTaskExceptionCount);
					if (failure.emit) {
						logger::error(
							"event=input_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
								"action={} outcome=queue_failed reason=input_action_callback_exception exception=callback "
								"source_session={} ready_generation={} count={} recovered={} error=unknown",
							State::StructuredDiagnosticSchemaVersion,
							a_action.actionId,
							a_action.sourceSession,
							a_action.inputSequenceDomain,
							InputActionName(a_action.action),
							a_action.sourceSession,
							a_action.readyToken.generation,
							failure.count,
							recovered);
					}
				}
			});
		} catch (const std::exception& e) {
			ArmPendingRelease(a_action, "input_add_task_exception");
			retireReadyToken("input_add_task_exception");
			const bool recovered = RecoverFlightActionQueueFailure(
				a_action,
				sequenceDomain,
				"input_add_task_exception");
			if (dispatchBoundaryWasActive &&
				State::IsRuntimeSessionBoundaryAction(a_action.action)) {
				_dispatchBoundaryState = dispatchBoundaryStateBefore;
			}
			const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
			if (failure.emit) {
				logger::error(
					"event=input_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
						"action={} outcome=queue_failed reason=input_add_task_exception exception=add_task "
						"source_session={} ready_generation={} count={} recovered={} boundary_restored=true error={}",
					State::StructuredDiagnosticSchemaVersion,
					a_action.actionId,
					a_action.sourceSession,
					a_action.inputSequenceDomain,
					InputActionName(a_action.action),
					a_action.sourceSession,
					a_action.readyToken.generation,
					failure.count,
					recovered,
					e.what());
			}
			return false;
		} catch (...) {
			ArmPendingRelease(a_action, "input_add_task_exception");
			retireReadyToken("input_add_task_exception");
			const bool recovered = RecoverFlightActionQueueFailure(
				a_action,
				sequenceDomain,
				"input_add_task_exception");
			if (dispatchBoundaryWasActive &&
				State::IsRuntimeSessionBoundaryAction(a_action.action)) {
				_dispatchBoundaryState = dispatchBoundaryStateBefore;
			}
			const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
			if (failure.emit) {
				logger::error(
					"event=input_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
						"action={} outcome=queue_failed reason=input_add_task_exception exception=add_task "
						"source_session={} ready_generation={} count={} recovered={} boundary_restored=true error=unknown",
					State::StructuredDiagnosticSchemaVersion,
					a_action.actionId,
					a_action.sourceSession,
					a_action.inputSequenceDomain,
					InputActionName(a_action.action),
					a_action.sourceSession,
					a_action.readyToken.generation,
					failure.count,
					recovered);
			}
			return false;
		}
		return true;
	}

	void InputHandler::NotifyFlightActionQueueFailure(
		const State::FlightInputActionSnapshot& a_action,
		std::string_view a_reason) noexcept
	{
		ArmPendingRelease(a_action, a_reason);
	}

	void InputHandler::ArmPendingRelease(
		const State::FlightInputActionSnapshot& a_action,
		std::string_view a_reason) noexcept
	{
		if (!IsPendingReleaseAction(a_action.action)) {
			return;
		}

		try {
			const auto channel = GetPendingReleaseChannel(a_action.action);
			if (channel >= PendingReleaseChannelCount) {
				return;
			}
			State::FlightInputActionSnapshot action = a_action;
			const auto currentSession = _publishedFlightSession.load(std::memory_order_acquire);
			if (action.sourceSession == State::FlightInputActionSnapshot::UncapturedSession) {
				action.sourceSession = currentSession;
			}

			bool discardedForSession = false;
			bool discardedForNewInput = false;
			std::uint32_t failureCount = 0;
			bool armed = false;
			{
				std::scoped_lock lock(_inputMutex);
				const auto lockedSession = _publishedFlightSession.load(std::memory_order_acquire);
				if (action.sourceSession != lockedSession) {
					discardedForSession = true;
				} else if ((channel == 0 && _blockHeld) ||
					(channel == 1 && _shoutHeld)) {
					// A newer locally-owned press/request supersedes an old failed
					// release.  Re-arming it would clear the new intent on replay.
					discardedForNewInput = true;
				} else {
					auto& pending = _pendingReleases[channel];
					pending.action = action;
					pending.pending = true;
					if (pending.failureCount < std::numeric_limits<std::uint32_t>::max()) {
						++pending.failureCount;
					}
					failureCount = pending.failureCount;
					armed = true;
				}
			}

			if (discardedForSession || discardedForNewInput) {
				try {
					logger::info(
						"event=input_release_recovery schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
						"action={} outcome=discarded reason={} source_session={} current_session={} superseded_by_local_input={}",
						State::StructuredDiagnosticSchemaVersion,
						action.actionId,
						action.sourceSession,
						action.inputSequenceDomain,
						InputActionName(action.action),
						a_reason,
						action.sourceSession,
						currentSession,
						discardedForNewInput);
				} catch (...) {
					// Diagnostics must not escape queue recovery.
				}
				return;
			}

			if (armed && (failureCount == 1 || failureCount % 8 == 0)) {
				try {
					logger::warn(
						"event=input_release_recovery schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
						"action={} outcome=armed reason={} source_session={} failure_count={} retry=next_input_dispatch",
						State::StructuredDiagnosticSchemaVersion,
						action.actionId,
						action.sourceSession,
						action.inputSequenceDomain,
						InputActionName(action.action),
						a_reason,
						action.sourceSession,
						failureCount);
				} catch (...) {
					// Diagnostics must not escape queue recovery.
				}
			}
		} catch (...) {
			// A failed diagnostic or lock operation cannot be allowed to escape the
			// task callback and must not mutate gameplay state.
		}
	}

	void InputHandler::ClearPendingReleaseForNewInput(
		State::FlightInputAction a_action) noexcept
	{
		if (!IsPendingReleaseReplacement(a_action)) {
			return;
		}
		try {
			const auto channel = GetPendingReleaseChannel(a_action);
			if (channel >= PendingReleaseChannelCount) {
				return;
			}
			State::FlightInputActionSnapshot oldActionSnapshot{};
			bool cleared = false;
			{
				std::scoped_lock lock(_inputMutex);
				auto& pending = _pendingReleases[channel];
				if (pending.pending) {
					oldActionSnapshot = pending.action;
					pending = {};
					cleared = true;
				}
			}
			if (cleared) {
				try {
					logger::info(
						"event=input_release_recovery schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
						"action={} outcome=replaced reason=new_local_input old_action={} new_action={}",
						State::StructuredDiagnosticSchemaVersion,
						oldActionSnapshot.actionId,
						oldActionSnapshot.sourceSession,
						oldActionSnapshot.inputSequenceDomain,
						InputActionName(oldActionSnapshot.action),
						InputActionName(oldActionSnapshot.action),
						InputActionName(a_action));
				} catch (...) {
					// Diagnostics must not escape queue recovery.
				}
			}
		} catch (...) {
			// The input callback remains contained if recovery bookkeeping fails.
		}
	}

	void InputHandler::RetryPendingReleasesLocked() noexcept
	{
		for (std::size_t channel = 0; channel < PendingReleaseChannelCount; ++channel) {
			if (!_pendingReleases[channel].pending) {
				continue;
			}

			const auto action = _pendingReleases[channel].action;
			try {
				const auto currentSession = _publishedFlightSession.load(std::memory_order_acquire);
				if (action.sourceSession != currentSession) {
					_pendingReleases[channel] = {};
					try {
						logger::info(
							"event=input_release_recovery schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
							"action={} outcome=discarded reason=session_changed source_session={} current_session={}",
							State::StructuredDiagnosticSchemaVersion,
							action.actionId,
							action.sourceSession,
							action.inputSequenceDomain,
							InputActionName(action.action),
							action.sourceSession,
							currentSession);
					} catch (...) {
						// Diagnostics must not escape queue recovery.
					}
					continue;
				}

				// QueueFlightAction clears this slot before submission.  A submission
				// failure re-arms it, while a later manager-task failure calls back into
				// ArmPendingRelease with the same session envelope.
				const bool queued = QueueFlightAction(action, true);
				if (!queued && !_pendingReleases[channel].pending) {
					ArmPendingRelease(action, "pending_release_retry_failed");
				}
				if (queued) {
					try {
						logger::info(
							"event=input_release_recovery schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
							"action={} outcome=retry_queued reason=pending_release_retry source_session={}",
							State::StructuredDiagnosticSchemaVersion,
							action.actionId,
							action.sourceSession,
							action.inputSequenceDomain,
							InputActionName(action.action),
							action.sourceSession);
					} catch (...) {
						// Diagnostics must not escape queue recovery.
					}
				}
			} catch (...) {
				ArmPendingRelease(action, "pending_release_retry_exception");
			}
		}
	}

	bool InputHandler::RecoverFlightActionQueueFailure(
		const State::FlightInputActionSnapshot& a_action,
		std::uint64_t a_sequenceDomain,
		std::string_view a_reason) noexcept
	{
		try {
			bool recovered = false;
			std::array<
				State::InputActionSuppressionSummary,
				State::InputActionCoalescingChannelCount> flushedSummaries{};
			std::size_t flushedSummaryCount = 0;
			const auto currentSession = _publishedFlightSession.load(std::memory_order_acquire);
			{
				std::scoped_lock lock(_inputActionCoalescingMutex);
				if (a_action.action == State::FlightInputAction::kStopFlight) {
					// A stale stop callback must not clear coalescing state belonging to
					// a later session.  A same-session stop failure is terminal input
					// cleanup, even though its manager action never reached the queue.
					if (a_action.sourceSession == currentSession) {
						++_inputActionSequenceDomain;
						for (auto& state : _inputActionCoalescingStates) {
							const auto decision = State::FlushInputActionCoalescing(state, true);
							state = decision.state;
							if (decision.flushSummary) {
								flushedSummaries[flushedSummaryCount++] = decision.flushedSummary;
							}
						}
						recovered = true;
					}
				} else if (State::IsCoalescibleInputAction(a_action.action)) {
					const auto channel = State::GetInputActionCoalescingChannel(a_action.action);
					auto& state = _inputActionCoalescingStates[channel];
					// Roll back only the exact accepted state that belongs to this
					// queue attempt.  A delayed failure cannot erase a newer, different
					// state action queued by the same producer.
					const bool exactQueuedState = state.initialized &&
						state.sequenceDomain == a_sequenceDomain &&
						state.lastAccepted.sourceSession == a_action.sourceSession &&
						State::AreInputActionStatesEquivalent(state.lastAccepted, a_action);
					if (exactQueuedState) {
						const auto decision = State::FlushInputActionCoalescing(state, true);
						state = decision.state;
						if (decision.flushSummary) {
							flushedSummaries[flushedSummaryCount++] = decision.flushedSummary;
						}
						recovered = true;
					}
				}
			}

			for (std::size_t i = 0; i < flushedSummaryCount; ++i) {
				try {
					LogInputActionSuppressionSummary(flushedSummaries[i], a_reason);
				} catch (...) {
					// Diagnostics must never turn queue recovery into another failure.
				}
			}
			return recovered;
		} catch (...) {
			return false;
		}
	}

	void InputHandler::FlushInputActionCoalescing(
		std::string_view a_reason,
		bool a_resetBaseline)
	{
		std::array<
			State::InputActionSuppressionSummary,
			State::InputActionCoalescingChannelCount> summaries{};
		std::size_t summaryCount = 0;
		{
			std::scoped_lock lock(_inputActionCoalescingMutex);
			if (a_resetBaseline) {
				++_inputActionSequenceDomain;
			}
			for (auto& state : _inputActionCoalescingStates) {
				const auto decision = State::FlushInputActionCoalescing(
					state,
					a_resetBaseline);
				state = decision.state;
				if (decision.flushSummary) {
					summaries[summaryCount++] = decision.flushedSummary;
				}
			}
		}

		for (std::size_t i = 0; i < summaryCount; ++i) {
			LogInputActionSuppressionSummary(summaries[i], a_reason);
		}
	}

	void InputHandler::LogInputActionSuppressionSummary(
		const State::InputActionSuppressionSummary& a_summary,
		std::string_view a_reason) const
	{
		if (!a_summary.pending || a_summary.count == 0) {
			return;
		}
		logger::info(
			"event=input_action_suppressed_summary schema={} correlation=aggregate "
			"first_action_id={} last_action_id={} first_session={} last_session={} "
			"first_input_sequence_domain={} last_input_sequence_domain={} manager_sequence=0 "
			"outcome=suppressed reason={} count={} "
			"first_timestamp_ms={} last_timestamp_ms={} "
			"first_action={} last_action={} "
			"first_source_session={} last_source_session={} "
			"first_sequence_domain={} last_sequence_domain={} "
			"first_value_a={:.4f} first_value_b={:.4f} first_flag={} "
			"last_value_a={:.4f} last_value_b={:.4f} last_flag={}",
			State::StructuredDiagnosticSchemaVersion,
			a_summary.firstActionId,
			a_summary.lastActionId,
			a_summary.firstSourceSession,
			a_summary.lastSourceSession,
			a_summary.firstSequenceDomain,
			a_summary.lastSequenceDomain,
			a_reason,
			a_summary.count,
			a_summary.firstTimestampMs,
			a_summary.lastTimestampMs,
			InputActionName(a_summary.firstAction),
			InputActionName(a_summary.lastAction),
			a_summary.firstSourceSession,
			a_summary.lastSourceSession,
			a_summary.firstSequenceDomain,
			a_summary.lastSequenceDomain,
			a_summary.firstValueA,
			a_summary.firstValueB,
			a_summary.firstFlag,
			a_summary.lastValueA,
			a_summary.lastValueB,
			a_summary.lastFlag);
	}

	std::uint64_t InputHandler::AllocateInputCorrelationId() noexcept
	{
		return _nextDiagnosticActionId.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	bool InputHandler::QueueInputDiagnostic(
		std::string_view a_action,
		const State::ButtonInputSnapshot& a_event,
		std::string_view a_outcome,
		std::uint64_t a_actionId)
	{
		try {
			State::InputDiagnosticSnapshot snapshot;
			snapshot.action = a_action;
			snapshot.userEvent = a_event.userEvent;
			snapshot.outcome = a_outcome;
			snapshot.device = a_event.device;
			snapshot.code = a_event.code;
			snapshot.heldDuration = a_event.heldDuration;
			snapshot.actionId = a_actionId != 0 ? a_actionId :
				(_activeInputCorrelationId != 0 ? _activeInputCorrelationId : AllocateInputCorrelationId());
			snapshot.inputSequenceDomain = _activeInputSequenceDomain;
			snapshot.phase = a_event.IsUp() ? State::InputButtonPhase::kUp :
				(a_event.IsDown() ? State::InputButtonPhase::kDown :
					(a_event.IsHeld() ? State::InputButtonPhase::kHeld :
						(a_event.IsPressed() ? State::InputButtonPhase::kPressed :
							State::InputButtonPhase::kOther)));

			const SKSE::TaskInterface* taskInterface = SKSE::GetTaskInterface();
			if (!taskInterface) {
				LogInputBoundaryException(
					InputDiagnosticQueueExceptionCount,
					"input_diagnostic",
					"queue",
					"task_interface_unavailable",
					"unavailable");
				return false;
			}
			taskInterface->AddTask([snapshot = std::move(snapshot)]() {
				try {
					FlightManager::GetSingleton().LogInputDiagnostic(snapshot);
				} catch (const std::exception& e) {
					LogInputBoundaryException(
						InputDiagnosticTaskExceptionCount,
						"input_diagnostic",
						"callback",
						"log",
						e.what());
				} catch (...) {
					LogInputBoundaryException(
						InputDiagnosticTaskExceptionCount,
						"input_diagnostic",
						"callback",
						"log",
						"unknown");
				}
			});
			return true;
		} catch (const std::exception& e) {
			LogInputBoundaryException(
				InputDiagnosticQueueExceptionCount,
				"input_diagnostic",
				"queue",
				a_action,
				e.what());
			return false;
		} catch (...) {
			LogInputBoundaryException(
				InputDiagnosticQueueExceptionCount,
				"input_diagnostic",
				"queue",
				a_action,
				"unknown");
			return false;
		}
	}

	void InputHandler::UpdateMovementInput(bool a_forceEmission)
	{
		QueueFlightAction(State::FlightInputActionSnapshot{
			State::FlightInputAction::kSetMovementInput,
			std::clamp(_keyboardForwardInput + _thumbstickForwardInput, -1.0F, 1.0F),
			std::clamp(_keyboardStrafeInput + _thumbstickStrafeInput, -1.0F, 1.0F),
			false },
			a_forceEmission);
	}

	void InputHandler::UpdateVerticalInput(bool a_forceEmission)
	{
		const float verticalInput =
			(_ascendHeld ? 1.0F : 0.0F) +
			(_descendHeld ? -1.0F : 0.0F);

		QueueFlightAction(State::FlightInputActionSnapshot{
			State::FlightInputAction::kSetVerticalInput,
			std::clamp(verticalInput, -1.0F, 1.0F),
			0.0F,
			false },
			a_forceEmission);
	}
}
