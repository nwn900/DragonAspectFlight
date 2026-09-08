#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/InputHandler.h"
#include "DragonAspectFlight/Settings.h"
#include "DragonAspectFlight/Version.h"
#include "SKSEMenuFramework.h"

#include "RE/B/bhkCharacterController.h"
#include "RE/B/BSFixedString.h"
#include "RE/H/hkVector4.h"
#include "RE/H/hkpCharacterState.h"
#include "RE/M/MagicTarget.h"
#include "RE/T/TES.h"
#include "RE/U/UI.h"

#include <array>

namespace
{
	// Keep the state reducer namespace local to this implementation file.  The
	// anonymous namespace is at global scope, so an explicit alias is required
	// for the equipment helpers used by the private diagnostics below.
	namespace State = DragonAspectFlight::State;

	constexpr float TickSeconds = 1.0F / 60.0F;
	constexpr float DegreesToRadians = 0.01745329251994329577F;
	constexpr float InputDeadzone = 0.25F;
	constexpr float BaseVerticalVelocityScale = 0.72F;
	constexpr float MaxHorizontalVelocity = 14.0F;
	constexpr float MaxVerticalVelocity = 12.0F;
	constexpr float LaunchBoostVelocity = 8.0F;
	constexpr float BoostHorizontalVelocity = 30.0F;
	constexpr float BoostVerticalVelocity = 22.0F;
	constexpr float BoostedMaxHorizontalVelocity = 32.0F;
	constexpr float BoostedMaxVerticalVelocity = 22.0F;
	constexpr float VelocitySmoothing = 0.10F;
	constexpr float BoostVelocitySmoothing = 0.78F;
	constexpr float TurnVelocitySmoothing = 0.68F;
	constexpr float CollisionCatchUpBrake = 0.65F;
	constexpr float MinFlightHoverVelocity = 0.0F;
	constexpr float StaminaRestorePerUpdate = 6.0F;
	constexpr float MaxStopDownwardVelocity = -2.0F;
	constexpr float DescentVerticalVelocity = -4.5F;
	constexpr float DescentHorizontalDamping = 0.38F;
	constexpr float WaterLandingTolerance = 18.0F;
	constexpr float WaterLandingOffset = 12.0F;
	constexpr float GroundLandingTolerance = 18.0F;
	constexpr std::uint32_t StableLandingContactTicks = 8;
	constexpr std::uint32_t StableFallbackLandingContactTicks = 18;
	constexpr auto ShoutGraphOverrideDuration = 1400ms;
	constexpr auto WhirlwindSprintControlWindow = 1200ms;
	constexpr auto WeaponTransitionTimeout = 2200ms;
	constexpr auto WeaponNativeFallbackDelay = 500ms;
	constexpr auto WeaponNativeFallbackRetryDelay = 250ms;
	constexpr auto WeaponNativeFallbackSafetyMargin = 350ms;
	constexpr auto GroundWeaponObservationTimeout = 650ms;
	constexpr auto GroundWeaponProgressExtension = 1200ms;
	constexpr auto GroundWeaponFallbackObservationTimeout = 2200ms;
	constexpr auto MagickaRegenObservationSampleInterval = 50ms;
	constexpr auto MagickaRegenObservationWindow = 400ms;
	constexpr std::uint32_t MagickaRegenObservationRequiredSamples = 3;
	constexpr float MagickaRegenObservationTolerance = 0.01F;

	[[nodiscard]] DragonAspectFlight::State::DiagnosticPhase ResolveMagickaDiagnosticPhase(
		bool a_flightActive,
		bool a_postFlightTransition,
		bool a_postFlightPump = false) noexcept
	{
		// The post-flight pump is an actual lifecycle phase, not a logging
		// default.  The transition bit covers the short interval before the
		// observer pump has been scheduled; the caller-provided pump bit covers
		// the interval after the transition graph has already reconciled.
		return DragonAspectFlight::State::ResolveDiagnosticPhase(
			a_flightActive,
			!a_flightActive && (a_postFlightTransition || a_postFlightPump));
	}
	constexpr const char* GraphVarDragonAspectActive = "bDAF_DragonAspectActive";
	constexpr const char* GraphVarFlightActive = "bDAF_FlightActive";
	constexpr const char* GraphVarFlightCombatActive = "bDAF_FlightCombatActive";
	constexpr const char* GraphVarLaunchBoost = "bDAF_LaunchBoost";
	constexpr const char* GraphVarFlightShout = "bDAF_FlightShout";
	constexpr const char* GraphVarFlightState = "iDAF_FlightState";
	constexpr const char* GraphVarVanillaInJumpState = "bInJumpState";
	constexpr const char* GraphVarVanillaIsBlocking = "IsBlocking";
	constexpr std::array QuarterstaffKeywords{
		"WeapTypeQuarterstaff",       // active canonical keyword; lowercase s
		"WeapTypeQtrStaff",           // legacy DAF/Animated Armoury spelling
		"WeapTypeQuarterStaff",       // proven capitalization variant
		"OCF_WeapTypeQuarterstaff2H"  // proven OCF two-handed alias
	};
	std::atomic_bool GraphVariableWriteFailureLogged{ false };

	enum class FlightGraphState : std::int32_t
	{
		kOff = 0,
		kIdle = 1,
		kMoving = 2,
		kLaunch = 3,
		kDescent = 4
	};

	struct EquipmentDiagnostic
	{
		RE::TESForm* right{ nullptr };
		RE::TESForm* left{ nullptr };
		std::int32_t rightWeaponType{ -1 };
		std::int32_t leftWeaponType{ -1 };
		const char* expectedOarFamily{ "unarmed" };
		State::WeaponEquipmentFamily family{ State::WeaponEquipmentFamily::kUnknown };
		State::WeaponEquipmentIdentity identity{};
		bool rightIsWeapon{ false };
		bool leftIsWeapon{ false };
		bool quarterstaffEquipped{ false };
		const char* quarterstaffKeyword{ "none" };
		bool blockCapable{ false };
		std::uint64_t signature{ 0 };
	};

	const char* QueuedFlightActionName(DragonAspectFlight::State::FlightInputAction a_action)
	{
		switch (a_action) {
		case DragonAspectFlight::State::FlightInputAction::kStartFlight: return "start_flight";
		case DragonAspectFlight::State::FlightInputAction::kStopFlight: return "stop_flight";
		case DragonAspectFlight::State::FlightInputAction::kBeginDescent: return "begin_descent";
		case DragonAspectFlight::State::FlightInputAction::kCancelDescent: return "cancel_descent";
		case DragonAspectFlight::State::FlightInputAction::kToggleCombatReady: return "toggle_combat_ready";
		case DragonAspectFlight::State::FlightInputAction::kBeginCombat: return "begin_combat";
		case DragonAspectFlight::State::FlightInputAction::kBlockRequest: return "block_request";
		case DragonAspectFlight::State::FlightInputAction::kBlockRelease: return "block_release";
		case DragonAspectFlight::State::FlightInputAction::kShoutPress: return "shout_press";
		case DragonAspectFlight::State::FlightInputAction::kShoutRelease: return "shout_release";
		case DragonAspectFlight::State::FlightInputAction::kClearShout: return "clear_shout";
		case DragonAspectFlight::State::FlightInputAction::kLaunchBoost: return "launch_boost";
		case DragonAspectFlight::State::FlightInputAction::kSetMovementInput: return "set_movement_input";
		case DragonAspectFlight::State::FlightInputAction::kSetVerticalInput: return "set_vertical_input";
		case DragonAspectFlight::State::FlightInputAction::kSetBoostHeld: return "set_boost_held";
		case DragonAspectFlight::State::FlightInputAction::kObserveGroundWeaponTransition: return "observe_ground_weapon_transition";
		default: return "unknown";
		}
	}

	struct ExceptionLogDecision
	{
		std::uint32_t count{ 0 };
		bool emit{ false };
	};

	[[nodiscard]] ExceptionLogDecision NextExceptionLog(
		std::atomic_uint32_t& a_count) noexcept
	{
		const auto count = a_count.fetch_add(1, std::memory_order_relaxed) + 1;
		return ExceptionLogDecision{ count, count == 1 || count % 8 == 0 };
	}

	std::int32_t GetWeaponType(RE::TESForm* a_form)
	{
		if (const auto* weapon = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr) {
			return static_cast<std::int32_t>(weapon->GetWeaponType());
		}
		return -1;
	}

	bool IsOneHandedWeaponType(std::int32_t a_type)
	{
		return State::IsOneHandedWeaponTypeValue(a_type);
	}

	bool IsEquippedMagic(RE::TESForm* a_form)
	{
		return a_form && !a_form->As<RE::TESObjectWEAP>() && a_form->As<RE::MagicItem>();
	}

	const char* FindQuarterstaffKeyword(RE::TESForm* a_form)
	{
		const auto* weapon = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr;
		if (!weapon) {
			return nullptr;
		}
		for (const auto* keyword : QuarterstaffKeywords) {
			if (weapon->HasKeywordString(keyword)) {
				return keyword;
			}
		}
		return nullptr;
	}

	bool IsShield(RE::TESForm* a_form)
	{
		const auto* armor = a_form ? a_form->As<RE::TESObjectARMO>() : nullptr;
		return armor && armor->IsShield();
	}

	bool IsTwoHandedWeaponType(std::int32_t a_type)
	{
		return State::IsTwoHandedWeaponTypeValue(a_type);
	}

	bool IsBlockCapableEquipment(const EquipmentDiagnostic& a_equipment)
	{
		if (IsShield(a_equipment.left) || IsShield(a_equipment.right) ||
			IsTwoHandedWeaponType(a_equipment.rightWeaponType) ||
			IsTwoHandedWeaponType(a_equipment.leftWeaponType)) {
			return true;
		}
		if (State::IsBlockCapableWeaponFamily(a_equipment.family)) {
			return true;
		}

		// Skyrim permits a one-handed weapon to block when the opposite hand is
		// empty. Dual wield, magic, bows, crossbows, and ordinary staves do not.
		return (IsOneHandedWeaponType(a_equipment.rightWeaponType) && !a_equipment.left) ||
			(IsOneHandedWeaponType(a_equipment.leftWeaponType) && !a_equipment.right);
	}

	bool IsWeaponTransitionInProgress(RE::WEAPON_STATE a_state, bool a_targetDrawn)
	{
		if (a_targetDrawn) {
			return a_state == RE::WEAPON_STATE::kWantToDraw || a_state == RE::WEAPON_STATE::kDrawing;
		}
		return a_state == RE::WEAPON_STATE::kWantToSheathe || a_state == RE::WEAPON_STATE::kSheathing;
	}

	DragonAspectFlight::State::WeaponFallbackMotion GetWeaponFallbackMotion(
		RE::WEAPON_STATE a_state,
		bool a_targetDrawn)
	{
		if (IsWeaponTransitionInProgress(a_state, a_targetDrawn)) {
			return DragonAspectFlight::State::WeaponFallbackMotion::kTowardTarget;
		}
		if (IsWeaponTransitionInProgress(a_state, !a_targetDrawn)) {
			return DragonAspectFlight::State::WeaponFallbackMotion::kOppositeTarget;
		}
		return DragonAspectFlight::State::WeaponFallbackMotion::kStable;
	}

	bool IsWeaponStateAtTarget(RE::WEAPON_STATE a_state, bool a_targetDrawn)
	{
		return a_targetDrawn ?
			a_state == RE::WEAPON_STATE::kDrawn :
			a_state == RE::WEAPON_STATE::kSheathed;
	}

	bool WeaponStateIntendsDrawn(RE::WEAPON_STATE a_state)
	{
		return a_state == RE::WEAPON_STATE::kWantToDraw ||
			a_state == RE::WEAPON_STATE::kDrawing ||
			a_state == RE::WEAPON_STATE::kDrawn;
	}

	EquipmentDiagnostic GetEquipmentDiagnostic(RE::PlayerCharacter* a_player)
	{
		EquipmentDiagnostic result;
		if (!a_player) {
			return result;
		}

		result.right = a_player->GetEquippedObject(false);
		result.left = a_player->GetEquippedObject(true);
		result.rightWeaponType = GetWeaponType(result.right);
		result.leftWeaponType = GetWeaponType(result.left);
		result.quarterstaffKeyword = FindQuarterstaffKeyword(result.right);
		if (!result.quarterstaffKeyword) {
			result.quarterstaffKeyword = FindQuarterstaffKeyword(result.left);
		}
		result.quarterstaffEquipped = result.quarterstaffKeyword != nullptr;
		const bool rightMagic = IsEquippedMagic(result.right);
		const bool leftMagic = IsEquippedMagic(result.left);
		result.rightIsWeapon = result.right && result.right->As<RE::TESObjectWEAP>() != nullptr;
		result.leftIsWeapon = result.left && result.left->As<RE::TESObjectWEAP>() != nullptr;
		result.family = State::ResolveWeaponEquipmentFamily(
			result.rightWeaponType,
			result.leftWeaponType,
			result.quarterstaffEquipped,
			rightMagic,
			leftMagic,
			result.rightIsWeapon,
			result.leftIsWeapon);
		result.expectedOarFamily = State::WeaponEquipmentFamilyName(result.family).data();
		result.blockCapable = IsBlockCapableEquipment(result);

		const auto rightFormID = result.right ? result.right->GetFormID() : 0;
		const auto leftFormID = result.left ? result.left->GetFormID() : 0;
		result.identity = State::WeaponEquipmentIdentity{
			rightFormID,
			leftFormID,
			result.rightWeaponType,
			result.leftWeaponType,
			result.family };
		const auto* actorState = a_player->AsActorState();
		const auto drawn = actorState && actorState->IsWeaponDrawn();
		result.signature = static_cast<std::uint64_t>(rightFormID) |
			(static_cast<std::uint64_t>(leftFormID) << 32U);
		result.signature ^= static_cast<std::uint64_t>(result.rightWeaponType + 1) << 8U;
		result.signature ^= static_cast<std::uint64_t>(result.leftWeaponType + 1) << 16U;
		result.signature ^= result.quarterstaffEquipped ? (std::uint64_t{ 1 } << 62U) : 0;
		result.signature ^= drawn ? (std::uint64_t{ 1 } << 63U) : 0;
		return result;
	}

	std::uint32_t GetFormTypeValue(RE::TESForm* a_form)
	{
		return a_form ? static_cast<std::uint32_t>(a_form->GetFormType()) : 0;
	}

	const char* GetFormName(RE::TESForm* a_form)
	{
		if (!a_form) {
			return "";
		}
		const auto* name = a_form->GetName();
		return name ? name : "";
	}

	bool IsNearSolidGroundSurface(RE::PlayerCharacter* a_player, float a_tolerance = GroundLandingTolerance);
	bool IsNearWaterSurface(RE::PlayerCharacter* a_player, float a_tolerance = WaterLandingTolerance);
	bool ResolveWaterLanding(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller);
	void HoldGroundedDescentContact(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller);
	void ResolveSolidLanding(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller);

	// Dragon Aspect magic effect form IDs from Dragonborn.esm.
	// We check for active magic effects rather than HasSpell() because
	// shout-applied temporary ability spells may not register as "known" spells.
	constexpr RE::FormID DA_ArmsEffect = 0x021730;  // DLC2DragonAspectArmsEffect02 "Dragon Aspect - Arms"
	constexpr const char* DragonbornPlugin = "Dragonborn.esm";

	// More Draconic Aspect wings magic effect (form 0x804 in the ESL)
	constexpr RE::FormID DA_WingsEffect = 0x00804;
	constexpr const char* MoreDraconicPlugin = "More Draconic Aspect - Become The Dragonborn ESL.esp";
	constexpr RE::FormID WhirlwindSprintShout = 0x02F7BA;
	constexpr RE::FormID WhirlwindSprintQuestShout = 0x07A4C8;

	RE::PlayerCharacter* GetPlayer()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	// Check if full-power Dragon Aspect is active on the player.
	// Uses MagicTarget::HasMagicEffect which checks the active effect list
	// directly - works for vanilla and modded setups.
	bool HasDragonAspectActive()
	{
		auto* player = GetPlayer();
		if (!player) return false;

		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) return false;

		auto* magicTarget = player->AsMagicTarget();
		if (!magicTarget) return false;

		// The body effects also appear on weaker casts. The arms effect is the
		// vanilla full-form marker from the third word of power.
		auto* fullPowerArms = dh->LookupForm<RE::EffectSetting>(DA_ArmsEffect, DragonbornPlugin);
		if (fullPowerArms && magicTarget->HasMagicEffect(fullPowerArms)) {
			return true;
		}

		// More Draconic uses the wings effect as the corresponding full-form marker.
		auto* wings = dh->LookupForm<RE::EffectSetting>(DA_WingsEffect, MoreDraconicPlugin);
		if (wings && magicTarget->HasMagicEffect(wings)) {
			return true;
		}

		return false;
	}

	RE::NiPoint3 NormalizeVector(const RE::NiPoint3& a_vector)
	{
		const float length = std::sqrt(
			a_vector.x * a_vector.x +
			a_vector.y * a_vector.y +
			a_vector.z * a_vector.z);

		if (length <= 0.0001F) {
			return RE::NiPoint3{ 0.0F, 0.0F, 0.0F };
		}

		return RE::NiPoint3{
			a_vector.x / length,
			a_vector.y / length,
			a_vector.z / length
		};
	}

	float ClampMagnitude(float a_value, float a_maxMagnitude)
	{
		return std::clamp(a_value, -a_maxMagnitude, a_maxMagnitude);
	}

	RE::hkVector4 LerpVelocity(const RE::hkVector4& a_current, const RE::hkVector4& a_target, float a_smoothing = VelocitySmoothing)
	{
		const float smoothing = std::clamp(a_smoothing, 0.01F, 1.0F);

		return RE::hkVector4{
			a_current.quad.m128_f32[0] + ((a_target.quad.m128_f32[0] - a_current.quad.m128_f32[0]) * smoothing),
			a_current.quad.m128_f32[1] + ((a_target.quad.m128_f32[1] - a_current.quad.m128_f32[1]) * smoothing),
			a_current.quad.m128_f32[2] + ((a_target.quad.m128_f32[2] - a_current.quad.m128_f32[2]) * smoothing),
			0.0F
		};
	}

	void RestoreFlightStamina(RE::PlayerCharacter* a_player)
	{
		if (!a_player) {
			return;
		}

		auto* actorValueOwner = a_player->AsActorValueOwner();

		if (!actorValueOwner) {
			return;
		}

		const float currentStamina = actorValueOwner->GetActorValue(RE::ActorValue::kStamina);
		const float maxStamina = std::max(actorValueOwner->GetPermanentActorValue(RE::ActorValue::kStamina), 0.0F);

		if (maxStamina <= 0.0F || currentStamina >= maxStamina) {
			return;
		}

		const float restoreAmount = std::min(StaminaRestorePerUpdate, maxStamina - currentStamina);
		actorValueOwner->RestoreActorValue(RE::ActorValue::kStamina, restoreAmount);
	}

	void ResetFlightFallState(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller)
	{
		if (!a_player || !a_controller) {
			return;
		}

		const auto currentPosition = a_player->GetPosition();
		a_controller->fallStartHeight = currentPosition.z;
		a_controller->fallTime = 0.0F;
	}

	void ApplyControlledAirState(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller)
	{
		(void)a_player;

		if (!a_controller) {
			return;
		}

		a_controller->gravity = 0.0F;
		a_controller->flags.set(RE::CHARACTER_FLAGS::kNoFriction);
		if (a_controller->wantState == RE::hkpCharacterStateType::kOnGround ||
			a_controller->context.currentState == RE::hkpCharacterStateType::kOnGround ||
			a_controller->wantState == RE::hkpCharacterStateType::kSwimming ||
			a_controller->context.currentState == RE::hkpCharacterStateType::kSwimming) {
			a_controller->wantState = RE::hkpCharacterStateType::kInAir;
			a_controller->context.currentState = RE::hkpCharacterStateType::kInAir;
		}
	}

	void HoldContinuousFlightAirState(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller)
	{
		ResetFlightFallState(a_player, a_controller);
		ApplyControlledAirState(a_player, a_controller);
	}

	bool IsControllerGrounded(RE::bhkCharacterController* a_controller)
	{
		if (!a_controller) {
			return false;
		}

		return a_controller->wantState == RE::hkpCharacterStateType::kOnGround ||
			a_controller->context.currentState == RE::hkpCharacterStateType::kOnGround ||
			a_controller->wantState == RE::hkpCharacterStateType::kSwimming ||
			a_controller->context.currentState == RE::hkpCharacterStateType::kSwimming;
	}

	bool IsWhirlwindSprintSelected(RE::PlayerCharacter* a_player)
	{
		if (!a_player) {
			return false;
		}

		const auto* shout = a_player->GetCurrentShout();
		if (!shout) {
			return false;
		}

		const auto formID = shout->GetFormID();
		return formID == WhirlwindSprintShout || formID == WhirlwindSprintQuestShout;
	}

	bool PreserveWhirlwindSprintVelocity(RE::PlayerCharacter* a_player, RE::hkVector4& a_smoothedVelocity)
	{
		if (!a_player || !a_player->Is3DLoaded()) {
			return false;
		}

		auto* controller = a_player->GetCharController();
		if (!controller) {
			return false;
		}

		HoldContinuousFlightAirState(a_player, controller);
		controller->GetLinearVelocityImpl(a_smoothedVelocity);
		a_smoothedVelocity.quad.m128_f32[3] = 0.0F;
		return true;
	}

	bool SetFlightGraphVariables(
		RE::PlayerCharacter* a_player,
		bool a_dragonAspectActive,
		bool a_flightActive,
		bool a_flightCombatActive,
		bool a_useGeneratedCombatTopology,
		bool a_launchBoost,
		bool a_flightShout,
		FlightGraphState a_state)
	{
		if (!a_player || !a_player->Is3DLoaded()) {
			return false;
		}

		bool customVariablesWritten = true;
		const bool dragonAspectWritten = a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarDragonAspectActive), a_dragonAspectActive);
		const bool flightActiveWritten = a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarFlightActive), a_flightActive);
		const bool combatWritten = a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarFlightCombatActive), a_flightCombatActive);
		const bool launchWritten = a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarLaunchBoost), a_launchBoost);
		const bool shoutWritten = a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarFlightShout), a_flightShout);
		const bool stateWritten = a_player->SetGraphVariableInt(
			RE::BSFixedString(GraphVarFlightState), static_cast<std::int32_t>(a_state));
		customVariablesWritten = dragonAspectWritten && flightActiveWritten && combatWritten &&
			launchWritten && shoutWritten && stateWritten;
		// FlightActive and FlightState are the presentation gates consumed by
		// OAR.  The other variables are useful state, but their absence must not
		// silently turn an airborne physics session into an unobserved failure.
		const bool essentialVariablesWritten = flightActiveWritten && stateWritten;

		if (!customVariablesWritten && !GraphVariableWriteFailureLogged.exchange(true)) {
			logger::warn(
				"Dragon Aspect Flight: one or more DAF graph variables were unavailable; "
				"verify Behavior Data Injector and DragonAspectFlight_BDI.json");
		}

		// Keep the normal combat graph active while the controller remains airborne.
		// Jumping Attack's branch cannot reliably transition to block, bash, shout,
		// draw, or sheathe; DAF owns the corresponding visuals through OAR instead.
		(void)a_useGeneratedCombatTopology;
		if (a_flightActive) {
			// This vanilla graph flag is an active-flight override only. Writing it
			// on stop/idle fabricated a jump edge and left the normal combat graph in
			// a walking/falling presentation state.
			a_player->SetGraphVariableBool(
				RE::BSFixedString(GraphVarVanillaInJumpState),
				false);
		}
		return essentialVariablesWritten;
	}

	bool ProbeFlightGraphGates(RE::PlayerCharacter* a_player)
	{
		if (!a_player || !a_player->Is3DLoaded()) {
			return false;
		}

		// OAR's flight families require these two variables. Probe them before
		// claiming the session; missing BDI data must fail closed instead of
		// running physics with no presentation owner.
		const bool activeAvailable = a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarFlightActive), false);
		const bool stateAvailable = a_player->SetGraphVariableInt(
			RE::BSFixedString(GraphVarFlightState), 0);
		if (!activeAvailable || !stateAvailable) {
			logger::warn(
				"event=flight_start_refused reason=graph_gate_unavailable "
				"flight_active_write={} flight_state_write={} verify_bdi=true",
				activeAvailable,
				stateAvailable);
		}
		return activeAvailable && stateAvailable;
	}

	void ClampStopVelocityForSafeRelease(RE::PlayerCharacter* a_player)
	{
		if (!a_player || !a_player->Is3DLoaded()) {
			return;
		}

		auto* controller = a_player->GetCharController();

		if (!controller) {
			return;
		}

		ResetFlightFallState(a_player, controller);

		RE::hkVector4 currentVelocity{ 0.0F, 0.0F, 0.0F, 0.0F };
		controller->GetLinearVelocityImpl(currentVelocity);

		currentVelocity.quad.m128_f32[0] = 0.0F;
		currentVelocity.quad.m128_f32[1] = 0.0F;
		currentVelocity.quad.m128_f32[2] = std::max(currentVelocity.quad.m128_f32[2], MaxStopDownwardVelocity);
		currentVelocity.quad.m128_f32[3] = 0.0F;

		controller->SetLinearVelocityImpl(currentVelocity);
	}

	RE::NiPoint3 GetCameraForwardVector()
	{
		auto playerCamera = RE::PlayerCamera::GetSingleton();

		if (playerCamera && playerCamera->cameraRoot) {
			const auto cameraRoot = playerCamera->cameraRoot.get();
			const auto forward = cameraRoot->world.rotate * RE::NiPoint3{ 0.0F, 1.0F, 0.0F };

			return NormalizeVector(forward);
		}

		auto player = GetPlayer();

		if (!player) {
			return RE::NiPoint3{ 0.0F, 0.0F, 0.0F };
		}

		const float yaw = player->GetAngleZ() * DegreesToRadians;

		return NormalizeVector(RE::NiPoint3{
			std::sin(yaw),
			std::cos(yaw),
			0.0F
		});
	}

	RE::NiPoint3 GetCameraRightVector(const RE::NiPoint3& a_forward)
	{
		return NormalizeVector(RE::NiPoint3{
			a_forward.y,
			-a_forward.x,
			0.0F
		});
	}

	bool HasMovementInput(float a_forwardInput, float a_strafeInput)
	{
		return std::abs(a_forwardInput) > InputDeadzone || std::abs(a_strafeInput) > InputDeadzone;
	}

	bool HasFlightControlInput(float a_forwardInput, float a_strafeInput, float a_verticalInput)
	{
		return HasMovementInput(a_forwardInput, a_strafeInput) || std::abs(a_verticalInput) > InputDeadzone;
	}

	void MovePlayerWithCharacterControllerVelocity(float a_horizontalSpeed, float a_verticalSpeed, float a_liftScale, float a_forwardInput, float a_strafeInput, float a_verticalInput, float a_launchBoost, bool a_boostHeld, RE::hkVector4& smoothedVelocity)
	{
		auto player = GetPlayer();

		if (!player || !player->Is3DLoaded()) {
			return;
		}

		auto* controller = player->GetCharController();

		if (!controller) {
			return;
		}

		RestoreFlightStamina(player);
		HoldContinuousFlightAirState(player, controller);

		const float maxHorizontalForMode = a_boostHeld ? BoostedMaxHorizontalVelocity : MaxHorizontalVelocity;
		const float maxVerticalForMode = a_boostHeld ? BoostedMaxVerticalVelocity : MaxVerticalVelocity;
		const float activeHorizontalSpeed = a_boostHeld ? BoostHorizontalVelocity : a_horizontalSpeed;
		const float activeVerticalSpeed = a_boostHeld ? BoostVerticalVelocity : a_verticalSpeed;
		const float activeSmoothing = a_boostHeld ? BoostVelocitySmoothing : TurnVelocitySmoothing;
		const bool hasVerticalInput = std::abs(a_verticalInput) > InputDeadzone;
		const float verticalControlVelocity = std::clamp(a_verticalInput, -1.0F, 1.0F) * activeVerticalSpeed;

		if (a_horizontalSpeed <= 0.0F || !HasMovementInput(a_forwardInput, a_strafeInput)) {
			const float idleVerticalVelocity = hasVerticalInput ?
				verticalControlVelocity :
				std::max(a_launchBoost, MinFlightHoverVelocity);
			const RE::hkVector4 idleTargetVelocity{ 0.0F, 0.0F, std::clamp(idleVerticalVelocity, -maxVerticalForMode, maxVerticalForMode), 0.0F };
			smoothedVelocity = LerpVelocity(smoothedVelocity, idleTargetVelocity, TurnVelocitySmoothing);
			if (!hasVerticalInput && a_launchBoost <= 0.0F && std::abs(smoothedVelocity.quad.m128_f32[2]) < 0.20F) {
				smoothedVelocity.quad.m128_f32[2] = 0.0F;
			}
			controller->SetLinearVelocityImpl(smoothedVelocity);
			return;
		}

		const auto cameraForward = GetCameraForwardVector();
		const auto cameraRight = GetCameraRightVector(cameraForward);

		RE::NiPoint3 desiredDirection{
			(cameraForward.x * a_forwardInput) + (cameraRight.x * a_strafeInput),
			(cameraForward.y * a_forwardInput) + (cameraRight.y * a_strafeInput),
			(cameraForward.z * a_forwardInput)
		};

		const float inputMagnitude = std::clamp(
			std::sqrt((a_forwardInput * a_forwardInput) + (a_strafeInput * a_strafeInput)),
			0.0F,
			1.0F);

		desiredDirection = NormalizeVector(desiredDirection);

		if (desiredDirection.SqrLength() <= 0.0001F || inputMagnitude <= InputDeadzone) {
			controller->SetLinearVelocityImpl(RE::hkVector4{ 0.0F, 0.0F, hasVerticalInput ? verticalControlVelocity : MinFlightHoverVelocity, 0.0F });
			return;
		}

		const float tunedHorizontalSpeed = std::min(activeHorizontalSpeed * inputMagnitude, maxHorizontalForMode);
		const float tunedVerticalSpeed = std::min(activeVerticalSpeed * inputMagnitude, maxVerticalForMode);

		float targetVerticalVelocity =
			(desiredDirection.z * tunedVerticalSpeed * BaseVerticalVelocityScale * std::clamp(a_liftScale, 0.25F, 2.50F)) +
			verticalControlVelocity +
			a_launchBoost;

		if (!hasVerticalInput && a_launchBoost <= 0.0F) {
			targetVerticalVelocity = std::max(targetVerticalVelocity, MinFlightHoverVelocity);
		}

		RE::hkVector4 targetVelocity{
			ClampMagnitude(desiredDirection.x * tunedHorizontalSpeed, maxHorizontalForMode),
			ClampMagnitude(desiredDirection.y * tunedHorizontalSpeed, maxHorizontalForMode),
			ClampMagnitude(targetVerticalVelocity, maxVerticalForMode),
			0.0F
		};

		const float horizontalMagnitude = std::sqrt(
			(targetVelocity.quad.m128_f32[0] * targetVelocity.quad.m128_f32[0]) +
			(targetVelocity.quad.m128_f32[1] * targetVelocity.quad.m128_f32[1]));

		if (horizontalMagnitude > maxHorizontalForMode) {
			const float horizontalScale = maxHorizontalForMode / horizontalMagnitude;
			targetVelocity.quad.m128_f32[0] *= horizontalScale;
			targetVelocity.quad.m128_f32[1] *= horizontalScale;
		}

		if (std::abs(smoothedVelocity.quad.m128_f32[0]) > maxHorizontalForMode ||
			std::abs(smoothedVelocity.quad.m128_f32[1]) > maxHorizontalForMode ||
			std::abs(smoothedVelocity.quad.m128_f32[2]) > maxVerticalForMode) {
			smoothedVelocity.quad.m128_f32[0] *= CollisionCatchUpBrake;
			smoothedVelocity.quad.m128_f32[1] *= CollisionCatchUpBrake;
			smoothedVelocity.quad.m128_f32[2] *= CollisionCatchUpBrake;
		}

		smoothedVelocity = LerpVelocity(smoothedVelocity, targetVelocity, activeSmoothing);

		RE::hkVector4 clampedVelocity{
			ClampMagnitude(smoothedVelocity.quad.m128_f32[0], maxHorizontalForMode),
			ClampMagnitude(smoothedVelocity.quad.m128_f32[1], maxHorizontalForMode),
			ClampMagnitude(smoothedVelocity.quad.m128_f32[2], maxVerticalForMode),
			0.0F
		};

		if (!hasVerticalInput && a_launchBoost <= 0.0F && std::abs(clampedVelocity.quad.m128_f32[2]) < 0.20F) {
			clampedVelocity.quad.m128_f32[2] = 0.0F;
			smoothedVelocity.quad.m128_f32[2] = 0.0F;
		}

		controller->SetLinearVelocityImpl(clampedVelocity);
	}

	bool MovePlayerWithControlledDescent(std::uint32_t& a_landingContactTicks)
	{
		auto* player = GetPlayer();

		if (!player || !player->Is3DLoaded()) {
			return false;
		}

		auto* controller = player->GetCharController();

		if (!controller) {
			return false;
		}

		const bool nearWater = IsNearWaterSurface(player);
		const bool nearSolidGround = IsNearSolidGroundSurface(player);
		const bool grounded = IsControllerGrounded(controller);

		if (nearWater || grounded) {
			++a_landingContactTicks;

			const auto requiredContactTicks =
				nearWater || nearSolidGround ? StableLandingContactTicks : StableFallbackLandingContactTicks;

			if (a_landingContactTicks >= requiredContactTicks) {
				if (nearWater && ResolveWaterLanding(player, controller)) {
					logger::info("Flight descent resolved on stable water surface");
					return true;
				}

				if (grounded) {
					ResolveSolidLanding(player, controller);
					if (nearSolidGround) {
						logger::info("Flight descent resolved on stable solid ground");
					} else {
						logger::info("Flight descent resolved on stable collision ground");
					}
					return true;
				}

				if (nearSolidGround) {
					ResolveSolidLanding(player, controller);
					logger::info("Flight descent resolved on stable solid ground");
					return true;
				}
			}

			if (grounded) {
				HoldGroundedDescentContact(player, controller);
				return false;
			}
		} else {
			a_landingContactTicks = 0;
		}

		RestoreFlightStamina(player);
		ResetFlightFallState(player, controller);
		ApplyControlledAirState(player, controller);

		RE::hkVector4 currentVelocity{ 0.0F, 0.0F, 0.0F, 0.0F };
		controller->GetLinearVelocityImpl(currentVelocity);

		currentVelocity.quad.m128_f32[0] *= DescentHorizontalDamping;
		currentVelocity.quad.m128_f32[1] *= DescentHorizontalDamping;
		currentVelocity.quad.m128_f32[2] = DescentVerticalVelocity;
		currentVelocity.quad.m128_f32[3] = 0.0F;

		controller->SetLinearVelocityImpl(currentVelocity);
		return false;
	}

	bool IsNearWaterSurface(RE::PlayerCharacter* a_player, float a_tolerance)
	{
		if (!a_player) {
			return false;
		}

		const float waterHeight = a_player->GetWaterHeight();

		if (!std::isfinite(waterHeight) || waterHeight < -100000.0F) {
			return false;
		}

		return a_player->GetPositionZ() <= waterHeight + a_tolerance;
	}

	bool IsNearSolidGroundSurface(RE::PlayerCharacter* a_player, float a_tolerance)
	{
		if (!a_player) {
			return false;
		}

		auto* tes = RE::TES::GetSingleton();

		if (!tes) {
			return true;
		}

		const auto position = a_player->GetPosition();
		float landHeight = 0.0F;

		if (!tes->GetLandHeight(position, landHeight)) {
			return true;
		}

		return position.z <= landHeight + a_tolerance;
	}

	void HoldGroundedDescentContact(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller)
	{
		if (!a_player || !a_controller) {
			return;
		}

		RestoreFlightStamina(a_player);
		ResetFlightFallState(a_player, a_controller);

		RE::hkVector4 currentVelocity{ 0.0F, 0.0F, 0.0F, 0.0F };
		a_controller->GetLinearVelocityImpl(currentVelocity);
		currentVelocity.quad.m128_f32[0] *= DescentHorizontalDamping;
		currentVelocity.quad.m128_f32[1] *= DescentHorizontalDamping;
		currentVelocity.quad.m128_f32[2] = 0.0F;
		currentVelocity.quad.m128_f32[3] = 0.0F;
		a_controller->SetLinearVelocityImpl(currentVelocity);
	}

	void ResolveSolidLanding(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller)
	{
		if (!a_player || !a_controller) {
			return;
		}

		ResetFlightFallState(a_player, a_controller);
		a_controller->flags.reset(RE::CHARACTER_FLAGS::kNoFriction);
		a_controller->wantState = RE::hkpCharacterStateType::kOnGround;
		a_controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
		a_controller->SetLinearVelocityImpl(RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F });
	}

	bool ResolveWaterLanding(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller)
	{
		if (!a_player || !a_controller || !IsNearWaterSurface(a_player)) {
			return false;
		}

		auto position = a_player->GetPosition();
		const float waterHeight = a_player->GetWaterHeight();

		if (position.z < waterHeight + WaterLandingOffset) {
			position.z = waterHeight + WaterLandingOffset;
			a_player->SetPosition(position, true);
		}

		ResetFlightFallState(a_player, a_controller);
		a_controller->gravity = 0.0F;
		a_controller->flags.reset(RE::CHARACTER_FLAGS::kNoFriction);
		a_controller->wantState = RE::hkpCharacterStateType::kOnGround;
		a_controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
		a_controller->SetLinearVelocityImpl(RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F });
		return true;
	}
}

namespace DragonAspectFlight
{
	FlightManager& FlightManager::GetSingleton()
	{
		static FlightManager singleton;
		return singleton;
	}

	void FlightManager::LogWeaponRoutingDiagnostic(
		RE::PlayerCharacter* a_player,
		std::string_view a_reason,
		std::string_view a_fallbackReason,
		bool a_force,
		std::optional<bool> a_targetDrawn)
	{
		if (!a_player) {
			return;
		}

		const auto equipment = GetEquipmentDiagnostic(a_player);
		std::uint64_t session = 0;
		bool detailedLogging = false;
		bool emit = a_force;
		{
			std::unique_lock lock(_mutex);
			session = _flightSessionId;
			detailedLogging = _detailedLogging;
			if (equipment.signature != _lastWeaponRoutingSignature) {
				emit = true;
			}
			if (emit) {
				_lastWeaponRoutingSignature = equipment.signature;
			}
		}
		if (!emit || (!detailedLogging && !a_force)) {
			return;
		}

		const auto* actorState = a_player->AsActorState();
		const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		bool flight = false;
		bool postFlight = false;
		{
			std::shared_lock lock(_mutex);
			flight = _isFlying;
			postFlight = !_isFlying && _weaponTransitionPostFlight;
		}
		const auto phaseState = State::ResolveDiagnosticPhase(flight, postFlight);
		const auto routingState = State::MakeWeaponRoutingDiagnosticState(
			a_targetDrawn.value_or(false), weaponsDrawn);
		const char* requestedTarget = a_targetDrawn.has_value() ?
			(a_targetDrawn.value() ? "true" : "false") : "none";

		const bool fallbackDecision = a_reason == "ground_fallback_decision" ||
			a_reason == "native_fallback_decision";
		logger::info(
			"event=weapon_routing session={} reason={} phase_flight={} phase_postflight={} "
			"expected_oar_family={} actual_oar_winner=unknown "
			"right_form=0x{:08X} right_name=\"{}\" right_form_type={} right_weapon_type={} "
			"left_form=0x{:08X} left_name=\"{}\" left_form_type={} left_weapon_type={} "
			"quarterstaff={} quarterstaff_keyword={} keyword_evidence={} weapon_evidence={} form_evidence={} "
			"fallback_reason={} fallback_method={} fallback_result={} requested_target_drawn={} "
			"actual_drawn={} engine_weapon_state={} engine_state_source=actor_state",
			session,
			a_reason,
			phaseState.flight,
			phaseState.postFlight,
			equipment.expectedOarFamily,
			equipment.right ? equipment.right->GetFormID() : 0,
			GetFormName(equipment.right),
			GetFormTypeValue(equipment.right),
			equipment.rightWeaponType,
			equipment.left ? equipment.left->GetFormID() : 0,
			GetFormName(equipment.left),
			GetFormTypeValue(equipment.left),
			equipment.leftWeaponType,
			equipment.quarterstaffEquipped,
			equipment.quarterstaffKeyword,
			equipment.quarterstaffEquipped,
			equipment.rightIsWeapon || equipment.leftIsWeapon,
			equipment.right != nullptr || equipment.left != nullptr,
			a_fallbackReason,
			fallbackDecision ? (a_reason == "ground_fallback_decision" ? "DrawWeaponMagicHands" : "DrawWeaponMagicHands") : "none",
			fallbackDecision ? "issued" : "not_applicable",
			requestedTarget,
			routingState.actualDrawn,
			static_cast<std::int32_t>(weaponState));
	}

	void FlightManager::StartFlight()
	{
		// An already-active session is a strict no-op.  In particular, do not
		// probe/reset graph variables on a duplicate Papyrus or input request;
		// those writes can briefly disable OAR's flight presentation.
		{
			std::shared_lock lock(_mutex);
			if (_isFlying) {
				logger::debug("event=flight_start_ignored reason=already_flying");
				return;
			}
		}
		if (!HasDragonAspectActive()) {
			logger::info("Dragon Aspect not active; flight cancelled");
			return;
		}

		auto* player = GetPlayer();
		if (player && player->IsOnMount()) {
			logger::info("Dragon Aspect Flight: flight start refused while mounted");
			return;
		}
		if (!player || !player->Is3DLoaded()) {
			logger::warn("event=flight_start_refused reason=player_unloaded");
			return;
		}
		auto* controller = player->GetCharController();
		if (!controller) {
			logger::warn("event=flight_start_refused reason=character_controller_unavailable");
			return;
		}
		if (!ProbeFlightGraphGates(player)) {
			logger::warn("event=flight_start_refused reason=essential_oar_graph_gate_unavailable");
			return;
		}

		const auto* actorState = player->AsActorState();
		const bool startWithWeaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const auto startingEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
		const auto startingEquipment = startingEquipmentSnapshot.identity;
		// Resolve the controller before claiming the session.  The ownership bit
		// is published in the same locked state transition that sets _isFlying, so
		// a concurrent stop can never observe flying=true with an unclaimed world
		// state and a later start can never republish the token after cleanup.
		{
			std::unique_lock nativeGate(_readyNativeActionMutex);
			std::unique_lock lock(_mutex);

			if (_isFlying) {
				return;
			}

			_isFlying = true;
			_isDescending = false;
			_flightCombatActive = false;
			_flightBlockRequested = false;
			_flightBlockNativeStateOwned = false;
			_flightBlockLease = {};
			_flightBlockRequestedIdentity = {};
			_flightBlockRequestedIdentityEpoch = 0;
			_flightBlockRequestedIdentityCaptured = false;
			_weaponTransitionPending = false;
			_weaponTransitionTargetDrawn = startWithWeaponsDrawn;
			_weaponTransitionNativeFallbackArmed = false;
			_weaponTransitionExpiryRecoveryAttempted = false;
			_weaponTransitionNativeFallbackRetryUsed = false;
			_weaponTransitionProgressExtensionUsed = false;
			_weaponTransitionPostFlight = false;
			_weaponTransitionTerminalFailureHold = {};
			_weaponTransitionDeadline = {};
			_weaponTransitionNativeFallbackAt = {};
			_weaponTransitionPreRequestState = -1;
			_weaponTransitionActorFormId = 0;
			_weaponTransitionSessionId = 0;
			_weaponTransitionIdentityEpoch = 0;
			_weaponTransitionEquipmentIdentity = {};
			_weaponTransitionEquipmentIdentityCaptured = false;
			_groundWeaponObservationPending = false;
			_groundWeaponFallbackIssued = false;
			_groundWeaponFallbackRetryUsed = false;
			_groundWeaponProgressExtensionUsed = false;
			_groundWeaponDeadline = {};
			_groundWeaponActorFormId = 0;
			_groundWeaponPreEdgeState = -1;
			_groundWeaponSessionId = 0;
			_groundWeaponIdentityEpoch = 0;
			_groundWeaponEquipmentIdentity = {};
			_groundWeaponEquipmentIdentityCaptured = false;
			// Session start invalidates queued actions through the session barrier; the
			// equipment epoch advances only when this live observation proves an
			// identity change, so a stable A does not receive a synthetic epoch.
			(void)ObserveWeaponEquipmentIdentityLocked(
				startingEquipment,
				startingEquipmentSnapshot.captured);
			_weaponEquipmentIdentityChangePending = false;
			_weaponEquipmentPreviousIdentityPending = {};
			_weaponEquipmentIdentityPendingFromEpoch = 0;
			_weaponEquipmentIdentityPendingToEpoch = 0;
			_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
			_weaponEquipmentSwapPreviousIdentity = {};
			_weaponEquipmentSwapCurrentIdentity = {};
			_weaponEquipmentSwapIdentityCaptured = false;
			_weaponEquipmentSwapPinnedDiagnosticLogged = false;
			_useGeneratedCombatTopology = false;
			_aerialCombatUnsupportedNotified = false;
			_flightWorldStateOwned = controller != nullptr;
			_flightOwnedController = controller;
			if (controller) {
				_originalGravity = controller->gravity;
				_originalNoFriction = controller->flags.all(RE::CHARACTER_FLAGS::kNoFriction);
			}
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kIdle);
			_landingContactTicks = 0;
			_shoutGraphOverrideUntil = {};
			_whirlwindSprintUntil = {};
			_whirlwindSprintShoutPending = false;
			_flightShoutHeld = false;
			_magickaDrainCarrySeconds = 0.0F;
			_lastMagickaDrainAt = {};
			_magickaDrainSequence = 0;
			_magickaRegenObservationSessionId = 0;
			_magickaRegenObservationDrainSequence = 0;
			_magickaRegenObservationActorFormId = 0;
			_magickaRegenDelayBaseline = 0.0F;
			_magickaRegenDelayAfterDrain = 0.0F;
			_magickaRegenDelayOwned = false;
			_magickaRegenAwaitingDelayedIncrease = false;
			_magickaRegenObservationStableLogged = false;
			_magickaRegenObservationPending = false;
			_magickaRegenObservationSamples = 0;
			_magickaRegenObservationStarted = {};
			_magickaRegenObservationNextSample = {};
			_smoothedFlightVelocity = RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F };
			++_flightSessionId;
			_publishedFlightSessionId.store(_flightSessionId, std::memory_order_release);
			(void)AdvanceUpdateTaskGeneration();
			_readyGenerationBarrier = State::ResetReadyGenerationBarrier(
				_readyGenerationBarrier,
				_flightSessionId,
				_readyGenerationCounter.load(std::memory_order_relaxed));
			ClearReadyGenerationLeaseLocked();
			_readyGenerationBlockedDiagnosticLogged = false;
			_flightSessionStartActionSequence = _activeQueuedActionSequence;
			_lastDiagnosticSnapshot = {};
			_lastDiagnosticEquipmentSignature = ~std::uint64_t{ 0 };
			_lastWeaponRoutingSignature = ~std::uint64_t{ 0 };
			_lastDiagnosticStateSignature = ~std::uint64_t{ 0 };
			logger::info(
				"event=flight_start session={} version={} weapons_drawn={} dragon_aspect_active=true",
				_flightSessionId,
				BuildVersion,
				startWithWeaponsDrawn);
		}
		FlushDiagnosticAggregates("session_start");

		// Keep the physics state airborne for OAR without firing sprint/jump
		// animation graph events that can collide with Better Jumping.
		if (player && player->Is3DLoaded()) {
			if (controller) {
				ApplyControlledAirState(player, controller);
			}
			if (!SetFlightGraphVariables(player, true, true, false, false, false, false, FlightGraphState::kIdle)) {
				logger::error("event=flight_start_refused reason=essential_graph_gate_write_failed rollback=true");
				StopFlight();
				return;
			}
			LogWeaponRoutingDiagnostic(player, "flight_start", "none", true, startWithWeaponsDrawn);

			if (startWithWeaponsDrawn && !SetFlightCombatActive(true)) {
				logger::warn(
					"Dragon Aspect Flight: flight began with equipment drawn, but combat state initialization failed");
				StopFlight();
				return;
			}
		}

		if (!StartUpdateThread()) {
			logger::error(
				"event=flight_start_failed reason=update_thread_unavailable rollback=true session={}",
				_flightSessionId);
			StopFlight();
			return;
		}
		if (auto* inputHandler = InputHandler::GetSingleton()) {
			inputHandler->RefreshGameThreadState();
		}
	}

	void FlightManager::BeginDescent()
	{
		bool combatActive = false;
		bool useGeneratedCombatTopology = false;
		bool shoutHeld = false;

		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || _isDescending) {
				return;
			}

			_isDescending = true;
			combatActive = _flightCombatActive;
			useGeneratedCombatTopology = _useGeneratedCombatTopology;
			_forwardInput = 0.0F;
			_strafeInput = 0.0F;
			_verticalInput = 0.0F;
			_pendingLaunchBoost = 0.0F;
			_boostHeld = false;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kDescent);
			_landingContactTicks = 0;
			shoutHeld = _flightShoutHeld;
			logger::info("Flight descent started - {}", BuildVersion);
		}

		if (auto* player = GetPlayer(); player && player->Is3DLoaded()) {
			SetFlightGraphVariables(
				player,
				HasDragonAspectActive(),
				true,
				combatActive,
				useGeneratedCombatTopology,
				false,
				shoutHeld,
				FlightGraphState::kDescent);
		}

		(void)StartUpdateThread();
		if (auto* inputHandler = InputHandler::GetSingleton()) {
			inputHandler->RefreshGameThreadState();
		}
	}

	void FlightManager::CancelDescent()
	{
		bool combatActive = false;
		bool useGeneratedCombatTopology = false;
		bool shoutHeld = false;
		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || !_isDescending || !HasDragonAspectActive()) {
				return;
			}

			_isDescending = false;
			combatActive = _flightCombatActive;
			useGeneratedCombatTopology = _useGeneratedCombatTopology;
			_forwardInput = 0.0F;
			_strafeInput = 0.0F;
			_verticalInput = 0.0F;
			_pendingLaunchBoost = 0.0F;
			_boostHeld = false;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kIdle);
			_landingContactTicks = 0;
			shoutHeld = _flightShoutHeld;
			logger::info("Flight descent cancelled - {}", BuildVersion);
		}

		if (auto* player = GetPlayer(); player && player->Is3DLoaded()) {
			if (auto* controller = player->GetCharController()) {
				ApplyControlledAirState(player, controller);
				ResetFlightFallState(player, controller);

				RE::hkVector4 currentVelocity{ 0.0F, 0.0F, 0.0F, 0.0F };
				controller->GetLinearVelocityImpl(currentVelocity);
				currentVelocity.quad.m128_f32[2] = std::max(currentVelocity.quad.m128_f32[2], MinFlightHoverVelocity);
				currentVelocity.quad.m128_f32[3] = 0.0F;
				controller->SetLinearVelocityImpl(currentVelocity);
			}

			SetFlightGraphVariables(
				player,
				true,
				true,
				combatActive,
				useGeneratedCombatTopology,
				false,
				shoutHeld,
				FlightGraphState::kIdle);
		}

		(void)StartUpdateThread();
		if (auto* inputHandler = InputHandler::GetSingleton()) {
			inputHandler->RefreshGameThreadState();
		}
	}

	void FlightManager::StopFlight()
	{
		FlushDiagnosticAggregates("flight_stop");
		auto* player = GetPlayer();
		auto* currentController = player ? player->GetCharController() : nullptr;
		const auto stopEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
		const bool stopIdentityCaptured = stopEquipmentSnapshot.captured;
		const auto stopEquipmentIdentity = stopEquipmentSnapshot.identity;
		if (stopIdentityCaptured) {
			// Rebase before cleanup so a stop racing an unobserved equipment swap
			// revokes the old DAF token instead of clearing replacement state.
			(void)HandleEquipmentIdentityChange(stopEquipmentIdentity, "flight_stop");
		}
		bool clearOwnedBlock = false;
		{
			std::shared_lock lock(_mutex);
			clearOwnedBlock = _flightBlockRequested || _flightBlockLease.active;
		}
		if (clearOwnedBlock) {
			SetFlightBlockRequested(false);
		}
		const auto* actorState = player ? player->AsActorState() : nullptr;
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
		const bool actorInWeaponTransition = actorState &&
			(IsWeaponTransitionInProgress(weaponState, true) ||
				IsWeaponTransitionInProgress(weaponState, false));
		bool wasFlying = false;
		bool keepTransitionPump = false;
		bool keepRegenObservationPump = false;
		bool keepGroundWeaponObservationPump = false;
		bool hadPendingTransition = false;
		bool detailedLogging = false;
		std::uint64_t session = 0;
		float originalGravity = 0.0F;
		bool originalNoFriction = false;
		bool transitionTargetDrawn = false;
		bool worldCleanupOwned = false;
		bool controllerMatches = false;
		bool logicalCleanupOwned = false;
		bool landingTransitionPending = false;
		bool landingHandoffTargetDrawn = false;
		bool landingHandoffQueued = false;
		std::uint64_t landingHandoffEpoch = 0;
		bool groundObserverPreserved = false;
		bool groundObserverRebased = false;
		bool groundObserverTargetDrawn = false;
		std::uint64_t groundObserverEdgeSequence = 0;
		std::uint64_t groundObserverIdentityEpoch = 0;

		{
			std::unique_lock nativeGate(_readyNativeActionMutex);
			std::unique_lock lock(_mutex);
			wasFlying = _isFlying;
			controllerMatches = currentController != nullptr &&
				_flightOwnedController != nullptr && currentController == _flightOwnedController;
			worldCleanupOwned = State::OwnsWorldStateForStop(_isFlying, _flightWorldStateOwned) &&
				controllerMatches;
			logicalCleanupOwned = _isFlying || _flightWorldStateOwned;
			detailedLogging = _detailedLogging;
			session = _flightSessionId;
			originalGravity = _originalGravity;
			originalNoFriction = _originalNoFriction;
			landingTransitionPending = _weaponTransitionPending;
			landingHandoffTargetDrawn = _weaponTransitionPending ?
				_weaponTransitionTargetDrawn : _flightCombatActive;
			transitionTargetDrawn = landingHandoffTargetDrawn;
			// A terminal failure hold is flight-session state and cannot survive
			// landing, including an idempotent repeated stop.
			_weaponTransitionTerminalFailureHold = {};

			State::FlightStopState stopState{
				_isFlying,
				_isDescending,
				_flightCombatActive,
				_flightBlockRequested,
				_flightShoutHeld,
				_pendingLaunchBoost > 0.0F,
				_boostHeld,
				State::WeaponTransitionState{
					_weaponTransitionPending,
					_weaponTransitionTargetDrawn,
					_weaponTransitionNativeFallbackArmed,
					_weaponTransitionPostFlight,
					_weaponTransitionSequence },
				_flightWorldStateOwned };
			const auto reduced = State::ReduceFlightStop(stopState);

			_isFlying = reduced.flying;
			_isDescending = reduced.descending;
			_flightCombatActive = reduced.combatActive;
			_flightBlockRequested = reduced.blockRequested;
			if (!_flightBlockRequested) {
				_flightBlockRequestedIdentity = {};
				_flightBlockRequestedIdentityEpoch = 0;
				_flightBlockRequestedIdentityCaptured = false;
			}
			_flightShoutHeld = reduced.shoutHeld;
			_boostHeld = reduced.boostHeld;
			_weaponTransitionPending = reduced.weapon.pending;
			_weaponTransitionTargetDrawn = reduced.weapon.targetDrawn;
			transitionTargetDrawn = _weaponTransitionTargetDrawn;
			_weaponTransitionNativeFallbackArmed = reduced.weapon.nativeFallbackArmed;
			_weaponTransitionPostFlight = reduced.weapon.postFlight;
			_flightWorldStateOwned = reduced.worldStateOwned;
			if (!controllerMatches) {
				// The original controller is gone or has been replaced.  Retaining the
				// ownership bit would permit a later stop to restore stale values onto
				// an unrelated controller.
				_flightWorldStateOwned = false;
			}
			_flightOwnedController = nullptr;
			keepTransitionPump = reduced.weapon.pending;
			hadPendingTransition = landingTransitionPending;
			// A transition is an actor/3D-owned transaction.  When the actor is
			// unavailable there is no safe identity to observe or to pass to
			// DrawWeaponMagicHands; preserving this transaction would re-arm the
			// worker on every actor_unloaded update and leave a permanent pump with
			// no engine state it can reconcile.
			if (!stopIdentityCaptured) {
				_weaponTransitionPending = false;
				_weaponTransitionTargetDrawn = false;
				_weaponTransitionNativeFallbackArmed = false;
				_weaponTransitionNativeFallbackRetryUsed = false;
				_weaponTransitionPostFlight = false;
				keepTransitionPump = false;
			}
			if (!keepTransitionPump) {
				_weaponTransitionDeadline = {};
				_weaponTransitionNativeFallbackAt = {};
				_weaponTransitionNativeFallbackRetryUsed = false;
				_weaponTransitionProgressExtensionUsed = false;
				_weaponTransitionPreRequestState = -1;
				_weaponTransitionActorFormId = 0;
				_weaponTransitionSessionId = 0;
				_weaponTransitionIdentityEpoch = 0;
				_weaponTransitionEquipmentIdentity = {};
				_weaponTransitionEquipmentIdentityCaptured = false;
			}
			// Swap settle state is flight-session scoped and never survives landing.
			_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
			_weaponEquipmentSwapPreviousIdentity = {};
			_weaponEquipmentSwapCurrentIdentity = {};
			_weaponEquipmentSwapIdentityCaptured = false;
			_weaponEquipmentSwapPinnedDiagnosticLogged = false;
			// Landing retires every flight identity transaction.  A repeated stop is
			// also allowed to preserve an already-valid ground observer; only a real
			// rebase/retirement advances the epoch so its delayed fallback remains
			// executable exactly once.
			const bool groundIdentityMatches = stopIdentityCaptured &&
				_groundWeaponEquipmentIdentityCaptured &&
				State::SameWeaponEquipmentIdentity(
					_groundWeaponEquipmentIdentity,
					stopEquipmentIdentity);
			const bool groundActorMatches = stopIdentityCaptured && player &&
				_groundWeaponActorFormId == static_cast<std::uint32_t>(player->GetFormID());
			const auto repeatedGroundStop = State::ReduceGroundObserverAfterStop(
				State::GroundWeaponObserverState{
					_groundWeaponObservationPending,
					_groundWeaponFallbackIssued,
					_groundWeaponEdgeSequence },
				wasFlying,
				stopIdentityCaptured,
				groundIdentityMatches,
				_groundWeaponSessionId == _flightSessionId,
				State::IsUsableWeaponEquipmentEpoch(_groundWeaponIdentityEpoch) &&
					State::IsUsableWeaponEquipmentEpoch(_weaponEquipmentIdentityEpoch) &&
					_groundWeaponIdentityEpoch == _weaponEquipmentIdentityEpoch,
				groundActorMatches);
			groundObserverPreserved = repeatedGroundStop.preserve;
			groundObserverRebased = repeatedGroundStop.rebase;
			if (!stopIdentityCaptured) {
				// The ground observer is also actor-bound.  A stop caused by an
				// unloaded actor must discard it rather than retain a pump that can
				// never validate its identity or issue a safe fallback.
				_groundWeaponObservationPending = false;
				_groundWeaponFallbackIssued = false;
				_groundWeaponFallbackRetryUsed = false;
				_groundWeaponProgressExtensionUsed = false;
				_groundWeaponTargetDrawn = false;
				_groundWeaponActorFormId = 0;
				_groundWeaponPreEdgeState = -1;
				_groundWeaponSessionId = 0;
				_groundWeaponIdentityEpoch = 0;
				_groundWeaponDeadline = {};
				_groundWeaponEquipmentIdentity = {};
				_groundWeaponEquipmentIdentityCaptured = false;
				groundObserverPreserved = false;
				groundObserverRebased = false;
				keepGroundWeaponObservationPump = false;
			}
			if (!stopIdentityCaptured) {
				// An unload is the only stop path that needs to change the capture bit;
				// the loaded path already passed through the authoritative snapshot and
				// HandleEquipmentIdentityChange observer above.
				SetWeaponEquipmentEpochBaselineLocked({}, false);
			}
			landingHandoffEpoch = _weaponEquipmentIdentityEpoch;
			_useGeneratedCombatTopology = false;
			_aerialCombatUnsupportedNotified = false;
			_forwardInput = 0.0F;
			_strafeInput = 0.0F;
			_verticalInput = 0.0F;
			_pendingLaunchBoost = 0.0F;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kOff);
			_landingContactTicks = 0;
			_shoutGraphOverrideUntil = {};
			_whirlwindSprintUntil = {};
			_whirlwindSprintShoutPending = false;
			_smoothedFlightVelocity = RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F };
			_magickaDrainCarrySeconds = 0.0F;
			_lastMagickaDrainAt = {};
			keepGroundWeaponObservationPump = repeatedGroundStop.keepPump;
			if (groundObserverRebased) {
				const bool previousGroundTargetDrawn = _groundWeaponTargetDrawn;
				_groundWeaponObservationPending = true;
				_groundWeaponFallbackIssued = false;
				_groundWeaponFallbackRetryUsed = false;
				_groundWeaponProgressExtensionUsed = false;
				_groundWeaponTargetDrawn = previousGroundTargetDrawn;
				_groundWeaponActorFormId = stopIdentityCaptured && player ?
					static_cast<std::uint32_t>(player->GetFormID()) : 0;
				_groundWeaponPreEdgeState = static_cast<std::int32_t>(weaponState);
				_groundWeaponSessionId = _flightSessionId;
				_groundWeaponEdgeSequence = repeatedGroundStop.state.edgeSequence;
				_groundWeaponIdentityEpoch = _weaponEquipmentIdentityEpoch;
				_groundWeaponDeadline = std::chrono::steady_clock::now() + GroundWeaponObservationTimeout;
				_groundWeaponEquipmentIdentity = stopEquipmentIdentity;
				_groundWeaponEquipmentIdentityCaptured = stopIdentityCaptured;
			} else if (!groundObserverPreserved && repeatedGroundStop.clear) {
				_groundWeaponObservationPending = false;
				_groundWeaponFallbackIssued = false;
				_groundWeaponFallbackRetryUsed = false;
				_groundWeaponProgressExtensionUsed = false;
				_groundWeaponDeadline = {};
				_groundWeaponActorFormId = 0;
				_groundWeaponPreEdgeState = -1;
				_groundWeaponSessionId = 0;
				_groundWeaponIdentityEpoch = 0;
				_groundWeaponEquipmentIdentity = {};
				_groundWeaponEquipmentIdentityCaptured = false;
			}
			groundObserverTargetDrawn = _groundWeaponTargetDrawn;
			groundObserverEdgeSequence = _groundWeaponEdgeSequence;
			groundObserverIdentityEpoch = _groundWeaponIdentityEpoch;
			// Stop ends the Ready command's ownership window.  Rebase the barrier
			// under the same native-action gate so a delayed in-flight token cannot
			// mutate the post-flight observer or fallback.
			_readyGenerationBarrier = State::ResetReadyGenerationBarrier(
				_readyGenerationBarrier,
				_flightSessionId,
				_readyGenerationCounter.load(std::memory_order_relaxed));
			ClearReadyGenerationLeaseLocked();
			_readyGenerationBlockedDiagnosticLogged = false;
			(void)AdvanceUpdateTaskGeneration();
		}

		// StopFlight is also the idempotent landing cleanup path.  It disables
		// DAF graph state, but deliberately never requests a new sheathe/draw.
		if (worldCleanupOwned) {
			ClampStopVelocityForSafeRelease(player);
		}
		if (logicalCleanupOwned) {
			ArmDafMagickaRegenObservation(std::chrono::steady_clock::now());
		} else {
			// DAF-only ledger cleanup is still safe on a repeated stop, but no
			// actor-value write is permitted once the world token was consumed.
			bool observationPending = false;
			{
				std::shared_lock lock(_mutex);
				observationPending = _magickaRegenObservationPending;
			}
			if (!observationPending) {
				std::unique_lock lock(_mutex);
				_magickaRegenDelayBaseline = 0.0F;
				_magickaRegenDelayAfterDrain = 0.0F;
				_magickaRegenDelayOwned = false;
				_magickaRegenAwaitingDelayedIncrease = false;
			}
		}
		{
			std::shared_lock lock(_mutex);
			keepRegenObservationPump = _magickaRegenObservationPending;
		}
		if (logicalCleanupOwned && player && player->Is3DLoaded()) {
			SetFlightGraphVariables(
				player,
				HasDragonAspectActive(),
				false,
				false,
				false,
				false,
				false,
				FlightGraphState::kOff);
			if (worldCleanupOwned) {
				auto* restoreController = player->GetCharController();
				if (restoreController != currentController) {
					// The controller can be replaced between the ownership snapshot and
					// cleanup.  Never apply controller A's baseline to controller B (or
					// to a newly absent controller); the next verified flight session must
					// capture the replacement's own baseline instead.
					logger::error(
						"event=flight_cleanup_skipped reason=controller_replaced_before_restore "
						"session={} world_cleanup=retired",
						session);
				} else if (restoreController) {
					restoreController->gravity = originalGravity;
					if (originalNoFriction) {
						restoreController->flags.set(RE::CHARACTER_FLAGS::kNoFriction);
					} else {
						restoreController->flags.reset(RE::CHARACTER_FLAGS::kNoFriction);
					}
				}
			}
		}

		if (auto* inputHandler = InputHandler::GetSingleton()) {
			inputHandler->ResetFlightInputState("flight_stop");
		}

		// Landing retires the flight identity transaction, then hands any still
		// transitional weapon state to a fresh ground observer bound to the live
		// identity.  No pre-landing form/epoch is carried across this boundary.
		const auto landingHandoff = State::DecideLandingWeaponHandoff(
			wasFlying,
			landingTransitionPending,
			landingHandoffTargetDrawn,
			actorInWeaponTransition,
			stopIdentityCaptured,
			stopIdentityCaptured);
		if (!groundObserverPreserved && !groundObserverRebased &&
			landingHandoff.armGroundObserver && player && player->Is3DLoaded()) {
			const auto liveIdentity = GetEquipmentDiagnostic(player).identity;
			const auto* liveActorState = player->AsActorState();
			State::FlightInputActionSnapshot handoff{
				State::FlightInputAction::kObserveGroundWeaponTransition };
			handoff.flag = landingHandoff.targetDrawn;
			handoff.actorFormId = static_cast<std::uint32_t>(player->GetFormID());
			handoff.actorWeaponState = liveActorState ?
				static_cast<std::int32_t>(liveActorState->GetWeaponState()) :
				static_cast<std::int32_t>(RE::WEAPON_STATE::kSheathed);
			handoff.equipmentIdentity = liveIdentity;
			handoff.equipmentIdentityCaptured = true;
			handoff.equipmentIdentityEpoch = landingHandoffEpoch;
			StartGroundWeaponObservation(handoff);
			landingHandoffQueued = true;
			logger::info(
				"event=landing_weapon_handoff session={} decision=armed target_drawn={} "
				"identity_epoch={} actor_form=0x{:08X} actor_weapon_state={} "
				"reason=landing_transition_current_identity bounded_observer=true",
				session,
				landingHandoff.targetDrawn,
				landingHandoffEpoch,
				handoff.actorFormId,
				handoff.actorWeaponState);
		}
		if (groundObserverPreserved || groundObserverRebased) {
			landingHandoffQueued = true;
			logger::info(
				"event=ground_weapon_observer session={} decision={} reason=repeated_stop_current_identity "
				"target_drawn={} edge_seq={} identity_epoch={} pump_retained=true",
				session,
				groundObserverPreserved ? "preserved" : "rebased",
				groundObserverTargetDrawn,
				groundObserverEdgeSequence,
				groundObserverIdentityEpoch);
		}

		logger::info(
			"event=landing_cleanup session={} was_flying={} pending_transition={} post_flight_transition={} "
			"target_drawn={} actual_drawn={} weapon_state={} graph_active=false combat_active=false "
			"shout_held=false block_requested=false regen_observation_pending={} world_cleanup={}",
			session,
			wasFlying,
			hadPendingTransition,
			keepTransitionPump,
			transitionTargetDrawn,
			weaponsDrawn,
			static_cast<std::int32_t>(weaponState),
			keepRegenObservationPump,
			worldCleanupOwned ? "performed" : "skipped_idempotent");
		if (!worldCleanupOwned) {
			logger::info(
				"event=landing_cleanup_idempotent session={} world_cleanup=skipped "
				"controller_velocity_untouched=true gravity_untouched=true fall_state_untouched=true",
				session);
		}
		if (wasFlying) {
			logger::info(
				"event=flight_stop session={} version={} reason=controller_stop pending_transition={} post_flight_transition={}",
				session,
				BuildVersion,
				hadPendingTransition,
				keepTransitionPump);
		}
		if (detailedLogging && hadPendingTransition) {
			logger::info(
				"event=post_flight_weapon_observer session={} target_drawn={} actual_drawn={} weapon_state={} "
				"pump_retained=true",
				session,
				transitionTargetDrawn,
				weaponsDrawn,
				static_cast<std::int32_t>(weaponState));
		}

		if (keepTransitionPump || keepRegenObservationPump || landingHandoffQueued) {
			(void)StartUpdateThread();
		} else {
			StopUpdateThread();
		}
		if (keepGroundWeaponObservationPump && !groundObserverPreserved && !groundObserverRebased) {
			logger::info(
				"event=ground_weapon_observer session={} decision=abort reason=flight_stop "
				"flight_graph_untouched=true controller_untouched=true gravity_untouched=true",
				session);
		}
		if (auto* inputHandler = InputHandler::GetSingleton()) {
			inputHandler->RefreshGameThreadState();
		}
	}

	bool FlightManager::IsFlying() const
	{
		std::shared_lock lock(_mutex);
		return _isFlying;
	}

	bool FlightManager::IsDescending() const
	{
		std::shared_lock lock(_mutex);
		return _isDescending;
	}

	bool FlightManager::IsFlightCombatActive() const
	{
		std::shared_lock lock(_mutex);
		return _flightCombatActive;
	}

	bool FlightManager::IsFlightBlockRequested() const
	{
		std::shared_lock lock(_mutex);
		return _flightBlockRequested;
	}

	bool FlightManager::IsDragonAspectActive() const
	{
		return HasDragonAspectActive();
	}

	State::WeaponEquipmentEpochSnapshot FlightManager::GetCurrentWeaponEquipmentSnapshot()
	{
		// Hold the producer gate while reading the engine identity as well as while
		// committing the tracker state.  This makes the returned tuple one serialized
		// observation relative to HandleEquipmentIdentityChange and block requests.
		std::unique_lock nativeGate(_readyNativeActionMutex);
		auto* player = GetPlayer();
		const bool identityCaptured = player && player->Is3DLoaded();
		const auto identity = identityCaptured ?
			GetEquipmentDiagnostic(player).identity : State::WeaponEquipmentIdentity{};
		std::unique_lock lock(_mutex);
		const auto observation = ObserveWeaponEquipmentIdentityLocked(identity, identityCaptured);
		return {
			identityCaptured ? identity : State::WeaponEquipmentIdentity{},
			observation.state.epoch,
			identityCaptured };
	}

	State::WeaponEquipmentEpochObservation FlightManager::ObserveWeaponEquipmentIdentityLocked(
		const State::WeaponEquipmentIdentity& a_identity,
		bool a_identityCaptured)
	{
		const auto observation = State::ObserveWeaponEquipmentEpoch(
			_weaponEquipmentEpochTracker,
			a_identity,
			a_identityCaptured);
		_weaponEquipmentEpochTracker = observation.state;
		_lastEquipmentIdentity = observation.state.identity;
		_lastEquipmentIdentityCaptured = observation.state.captured;
		_weaponEquipmentIdentityEpoch = observation.state.epoch;
		if (observation.identityChanged) {
			// GetCurrentWeaponEquipmentSnapshot() is called by the input producer.
			// Retain a coalesced edge until HandleEquipmentIdentityChange consumes it,
			// so A -> B -> C observed before one update cannot be lost.  Keep the first
			// prior identity for diagnostics and the latest epoch for invalidation; one
			// revocation is sufficient because every intermediate token is stale.
			if (!_weaponEquipmentIdentityChangePending) {
				_weaponEquipmentPreviousIdentityPending = observation.previousIdentity;
				_weaponEquipmentIdentityPendingFromEpoch = observation.previousEpoch;
			}
			_weaponEquipmentIdentityChangePending = true;
			_weaponEquipmentIdentityPendingToEpoch = observation.state.epoch;
		}
		return observation;
	}

	void FlightManager::SetWeaponEquipmentEpochBaselineLocked(
		const State::WeaponEquipmentIdentity& a_identity,
		bool a_identityCaptured)
	{
		// This is a mirror/rebase helper only.  It deliberately never initializes or
		// advances the epoch; all captured baselines must come from the authoritative
		// ObserveWeaponEquipmentIdentityLocked producer observation.
		_weaponEquipmentEpochTracker.identity = a_identity;
		_weaponEquipmentEpochTracker.captured = a_identityCaptured;
		_lastEquipmentIdentity = a_identity;
		_lastEquipmentIdentityCaptured = a_identityCaptured;
		_weaponEquipmentIdentityEpoch = _weaponEquipmentEpochTracker.epoch;
		_weaponEquipmentIdentityChangePending = false;
		_weaponEquipmentPreviousIdentityPending = {};
		_weaponEquipmentIdentityPendingFromEpoch = 0;
		_weaponEquipmentIdentityPendingToEpoch = 0;
	}

	std::uint64_t FlightManager::GetCurrentWeaponEquipmentIdentityEpoch()
	{
		return GetCurrentWeaponEquipmentSnapshot().epoch;
	}

	bool FlightManager::HandleEquipmentIdentityChange(
		const State::WeaponEquipmentIdentity& a_current,
		std::string_view a_reason)
	{
		// This method can be reached from UpdateFlight while the gate is already
		// held, so the recursive mutex is intentional.  Keeping identity rebases
		// and block-intent metadata in the same gate prevents an old queued edge from
		// surviving after we revoke the retired DAF lease.
		std::unique_lock nativeGate(_readyNativeActionMutex);
		State::WeaponEquipmentIdentity previous{};
		bool previousCaptured = false;
		bool baselineChanged = false;
		bool transitionChanged = false;
		bool groundChanged = false;
		bool transitionCancelled = false;
		bool groundObserverCancelled = false;
		bool fallbackDisarmed = false;
		bool combatReadyPreserved = false;
		bool combatReadyTarget = false;
		bool hadSwapState = false;
		bool flightSwapArmed = false;
		bool blockOwnershipRevoked = false;
		bool blockNativeOwnershipRevoked = false;
		std::uint64_t session = 0;
		std::uint64_t sequence = 0;

		{
			std::unique_lock lock(_mutex);
			const auto identityObservation = ObserveWeaponEquipmentIdentityLocked(a_current, true);
			const bool pendingIdentityChange = _weaponEquipmentIdentityChangePending;
			if (identityObservation.identityChanged) {
				previous = identityObservation.previousIdentity;
				previousCaptured = true;
			} else if (pendingIdentityChange) {
				previous = _weaponEquipmentPreviousIdentityPending;
				previousCaptured = true;
			}
			const bool producerIdentityChanged =
				identityObservation.identityChanged || pendingIdentityChange;
			const auto baselineDecision = producerIdentityChanged ?
				State::WeaponEquipmentChangeDecision{
					State::WeaponEquipmentChange::kEquipmentSwap,
					true,
					true,
					true } :
				(_lastEquipmentIdentityCaptured ?
					State::DecideWeaponEquipmentChange(_lastEquipmentIdentity, a_current) :
					State::WeaponEquipmentChangeDecision{});
			const auto transitionDecision =
				(_weaponTransitionPending && _weaponTransitionEquipmentIdentityCaptured) ?
				State::DecideWeaponEquipmentChange(_weaponTransitionEquipmentIdentity, a_current) :
				State::WeaponEquipmentChangeDecision{};
			const auto groundDecision =
				(_groundWeaponObservationPending && _groundWeaponEquipmentIdentityCaptured) ?
				State::DecideWeaponEquipmentChange(_groundWeaponEquipmentIdentity, a_current) :
				State::WeaponEquipmentChangeDecision{};
			baselineChanged = producerIdentityChanged ||
				baselineDecision.change == State::WeaponEquipmentChange::kEquipmentSwap;
			transitionChanged = transitionDecision.change == State::WeaponEquipmentChange::kEquipmentSwap;
			groundChanged = groundDecision.change == State::WeaponEquipmentChange::kEquipmentSwap;

			if (!baselineChanged && !transitionChanged && !groundChanged) {
				return false;
			}
			// A new equipment incarnation releases any quarantine tied to the
			// retired identity, even when no flight transition is pending.
			_weaponTransitionTerminalFailureHold = {};

			if (!previousCaptured && _lastEquipmentIdentityCaptured && baselineChanged) {
				previous = _lastEquipmentIdentity;
				previousCaptured = true;
			} else if (transitionChanged) {
				previous = _weaponTransitionEquipmentIdentity;
				previousCaptured = true;
			} else if (groundChanged) {
				previous = _groundWeaponEquipmentIdentity;
				previousCaptured = true;
			}

			hadSwapState = _weaponEquipmentSwap.pending ||
				State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap) ||
				State::IsWeaponEquipmentSwapReconciliationRequired(_weaponEquipmentSwap);
			combatReadyTarget = _flightCombatActive;
			flightSwapArmed = _isFlying;
			if (flightSwapArmed) {
				_weaponEquipmentSwap = State::ArmWeaponEquipmentSwap(_weaponEquipmentIdentityEpoch);
				_weaponEquipmentSwapPreviousIdentity = previous;
				_weaponEquipmentSwapCurrentIdentity = a_current;
				_weaponEquipmentSwapIdentityCaptured = previousCaptured;
				_weaponEquipmentSwapPinnedDiagnosticLogged = false;
				combatReadyPreserved = true;
			} else {
				// Ground equipment changes cancel only the observer.  They must not
				// leave a flight-only readiness pin behind for the next Ready edge.
				_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
				_weaponEquipmentSwapPreviousIdentity = {};
				_weaponEquipmentSwapCurrentIdentity = {};
				_weaponEquipmentSwapIdentityCaptured = false;
				_weaponEquipmentSwapPinnedDiagnosticLogged = false;
			}
			session = _flightSessionId;
			sequence = _weaponTransitionSequence;
			const bool blockIdentityChanged =
				baselineChanged || transitionChanged || groundChanged;
			const bool blockRequestBoundToCurrent =
				_flightBlockRequested &&
				_flightBlockRequestedIdentityCaptured &&
				State::IsUsableWeaponEquipmentEpoch(_flightBlockRequestedIdentityEpoch) &&
				_flightBlockRequestedIdentityEpoch == _weaponEquipmentIdentityEpoch &&
				State::ExactWeaponEquipmentIdentity(
					_flightBlockRequestedIdentity,
					a_current);
			const bool blockLeaseBoundToCurrent =
				_flightBlockLease.active &&
				State::IsUsableWeaponEquipmentEpoch(_flightBlockLease.identityEpoch) &&
				State::IsUsableWeaponEquipmentEpoch(_weaponEquipmentIdentityEpoch) &&
				_flightBlockLease.identityEpoch == _weaponEquipmentIdentityEpoch &&
				State::ExactWeaponEquipmentIdentity(_flightBlockLease.identity, a_current);
			const bool blockRequestRevoked = blockIdentityChanged &&
				_flightBlockRequested && !blockRequestBoundToCurrent;
			blockNativeOwnershipRevoked = blockIdentityChanged &&
				_flightBlockLease.active && !blockLeaseBoundToCurrent;
			blockOwnershipRevoked = blockRequestRevoked || blockNativeOwnershipRevoked;
			if (blockRequestRevoked) {
				_flightBlockRequested = false;
				_flightBlockRequestedIdentity = {};
				_flightBlockRequestedIdentityEpoch = 0;
				_flightBlockRequestedIdentityCaptured = false;
			}
			if (blockNativeOwnershipRevoked) {
				// The old identity's lease is no longer current.  Revoke only the
				// local authority; the actor/graph state is deliberately left untouched
				// because writing after the identity edge could clear a replacement or
				// third-party block.  A later release is therefore unable to authorize a
				// stale native write.
				_flightBlockNativeStateOwned = false;
				_flightBlockLease.active = false;
			}

			// InputHandler may observe A -> B, then arm a B-bound transaction before
			// this update callback consumes the pending producer edge.  Revoke only
			// transactions still bound to the retired identity; a current-epoch B
			// transaction has already passed the strict producer guard.
			const bool transitionBoundToCurrent =
				_weaponTransitionPending &&
				_weaponTransitionEquipmentIdentityCaptured &&
				State::IsUsableWeaponEquipmentEpoch(_weaponTransitionIdentityEpoch) &&
				_weaponTransitionIdentityEpoch == _weaponEquipmentIdentityEpoch &&
				State::ExactWeaponEquipmentIdentity(
					_weaponTransitionEquipmentIdentity,
					a_current);
			const bool groundBoundToCurrent =
				_groundWeaponObservationPending &&
				_groundWeaponEquipmentIdentityCaptured &&
				State::IsUsableWeaponEquipmentEpoch(_groundWeaponIdentityEpoch) &&
				_groundWeaponIdentityEpoch == _weaponEquipmentIdentityEpoch &&
				State::ExactWeaponEquipmentIdentity(
					_groundWeaponEquipmentIdentity,
					a_current);
			const auto& identityDecision = baselineChanged ? baselineDecision :
				(transitionChanged ? transitionDecision : groundDecision);
			if (identityDecision.cancelPending &&
				(transitionChanged ||
					(baselineChanged && _weaponTransitionPending && !transitionBoundToCurrent))) {
				transitionCancelled = true;
				fallbackDisarmed = identityDecision.disarmNativeFallback &&
					_weaponTransitionNativeFallbackArmed;
				_weaponTransitionPending = false;
				_weaponTransitionNativeFallbackArmed = false;
				_weaponTransitionExpiryRecoveryAttempted = false;
				_weaponTransitionNativeFallbackRetryUsed = false;
				_weaponTransitionProgressExtensionUsed = false;
				_weaponTransitionPostFlight = false;
				_weaponTransitionDeadline = {};
				_weaponTransitionNativeFallbackAt = {};
				_weaponTransitionPreRequestState = -1;
				_weaponTransitionActorFormId = 0;
				_weaponTransitionSessionId = 0;
				_weaponTransitionIdentityEpoch = 0;
				_weaponTransitionEquipmentIdentity = {};
				_weaponTransitionEquipmentIdentityCaptured = false;
			}
			if (identityDecision.cancelPending &&
				(groundChanged ||
					(baselineChanged && _groundWeaponObservationPending && !groundBoundToCurrent))) {
				const auto observerDecision = State::ReduceGroundWeaponObserver(
					State::GroundWeaponObserverState{
						_groundWeaponObservationPending,
						_groundWeaponFallbackIssued,
						_groundWeaponEdgeSequence },
					State::GroundWeaponObserverEvent::kIdentityChanged);
				groundObserverCancelled = observerDecision.cancelled;
				_groundWeaponObservationPending = observerDecision.state.pending;
				_groundWeaponFallbackIssued = observerDecision.state.fallbackIssued;
				_groundWeaponProgressExtensionUsed = false;
				_groundWeaponDeadline = {};
				_groundWeaponEquipmentIdentity = {};
				_groundWeaponEquipmentIdentityCaptured = false;
			}
			// The producer observation (possibly performed by InputHandler before this
			// callback) has already advanced the epoch.  Consume exactly that edge so a
			// later callback observing the same identity cannot advance it again.
			_weaponEquipmentIdentityChangePending = false;
			_weaponEquipmentPreviousIdentityPending = {};
			_weaponEquipmentIdentityPendingFromEpoch = 0;
			_weaponEquipmentIdentityPendingToEpoch = 0;
		}

		logger::info(
			"event=weapon_transition_equipment_swap session={} sequence={} phase={} reason={} outcome=equipment_swap "
			"transition_cancelled={} ground_observer_cancelled={} fallback_disarmed={} combat_ready_preserved={} "
			"combat_ready_target={} flight_pin_armed={} block_ownership_revoked={} "
			"block_native_ownership_revoked={} block_operation=none "
			"previous_identity_captured={} previous_right_form=0x{:08X} previous_left_form=0x{:08X} "
			"previous_right_type={} previous_left_type={} previous_family={} "
			"current_right_form=0x{:08X} current_left_form=0x{:08X} current_right_type={} current_left_type={} "
			"current_family={} engine_operation=transition_passthrough",
			session,
			sequence,
			hadSwapState ? "rebase" : "start",
			a_reason,
			transitionCancelled,
			groundObserverCancelled,
			fallbackDisarmed,
			combatReadyPreserved,
			combatReadyTarget,
			flightSwapArmed,
			blockOwnershipRevoked,
			blockNativeOwnershipRevoked,
			previousCaptured,
			previous.rightFormId,
			previous.leftFormId,
			previous.rightWeaponType,
			previous.leftWeaponType,
			State::WeaponEquipmentFamilyName(previous.family),
			a_current.rightFormId,
			a_current.leftFormId,
			a_current.rightWeaponType,
			a_current.leftWeaponType,
			State::WeaponEquipmentFamilyName(a_current.family));
		return true;
	}

	bool FlightManager::SetFlightCombatActive(bool a_active)
	{
		auto* player = GetPlayer();
		if (!player || !player->Is3DLoaded()) {
			return false;
		}

		const auto* actorState = player->AsActorState();
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
		const auto equipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
		if (!equipmentSnapshot.captured) {
			return false;
		}
		const auto equipment = GetEquipmentDiagnostic(player);
		HandleEquipmentIdentityChange(equipmentSnapshot.identity, "combat_request");
		bool transitionPending = false;
		bool transitionTargetDrawn = false;
		std::uint64_t transitionSequence = 0;
		{
			std::unique_lock lock(_mutex);
			if (!_isFlying || (a_active && !HasDragonAspectActive())) {
				return false;
			}
			// An accepted explicit Ready/combat request is a fresh owner decision;
			// release any prior terminal-failure quarantine before applying it.
			_weaponTransitionTerminalFailureHold = {};
			_flightCombatActive = a_active;
			_useGeneratedCombatTopology = false;
			if (IsWeaponStateAtTarget(weaponState, a_active)) {
				const bool readinessPinActive = State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap);
				// The requested state is already authoritative. Cancel any stale
				// transition, including one that was targeting the opposite state.
				_weaponTransitionPending = false;
				_weaponTransitionTargetDrawn = a_active;
				_weaponTransitionNativeFallbackArmed = false;
				_weaponTransitionExpiryRecoveryAttempted = false;
				_weaponTransitionNativeFallbackRetryUsed = false;
				_weaponTransitionProgressExtensionUsed = false;
				_weaponTransitionPostFlight = false;
				_weaponTransitionDeadline = {};
				_weaponTransitionNativeFallbackAt = {};
				_weaponTransitionPreRequestState = -1;
				_weaponTransitionActorFormId = 0;
				_weaponTransitionSessionId = 0;
				_weaponTransitionIdentityEpoch = 0;
				_weaponTransitionEquipmentIdentity = {};
				_weaponTransitionEquipmentIdentityCaptured = false;
				if (!readinessPinActive) {
					_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
					_weaponEquipmentSwapPreviousIdentity = {};
					_weaponEquipmentSwapCurrentIdentity = {};
					_weaponEquipmentSwapIdentityCaptured = false;
					_weaponEquipmentSwapPinnedDiagnosticLogged = false;
				}
			} else {
				if (!_weaponTransitionPending || _weaponTransitionTargetDrawn != a_active) {
					++_weaponTransitionSequence;
					_weaponTransitionActionId = _applyingActionId;
				_weaponTransitionPending = true;
					_weaponTransitionTargetDrawn = a_active;
					_weaponTransitionNativeFallbackArmed = false;
					_weaponTransitionExpiryRecoveryAttempted = false;
					_weaponTransitionNativeFallbackRetryUsed = false;
					_weaponTransitionProgressExtensionUsed = false;
					_weaponTransitionPostFlight = false;
					_weaponTransitionDeadline = std::chrono::steady_clock::now() + WeaponTransitionTimeout;
					_weaponTransitionNativeFallbackAt = {};
					_weaponTransitionPreRequestState = static_cast<std::int32_t>(weaponState);
					_weaponTransitionActorFormId = static_cast<std::uint32_t>(player->GetFormID());
					_weaponTransitionSessionId = _flightSessionId;
					_weaponTransitionIdentityEpoch = _weaponEquipmentIdentityEpoch;
					_weaponTransitionEquipmentIdentity = equipment.identity;
					_weaponTransitionEquipmentIdentityCaptured = true;
					_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
					_weaponEquipmentSwapPreviousIdentity = {};
					_weaponEquipmentSwapCurrentIdentity = {};
					_weaponEquipmentSwapIdentityCaptured = false;
					_weaponEquipmentSwapPinnedDiagnosticLogged = false;
				}
				// A repeated request for the same target keeps the original deadline
				// and any Ready Weapon fallback already armed for that transition.
			}
			transitionPending = _weaponTransitionPending;
			transitionTargetDrawn = _weaponTransitionTargetDrawn;
			transitionSequence = _weaponTransitionSequence;
		}

		const bool combatVariableWritten =
			player->SetGraphVariableBool(RE::BSFixedString(GraphVarFlightCombatActive), a_active);
		if (!combatVariableWritten && !GraphVariableWriteFailureLogged.exchange(true)) {
			logger::warn(
				"Dragon Aspect Flight: bDAF_FlightCombatActive was unavailable; "
				"verify Behavior Data Injector and DragonAspectFlight_BDI.json");
		}

		bool enabled = false;
		std::uint64_t session = 0;
		{
			std::shared_lock lock(_mutex);
			enabled = _detailedLogging;
			session = _flightSessionId;
		}
		if (enabled) {
			logger::info(
				"event=combat_state session={} active={} graph_write_ok={} weapons_drawn={} weapon_state={} "
				"transition_pending={} transition_target_drawn={} transition_sequence={} "
				"expected_oar_family={} quarterstaff={} quarterstaff_keyword={} block_capable={}",
				session,
				a_active,
				combatVariableWritten,
				weaponsDrawn,
				static_cast<std::int32_t>(weaponState),
				transitionPending,
				transitionTargetDrawn,
				transitionSequence,
				equipment.expectedOarFamily,
				equipment.quarterstaffEquipped,
				equipment.quarterstaffKeyword,
				equipment.blockCapable);
		}

		return true;
	}

	bool FlightManager::ToggleFlightCombatReady()
	{
		return ToggleFlightCombatReadyWithIdentity(std::nullopt, 0);
	}

	bool FlightManager::ToggleFlightCombatReadyWithIdentity(
		std::optional<State::WeaponEquipmentIdentity> a_observedIdentity,
		std::uint64_t a_observedIdentityEpoch)
	{
		std::uint64_t applyingActionId = 0;
		std::uint64_t applyingInputSequenceDomain = 0;
		std::uint64_t managerSequence = 0;
		std::uint64_t session = 0;
		{
			std::shared_lock lock(_mutex);
			applyingActionId = _applyingActionId;
			applyingInputSequenceDomain = _applyingInputSequenceDomain;
			managerSequence = _activeQueuedActionSequence;
			session = _flightSessionId;
		}
		auto* player = GetPlayer();
		if (!player || !player->Is3DLoaded()) {
			return false;
		}

		const auto* actorState = player->AsActorState();
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
		const auto currentEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
		const auto currentIdentity = currentEquipmentSnapshot.identity;
		const auto currentIdentityEpoch = currentEquipmentSnapshot.epoch;
		const bool currentIdentityCaptured = currentEquipmentSnapshot.captured;
		if (a_observedIdentity.has_value() &&
			!State::ShouldApplyCurrentEquipmentAction(
				true,
				currentIdentityCaptured,
				a_observedIdentity.value(),
				currentIdentity,
				a_observedIdentityEpoch,
				currentIdentityEpoch)) {
			logger::info(
				"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} action=toggle_combat_ready outcome=rejected "
				"reason=equipment_identity_or_epoch_stale action_epoch={} current_epoch={}",
				State::StructuredDiagnosticSchemaVersion,
				applyingActionId,
				session,
				applyingInputSequenceDomain,
				managerSequence,
				a_observedIdentityEpoch,
				currentIdentityEpoch);
			return false;
		}
		const bool observedIdentityMismatch = a_observedIdentity.has_value() &&
			!State::SameWeaponEquipmentIdentity(a_observedIdentity.value(), currentIdentity);
		const bool equipmentChanged = HandleEquipmentIdentityChange(currentIdentity, "ready_input");
		bool explicitReadyClearedPin = false;
		State::WeaponEquipmentIdentity pinPreviousIdentity{};
		State::WeaponEquipmentIdentity pinCurrentIdentity{};
		std::uint64_t pinSession = 0;
		std::uint64_t pinSequence = 0;
		bool pinTargetDrawn = false;
		bool mismatchTransitionCancelled = false;
		bool mismatchFallbackDisarmed = false;
		if (!equipmentChanged && !observedIdentityMismatch) {
			std::unique_lock lock(_mutex);
			if (State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap) ||
				State::IsWeaponEquipmentSwapReconciliationRequired(_weaponEquipmentSwap)) {
				explicitReadyClearedPin = true;
				pinPreviousIdentity = _weaponEquipmentSwapPreviousIdentity;
				pinCurrentIdentity = _weaponEquipmentSwapCurrentIdentity;
				pinSession = _flightSessionId;
				pinSequence = _weaponTransitionSequence;
				pinTargetDrawn = _flightCombatActive;
				_weaponEquipmentSwap = State::ObserveWeaponEquipmentSwap(
					_weaponEquipmentSwap,
					State::WeaponEquipmentSwapObservation::kStableAtLogicalTarget,
					true);
				_weaponEquipmentSwapPreviousIdentity = {};
				_weaponEquipmentSwapCurrentIdentity = {};
				_weaponEquipmentSwapIdentityCaptured = false;
				_weaponEquipmentSwapPinnedDiagnosticLogged = false;
			}
		}
		if (explicitReadyClearedPin) {
			logger::info(
				"event=weapon_transition_equipment_swap session={} sequence={} phase=explicit-ready-clear "
				"pending_after=false edge_observed_after=false settled=false pinned_after=false "
				"combat_ready_target={} target_drawn={} previous_right_form=0x{:08X} previous_left_form=0x{:08X} "
				"previous_right_type={} previous_left_type={} previous_family={} "
				"current_right_form=0x{:08X} current_left_form=0x{:08X} current_right_type={} current_left_type={} "
				"current_family={} engine_operation=ready_command",
				pinSession,
				pinSequence,
				pinTargetDrawn,
				pinTargetDrawn,
				pinPreviousIdentity.rightFormId,
				pinPreviousIdentity.leftFormId,
				pinPreviousIdentity.rightWeaponType,
				pinPreviousIdentity.leftWeaponType,
				State::WeaponEquipmentFamilyName(pinPreviousIdentity.family),
				pinCurrentIdentity.rightFormId,
				pinCurrentIdentity.leftFormId,
				pinCurrentIdentity.rightWeaponType,
				pinCurrentIdentity.leftWeaponType,
				State::WeaponEquipmentFamilyName(pinCurrentIdentity.family));
		}
		if (observedIdentityMismatch && !equipmentChanged) {
			// The input snapshot is captured before vanilla's equipment operation.
			// If the queued game-thread task sees a different stable identity, this
			// edge belongs to that operation and must not become a DAF readiness
			// toggle or a stale native fallback request.
			{
				std::unique_lock lock(_mutex);
				// This is an accepted explicit Ready edge with a newer observed
				// identity; it releases any prior terminal-failure quarantine even
				// when the edge is consumed as a rebind rather than a toggle.
				_weaponTransitionTerminalFailureHold = {};
				const auto transitionDecision =
					(_weaponTransitionPending && _weaponTransitionEquipmentIdentityCaptured) ?
					State::DecideWeaponEquipmentChange(_weaponTransitionEquipmentIdentity, currentIdentity) :
					State::WeaponEquipmentChangeDecision{};
				const bool incompatibleTransition =
					(_weaponTransitionPending || _weaponTransitionNativeFallbackArmed) &&
					(!_weaponTransitionEquipmentIdentityCaptured || transitionDecision.cancelPending);
				if (incompatibleTransition) {
					mismatchTransitionCancelled = _weaponTransitionPending;
					mismatchFallbackDisarmed = _weaponTransitionNativeFallbackArmed;
					_weaponTransitionPending = false;
					_weaponTransitionTargetDrawn = _flightCombatActive;
					_weaponTransitionNativeFallbackArmed = false;
					_weaponTransitionExpiryRecoveryAttempted = false;
					_weaponTransitionNativeFallbackRetryUsed = false;
					_weaponTransitionProgressExtensionUsed = false;
					_weaponTransitionPostFlight = false;
					_weaponTransitionDeadline = {};
					_weaponTransitionNativeFallbackAt = {};
					_weaponTransitionPreRequestState = -1;
					_weaponTransitionActorFormId = 0;
					_weaponTransitionSessionId = 0;
					_weaponTransitionIdentityEpoch = 0;
					_weaponTransitionEquipmentIdentity = {};
					_weaponTransitionEquipmentIdentityCaptured = false;
				}
				// The live identity was already observed by the authoritative producer
				// before this mismatch branch.  Leave its pending side-effect edge intact;
				// rebasing here would duplicate the observation and could erase that edge.
				if (_isFlying) {
					_weaponEquipmentSwap = State::ArmWeaponEquipmentSwap(_weaponEquipmentIdentityEpoch);
					_weaponEquipmentSwapPreviousIdentity = *a_observedIdentity;
					_weaponEquipmentSwapCurrentIdentity = currentIdentity;
					_weaponEquipmentSwapIdentityCaptured = true;
					_weaponEquipmentSwapPinnedDiagnosticLogged = false;
				} else {
					_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
					_weaponEquipmentSwapPreviousIdentity = {};
					_weaponEquipmentSwapCurrentIdentity = {};
					_weaponEquipmentSwapIdentityCaptured = false;
					_weaponEquipmentSwapPinnedDiagnosticLogged = false;
				}
			}
			std::uint64_t session = 0;
			std::uint64_t sequence = 0;
			{
				std::shared_lock lock(_mutex);
				session = _flightSessionId;
				sequence = _weaponTransitionSequence;
			}
			logger::info(
			"event=weapon_transition_equipment_swap session={} sequence={} phase=start reason=ready_snapshot_mismatch "
				"outcome=equipment_swap transition_cancelled={} ground_observer_cancelled=false "
				"fallback_disarmed={} combat_ready_preserved=true previous_right_form=0x{:08X} "
				"previous_left_form=0x{:08X} previous_right_type={} previous_left_type={} previous_family={} "
				"current_right_form=0x{:08X} current_left_form=0x{:08X} current_right_type={} "
				"current_left_type={} current_family={} engine_operation=passthrough",
				session,
				sequence,
				mismatchTransitionCancelled,
				mismatchFallbackDisarmed,
				a_observedIdentity->rightFormId,
				a_observedIdentity->leftFormId,
				a_observedIdentity->rightWeaponType,
				a_observedIdentity->leftWeaponType,
				State::WeaponEquipmentFamilyName(a_observedIdentity->family),
				currentIdentity.rightFormId,
				currentIdentity.leftFormId,
				currentIdentity.rightWeaponType,
				currentIdentity.leftWeaponType,
				State::WeaponEquipmentFamilyName(currentIdentity.family));
		}
		if (equipmentChanged || observedIdentityMismatch) {
			return true;
		}
		bool logicalCurrent = false;
		bool nextCombatActive = false;
		{
			std::shared_lock lock(_mutex);
			if (!_isFlying) {
				return false;
			}
			nextCombatActive = State::ResolveWeaponToggleTarget(
				_flightCombatActive,
				_weaponTransitionPending,
				_weaponTransitionTargetDrawn,
				State::ObservedWeaponEdge::kSheathed);
			logicalCurrent = !nextCombatActive;
		}

		if (!nextCombatActive) {
			SetFlightBlockRequested(false);
		}
		if (!SetFlightCombatActive(nextCombatActive)) {
			return false;
		}

		std::uint64_t sequence = 0;
		{
			std::unique_lock lock(_mutex);
			if (!_weaponTransitionPending) {
				// The actor already reached the requested state synchronously.
				return true;
			}
			_weaponTransitionNativeFallbackArmed = true;
			_weaponTransitionNativeFallbackAt =
				std::chrono::steady_clock::now() + WeaponNativeFallbackDelay;
			session = _flightSessionId;
			sequence = _weaponTransitionSequence;
		}

		logger::info(
			"event=weapon_transition_request session={} sequence={} actual_drawn={} weapon_state={} logical_current={} "
			"target_drawn={} native_fallback_delay_ms={} timeout_ms={}",
			session,
			sequence,
			weaponsDrawn,
			static_cast<std::int32_t>(weaponState),
			logicalCurrent,
			nextCombatActive,
			std::chrono::duration_cast<std::chrono::milliseconds>(WeaponNativeFallbackDelay).count(),
			std::chrono::duration_cast<std::chrono::milliseconds>(WeaponTransitionTimeout).count());
		return true;
	}

	bool FlightManager::BeginFlightCombat()
	{
		const bool wasActive = IsFlightCombatActive();
		const bool activated = SetFlightCombatActive(true);
		if (activated && !wasActive) {
			logger::info("Dragon Aspect Flight combat activated by attack/cast input");
		}
		return activated;
	}

	bool FlightManager::QueueFlightAction(State::FlightInputActionSnapshot a_action)
	{
		const auto sourceSession = a_action.sourceSession == State::FlightInputActionSnapshot::UncapturedSession ?
			_publishedFlightSessionId.load(std::memory_order_acquire) : a_action.sourceSession;
		a_action.sourceSession = sourceSession;
		const auto retireReadyToken = [a_action, sourceSession](std::string_view a_reason) noexcept {
			if (!State::IsReadyGenerationTokenValid(a_action.readyToken)) {
				return;
			}
			try {
				FlightManager::GetSingleton().RetireReadyGeneration(
					a_action.readyToken,
					a_reason,
					sourceSession);
			} catch (...) {
				// A failed queue path must not propagate a second failure from token
				// retirement or diagnostics.
			}
		};

		const SKSE::TaskInterface* taskInterface = nullptr;
		try {
			taskInterface = SKSE::GetTaskInterface();
		} catch (const std::exception& e) {
			retireReadyToken("manager_get_task_interface_exception");
			const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
			if (failure.emit) {
				try {
					logger::error(
						"event=flight_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
							"action={} outcome=exception reason=get_task_interface_exception exception=get_task_interface source_session={} "
							"ready_generation={} count={} lease_released=true error={}",
						State::StructuredDiagnosticSchemaVersion, a_action.actionId, a_action.sourceSession,
						a_action.inputSequenceDomain, QueuedFlightActionName(a_action.action),
						a_action.sourceSession,
						a_action.readyToken.generation,
						failure.count,
						e.what());
				} catch (...) {
					// Logging is diagnostic-only and must not escape the queue boundary.
				}
			}
			return false;
		} catch (...) {
			retireReadyToken("manager_get_task_interface_exception");
			const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
			if (failure.emit) {
				try {
					logger::error(
						"event=flight_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
							"action={} outcome=exception reason=get_task_interface_exception exception=get_task_interface source_session={} "
							"ready_generation={} count={} lease_released=true error=unknown",
						State::StructuredDiagnosticSchemaVersion, a_action.actionId, a_action.sourceSession,
						a_action.inputSequenceDomain, QueuedFlightActionName(a_action.action),
						a_action.sourceSession,
						a_action.readyToken.generation,
						failure.count);
				} catch (...) {
					// Logging is diagnostic-only and must not escape the queue boundary.
				}
			}
			return false;
		}
		if (!taskInterface) {
			const auto unavailableDecision = State::DecideTaskInterfaceUnavailable(
				State::IsReadyGenerationTokenValid(a_action.readyToken));
			if (unavailableDecision.retireReadyToken) {
				retireReadyToken("manager_task_interface_unavailable");
			}
			logger::warn(
				"event=flight_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
				"action={} outcome=failed reason=task_interface_unavailable queued=false",
				State::StructuredDiagnosticSchemaVersion, a_action.actionId, a_action.sourceSession,
				a_action.inputSequenceDomain, QueuedFlightActionName(a_action.action));
			return false;
		}

		// Do not inspect FlightManager state on the caller's thread.  Papyrus and
		// input callbacks may run off-thread; the immutable action, sequence, and
		// published source session are the only values that cross this boundary.
		// FlightManager is a process-lifetime singleton.  Capturing this is
		// intentional; the session/action envelope rejects stale lifecycle work,
		// while the active sequence release below cannot clear a newer sequence.
		try {
			taskInterface->AddTask([this, a_action, sourceSession]() {
				std::uint64_t sequence = 0;
				const auto releaseActiveSequence = [this, &sequence]() noexcept {
					if (sequence == 0) {
						return;
					}
					try {
						std::unique_lock lock(_mutex);
						if (_activeQueuedActionSequence == sequence) {
							_activeQueuedActionSequence = 0;
						}
					} catch (...) {
						// Never allow cleanup failure to escape the game-thread task.
					}
				};
				const auto notifyInputReleaseFailure = [a_action](std::string_view a_reason) noexcept {
					try {
						if (auto* inputHandler = InputHandler::GetSingleton()) {
							inputHandler->NotifyFlightActionQueueFailure(a_action, a_reason);
						}
					} catch (...) {
						// Release recovery is best effort and must never reopen the task catch.
					}
				};

				try {
					// Allocate only when this task reaches the game thread.  Cross-thread
					// producers may enqueue in a different order than their fetch_add order;
					// execution-order stamping prevents a legitimate earlier task from being
					// dropped as stale.
					sequence = State::AllocateQueuedActionExecutionSequence(
						_nextQueuedActionSequence);
					if (State::ShouldLogFlightActionQueueAcceptance(a_action.action)) {
						logger::info(
							"event=flight_action_queue action_id={} schema={} session={} input_sequence_domain={} manager_sequence={} "
							"action={} outcome=accepted reason=task_accepted queued=true "
							"session_capture=published_atomic execution_order=game_thread",
							a_action.actionId, State::StructuredDiagnosticSchemaVersion, a_action.sourceSession,
							a_action.inputSequenceDomain, sequence,
						QueuedFlightActionName(a_action.action));
					}
					if (!AcceptQueuedFlightAction(a_action, sequence, sourceSession)) {
						if (State::IsReadyGenerationTokenValid(a_action.readyToken)) {
							RetireReadyGeneration(
								a_action.readyToken,
								sourceSession == a_action.readyToken.session ?
									"queued_action_rejected" : "queued_action_stale_envelope",
								sourceSession);
						}
						if (auto* inputHandler = InputHandler::GetSingleton()) {
							inputHandler->RefreshGameThreadState();
						}
					} else {
						ApplyQueuedFlightAction(a_action);
						if (auto* inputHandler = InputHandler::GetSingleton()) {
							inputHandler->RefreshGameThreadState();
						}
					}
				} catch (const std::exception& e) {
					releaseActiveSequence();
					notifyInputReleaseFailure("manager_action_callback_exception");
					try {
						if (State::IsReadyGenerationTokenValid(a_action.readyToken)) {
							RetireReadyGeneration(
								a_action.readyToken,
								"queued_action_exception",
								sourceSession);
						}
					} catch (...) {
						// The callback is already contained; token cleanup is best effort.
					}
					const auto failure = NextExceptionLog(_flightActionTaskExceptionCount);
					if (failure.emit) {
						try {
							logger::error(
								"event=flight_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
									"action={} outcome=exception reason=callback_exception exception=callback source_session={} "
									"ready_generation={} count={} active_sequence_released=true error={}",
								State::StructuredDiagnosticSchemaVersion,
								a_action.actionId,
								a_action.sourceSession,
								a_action.inputSequenceDomain,
								sequence,
								QueuedFlightActionName(a_action.action),
								a_action.sourceSession,
								a_action.readyToken.generation,
								failure.count,
								e.what());
						} catch (...) {
							// Diagnostics must not escape the callback catch block.
						}
					}
				} catch (...) {
					releaseActiveSequence();
					notifyInputReleaseFailure("manager_action_callback_exception");
					try {
						if (State::IsReadyGenerationTokenValid(a_action.readyToken)) {
							RetireReadyGeneration(
								a_action.readyToken,
								"queued_action_exception",
								sourceSession);
						}
					} catch (...) {
						// The callback is already contained; token cleanup is best effort.
					}
					const auto failure = NextExceptionLog(_flightActionTaskExceptionCount);
					if (failure.emit) {
						try {
							logger::error(
								"event=flight_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
									"action={} outcome=exception reason=callback_exception exception=callback source_session={} "
									"ready_generation={} count={} active_sequence_released=true error=unknown",
								State::StructuredDiagnosticSchemaVersion,
								a_action.actionId,
								a_action.sourceSession,
								a_action.inputSequenceDomain,
								sequence,
								QueuedFlightActionName(a_action.action),
								a_action.sourceSession,
								a_action.readyToken.generation,
								failure.count);
						} catch (...) {
							// Diagnostics must not escape the callback catch block.
						}
					}
				}
				releaseActiveSequence();
			});
		} catch (const std::exception& e) {
			retireReadyToken("manager_add_task_exception");
			const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
			if (failure.emit) {
				try {
					logger::error(
						"event=flight_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
							"action={} outcome=exception reason=add_task_exception exception=add_task source_session={} "
							"ready_generation={} count={} active_sequence_released=true error={}",
						State::StructuredDiagnosticSchemaVersion,
						a_action.actionId,
						a_action.sourceSession,
						a_action.inputSequenceDomain,
						QueuedFlightActionName(a_action.action),
						a_action.sourceSession,
						a_action.readyToken.generation,
						failure.count,
						e.what());
				} catch (...) {
					// Diagnostics must not escape the queue boundary.
				}
			}
			return false;
		} catch (...) {
			retireReadyToken("manager_add_task_exception");
			const auto failure = NextExceptionLog(_flightActionQueueExceptionCount);
			if (failure.emit) {
				try {
					logger::error(
						"event=flight_action_queue schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 "
							"action={} outcome=exception reason=add_task_exception exception=add_task source_session={} "
							"ready_generation={} count={} active_sequence_released=true error=unknown",
						State::StructuredDiagnosticSchemaVersion,
						a_action.actionId,
						a_action.sourceSession,
						a_action.inputSequenceDomain,
						QueuedFlightActionName(a_action.action),
						a_action.sourceSession,
						a_action.readyToken.generation,
						failure.count);
				} catch (...) {
					// Diagnostics must not escape the queue boundary.
				}
			}
			return false;
		}

		return true;
	}

	bool FlightManager::AcceptQueuedFlightAction(
		const State::FlightInputActionSnapshot& a_action,
		std::uint64_t a_sequence,
		std::uint64_t a_sourceSession)
	{
		std::unique_lock lock(_mutex);
		if (!State::IsQueuedActionSequenceNewer(_lastQueuedActionSequence, a_sequence)) {
			logger::info(
				"event=flight_action_drop action_id={} schema={} session={} input_sequence_domain={} manager_sequence={} "
				"action={} outcome=dropped reason=sequence_stale",
				a_action.actionId,
				State::StructuredDiagnosticSchemaVersion,
				a_action.sourceSession,
				a_action.inputSequenceDomain,
				a_sequence,
				QueuedFlightActionName(a_action.action));
			return false;
		}

		const auto currentSession = _flightSessionId;
		// A stop queued immediately after a queued start carries the old session.
		// The pure reducer admits exactly that successor and rejects old tasks
		// before any manager or engine mutation.
		const bool sessionValid = State::IsQueuedActionSessionCurrent(
			a_action.action,
			currentSession,
			a_sourceSession,
			_isFlying,
			_flightSessionStartActionSequence,
			a_sequence);

		_lastQueuedActionSequence = a_sequence;
		if (!sessionValid) {
			logger::info(
				"event=flight_action_drop action_id={} schema={} session={} input_sequence_domain={} manager_sequence={} "
				"action={} outcome=dropped reason=session_stale current_session={}",
				a_action.actionId,
				State::StructuredDiagnosticSchemaVersion,
				a_action.sourceSession,
				a_action.inputSequenceDomain,
				a_sequence,
				QueuedFlightActionName(a_action.action),
				currentSession);
			return false;
		}

		_activeQueuedActionSequence = a_sequence;
		return true;
	}

	void FlightManager::ApplyQueuedFlightAction(State::FlightInputActionSnapshot a_action)
	{
		struct ScopedApplyingCorrelation final {
			std::shared_mutex& mutex;
			std::uint64_t& actionId;
			std::uint64_t& inputDomain;
			std::uint64_t priorActionId;
			std::uint64_t priorInputDomain;
			~ScopedApplyingCorrelation() noexcept { try { std::unique_lock lock(mutex); actionId = priorActionId; inputDomain = priorInputDomain; } catch (...) {} }
		};
		std::unique_lock correlationLock(_mutex);
		ScopedApplyingCorrelation correlationScope{
			_mutex, _applyingActionId, _applyingInputSequenceDomain,
			_applyingActionId, _applyingInputSequenceDomain };
		_applyingActionId = a_action.actionId;
		_applyingInputSequenceDomain = a_action.inputSequenceDomain;
		const auto managerSequence = _activeQueuedActionSequence;
		correlationLock.unlock();
		logger::info(
			"event=action_applied schema={} action_id={} session={} action={} input_sequence_domain={} manager_sequence={} ready_generation={} equipment_epoch={} outcome=applied reason=manager_dispatch",
			State::StructuredDiagnosticSchemaVersion,
			a_action.actionId,
			a_action.sourceSession,
			QueuedFlightActionName(a_action.action),
			a_action.inputSequenceDomain,
			managerSequence,
			a_action.readyToken.generation,
			a_action.equipmentIdentityEpoch);
		switch (a_action.action) {
		case State::FlightInputAction::kStartFlight:
			StartFlight();
			break;
		case State::FlightInputAction::kStopFlight:
			StopFlight();
			break;
		case State::FlightInputAction::kBeginDescent:
			BeginDescent();
			break;
		case State::FlightInputAction::kCancelDescent:
			CancelDescent();
			break;
		case State::FlightInputAction::kToggleCombatReady: {
			{
				const auto currentReadySession = GetReadyGenerationSession();
				if (State::IsReadyGenerationTokenValid(a_action.readyToken) &&
					!State::DecideReadyGenerationEnvelope(
						a_action.readyToken,
						a_action.sourceSession,
						currentReadySession,
						currentReadySession).valid) {
				RetireReadyGeneration(
					a_action.readyToken,
					"apply_rejected_stale_envelope",
					a_action.sourceSession);
					break;
				}
			}
			{
				const auto currentEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
				const auto currentIdentity = currentEquipmentSnapshot.identity;
				const auto currentIdentityEpoch = currentEquipmentSnapshot.epoch;
				const bool currentIdentityCaptured = currentEquipmentSnapshot.captured;
				if (!State::ShouldApplyCurrentEquipmentAction(
					a_action.equipmentIdentityCaptured,
					currentIdentityCaptured,
					a_action.equipmentIdentity,
					currentIdentity,
					a_action.equipmentIdentityEpoch,
					currentIdentityEpoch)) {
						RetireReadyGeneration(
							a_action.readyToken,
							"apply_rejected_stale_equipment_epoch",
							a_action.sourceSession);
					logger::info(
						"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
						"action=toggle_combat_ready outcome=rejected reason=equipment_identity_or_epoch_stale queued=true "
						"action_epoch={} current_epoch={} accepted=false",
						State::StructuredDiagnosticSchemaVersion,
						a_action.actionId,
						a_action.sourceSession,
						a_action.inputSequenceDomain,
						managerSequence,
						a_action.equipmentIdentityEpoch,
						currentIdentityEpoch);
					break;
				}
			}
			if (!PrepareReadyGeneration(a_action.readyToken)) {
				RetireReadyGeneration(
					a_action.readyToken,
					"apply_rejected",
						a_action.sourceSession);
				logger::info(
					"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
					"action=toggle_combat_ready outcome=rejected reason=ready_generation_stale queued=true accepted=false",
					State::StructuredDiagnosticSchemaVersion,
					a_action.actionId,
					a_action.sourceSession,
					a_action.inputSequenceDomain,
					managerSequence);
				break;
			}
			const bool accepted = ToggleFlightCombatReadyWithIdentity(
				a_action.equipmentIdentityCaptured ?
					std::optional<State::WeaponEquipmentIdentity>(a_action.equipmentIdentity) :
					std::nullopt,
				a_action.equipmentIdentityEpoch);
			logger::info(
				"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
				"action=toggle_combat_ready outcome={} reason=toggle_combat_ready queued=true accepted={}",
				State::StructuredDiagnosticSchemaVersion,
				a_action.actionId,
				a_action.sourceSession,
				a_action.inputSequenceDomain,
				managerSequence,
				accepted ? "accepted" : "rejected",
				accepted);
			break;
		}
		case State::FlightInputAction::kBeginCombat: {
			{
				const auto currentEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
				const auto currentIdentity = currentEquipmentSnapshot.identity;
				const auto currentIdentityEpoch = currentEquipmentSnapshot.epoch;
				const bool currentIdentityCaptured = currentEquipmentSnapshot.captured;
				if (!State::ShouldApplyCurrentEquipmentAction(
					a_action.equipmentIdentityCaptured,
					currentIdentityCaptured,
					a_action.equipmentIdentity,
					currentIdentity,
					a_action.equipmentIdentityEpoch,
					currentIdentityEpoch)) {
					logger::info(
						"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
						"action=begin_combat outcome=rejected reason=equipment_identity_or_epoch_stale queued=true accepted=false "
						"action_epoch={} current_epoch={}",
						State::StructuredDiagnosticSchemaVersion,
						a_action.actionId,
						a_action.sourceSession,
						a_action.inputSequenceDomain,
						managerSequence,
						a_action.equipmentIdentityEpoch,
						currentIdentityEpoch);
					break;
				}
			}
			const bool accepted = BeginFlightCombat();
			logger::info(
				"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
				"action=begin_combat outcome={} reason=begin_combat queued=true accepted={}",
				State::StructuredDiagnosticSchemaVersion,
				a_action.actionId,
				a_action.sourceSession,
				a_action.inputSequenceDomain,
				managerSequence,
				accepted ? "accepted" : "rejected",
				accepted);
			break;
		}
		case State::FlightInputAction::kBlockRequest:
			{
				const auto actionIdentity = a_action.equipmentIdentityCaptured ?
					std::optional<State::WeaponEquipmentIdentity>(a_action.equipmentIdentity) :
					std::nullopt;
				const bool accepted = SetFlightBlockRequested(
					true,
					actionIdentity,
					a_action.equipmentIdentityEpoch);
				logger::info(
					"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
					"action=block_request outcome={} reason=block_request queued=true accepted={}",
					State::StructuredDiagnosticSchemaVersion,
					a_action.actionId,
					a_action.sourceSession,
					a_action.inputSequenceDomain,
					managerSequence,
					accepted ? "accepted" : "rejected",
					accepted);
			}
			break;
		case State::FlightInputAction::kBlockRelease:
			{
				const auto actionIdentity = a_action.equipmentIdentityCaptured ?
					std::optional<State::WeaponEquipmentIdentity>(a_action.equipmentIdentity) :
					std::nullopt;
				const bool accepted = SetFlightBlockRequested(
					false,
					actionIdentity,
					a_action.equipmentIdentityEpoch);
				logger::info(
					"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
					"action=block_release outcome={} reason=block_release queued=true accepted={}",
					State::StructuredDiagnosticSchemaVersion,
					a_action.actionId,
					a_action.sourceSession,
					a_action.inputSequenceDomain,
					managerSequence,
					accepted ? "accepted" : "rejected",
					accepted);
			}
			break;
		case State::FlightInputAction::kShoutPress:
			NotifyFlightShout(false);
			break;
		case State::FlightInputAction::kShoutRelease:
			NotifyFlightShout(true);
			break;
		case State::FlightInputAction::kClearShout:
			ClearFlightShoutState();
			break;
		case State::FlightInputAction::kLaunchBoost:
			TriggerLaunchBoost();
			break;
		case State::FlightInputAction::kSetMovementInput:
			SetMovementInput(a_action.valueA, a_action.valueB);
			break;
		case State::FlightInputAction::kSetVerticalInput:
			SetVerticalInput(a_action.valueA);
			break;
		case State::FlightInputAction::kSetBoostHeld:
			SetBoostHeld(a_action.flag);
			break;
		case State::FlightInputAction::kObserveGroundWeaponTransition:
			{
				const auto currentReadySession = GetReadyGenerationSession();
				if (State::IsReadyGenerationTokenValid(a_action.readyToken) &&
					!State::DecideReadyGenerationEnvelope(
						a_action.readyToken,
						a_action.sourceSession,
						currentReadySession,
						currentReadySession).valid) {
				RetireReadyGeneration(
					a_action.readyToken,
					"ground_apply_rejected_stale_envelope",
					a_action.sourceSession);
					break;
				}
			}
			{
				const auto currentEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
				const auto currentIdentity = currentEquipmentSnapshot.identity;
				const auto currentIdentityEpoch = currentEquipmentSnapshot.epoch;
				const bool currentIdentityCaptured = currentEquipmentSnapshot.captured;
				if (!State::ShouldApplyCurrentEquipmentAction(
					a_action.equipmentIdentityCaptured,
					currentIdentityCaptured,
					a_action.equipmentIdentity,
					currentIdentity,
					a_action.equipmentIdentityEpoch,
					currentIdentityEpoch)) {
					RetireReadyGeneration(
						a_action.readyToken,
						"ground_apply_rejected_stale_equipment_epoch",
						a_action.sourceSession);
					logger::info(
						"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
						"action=observe_ground_weapon_transition outcome=rejected reason=equipment_identity_or_epoch_stale queued=true "
						"accepted=false action_epoch={} current_epoch={}",
						State::StructuredDiagnosticSchemaVersion,
						a_action.actionId,
						a_action.sourceSession,
						a_action.inputSequenceDomain,
						managerSequence,
						a_action.equipmentIdentityEpoch,
						currentIdentityEpoch);
					break;
				}
			}
			if (!PrepareReadyGeneration(a_action.readyToken)) {
				RetireReadyGeneration(
					a_action.readyToken,
					"ground_apply_rejected",
					a_action.sourceSession);
				logger::info(
					"event=flight_action_result schema={} action_id={} session={} input_sequence_domain={} manager_sequence={} "
					"action=observe_ground_weapon_transition outcome=rejected reason=ready_generation_stale queued=true accepted=false",
					State::StructuredDiagnosticSchemaVersion,
					a_action.actionId,
					a_action.sourceSession,
					a_action.inputSequenceDomain,
					managerSequence);
				break;
			}
			StartGroundWeaponObservation(a_action);
			break;
		default:
			break;
		}
	}

	void FlightManager::StartGroundWeaponObservation(State::FlightInputActionSnapshot a_action)
	{
		// Serialize the producer snapshot and observer baseline with InputHandler and
		// HandleEquipmentIdentityChange.  The recursive gate permits the snapshot
		// helper to take the same gate while this action is being armed.
		std::unique_lock nativeGate(_readyNativeActionMutex);
		const auto now = std::chrono::steady_clock::now();
		auto* player = GetPlayer();
		const auto currentEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
		const auto currentEquipmentIdentity = currentEquipmentSnapshot.identity;
		const auto currentIdentityEpoch = currentEquipmentSnapshot.epoch;
		const bool currentIdentityCaptured = currentEquipmentSnapshot.captured;
		if (!State::ShouldApplyCurrentEquipmentAction(
			a_action.equipmentIdentityCaptured,
			currentIdentityCaptured,
			a_action.equipmentIdentity,
			currentEquipmentIdentity,
			a_action.equipmentIdentityEpoch,
			currentIdentityEpoch)) {
			logger::info(
				"event=ground_weapon_observer session={} decision=abort "
				"reason=stale_equipment_identity_or_epoch action_epoch={} current_epoch={} passthrough=true",
				GetPublishedFlightSession(),
				a_action.equipmentIdentityEpoch,
				currentIdentityEpoch);
			return;
		}
		const auto baselineDecision = State::DecideGroundWeaponObserverBaseline(
			a_action.equipmentIdentity,
			a_action.equipmentIdentityCaptured,
			currentEquipmentIdentity,
			currentIdentityCaptured);
		const auto observedEquipmentIdentity = baselineDecision.identity;
		const bool observedIdentityCaptured = baselineDecision.captured;
		bool started = false;
		bool abortedNewEdge = false;
		bool supersededPostFlightTransition = false;
		bool supersededNativeFallback = false;
		bool supersededOppositeTarget = false;
		std::uint64_t session = 0;
		std::uint64_t edgeSequence = 0;
		std::uint64_t oldEdgeSequence = 0;
		{
			std::unique_lock lock(_mutex);
			session = _flightSessionId;
			if (_isFlying) {
				logger::info(
					"event=ground_weapon_observer session={} decision=abort reason=flight_transition "
					"target_drawn={} actor_form=0x{:08X} passthrough=true",
					session,
					a_action.flag,
					a_action.actorFormId);
				return;
			}
			// A ground Ready edge supersedes a stale post-flight transition before
			// the observer is armed.  This keeps its fallback, target, and deadline
			// from being processed ahead of the newer ground edge.
			const auto supersession = State::DecideGroundReadySupersession(
				_weaponTransitionPending && _weaponTransitionPostFlight,
				_weaponTransitionNativeFallbackArmed,
				_weaponTransitionTargetDrawn,
				a_action.flag);
			supersededPostFlightTransition = supersession.supersedePostFlightTransition;
			supersededNativeFallback = supersession.disarmNativeFallback;
			supersededOppositeTarget = supersession.oppositeTarget;
			if (supersession.supersedePostFlightTransition) {
				_weaponTransitionPending = false;
				_weaponTransitionTargetDrawn = a_action.flag;
				_weaponTransitionNativeFallbackArmed = false;
				_weaponTransitionExpiryRecoveryAttempted = false;
				_weaponTransitionNativeFallbackRetryUsed = false;
				_weaponTransitionProgressExtensionUsed = false;
				_weaponTransitionPostFlight = false;
				_weaponTransitionDeadline = {};
				_weaponTransitionNativeFallbackAt = {};
				_weaponTransitionPreRequestState = -1;
				_weaponTransitionActorFormId = 0;
				_weaponTransitionSessionId = 0;
				_weaponTransitionIdentityEpoch = 0;
				_weaponTransitionEquipmentIdentity = {};
				_weaponTransitionEquipmentIdentityCaptured = false;
			}
			const auto observerDecision = State::ReduceGroundWeaponObserver(
				State::GroundWeaponObserverState{
					_groundWeaponObservationPending,
					_groundWeaponFallbackIssued,
					_groundWeaponEdgeSequence },
				State::GroundWeaponObserverEvent::kReadyEdge);
			abortedNewEdge = observerDecision.replaced;
			oldEdgeSequence = _groundWeaponEdgeSequence;
			_groundWeaponObservationPending = observerDecision.state.pending;
			_groundWeaponFallbackIssued = observerDecision.state.fallbackIssued;
			_groundWeaponFallbackRetryUsed = false;
			_groundWeaponProgressExtensionUsed = false;
			_groundWeaponTargetDrawn = a_action.flag;
			_groundWeaponActionId = a_action.actionId;
			_groundWeaponActorFormId = a_action.actorFormId;
			_groundWeaponPreEdgeState = a_action.actorWeaponState;
			_groundWeaponSessionId = session;
			_groundWeaponEdgeSequence = observerDecision.state.edgeSequence;
			_groundWeaponIdentityEpoch = _weaponEquipmentIdentityEpoch;
			_groundWeaponDeadline = now + GroundWeaponObservationTimeout;
			_groundWeaponEquipmentIdentity = observedEquipmentIdentity;
			_groundWeaponEquipmentIdentityCaptured = observedIdentityCaptured || a_action.actorFormId != 0;
			// The snapshot is the new observer baseline.  Without this rebase,
			// HandleEquipmentIdentityChange compares the next update against the
			// pre-edge identity and aborts the observer immediately.
			// This action was validated against the current producer epoch.  Align the
			// diagnostic baseline without manufacturing another epoch or clearing a
			// pending identity side-effect edge before update can consume it.
			(void)ObserveWeaponEquipmentIdentityLocked(
				observedEquipmentIdentity,
				_groundWeaponEquipmentIdentityCaptured);
			// Ground observers never carry a flight-only swap pin or stale flight
			// identity into the next Ready edge.
			_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
			_weaponEquipmentSwapPreviousIdentity = {};
			_weaponEquipmentSwapCurrentIdentity = {};
			_weaponEquipmentSwapIdentityCaptured = false;
			_weaponEquipmentSwapPinnedDiagnosticLogged = false;
			edgeSequence = _groundWeaponEdgeSequence;
			started = true;
		}

		if (abortedNewEdge) {
			logger::info(
				"event=ground_weapon_observer session={} edge_seq={} decision=abort reason=new_edge "
				"replaced_edge_seq={} passthrough=true",
				session,
				edgeSequence,
				oldEdgeSequence);
		}
		logger::info(
			"event=ground_weapon_observer session={} edge_seq={} decision=armed target_drawn={} "
			"actor_form=0x{:08X} pre_weapon_state={} timeout_ms={} superseded_postflight_transition={} "
			"superseded_native_fallback={} superseded_opposite_target={} passthrough=true "
			"flight_graph_untouched=true controller_untouched=true gravity_untouched=true",
			session,
			edgeSequence,
			a_action.flag,
			a_action.actorFormId,
			a_action.actorWeaponState,
			std::chrono::duration_cast<std::chrono::milliseconds>(GroundWeaponObservationTimeout).count(),
			supersededPostFlightTransition,
			supersededNativeFallback,
			supersededOppositeTarget);
		if (started) {
			(void)StartUpdateThread();
		}
	}

	void FlightManager::ObserveGroundWeaponTransition(
		RE::PlayerCharacter* a_player,
		std::chrono::steady_clock::time_point a_now)
	{
		bool pending = false;
		bool fallbackIssued = false;
		bool progressExtensionUsed = false;
		bool targetDrawn = false;
		std::uint32_t actorFormId = 0;
		std::int32_t preWeaponState = -1;
		std::uint64_t session = 0;
		std::uint64_t edgeSequence = 0;
		std::uint64_t identityEpoch = 0;
		std::chrono::steady_clock::time_point deadline{};
		std::uint64_t readyGeneration = 0;
		{
			std::shared_lock lock(_mutex);
			pending = _groundWeaponObservationPending;
			fallbackIssued = _groundWeaponFallbackIssued;
			progressExtensionUsed = _groundWeaponProgressExtensionUsed;
			targetDrawn = _groundWeaponTargetDrawn;
			actorFormId = _groundWeaponActorFormId;
			preWeaponState = _groundWeaponPreEdgeState;
			session = _groundWeaponSessionId;
			edgeSequence = _groundWeaponEdgeSequence;
			identityEpoch = _groundWeaponIdentityEpoch;
			deadline = _groundWeaponDeadline;
			readyGeneration = _readyGenerationBarrier.announced;
		}
		if (!pending) {
			return;
		}

		const auto abort = [&](std::string_view a_reason) {
			{
				std::unique_lock lock(_mutex);
				if (_groundWeaponObservationPending && _groundWeaponEdgeSequence == edgeSequence) {
					_groundWeaponObservationPending = false;
					_groundWeaponFallbackIssued = false;
					_groundWeaponFallbackRetryUsed = false;
					_groundWeaponProgressExtensionUsed = false;
					_groundWeaponDeadline = {};
					_groundWeaponActorFormId = 0;
					_groundWeaponPreEdgeState = -1;
					_groundWeaponSessionId = 0;
					_groundWeaponIdentityEpoch = 0;
					_groundWeaponEquipmentIdentity = {};
					_groundWeaponEquipmentIdentityCaptured = false;
				}
			}
			logger::info(
				"event=ground_weapon_observer session={} edge_seq={} decision=abort reason={} "
				"target_drawn={} actor_form=0x{:08X} pre_weapon_state={} fallback_issued={} "
				"passthrough=true flight_graph_untouched=true controller_untouched=true gravity_untouched=true",
				session,
				edgeSequence,
				a_reason,
				targetDrawn,
				actorFormId,
				preWeaponState,
				fallbackIssued);
		};

		bool flightActive = false;
		std::uint64_t currentSession = 0;
		{
			std::shared_lock lock(_mutex);
			flightActive = _isFlying;
			currentSession = _flightSessionId;
		}
		if (flightActive || currentSession != session) {
			abort(flightActive ? "flight_transition" : "session_mismatch");
			return;
		}
		if (!a_player || !a_player->Is3DLoaded()) {
			abort("actor_unloaded");
			return;
		}
		if (static_cast<std::uint32_t>(a_player->GetFormID()) != actorFormId) {
			abort("actor_changed");
			return;
		}
		// This observer is also an epoch producer.  Read and commit the identity as
		// one serialized snapshot so a queued ground edge cannot pair a form from one
		// incarnation with the epoch assigned to another.
		const auto currentEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();
		if (!currentEquipmentSnapshot.captured) {
			abort("actor_unloaded");
			return;
		}
		if (HandleEquipmentIdentityChange(currentEquipmentSnapshot.identity, "ground_observer")) {
			abort("equipment_swap");
			return;
		}

		const auto* actorState = a_player->AsActorState();
		if (!actorState) {
			abort("actor_state_unavailable");
			return;
		}
		const auto weaponState = actorState->GetWeaponState();
		const bool reached = IsWeaponStateAtTarget(weaponState, targetDrawn);
		if (reached) {
			{
				std::unique_lock lock(_mutex);
				if (_groundWeaponObservationPending && _groundWeaponEdgeSequence == edgeSequence) {
					_groundWeaponObservationPending = false;
					_groundWeaponFallbackIssued = false;
					_groundWeaponFallbackRetryUsed = false;
					_groundWeaponProgressExtensionUsed = false;
					_groundWeaponDeadline = {};
					_groundWeaponActorFormId = 0;
					_groundWeaponPreEdgeState = -1;
					_groundWeaponSessionId = 0;
					_groundWeaponIdentityEpoch = 0;
					_groundWeaponEquipmentIdentity = {};
					_groundWeaponEquipmentIdentityCaptured = false;
				}
			}
			logger::info(
				"event=ground_weapon_observer session={} edge_seq={} decision=target_reached "
				"target_drawn={} actor_form=0x{:08X} pre_weapon_state={} actual_weapon_state={} "
				"fallback_issued={} engine_method=none engine_result=already_at_target passthrough=true",
				session,
				edgeSequence,
				targetDrawn,
				actorFormId,
				preWeaponState,
				static_cast<std::int32_t>(weaponState),
				fallbackIssued);
			return;
		}

		const bool timedOut = a_now >= deadline;
		const bool stateUnchanged = static_cast<std::int32_t>(weaponState) == preWeaponState;
		const auto motion = GetWeaponFallbackMotion(weaponState, targetDrawn);
		const auto observerDecision = State::DecideWeaponFallbackObservation(
			timedOut,
			stateUnchanged,
			motion,
			progressExtensionUsed,
			fallbackIssued);
		if (observerDecision == State::WeaponFallbackDecision::kWait) {
			return;
		}
		if (observerDecision == State::WeaponFallbackDecision::kDeferForProgress) {
			{
				std::unique_lock lock(_mutex);
				if (!_groundWeaponObservationPending || _groundWeaponEdgeSequence != edgeSequence ||
					_groundWeaponProgressExtensionUsed) {
					return;
				}
				_groundWeaponProgressExtensionUsed = true;
				_groundWeaponDeadline = a_now + GroundWeaponProgressExtension;
			}
			logger::info(
				"event=ground_weapon_observer session={} edge_seq={} decision=defer reason=transition_in_progress "
				"target_drawn={} actor_form=0x{:08X} pre_weapon_state={} actual_weapon_state={} "
				"extension_ms={} fallback_issued=false passthrough=true",
				session,
				edgeSequence,
				targetDrawn,
				actorFormId,
				preWeaponState,
				static_cast<std::int32_t>(weaponState),
				std::chrono::duration_cast<std::chrono::milliseconds>(GroundWeaponProgressExtension).count());
			return;
		}
		if (observerDecision == State::WeaponFallbackDecision::kAbort && !fallbackIssued) {
			const auto reason = motion == State::WeaponFallbackMotion::kOppositeTarget ?
				"opposite_transition" :
				(motion == State::WeaponFallbackMotion::kTowardTarget ?
					"transition_in_progress_timeout" : "actor_state_changed");
			abort(reason);
			return;
		}
		if (observerDecision == State::WeaponFallbackDecision::kAbort && fallbackIssued &&
			(!timedOut || motion == State::WeaponFallbackMotion::kOppositeTarget ||
				(motion == State::WeaponFallbackMotion::kStable && !stateUnchanged))) {
			const auto reason = motion == State::WeaponFallbackMotion::kOppositeTarget ?
				"post_fallback_opposite_transition" : "post_fallback_actor_state_changed";
			abort(reason);
			return;
		}

		if (observerDecision == State::WeaponFallbackDecision::kFallback) {
			const bool issued = TryExecuteNativeWeaponFallback(
				a_player,
				true,
				targetDrawn,
				actorFormId,
				currentEquipmentSnapshot.identity,
				currentEquipmentSnapshot.captured,
				State::NativeFallbackGateRequest{
					session,
					edgeSequence,
					readyGeneration,
					identityEpoch,
					_groundWeaponActionId });
			if (!issued) {
				return;
			}
			LogWeaponRoutingDiagnostic(
				a_player,
				"ground_fallback_decision",
				"unchanged_actor_state_stalled",
				true,
				targetDrawn);
			logger::info(
				"event=ground_weapon_observer session={} edge_seq={} decision=fallback_issued "
				"target_drawn={} actor_form=0x{:08X} pre_weapon_state={} actual_weapon_state={} "
				"engine_method=DrawWeaponMagicHands engine_result=issued fallback_issued=true passthrough=true",
				session,
				edgeSequence,
				targetDrawn,
				actorFormId,
				preWeaponState,
				static_cast<std::int32_t>(weaponState));
			return;
		}

		// The bounded native fallback budget was issued and its result window
		// expired.  Clear the observer without another native request.
		{
			std::unique_lock lock(_mutex);
			if (!_groundWeaponObservationPending || _groundWeaponEdgeSequence != edgeSequence) {
				return;
			}
			_groundWeaponObservationPending = false;
			_groundWeaponFallbackIssued = false;
			_groundWeaponFallbackRetryUsed = false;
			_groundWeaponProgressExtensionUsed = false;
			_groundWeaponDeadline = {};
			_groundWeaponActorFormId = 0;
			_groundWeaponPreEdgeState = -1;
			_groundWeaponSessionId = 0;
			_groundWeaponIdentityEpoch = 0;
			_groundWeaponEquipmentIdentity = {};
			_groundWeaponEquipmentIdentityCaptured = false;
		}
		logger::info(
			"event=ground_weapon_observer session={} edge_seq={} decision=timeout_after_fallback "
			"target_drawn={} actor_form=0x{:08X} pre_weapon_state={} actual_weapon_state={} "
			"engine_method=DrawWeaponMagicHands engine_result=target_not_reached fallback_issued=true "
			"passthrough=true",
			session,
			edgeSequence,
			targetDrawn,
			actorFormId,
			preWeaponState,
			static_cast<std::int32_t>(weaponState));
	}

	bool FlightManager::SetFlightBlockRequested(
		bool a_requested,
		std::optional<State::WeaponEquipmentIdentity> a_actionIdentity,
		std::uint64_t a_actionIdentityEpoch)
	{
		// DAF block intent is deliberately metadata-only.  Skyrim exposes the block
		// booleans/events as shared actor/graph state and provides no producer token;
		// once vanilla/MCO writes the same values there is no way to prove ownership.
		// Keep the identity/epoch guard for queued intent, preserve vanilla/MCO
		// passthrough, and never write or clear shared native block state here.
		std::unique_lock nativeGate(_readyNativeActionMutex);
		auto* player = GetPlayer();
		const bool currentIdentityCaptured = player && player->Is3DLoaded();
		const auto equipment = GetEquipmentDiagnostic(player);
		const bool supported = !a_requested || equipment.blockCapable;
		const bool effectiveRequest = a_requested && supported;
		bool requestChanged = false;
		bool detailedLogging = false;
		bool deferredByEquipmentSwap = false;
		bool staleAction = false;
		bool nativeStateClaimed = false;
		bool nativeStateCleared = false;
		std::uint64_t session = 0;
		std::uint64_t currentIdentityEpoch = 0;

		{
			std::unique_lock lock(_mutex);
			session = _flightSessionId;
			// Block requests are also producer observations.  Advance the same
			// identity authority before validating the queued edge; otherwise a
			// swap first seen on this path could retain the old epoch.
			(void)ObserveWeaponEquipmentIdentityLocked(equipment.identity, currentIdentityCaptured);
			currentIdentityEpoch = _weaponEquipmentIdentityEpoch;
			const auto actionDecision = State::DecideFlightBlockAction(
				a_requested,
				a_actionIdentity.has_value(),
				currentIdentityCaptured,
				a_actionIdentity.value_or(State::WeaponEquipmentIdentity{}),
				equipment.identity,
				_flightBlockLease.active,
				a_actionIdentityEpoch,
				currentIdentityEpoch);
			staleAction = actionDecision.stale;
			detailedLogging = _detailedLogging;

			if (staleAction) {
				const bool blockRequestBoundToCurrent =
					_flightBlockRequested &&
					_flightBlockRequestedIdentityCaptured &&
					State::IsUsableWeaponEquipmentEpoch(_flightBlockRequestedIdentityEpoch) &&
					_flightBlockRequestedIdentityEpoch == currentIdentityEpoch &&
					State::ExactWeaponEquipmentIdentity(
						_flightBlockRequestedIdentity,
						equipment.identity);
				const bool blockLeaseBoundToCurrent =
					_flightBlockLease.active &&
					State::IsUsableWeaponEquipmentEpoch(_flightBlockLease.identityEpoch) &&
					State::IsUsableWeaponEquipmentEpoch(currentIdentityEpoch) &&
					_flightBlockLease.identityEpoch == currentIdentityEpoch &&
					State::ExactWeaponEquipmentIdentity(
						_flightBlockLease.identity,
						equipment.identity);
				// A stale action must not clear a newer B-bound logical request that was
				// accepted after the producer observed A -> B.  If no current-bound request
				// exists, retire the old logical/latch metadata; if an old native lease is
				// still active, retire that lease independently.  Neither path writes the
				// shared actor/graph block state.
				if (!currentIdentityCaptured || !blockRequestBoundToCurrent) {
					requestChanged = _flightBlockRequested;
					_flightBlockRequested = false;
					_flightBlockRequestedIdentity = {};
					_flightBlockRequestedIdentityEpoch = 0;
					_flightBlockRequestedIdentityCaptured = false;
				}
				if (!currentIdentityCaptured || !blockLeaseBoundToCurrent) {
					_flightBlockNativeStateOwned = false;
					_flightBlockLease = {};
				}
			} else if (a_requested && (!_isFlying || !HasDragonAspectActive())) {
				return false;
			} else if (a_requested && supported && _weaponEquipmentSwap.pending) {
				// A request without a settled replacement identity must wait.  A captured
				// edge for the retired identity was rejected above before this branch.
				deferredByEquipmentSwap = true;
			} else {
				requestChanged = _flightBlockRequested != effectiveRequest;
				_flightBlockRequested = effectiveRequest;
				if (!effectiveRequest) {
					// Metadata-only release.  There is no native producer proof to carry
					// across a release, unload, or unsupported equipment edge.
					_flightBlockNativeStateOwned = false;
					_flightBlockLease.active = false;
					_flightBlockRequestedIdentity = {};
					_flightBlockRequestedIdentityEpoch = 0;
					_flightBlockRequestedIdentityCaptured = false;
				} else {
					_flightBlockRequestedIdentity = equipment.identity;
					_flightBlockRequestedIdentityEpoch = currentIdentityEpoch;
					_flightBlockRequestedIdentityCaptured = currentIdentityCaptured;
				}
			}
		}

		if (staleAction) {
			bool nativeOwnerAfter = false;
			{
				std::shared_lock lock(_mutex);
				nativeOwnerAfter = _flightBlockLease.active;
			}
			logger::info(
				"event=block_state session={} requested={} supported={} effective={} changed={} "
				"decision=drop reason=identity_or_epoch_stale action_identity_captured={} "
				"action_epoch={} current_epoch={} native_owner={} right_form=0x{:08X} left_form=0x{:08X}",
				session,
				a_requested,
				supported,
				effectiveRequest,
				requestChanged,
				a_actionIdentity.has_value(),
				a_actionIdentityEpoch,
				currentIdentityEpoch,
				nativeOwnerAfter,
				equipment.identity.rightFormId,
				equipment.identity.leftFormId);
			return false;
		}

		if (deferredByEquipmentSwap) {
			logger::info(
				"event=block_state session={} requested=true supported=true effective=false changed=false "
				"decision=defer reason=equipment_swap_pending right_form=0x{:08X} left_form=0x{:08X} "
				"right_type={} left_type={} expected_oar_family={}",
				session,
				equipment.identity.rightFormId,
				equipment.identity.leftFormId,
				equipment.identity.rightWeaponType,
				equipment.identity.leftWeaponType,
				equipment.expectedOarFamily);
			return false;
		}

		if (!currentIdentityCaptured) {
			return !a_requested;
		}

		if (effectiveRequest) {
			SetFlightCombatActive(true);
		}

		if (detailedLogging || !supported || nativeStateClaimed || nativeStateCleared) {
			bool nativeOwnerAfter = false;
			{
				std::shared_lock lock(_mutex);
				nativeOwnerAfter = _flightBlockLease.active;
			}
			logger::info(
				"event=block_state session={} requested={} supported={} effective={} changed={} "
				"native_policy=metadata_only native_claimed={} native_cleared={} native_owner={} "
				"action_epoch={} current_epoch={} "
				"expected_oar_family={} quarterstaff={} quarterstaff_keyword={} right_weapon_type={} left_weapon_type={}",
				session,
				a_requested,
				supported,
				effectiveRequest,
				requestChanged,
				nativeStateClaimed,
				nativeStateCleared,
				nativeOwnerAfter,
				a_actionIdentityEpoch,
				currentIdentityEpoch,
				equipment.expectedOarFamily,
				equipment.quarterstaffEquipped,
				equipment.quarterstaffKeyword,
				equipment.rightWeaponType,
				equipment.leftWeaponType);
		}

		return supported;
	}

	bool FlightManager::ShouldSuppressInput()
	{
		// SMF blocking window is authoritative for the Mod Control Panel context.
		if (SKSEMenuFramework::IsInstalled() && SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
			return true;
		}

		bool suppressInMenus = true;
		{
			std::shared_lock lock(DragonAspectFlight::Settings::GetSingleton().mutex);
			suppressInMenus = DragonAspectFlight::Settings::GetSingleton().suppressInMenus;
		}
		if (!suppressInMenus) return false;

		auto* ui = RE::UI::GetSingleton();
		if (!ui) return false;

		// RE::UI::IsMenuOpen takes const std::string_view&.
		static constexpr std::string_view BlockingMenus[] = {
			"Console",
			"Journal Menu",
			"InventoryMenu",
			"ContainerMenu",
			"BarterMenu",
			"GiftMenu",
			"MagicMenu",
			"TweenMenu",
			"FavoritesMenu",
			"CraftingMenu",
			"SmithingMenu",
			"EnchantingMenu",
			"ItemCard",
			"MapMenu",
			"StatsMenu",
			"Book Menu",
			"RaceSex Menu",
			"Sleep/Wait Menu",
			"LevelUp Menu",
			"Mod Configuration Menu",
			"CustomSkill Menu",
			"MessageBoxMenu",
			"TextInput Menu",
		};

		for (const auto& name : BlockingMenus) {
			if (ui->IsMenuOpen(name)) return true;
		}
		return false;
	}

	void FlightManager::TriggerLaunchBoost()
	{
		std::unique_lock lock(_mutex);

		if (!_isFlying || _isDescending || !HasDragonAspectActive()) {
			return;
		}

		_pendingLaunchBoost = LaunchBoostVelocity;
		logger::info("Launch boost queued - {}", BuildVersion);
	}

	void FlightManager::NotifyFlightShout(bool a_released)
	{
		bool applyImmediately = false;
		bool acceptedEdge = false;
		bool ignoredHeld = false;
		bool shoutGraphActive = false;
		bool releaseWhirlwindSprint = false;
		bool combatActive = false;
		bool useGeneratedCombatTopology = false;
		bool descending = false;
		bool shoutHeldAfter = false;
		std::uint64_t session = 0;
		FlightGraphState graphState = FlightGraphState::kMoving;
		const bool currentShoutIsWhirlwindSprint = IsWhirlwindSprintSelected(GetPlayer());

		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || !HasDragonAspectActive()) {
				return;
			}

			const auto now = std::chrono::steady_clock::now();
			descending = _isDescending;
			graphState = descending ? FlightGraphState::kDescent : FlightGraphState::kMoving;
			combatActive = _flightCombatActive;
			useGeneratedCombatTopology = _useGeneratedCombatTopology;
			session = _flightSessionId;
			const auto edgeResult = State::ReduceShoutEdge(
				State::ShoutLatchState{ _flightShoutHeld, _whirlwindSprintShoutPending },
				a_released ? State::ShoutEdge::kRelease : State::ShoutEdge::kPress);
			acceptedEdge = edgeResult.acceptedPress || edgeResult.acceptedRelease;
			ignoredHeld = edgeResult.ignoredHeld;
			_flightShoutHeld = edgeResult.state.held;
			shoutHeldAfter = _flightShoutHeld;
			if (edgeResult.acceptedPress) {
				_shoutGraphOverrideUntil = now + ShoutGraphOverrideDuration;
				_whirlwindSprintShoutPending =
					edgeResult.state.whirlwindPending || currentShoutIsWhirlwindSprint;
				shoutGraphActive = true;
			} else if (edgeResult.acceptedRelease) {
				releaseWhirlwindSprint = _whirlwindSprintShoutPending;
				_whirlwindSprintShoutPending = false;
				_shoutGraphOverrideUntil = {};
				shoutGraphActive = false;
			} else if (ignoredHeld) {
				// Do not extend the override or queue another release for repeated
				// Held notifications.  The existing graph state is repaired by the
				// normal update tick if another system overwrote it.
				shoutGraphActive = _flightShoutHeld;
			}
			if (releaseWhirlwindSprint) {
				_whirlwindSprintUntil = now + WhirlwindSprintControlWindow;
			}
			_lastGraphState = static_cast<std::int32_t>(graphState);
			applyImmediately = true;
		}

		if (applyImmediately) {
			SetFlightGraphVariables(
				GetPlayer(),
				true,
				true,
				combatActive,
				useGeneratedCombatTopology,
				false,
				shoutGraphActive,
				graphState);
			logger::info(
				"event=shout_state session={} edge={} released={} accepted={} ignored_held={} held={} descending={} "
				"whirlwind_selected={} whirlwind_release_window={} graph_shout={} graph_state={}",
				session,
				a_released ? "release" : (ignoredHeld ? "held" : "press"),
				a_released,
				acceptedEdge,
				ignoredHeld,
				shoutHeldAfter,
				descending,
				currentShoutIsWhirlwindSprint,
				releaseWhirlwindSprint,
				shoutGraphActive,
				static_cast<std::int32_t>(graphState));
			if (releaseWhirlwindSprint) {
				logger::info(
					"Dragon Aspect Flight: yielding controller velocity to Whirlwind Sprint for {} ms",
					std::chrono::duration_cast<std::chrono::milliseconds>(WhirlwindSprintControlWindow).count());
			}
		}
	}

	void FlightManager::ClearFlightShoutState()
	{
		bool shouldWriteGraph = false;
		bool combatActive = false;
		bool useGeneratedCombatTopology = false;
		std::uint64_t session = 0;
		bool descending = false;
		{
			std::unique_lock lock(_mutex);
			shouldWriteGraph = _isFlying &&
				(_flightShoutHeld || _whirlwindSprintShoutPending ||
					_shoutGraphOverrideUntil.time_since_epoch().count() != 0);
			_flightShoutHeld = false;
			_whirlwindSprintShoutPending = false;
			_shoutGraphOverrideUntil = {};
			if (_isFlying) {
				combatActive = _flightCombatActive;
				useGeneratedCombatTopology = _useGeneratedCombatTopology;
				descending = _isDescending;
				session = _flightSessionId;
			}
		}

		if (shouldWriteGraph) {
			SetFlightGraphVariables(
				GetPlayer(),
				true,
				true,
				combatActive,
				useGeneratedCombatTopology,
				false,
				false,
				descending ? FlightGraphState::kDescent : FlightGraphState::kMoving);
			logger::info("event=shout_state_reset session={} graph_shout=false", session);
		}
	}

	void FlightManager::SetBoostHeld(bool a_boostHeld)
	{
		std::unique_lock lock(_mutex);
		_boostHeld = _isDescending ? false : a_boostHeld;
	}

	bool FlightManager::DrainFlightMagicka(
		RE::PlayerCharacter* a_player,
		std::chrono::steady_clock::time_point a_now)
	{
		bool enabled = false;
		float costPerSecond = 0.0F;
		{
			std::shared_lock lock(Settings::GetSingleton().mutex);
			enabled = Settings::GetSingleton().magickaCostEnabled;
			costPerSecond = Settings::GetSingleton().magickaCostPerSecond;
		}
		if (!enabled || !std::isfinite(costPerSecond) || costPerSecond <= 0.0F || !a_player) {
			return false;
		}

		auto* actorValueOwner = a_player->AsActorValueOwner();
		if (!actorValueOwner) {
			return false;
		}

		float chargeSeconds = 0.0F;
		std::uint64_t session = 0;
		{
			std::unique_lock lock(_mutex);
			const auto elapsed = _lastMagickaDrainAt.time_since_epoch().count() == 0 ?
				TickSeconds : std::chrono::duration<float>(a_now - _lastMagickaDrainAt).count();
			_lastMagickaDrainAt = a_now;
			const auto slice = State::AccumulateMagickaDrain(_magickaDrainCarrySeconds, elapsed);
			_magickaDrainCarrySeconds = slice.carrySeconds;
			chargeSeconds = slice.chargeSeconds;
			session = _flightSessionId;
		}

		if (chargeSeconds <= 0.0F) {
			return false;
		}

		std::uint64_t drainSequence = 0;
		bool flightActive = false;
		{
			std::shared_lock lock(_mutex);
			session = _flightSessionId;
			flightActive = _isFlying;
		}
		const auto actorFormId = static_cast<std::uint32_t>(a_player->GetFormID());
		const float currentMagicka = actorValueOwner->GetActorValue(RE::ActorValue::kMagicka);
		const float cost = costPerSecond * chargeSeconds;
		const float regenBefore = a_player->GetRegenDelay(RE::ActorValue::kMagicka);
		if (!State::ShouldAdvanceMagickaDrainSequence(currentMagicka > cost)) {
			RecordMagickaDrainDiagnostic(
				session,
				drainSequence,
				actorFormId,
				currentMagicka,
				0.0F,
				chargeSeconds,
				regenBefore,
				regenBefore,
				true,
				flightActive);
			return true;
		}

		// Advance the identity only for a real DamageActorValue operation.  A
		// depletion/descend tick must not invalidate the last real drain's
		// post-flight observation.
		{
			std::unique_lock lock(_mutex);
			drainSequence = ++_magickaDrainSequence;
			session = _flightSessionId;
			flightActive = _isFlying;
		}

		// CommonLibVR-NG implements DamageActorValue as a kDamage modifier.  The
		// process may publish its regen delay later than this call returns, but the
		// field has no writer provenance.  Observe it for diagnostics only; DAF
		// never calls an engine regen-delay writer or otherwise repairs the value.
		actorValueOwner->DamageActorValue(RE::ActorValue::kMagicka, cost);
		const float regenAfter = a_player->GetRegenDelay(RE::ActorValue::kMagicka);
		{
			std::unique_lock lock(_mutex);
			const bool priorOwnedValueChanged = _magickaRegenDelayOwned &&
				(std::fabs(regenBefore - _magickaRegenDelayAfterDrain) > MagickaRegenObservationTolerance);
			if (priorOwnedValueChanged) {
				// A lower/decaying or higher value appeared before this drain.  DAF
				// no longer owns that ledger and must not restore it later.
				_magickaRegenDelayOwned = false;
				_magickaRegenAwaitingDelayedIncrease = false;
				_magickaRegenObservationPending = false;
				_magickaRegenObservationSamples = 0;
				_magickaRegenObservationStableLogged = false;
			}

			_magickaRegenDelayBaseline = regenBefore;
			_magickaRegenDelayAfterDrain = regenAfter;
			_magickaRegenDelayOwned = regenAfter > regenBefore + MagickaRegenObservationTolerance;
			_magickaRegenAwaitingDelayedIncrease = !_magickaRegenDelayOwned;
			// Re-arm on every accepted DAF drain so a delayed process update is
			// observed before landing.  StopFlight resets the sample count and
			// starts a fresh post-flight stability window.
			_magickaRegenObservationPending = true;
			_magickaRegenObservationSamples = 0;
			_magickaRegenObservationStableLogged = false;
			_magickaRegenObservationStarted = a_now;
			_magickaRegenObservationNextSample = a_now + MagickaRegenObservationSampleInterval;
			_magickaRegenDelayAfterDrain = regenAfter;
			_magickaRegenObservationSessionId = session;
			_magickaRegenObservationDrainSequence = drainSequence;
			_magickaRegenObservationActorFormId = actorFormId;
			session = _flightSessionId;
		}

		RecordMagickaDrainDiagnostic(
			session,
			drainSequence,
			actorFormId,
			currentMagicka,
			cost,
			chargeSeconds,
			regenBefore,
			regenAfter,
			false,
			flightActive);
		return false;
	}

	void FlightManager::RecordMagickaDrainDiagnostic(
		std::uint64_t a_session,
		std::uint64_t a_drainSequence,
		std::uint32_t a_actorFormId,
		float a_current,
		float a_amount,
		float a_chargeSeconds,
		float a_regenBefore,
		float a_regenAfter,
		bool a_depleted,
		bool a_flight)
	{
		const auto now = std::chrono::steady_clock::now();
		State::MagickaDrainDiagnosticAggregate flushed{};
		bool flush = false;
		std::string_view flushReason = "none";
		{
			std::unique_lock lock(_mutex);
			const float elapsed = _magickaDrainDiagnosticStarted.time_since_epoch().count() == 0 ?
				0.0F : std::chrono::duration<float>(now - _magickaDrainDiagnosticStarted).count();
			const auto reason = State::GetMagickaDrainDiagnosticFlushReason(
					_magickaDrainDiagnosticAggregate,
					a_session,
					a_actorFormId,
					a_flight,
					a_depleted,
					 elapsed);
			if (reason != State::MagickaDiagnosticFlushReason::kNone) {
				flushed = _magickaDrainDiagnosticAggregate;
				_magickaDrainDiagnosticAggregate = {};
				_magickaDrainDiagnosticStarted = {};
				flush = true;
				flushReason = State::MagickaDiagnosticFlushReasonName(reason);
			}
			if (_magickaDrainDiagnosticAggregate.count == 0) {
				_magickaDrainDiagnosticStarted = now;
			}
			_magickaDrainDiagnosticAggregate = State::AccumulateMagickaDrainDiagnostic(
				_magickaDrainDiagnosticAggregate,
				a_session,
				a_drainSequence,
				a_actorFormId,
				a_current,
				a_amount,
				a_chargeSeconds,
				a_regenBefore,
				a_regenAfter,
				a_depleted,
				a_flight);
		}
		if (flush) {
			logger::info(
				"event=magicka_drain_summary session={} first_drain_seq={} last_drain_seq={} count={} "
				"actor=0x{:08X} total_amount={:.3f} charge_seconds={:.3f} first_current={:.3f} "
				"last_current={:.3f} first_regen_before={:.3f} last_regen_after={:.3f} depleted={} "
				"flight_first={} flight_last={} reason={} "
				"write_attempted=false write_performed=false",
				flushed.sessionId,
				flushed.firstDrainSequence,
				flushed.lastDrainSequence,
				flushed.count,
				flushed.actorFormId,
				flushed.totalAmount,
				flushed.totalChargeSeconds,
				flushed.firstCurrent,
				flushed.lastCurrent,
				flushed.firstRegenBefore,
				flushed.lastRegenAfter,
				flushed.depleted,
				flushed.firstFlight,
				flushed.lastFlight,
				flushReason);
		}
		if (a_depleted) {
			FlushMagickaDrainDiagnostic("depleted");
		}
	}

	void FlightManager::FlushMagickaDrainDiagnostic(std::string_view a_reason)
	{
		State::MagickaDrainDiagnosticAggregate aggregate{};
		{
			std::unique_lock lock(_mutex);
			if (!State::HasMagickaDrainDiagnosticAggregate(_magickaDrainDiagnosticAggregate)) {
				return;
			}
			aggregate = _magickaDrainDiagnosticAggregate;
			_magickaDrainDiagnosticAggregate = {};
			_magickaDrainDiagnosticStarted = {};
		}
		logger::info(
			"event=magicka_drain_summary session={} first_drain_seq={} last_drain_seq={} count={} "
			"actor=0x{:08X} total_amount={:.3f} charge_seconds={:.3f} first_current={:.3f} "
			"last_current={:.3f} first_regen_before={:.3f} last_regen_after={:.3f} depleted={} "
			"flight_first={} flight_last={} reason={} write_attempted=false write_performed=false",
			aggregate.sessionId,
			aggregate.firstDrainSequence,
			aggregate.lastDrainSequence,
			aggregate.count,
			aggregate.actorFormId,
			aggregate.totalAmount,
			aggregate.totalChargeSeconds,
			aggregate.firstCurrent,
			aggregate.lastCurrent,
			aggregate.firstRegenBefore,
			aggregate.lastRegenAfter,
			aggregate.depleted,
			aggregate.firstFlight,
			aggregate.lastFlight,
			a_reason);
	}

	void LogMagickaRegenDiagnosticSummary(
		const DragonAspectFlight::State::MagickaRegenDiagnosticEmission& a_emission)
	{
		if (!a_emission.emitted) {
			return;
		}
		const auto& aggregate = a_emission.aggregate;
		const auto kindName = [](DragonAspectFlight::State::MagickaRegenDiagnosticKind a_kind) {
			return a_kind == DragonAspectFlight::State::MagickaRegenDiagnosticKind::kWait ?
				"wait" : "observation_abort";
		};
		logger::info(
			"event=magicka_regen_summary session={} first_drain_seq={} last_drain_seq={} drain_seq={} "
			"actor=0x{:08X} count={} wait_count={} observation_count={} "
			"first_sample={:.3f} last_sample={:.3f} first_current={:.3f} last_current={:.3f} "
			"first_regen_before={:.3f} last_regen_before={:.3f} first_regen_after={:.3f} "
			"last_regen_after={:.3f} flight_first={} flight_last={} postflight_first={} "
			"postflight_last={} first_kind={} last_kind={} reason={} emitted_at_ms={} "
			"write_attempted=false write_performed=false",
			aggregate.sessionId,
			aggregate.firstDrainSequence,
			aggregate.lastDrainSequence,
			aggregate.drainSequence,
			aggregate.actorFormId,
			aggregate.count,
			aggregate.waitCount,
			aggregate.observationCount,
			aggregate.firstSample,
			aggregate.lastSample,
			aggregate.firstCurrent,
			aggregate.lastCurrent,
			aggregate.firstBaseline,
			aggregate.lastBaseline,
			aggregate.firstAfterDrain,
			aggregate.lastAfterDrain,
			aggregate.firstFlight,
			aggregate.lastFlight,
			aggregate.firstPostFlight,
			aggregate.lastPostFlight,
			kindName(aggregate.firstKind),
			kindName(aggregate.lastKind),
			a_emission.reason,
			a_emission.emittedAtMs);
	}

	void FlightManager::RecordMagickaRegenDiagnostic(
		std::uint64_t a_session,
		std::uint64_t a_drainSequence,
		std::uint32_t a_actorFormId,
		float a_sample,
		float a_current,
		float a_baseline,
		float a_afterDrain,
		bool a_flight,
		bool a_postFlight,
		State::MagickaRegenDiagnosticKind a_kind)
	{
		const auto emittedAtMs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		State::MagickaRegenDiagnosticEmission emission{};
		{
			std::unique_lock lock(_mutex);
			emission = _magickaRegenDiagnosticPipeline.Record(
				State::MagickaRegenDiagnosticRecord{
					a_session,
					a_drainSequence,
					a_actorFormId,
					a_sample,
					a_current,
					a_baseline,
					a_afterDrain,
					a_flight,
					a_postFlight,
					a_kind },
				emittedAtMs);
		}
		LogMagickaRegenDiagnosticSummary(emission);
	}

	void FlightManager::FlushMagickaRegenDiagnostic(std::string_view a_reason, bool a_terminal)
	{
		const auto emittedAtMs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		State::MagickaRegenDiagnosticEmission emission{};
		{
			std::unique_lock lock(_mutex);
			emission = a_terminal ?
				_magickaRegenDiagnosticPipeline.TerminalFlush(a_reason, emittedAtMs) :
				_magickaRegenDiagnosticPipeline.Flush(a_reason, emittedAtMs);
		}
		LogMagickaRegenDiagnosticSummary(emission);
	}

	void FlightManager::FlushDiagnosticAggregates(std::string_view a_reason)
	{
		FlushMagickaDrainDiagnostic(a_reason);
		FlushMagickaRegenDiagnostic(a_reason, true);
	}

	void FlightManager::ArmDafMagickaRegenObservation(std::chrono::steady_clock::time_point a_now)
	{
		bool armed = false;
		std::uint64_t session = 0;
		std::uint64_t drainSequence = 0;
		std::uint32_t actorFormId = 0;
		bool phaseFlight = false;
		bool phasePostFlight = false;
		{
			std::unique_lock lock(_mutex);
			session = _flightSessionId;
			drainSequence = _magickaRegenObservationDrainSequence;
			actorFormId = _magickaRegenObservationActorFormId;
			const auto phase = ResolveMagickaDiagnosticPhase(_isFlying, _weaponTransitionPostFlight);
			phaseFlight = phase.flight;
			phasePostFlight = phase.postFlight;
			if (_magickaRegenObservationPending ||
				(_magickaRegenDelayOwned &&
					_magickaRegenDelayAfterDrain > _magickaRegenDelayBaseline) ||
				_magickaRegenAwaitingDelayedIncrease) {
				_magickaRegenObservationPending = true;
				_magickaRegenObservationSamples = 0;
				_magickaRegenObservationStableLogged = false;
				_magickaRegenObservationStarted = a_now;
				_magickaRegenObservationNextSample = a_now + MagickaRegenObservationSampleInterval;
				_magickaRegenDiagnosticPipeline.BeginObservation();
				armed = true;
			} else {
				_magickaRegenDiagnosticPipeline.PumpStopped();
				_magickaRegenDelayBaseline = 0.0F;
				_magickaRegenDelayAfterDrain = 0.0F;
				_magickaRegenDelayOwned = false;
				_magickaRegenAwaitingDelayedIncrease = false;
				_magickaRegenObservationStableLogged = false;
				_magickaRegenObservationSessionId = 0;
				_magickaRegenObservationDrainSequence = 0;
				_magickaRegenObservationActorFormId = 0;
			}
		}
		logger::info(
			"event=magicka_regen_observation_armed session={} drain_seq={} actor=0x{:08X} armed={} "
			"sample_interval_ms={} required_samples={} window_ms={} flight={} postflight={} "
			"decision={} reason={} write_attempted=false write_performed=false",
			session,
			drainSequence,
			actorFormId,
			armed,
			std::chrono::duration_cast<std::chrono::milliseconds>(MagickaRegenObservationSampleInterval).count(),
			MagickaRegenObservationRequiredSamples,
			std::chrono::duration_cast<std::chrono::milliseconds>(MagickaRegenObservationWindow).count(),
			phaseFlight,
			phasePostFlight,
			armed ? "observe" : "discard",
			armed ? "pending_drain" : "no_daf_observation");
	}

	void FlightManager::ObserveDafMagickaRegen(
		RE::PlayerCharacter* a_player,
		std::chrono::steady_clock::time_point a_now,
		bool a_allowRelease)
	{
		bool pending = false;
		std::uint64_t session = 0;
		std::uint64_t observationSession = 0;
		std::uint64_t drainSequence = 0;
		std::uint64_t currentDrainSequence = 0;
		std::uint32_t actorFormId = 0;
		std::chrono::steady_clock::time_point started{};
		std::chrono::steady_clock::time_point nextSample{};
		bool phaseFlight = false;
		bool phasePostFlight = false;
		State::MagickaRegenObservationState observation{};
		{
			std::shared_lock lock(_mutex);
			pending = _magickaRegenObservationPending;
			session = _flightSessionId;
			observationSession = _magickaRegenObservationSessionId;
			drainSequence = _magickaRegenObservationDrainSequence;
			currentDrainSequence = _magickaDrainSequence;
			actorFormId = _magickaRegenObservationActorFormId;
			started = _magickaRegenObservationStarted;
			nextSample = _magickaRegenObservationNextSample;
			const auto phase = ResolveMagickaDiagnosticPhase(
				_isFlying,
				_weaponTransitionPostFlight,
				a_allowRelease);
			phaseFlight = phase.flight;
			phasePostFlight = phase.postFlight;
			observation = State::MagickaRegenObservationState{
				_magickaRegenDelayOwned,
				_magickaRegenDelayBaseline,
				_magickaRegenDelayAfterDrain,
				_magickaRegenObservationSamples,
				_magickaRegenAwaitingDelayedIncrease,
				_magickaRegenObservationSessionId,
				_magickaRegenObservationDrainSequence,
				_magickaRegenObservationActorFormId };
		}
		if (!pending) {
			return;
		}

		const bool actorLoaded = a_player && a_player->Is3DLoaded();
		const auto currentActorFormId = actorLoaded ?
			static_cast<std::uint32_t>(a_player->GetFormID()) : 0U;
		if (!State::IsMagickaRegenObservationCurrent(
			observation,
			session,
			currentDrainSequence,
			currentActorFormId,
			actorLoaded)) {
			{
				std::unique_lock lock(_mutex);
				if (_magickaRegenObservationPending && _flightSessionId == session) {
					_magickaRegenObservationPending = false;
					_magickaRegenObservationSamples = 0;
					_magickaRegenObservationStarted = {};
					_magickaRegenObservationNextSample = {};
					_magickaRegenDelayBaseline = 0.0F;
					_magickaRegenDelayAfterDrain = 0.0F;
					_magickaRegenDelayOwned = false;
					_magickaRegenAwaitingDelayedIncrease = false;
					_magickaRegenObservationStableLogged = false;
					_magickaRegenObservationSessionId = 0;
					_magickaRegenObservationDrainSequence = 0;
					_magickaRegenObservationActorFormId = 0;
				}
			}
			const bool sessionMismatch = observationSession != session;
			const bool drainMismatch = currentDrainSequence != drainSequence;
			const bool actorMismatch = actorLoaded && currentActorFormId != actorFormId;
			const std::string_view abortReason = !actorLoaded ? "actor_unloaded" :
				sessionMismatch ? "session_mismatch" :
				(drainMismatch ? "new_drain" :
					(actorMismatch ? "actor_mismatch" : "explicit_abort"));
			std::string_view aggregateReason = !actorLoaded ? "actor_boundary" :
				sessionMismatch ? "session_boundary" :
				(drainMismatch ? "new_drain" :
					(actorMismatch ? "actor_boundary" : "explicit_abort"));
			if (aggregateReason == "explicit_abort") {
				std::shared_lock lock(_mutex);
				if (_magickaRegenDiagnosticPipeline.HasAggregate() &&
					(_magickaRegenDiagnosticPipeline.aggregate.lastFlight != phaseFlight ||
						_magickaRegenDiagnosticPipeline.aggregate.lastPostFlight != phasePostFlight)) {
					aggregateReason = "phase_boundary";
				}
			}
			FlushMagickaRegenDiagnostic(aggregateReason, true);
			logger::info(
				"event=magicka_regen_observation session={} drain_seq={} actor=0x{:08X} "
				"current_delay=nan regen_before={:.3f} regen_after={:.3f} sample=nan flight={} postflight={} "
				"decision=abort reason={} write_attempted=false write_performed=false",
				session,
				drainSequence,
				actorFormId,
				observation.baseline,
				observation.observedAfterDrain,
				phaseFlight,
				phasePostFlight,
				abortReason);
			return;
		}

		const auto elapsed = std::chrono::duration<float>(a_now - started).count();
		if (!actorLoaded) {
			{
				std::unique_lock lock(_mutex);
				_magickaRegenObservationPending = false;
				_magickaRegenObservationSamples = 0;
				_magickaRegenObservationStarted = {};
				_magickaRegenObservationNextSample = {};
				_magickaRegenDelayBaseline = 0.0F;
				_magickaRegenDelayAfterDrain = 0.0F;
				_magickaRegenDelayOwned = false;
				_magickaRegenAwaitingDelayedIncrease = false;
				_magickaRegenObservationStableLogged = false;
				_magickaRegenObservationSessionId = 0;
				_magickaRegenObservationDrainSequence = 0;
				_magickaRegenObservationActorFormId = 0;
			}
			FlushMagickaRegenDiagnostic("actor_unloaded", true);
			logger::info(
				"event=magicka_regen_observation session={} drain_seq={} actor=0x{:08X} "
				"current_delay=nan regen_before={:.3f} regen_after={:.3f} sample=nan flight={} postflight={} "
				"decision=abort reason={} write_attempted=false write_performed=false",
				session,
				drainSequence,
				actorFormId,
				observation.baseline,
				observation.observedAfterDrain,
				phaseFlight,
				phasePostFlight,
				!actorLoaded ? "actor_unloaded" : "observation_timeout");
			return;
		}
		if (elapsed >= std::chrono::duration<float>(MagickaRegenObservationWindow).count()) {
			if (State::ShouldContinueMagickaObservationAfterAirborneTimeout(actorLoaded, phaseFlight)) {
				// Keep unresolved and stable candidates alive while airborne.  The
				// timeout is a sliding window: only the scheduled due edge may advance
				// it, so a 60 Hz observer cannot emit one extension per frame.
				const bool extensionDue = a_now >= nextSample;
				if (State::ShouldExtendMagickaObservationWindow(
					actorLoaded,
					phaseFlight,
					elapsed,
					extensionDue)) {
					{
						std::unique_lock lock(_mutex);
						if (_magickaRegenObservationPending && _flightSessionId == session) {
							_magickaRegenObservationStarted = a_now;
							_magickaRegenObservationNextSample = a_now + MagickaRegenObservationWindow;
						}
					}
					FlushMagickaRegenDiagnostic("airborne_window_extended");
					logger::info(
						"event=magicka_regen_observation session={} drain_seq={} actor=0x{:08X} "
						"current_delay=nan regen_before={:.3f} regen_after={:.3f} sample=nan "
						"flight={} postflight={} decision=observe reason=airborne_window_extended "
						"write_attempted=false write_performed=false",
						session,
						drainSequence,
						actorFormId,
						observation.baseline,
						observation.observedAfterDrain,
						phaseFlight,
						phasePostFlight);
				}
				return;
			}

			{
				std::unique_lock lock(_mutex);
				_magickaRegenObservationPending = false;
				_magickaRegenObservationSamples = 0;
				_magickaRegenObservationStarted = {};
				_magickaRegenObservationNextSample = {};
				_magickaRegenDelayBaseline = 0.0F;
				_magickaRegenDelayAfterDrain = 0.0F;
				_magickaRegenDelayOwned = false;
				_magickaRegenAwaitingDelayedIncrease = false;
				_magickaRegenObservationStableLogged = false;
				_magickaRegenObservationSessionId = 0;
				_magickaRegenObservationDrainSequence = 0;
				_magickaRegenObservationActorFormId = 0;
			}
			FlushMagickaRegenDiagnostic("observation_timeout", true);
			logger::info(
				"event=magicka_regen_observation session={} drain_seq={} actor=0x{:08X} "
				"current_delay=nan regen_before={:.3f} regen_after={:.3f} sample=nan "
				"flight={} postflight={} decision=abort reason=observation_timeout "
				"write_attempted=false write_performed=false",
				session,
				drainSequence,
				actorFormId,
				observation.baseline,
				observation.observedAfterDrain,
				phaseFlight,
				phasePostFlight);
			return;
		}
		if (a_now < nextSample) {
			return;
		}

		const float current = a_player->GetRegenDelay(RE::ActorValue::kMagicka);
		const auto step = State::ObserveMagickaRegenDelay(
			observation,
			current,
			MagickaRegenObservationRequiredSamples,
			MagickaRegenObservationTolerance);
		if (step.result == State::MagickaRegenObservationResult::kWait) {
			{
				std::unique_lock lock(_mutex);
				if (_magickaRegenObservationPending && _flightSessionId == session) {
					_magickaRegenDelayOwned = step.state.dafOwned;
					_magickaRegenDelayAfterDrain = step.state.observedAfterDrain;
					_magickaRegenAwaitingDelayedIncrease = step.state.awaitingDafIncrease;
					_magickaRegenObservationSamples = step.state.unchangedSamples;
					_magickaRegenObservationNextSample = a_now + MagickaRegenObservationSampleInterval;
				}
			}
			RecordMagickaRegenDiagnostic(
				session,
				drainSequence,
				actorFormId,
				static_cast<float>(step.state.unchangedSamples),
				current,
				step.state.baseline,
				step.state.observedAfterDrain,
				phaseFlight,
				phasePostFlight,
				State::MagickaRegenDiagnosticKind::kWait);
			return;
		}

		const bool stable = step.result == State::MagickaRegenObservationResult::kStable;
		bool logStableAirborne = false;
		{
			std::unique_lock lock(_mutex);
			if (_magickaRegenObservationPending && _flightSessionId == session) {
				_magickaRegenDelayOwned = step.state.dafOwned;
				_magickaRegenDelayAfterDrain = step.state.observedAfterDrain;
				_magickaRegenAwaitingDelayedIncrease = step.state.awaitingDafIncrease;
				_magickaRegenObservationSamples = step.state.unchangedSamples;
				if (stable && !_magickaRegenObservationStableLogged) {
					_magickaRegenObservationStableLogged = true;
					logStableAirborne = true;
				}
				if (stable && !a_allowRelease) {
					// Keep sampling while airborne, but let the one-second diagnostic
					// aggregate absorb unchanged repeats instead of logging every tick.
					_magickaRegenObservationNextSample = a_now + MagickaRegenObservationSampleInterval;
				}
			}
		}
		RecordMagickaRegenDiagnostic(
			session,
			drainSequence,
			actorFormId,
			static_cast<float>(step.state.unchangedSamples),
			current,
			step.state.baseline,
			step.state.observedAfterDrain,
			phaseFlight,
			phasePostFlight,
			State::MagickaRegenDiagnosticKind::kWait);

		// While airborne this is a probe only.  Keep the observation armed for
		// landing, but never write another system's regen delay in flight.
		if (stable && !a_allowRelease) {
			if (logStableAirborne) {
				FlushMagickaRegenDiagnostic("stable_airborne");
				logger::info(
					"event=magicka_regen_observation session={} drain_seq={} actor=0x{:08X} "
					"result=stable_airborne sample={} current_delay={:.3f} regen_before={:.3f} regen_after={:.3f} "
					"flight={} postflight={} decision=stable_observation reason=airborne_probe "
					"write_attempted=false write_performed=false",
					session,
					drainSequence,
					actorFormId,
					step.state.unchangedSamples,
					current,
					step.state.baseline,
					step.state.observedAfterDrain,
					phaseFlight,
					phasePostFlight);
			}
			return;
		}

		float baseline = step.state.baseline;
		const float observed = step.state.observedAfterDrain;
		bool terminalObservationOwned = false;
		bool terminalSessionChanged = false;
		{
			std::unique_lock lock(_mutex);
			if (!_magickaRegenObservationPending || _flightSessionId != session) {
				terminalObservationOwned = false;
				terminalSessionChanged = _flightSessionId != session;
			} else {
				terminalObservationOwned = true;
				_magickaRegenObservationPending = false;
				_magickaRegenObservationSamples = 0;
				_magickaRegenObservationStarted = {};
				_magickaRegenObservationNextSample = {};
				_magickaRegenDelayBaseline = 0.0F;
				_magickaRegenDelayAfterDrain = 0.0F;
				_magickaRegenDelayOwned = false;
				_magickaRegenAwaitingDelayedIncrease = false;
				_magickaRegenObservationStableLogged = false;
				_magickaRegenObservationSessionId = 0;
				_magickaRegenObservationDrainSequence = 0;
				_magickaRegenObservationActorFormId = 0;
			}
		}
		if (!terminalObservationOwned) {
			const std::string_view terminalReason = terminalSessionChanged ?
				"session_boundary" : "explicit_abort";
			FlushMagickaRegenDiagnostic(terminalReason, true);
			return;
		}

		// Re-read at the observation boundary for diagnostics.  Even an exact
		// stable candidate is not writable: the engine exposes no provenance that
		// distinguishes DAF's delayed publication from another system's change.
		const float finalCurrent = a_player->GetRegenDelay(RE::ActorValue::kMagicka);
		if (!stable) {
			// This is the steady engine-owned decay path.  Keep the first/last
			// values and drain provenance, then close the terminal observer window
			// before UpdateFlight is allowed to stop its pump.
			const bool valueBoundary =
				std::fabs(finalCurrent - observed) > MagickaRegenObservationTolerance ||
				std::fabs(finalCurrent - baseline) > MagickaRegenObservationTolerance;
			RecordMagickaRegenDiagnostic(
				session,
				drainSequence,
				actorFormId,
				finalCurrent,
				finalCurrent,
				baseline,
				observed,
				phaseFlight,
				phasePostFlight,
				State::MagickaRegenDiagnosticKind::kObservationAbort);
			FlushMagickaRegenDiagnostic(
				valueBoundary ? "value_boundary" : "terminal_observation_abort",
				true);
			return;
		}

		FlushMagickaRegenDiagnostic("stable_final", true);
		logger::info(
			"event=magicka_regen_observation session={} drain_seq={} actor=0x{:08X} "
			"result=stable current_delay={:.3f} regen_before={:.3f} regen_after={:.3f} sample={:.3f} "
			"flight={} postflight={} decision=observe_only reason=no_writer_provenance "
			"write_attempted=false write_performed=false",
			session,
			drainSequence,
			actorFormId,
			finalCurrent,
			baseline,
			observed,
			finalCurrent,
			phaseFlight,
			phasePostFlight);
	}

	void FlightManager::SetFlightSpeed(float a_speed)
	{
		const auto sanitizedSpeed = std::max(0.0F, a_speed);

		std::unique_lock lock(_mutex);
		_flightSpeed = sanitizedSpeed;
		logger::info("Flight speed set to {}", _flightSpeed);
	}

	void FlightManager::SetVerticalSpeed(float a_speed)
	{
		const auto sanitizedSpeed = std::max(0.0F, a_speed);

		std::unique_lock lock(_mutex);
		_verticalSpeed = sanitizedSpeed;
		logger::info("Vertical speed set to {}", _verticalSpeed);
	}

	void FlightManager::SetLiftScale(float a_scale)
	{
		const auto sanitizedScale = std::clamp(a_scale, 0.25F, 2.50F);

		std::unique_lock lock(_mutex);
		_liftScale = sanitizedScale;
		logger::info("Lift scale set to {} - {}", _liftScale, BuildVersion);
	}

	void FlightManager::SetDetailedLogging(bool a_enabled, float a_snapshotIntervalSeconds)
	{
		const auto interval = std::clamp(a_snapshotIntervalSeconds, 0.5F, 30.0F);
		std::unique_lock lock(_mutex);
		_detailedLogging = a_enabled;
		_diagnosticSnapshotIntervalSeconds = interval;
		_lastDiagnosticSnapshot = {};
		_lastDiagnosticEquipmentSignature = ~std::uint64_t{ 0 };
		_lastDiagnosticStateSignature = ~std::uint64_t{ 0 };
		logger::info(
			"event=diagnostics_config enabled={} snapshot_interval_seconds={}",
			_detailedLogging,
			_diagnosticSnapshotIntervalSeconds);
	}

	void FlightManager::SetMovementInput(float a_forwardInput, float a_strafeInput)
	{
		std::unique_lock lock(_mutex);
		_forwardInput = _isDescending ? 0.0F : std::clamp(a_forwardInput, -1.0F, 1.0F);
		_strafeInput = _isDescending ? 0.0F : std::clamp(a_strafeInput, -1.0F, 1.0F);
	}

	void FlightManager::SetVerticalInput(float a_verticalInput)
	{
		std::unique_lock lock(_mutex);
		const float nextVerticalInput =
			_isDescending ? 0.0F : std::clamp(a_verticalInput, -1.0F, 1.0F);
		if (nextVerticalInput != _verticalInput) {
			logger::info(
				"Dragon Aspect Flight vertical input {} -> {}",
				_verticalInput,
				nextVerticalInput);
		}
		_verticalInput = nextVerticalInput;
	}

	float FlightManager::GetFlightSpeed() const
	{
		std::shared_lock lock(_mutex);
		return _flightSpeed;
	}

	float FlightManager::GetVerticalSpeed() const
	{
		std::shared_lock lock(_mutex);
		return _verticalSpeed;
	}

	float FlightManager::GetLiftScale() const
	{
		std::shared_lock lock(_mutex);
		return _liftScale;
	}

	std::uint64_t FlightManager::GetPublishedFlightSession() const noexcept
	{
		return _publishedFlightSessionId.load(std::memory_order_acquire);
	}

	State::ReadyGenerationToken FlightManager::AnnounceReadyGeneration()
	{
		State::ReadyGenerationToken token{};
		{
			std::unique_lock nativeGate(_readyNativeActionMutex);
			std::unique_lock lock(_mutex);
			const auto previous = _readyGenerationCounter.load(std::memory_order_relaxed);
			const auto generation = previous == std::numeric_limits<std::uint64_t>::max() ?
				std::uint64_t{ 1 } : previous + 1;
			_readyGenerationCounter.store(generation, std::memory_order_relaxed);
			token = State::ReadyGenerationToken{ _flightSessionId, generation };
			const auto decision = State::ReduceReadyGenerationBarrier(
				_readyGenerationBarrier,
				State::ReadyGenerationBarrierEvent::kAnnounce,
				token);
			if (decision.accepted) {
				_readyGenerationBarrier = decision.state;
				_readyGenerationLeaseToken = token;
				_readyGenerationLeaseStartedAt = std::chrono::steady_clock::now();
			}
			_readyGenerationBlockedDiagnosticLogged = false;
		}
		logger::info(
			"event=ready_generation session={} phase=announced generation={} barrier_pending=true",
			token.session,
			token.generation);
		return token;
	}

	void FlightManager::ClearReadyGenerationLeaseLocked()
	{
		_readyGenerationLeaseToken = {};
		_readyGenerationLeaseStartedAt = {};
	}

	void FlightManager::ClearReadyProtectedStateLocked()
	{
		// Caller holds _readyNativeActionMutex before _mutex.  This is the common
		// terminal cleanup for a failed Ready and for an accepted Ready apply.
		ClearReadyGenerationLeaseLocked();
		_weaponTransitionPending = false;
		_weaponTransitionTargetDrawn = _flightCombatActive;
		_weaponTransitionNativeFallbackArmed = false;
		_weaponTransitionExpiryRecoveryAttempted = false;
		_weaponTransitionNativeFallbackRetryUsed = false;
		_weaponTransitionProgressExtensionUsed = false;
		_weaponTransitionPostFlight = false;
		_weaponTransitionDeadline = {};
		_weaponTransitionNativeFallbackAt = {};
		_weaponTransitionPreRequestState = -1;
		_weaponTransitionActorFormId = 0;
		_weaponTransitionSessionId = 0;
		_weaponTransitionIdentityEpoch = 0;
		_weaponTransitionEquipmentIdentity = {};
		_weaponTransitionEquipmentIdentityCaptured = false;
		++_weaponTransitionSequence;

		_groundWeaponObservationPending = false;
		_groundWeaponFallbackIssued = false;
		_groundWeaponFallbackRetryUsed = false;
		_groundWeaponProgressExtensionUsed = false;
		_groundWeaponTargetDrawn = false;
		_groundWeaponActorFormId = 0;
		_groundWeaponPreEdgeState = -1;
		_groundWeaponSessionId = 0;
		_groundWeaponEdgeSequence = 0;
		_groundWeaponIdentityEpoch = 0;
		_groundWeaponDeadline = {};
		_groundWeaponEquipmentIdentity = {};
		_groundWeaponEquipmentIdentityCaptured = false;

		_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
		_weaponEquipmentSwapPreviousIdentity = {};
		_weaponEquipmentSwapCurrentIdentity = {};
		_weaponEquipmentSwapIdentityCaptured = false;
		_weaponEquipmentSwapPinnedDiagnosticLogged = false;
	}

	void FlightManager::RetireReadyGeneration(
		State::ReadyGenerationToken a_token,
		std::string_view a_reason,
		std::uint64_t a_sourceSession)
	{
		if (!State::IsReadyGenerationTokenValid(a_token)) {
			return;
		}
		bool accepted = false;
		bool stale = false;
		bool blocked = false;
		bool pending = false;
		bool protectedStateCleared = false;
		bool envelopeValid = false;
		std::uint64_t session = 0;
		{
			std::unique_lock nativeGate(_readyNativeActionMutex);
			std::unique_lock lock(_mutex);
			const auto sourceSession = a_sourceSession == State::FlightInputActionSnapshot::UncapturedSession ?
				a_token.session : a_sourceSession;
			const auto decision = State::DecideReadyGenerationFailure(
				_readyGenerationBarrier,
				a_token,
				sourceSession,
				_flightSessionId);
			accepted = decision.retireAccepted;
			stale = decision.barrier.stale;
			blocked = decision.barrier.blocked;
			envelopeValid = decision.envelopeValid;
			if (accepted) {
				_readyGenerationBarrier = decision.barrier.state;
				_readyGenerationBlockedDiagnosticLogged = false;
			}
			if (decision.cancelProtectedState) {
				ClearReadyProtectedStateLocked();
				protectedStateCleared = true;
			}
			pending = State::ReadyGenerationBarrierPending(_readyGenerationBarrier);
			session = _flightSessionId;
		}
		logger::info(
			"event=ready_generation session={} phase=retired generation={} reason={} accepted={} retired={} stale={} blocked={} envelope_valid={} protected_state_cleared={} barrier_pending={}",
			session,
			a_token.generation,
			a_reason,
			accepted,
			accepted,
			stale,
			blocked,
			envelopeValid,
			protectedStateCleared,
			pending);
	}

	bool FlightManager::PrepareReadyGeneration(State::ReadyGenerationToken a_token)
	{
		if (!State::IsReadyGenerationTokenValid(a_token)) {
			logger::info(
				"event=ready_generation phase=rejected generation=0 accepted=false reason=invalid_token");
			return false;
		}

		bool accepted = false;
		bool stale = false;
		bool blocked = false;
		bool hadTransition = false;
		bool hadFallback = false;
		bool hadGroundObserver = false;
		bool hadSwapPending = false;
		bool hadSwapPin = false;
		bool leaseExpired = false;
		std::uint64_t leaseExpiredSession = 0;
		std::uint64_t leaseExpiredGeneration = 0;
		std::uint64_t session = 0;
		{
			std::unique_lock nativeGate(_readyNativeActionMutex);
			std::unique_lock lock(_mutex);
			const auto now = std::chrono::steady_clock::now();
			const auto leaseToken = _readyGenerationLeaseToken;
			const auto leaseStarted = _readyGenerationLeaseStartedAt;
			const auto leaseElapsedMs = leaseStarted.time_since_epoch().count() != 0 && now >= leaseStarted ?
				static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - leaseStarted).count()) :
				std::uint64_t{ 0 };
			const auto leaseDecision = State::DecideReadyGenerationLease(
				_readyGenerationBarrier,
				leaseToken,
				leaseElapsedMs);
			if (a_token == leaseToken && leaseDecision.expired) {
				const auto retired = State::ReduceReadyGenerationBarrier(
					_readyGenerationBarrier,
					State::ReadyGenerationBarrierEvent::kRetire,
					leaseToken);
				if (retired.accepted) {
					_readyGenerationBarrier = retired.state;
					_readyGenerationBlockedDiagnosticLogged = false;
					leaseExpired = true;
					leaseExpiredSession = _flightSessionId;
					leaseExpiredGeneration = leaseToken.generation;
					hadTransition = _weaponTransitionPending;
					hadFallback = _weaponTransitionNativeFallbackArmed;
					hadGroundObserver = _groundWeaponObservationPending;
					hadSwapPending = _weaponEquipmentSwap.pending;
					hadSwapPin = State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap);
					session = _flightSessionId;
					ClearReadyProtectedStateLocked();
				}
			}
			const auto decision = leaseExpired ?
				State::ReadyGenerationBarrierDecision{} :
				State::ReduceReadyGenerationBarrier(
					_readyGenerationBarrier,
					State::ReadyGenerationBarrierEvent::kApply,
					a_token);
			accepted = decision.accepted;
			stale = decision.stale;
			blocked = decision.blocked;
			if (!accepted && !leaseExpired) {
				session = _flightSessionId;
			} else if (!leaseExpired) {
				_readyGenerationBarrier = decision.state;
				_readyGenerationBlockedDiagnosticLogged = false;
				hadTransition = _weaponTransitionPending;
				hadFallback = _weaponTransitionNativeFallbackArmed;
				hadGroundObserver = _groundWeaponObservationPending;
				hadSwapPending = _weaponEquipmentSwap.pending;
				hadSwapPin = State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap);
				ClearReadyProtectedStateLocked();
				session = _flightSessionId;
			}
		}
		if (leaseExpired) {
			logger::warn(
				"event=ready_barrier_expired reason=task_lease_timeout session={} generation={} "
				"lease_ms={} barrier_retired=true late_task_policy=reject_stale apply_rejected=true",
				leaseExpiredSession,
				leaseExpiredGeneration,
				State::kReadyGenerationLeaseMilliseconds);
		}
		logger::info(
			"event=ready_generation session={} phase={} generation={} accepted={} stale={} blocked={} "
			"retired={} expired={} transition_cleared={} fallback_cleared={} ground_observer_cleared={} "
			"swap_pending_cleared={} swap_pin_cleared={}",
			session,
			accepted ? "applied" : "rejected",
			a_token.generation,
			accepted,
			stale,
			blocked,
			accepted || leaseExpired,
			leaseExpired,
			hadTransition,
			hadFallback,
			hadGroundObserver,
			hadSwapPending,
			hadSwapPin);
		return accepted;
	}

	std::uint64_t FlightManager::GetReadyGenerationSession() const
	{
		std::shared_lock lock(_mutex);
		return _readyGenerationBarrier.session;
	}

	bool FlightManager::TryExecuteNativeWeaponFallback(
		RE::PlayerCharacter* a_player,
		bool a_groundObserver,
		bool a_targetDrawn,
		std::uint32_t a_actorFormId,
		State::WeaponEquipmentIdentity a_observedIdentity,
		bool a_observedIdentityCaptured,
		State::NativeFallbackGateRequest a_request)
	{
		State::NativeFallbackGateDecision gateDecision{};
		State::NativeFallbackRevalidationDecision revalidation{};
		State::NativeFallbackStaleRecoveryDecision recoveryDecision{};
		std::uint64_t currentSession = 0;
		std::uint64_t currentReadyGeneration = 0;
		bool observerAborted = false;
		bool transitionDisarmed = false;
		bool reserved = false;
		bool engineAvailable = false;
		bool actorMatches = false;
		std::uint64_t reservedIdentityEpoch = 0;
		std::uint64_t currentIdentityEpoch = 0;
		bool recoveryIdentityCaptured = false;
		State::WeaponEquipmentIdentity recoveryIdentity{};
		std::uint64_t recoverySession = 0;
		std::uint64_t recoverySequence = 0;
		std::uint64_t recoveryIdentityEpoch = 0;
		bool recoveryTargetDrawn = false;
		std::int32_t recoveryWeaponState = -1;
		const char* recoveryOutcome = "none";
		const char* recoveryReason = "post_call_current";
		{
			// Reserve the next native attempt while holding the DAF gate and state lock.  The
			// directional engine call itself is deliberately outside both locks;
			// post-call validation below reconciles any stale side effect.
			std::unique_lock nativeGate(_readyNativeActionMutex);
			engineAvailable = a_player && a_player->Is3DLoaded();
			actorMatches = engineAvailable &&
				static_cast<std::uint32_t>(a_player->GetFormID()) == a_actorFormId;
			{
				std::unique_lock lock(_mutex);
				const auto& storedIdentity = a_groundObserver ?
					_groundWeaponEquipmentIdentity : _weaponTransitionEquipmentIdentity;
				const bool storedIdentityCaptured = a_groundObserver ?
					_groundWeaponEquipmentIdentityCaptured : _weaponTransitionEquipmentIdentityCaptured;
				const bool identityMatches = a_observedIdentityCaptured && storedIdentityCaptured &&
					State::SameWeaponEquipmentIdentity(storedIdentity, a_observedIdentity);
				const auto storedIdentityEpoch = a_groundObserver ?
					_groundWeaponIdentityEpoch : _weaponTransitionIdentityEpoch;
				const bool transitionMatches = a_groundObserver ?
					(_groundWeaponObservationPending &&
						_groundWeaponEdgeSequence == a_request.sequence &&
						_groundWeaponSessionId == a_request.session &&
						_groundWeaponTargetDrawn == a_targetDrawn &&
						State::IsUsableWeaponEquipmentEpoch(a_request.identityEpoch) &&
						State::IsUsableWeaponEquipmentEpoch(storedIdentityEpoch) &&
						storedIdentityEpoch == a_request.identityEpoch &&
						!_groundWeaponFallbackIssued && actorMatches) :
					(_weaponTransitionPending &&
						_weaponTransitionSequence == a_request.sequence &&
						_weaponTransitionSessionId == a_request.session &&
						_weaponTransitionTargetDrawn == a_targetDrawn &&
						State::IsUsableWeaponEquipmentEpoch(a_request.identityEpoch) &&
						State::IsUsableWeaponEquipmentEpoch(storedIdentityEpoch) &&
						storedIdentityEpoch == a_request.identityEpoch &&
						_weaponTransitionNativeFallbackArmed && actorMatches);
				currentSession = _flightSessionId;
				currentReadyGeneration = _readyGenerationBarrier.announced;
				gateDecision = State::DecideNativeFallbackGate(
					currentSession,
					currentReadyGeneration,
					a_request,
					identityMatches,
					transitionMatches,
					engineAvailable,
					a_groundObserver,
					State::ReadyGenerationBarrierPending(_readyGenerationBarrier),
					_weaponEquipmentIdentityEpoch);
				if (gateDecision.execute) {
					reservedIdentityEpoch = storedIdentityEpoch;
					reserved = true;
					if (a_groundObserver) {
						_groundWeaponFallbackIssued = true;
						_groundWeaponDeadline = std::chrono::steady_clock::now() + GroundWeaponFallbackObservationTimeout;
					} else {
						_weaponTransitionNativeFallbackArmed = false;
						_weaponTransitionNativeFallbackAt = {};
						_weaponTransitionExpiryRecoveryAttempted = true;
						_weaponTransitionDeadline =
							std::chrono::steady_clock::now() + GroundWeaponFallbackObservationTimeout;
					}
				} else if (gateDecision.abortGroundObserver && a_groundObserver &&
					_groundWeaponObservationPending &&
					_groundWeaponEdgeSequence == a_request.sequence &&
					_groundWeaponSessionId == a_request.session) {
					_groundWeaponObservationPending = false;
					_groundWeaponFallbackIssued = false;
					_groundWeaponFallbackRetryUsed = false;
					_groundWeaponProgressExtensionUsed = false;
					_groundWeaponDeadline = {};
					_groundWeaponActorFormId = 0;
					_groundWeaponPreEdgeState = -1;
					_groundWeaponSessionId = 0;
					_groundWeaponIdentityEpoch = 0;
					_groundWeaponEquipmentIdentity = {};
					_groundWeaponEquipmentIdentityCaptured = false;
					observerAborted = true;
				} else if (gateDecision.definitiveRejection && !a_groundObserver &&
					_weaponTransitionPending &&
					_weaponTransitionSequence == a_request.sequence &&
					_weaponTransitionSessionId == a_request.session) {
					_weaponTransitionNativeFallbackArmed = false;
					_weaponTransitionNativeFallbackAt = {};
					transitionDisarmed = true;
				}
			}
		}
		if (!reserved) {
			logger::info(
				"event=weapon_native_fallback_gate session={} sequence={} ready_generation={} decision=skip "
				"session_mismatch={} newer_ready={} ready_generation_changed={} identity_mismatch={} identity_epoch_mismatch={} transition_mismatch={} "
				"barrier_pending={} definitive_rejection={} retryable_engine_unavailable={} observer_aborted={} transition_disarmed={}",
				currentSession,
				a_request.sequence,
				currentReadyGeneration,
				gateDecision.sessionMismatch,
				gateDecision.newerReady,
				gateDecision.readyGenerationChanged,
				gateDecision.identityMismatch,
				gateDecision.identityEpochMismatch,
				gateDecision.transitionMismatch,
				gateDecision.barrierPending,
				gateDecision.definitiveRejection,
				gateDecision.retryableEngineUnavailable,
				observerAborted,
				transitionDisarmed);
			return false;
		}

		// DrawWeaponMagicHands(bool) is directional rather than a toggle.  No DAF
		// lock is held while entering the engine; the post-call gate below validates
		// the reservation and records a residual stale-directional side-effect risk
		// if Ready, identity, or lifecycle state changed during the call.
		const auto* preActorState = a_player->AsActorState();
		const auto preWeaponState = preActorState ? static_cast<std::int32_t>(preActorState->GetWeaponState()) : -1;
		const bool preDrawn = preActorState && preActorState->IsWeaponDrawn();
		logger::info(
			"event=native_call_begin schema={} action_id={} session={} sequence={} ready_generation={} equipment_epoch={} engine_method=DrawWeaponMagicHands target_drawn={} pre_weapon_state={} pre_drawn={}",
			State::StructuredDiagnosticSchemaVersion, a_request.actionId, a_request.session, a_request.sequence,
			a_request.readyGeneration, a_request.identityEpoch, a_targetDrawn, preWeaponState, preDrawn);
		a_player->DrawWeaponMagicHands(a_targetDrawn);

		const bool postIdentityCaptured = a_player && a_player->Is3DLoaded();
		const auto postIdentity = postIdentityCaptured ?
			GetEquipmentDiagnostic(a_player).identity : State::WeaponEquipmentIdentity{};
		bool postActorMatches = postIdentityCaptured &&
			static_cast<std::uint32_t>(a_player->GetFormID()) == a_actorFormId;
		const auto* postActorState = a_player ? a_player->AsActorState() : nullptr;
		const auto postWeaponState = postActorState ?
			postActorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
		const auto postWeaponStateValue = static_cast<std::int32_t>(postWeaponState);
		const auto postWeaponStateObservation = postActorState ?
			State::NormalizeNativeFallbackWeaponState(postWeaponStateValue) :
			State::NativeFallbackWeaponState::kUnknown;
		const bool postActorInTransition = postActorState &&
			(IsWeaponTransitionInProgress(postWeaponState, true) ||
				IsWeaponTransitionInProgress(postWeaponState, false));
		logger::info(
			"event=native_call_return schema={} action_id={} session={} sequence={} ready_generation={} equipment_epoch={} engine_method=DrawWeaponMagicHands post_weapon_state={} post_drawn={} post_transition={}",
			State::StructuredDiagnosticSchemaVersion, a_request.actionId, a_request.session, a_request.sequence,
			a_request.readyGeneration, a_request.identityEpoch, postWeaponStateValue,
			postActorState && postActorState->IsWeaponDrawn(), postActorInTransition);
		{
			std::unique_lock nativeGate(_readyNativeActionMutex);
			std::unique_lock lock(_mutex);
			const auto& storedIdentity = a_groundObserver ?
				_groundWeaponEquipmentIdentity : _weaponTransitionEquipmentIdentity;
			const bool storedIdentityCaptured = a_groundObserver ?
				_groundWeaponEquipmentIdentityCaptured : _weaponTransitionEquipmentIdentityCaptured;
			currentIdentityEpoch = _weaponEquipmentIdentityEpoch;
			const bool postIdentityMatches = postIdentityCaptured && storedIdentityCaptured &&
				State::SameWeaponEquipmentIdentity(storedIdentity, postIdentity);
			const bool transitionMatches = a_groundObserver ?
				(_groundWeaponObservationPending &&
					_groundWeaponEdgeSequence == a_request.sequence &&
					_groundWeaponSessionId == a_request.session &&
					_groundWeaponTargetDrawn == a_targetDrawn &&
						State::IsUsableWeaponEquipmentEpoch(a_request.identityEpoch) &&
						State::IsUsableWeaponEquipmentEpoch(_groundWeaponIdentityEpoch) &&
					_groundWeaponIdentityEpoch == a_request.identityEpoch &&
					_groundWeaponFallbackIssued && postActorMatches) :
				(_weaponTransitionPending &&
					_weaponTransitionSequence == a_request.sequence &&
					_weaponTransitionSessionId == a_request.session &&
					_weaponTransitionTargetDrawn == a_targetDrawn &&
						State::IsUsableWeaponEquipmentEpoch(a_request.identityEpoch) &&
						State::IsUsableWeaponEquipmentEpoch(_weaponTransitionIdentityEpoch) &&
					_weaponTransitionIdentityEpoch == a_request.identityEpoch &&
					postActorMatches);
			const auto postSession = _flightSessionId;
			const auto postReadyGeneration = _readyGenerationBarrier.announced;
			revalidation = State::DecideNativeFallbackRevalidation(
				postSession,
				postReadyGeneration,
				a_request,
				postIdentityMatches,
				transitionMatches,
				currentIdentityEpoch,
				postWeaponStateObservation,
				a_targetDrawn);
			currentSession = postSession;
			currentReadyGeneration = postReadyGeneration;

			// A newer observer/Ready generation owns the latest intent.  Treat only a
			// strictly newer generation as preservation; a reset or older generation
			// is a stale boundary that still needs a fresh current-identity rebase.
			const bool newerGroundObserver = _groundWeaponObservationPending &&
				(a_groundObserver ?
					(_groundWeaponEdgeSequence != a_request.sequence ||
						_groundWeaponSessionId != a_request.session) :
					true);
			const bool newerFlightTransition = !a_groundObserver &&
				_weaponTransitionPending &&
				(_weaponTransitionSequence != a_request.sequence ||
					_weaponTransitionSessionId != a_request.session);
			const bool newerObserver = newerGroundObserver || newerFlightTransition;
			const bool newerReady = postReadyGeneration > a_request.readyGeneration;
			const bool actorLoaded = postIdentityCaptured;
			recoveryDecision = State::DecideNativeFallbackStaleRecovery(
				revalidation.sessionMismatch,
				revalidation.readyGenerationChanged,
				revalidation.identityMismatch,
				revalidation.identityEpochMismatch,
				revalidation.transitionMismatch,
				a_groundObserver,
				_isFlying,
				actorLoaded,
				postActorInTransition,
				newerObserver,
				newerReady);

			const auto clearGroundObserver = [&]() {
				_groundWeaponObservationPending = false;
				_groundWeaponFallbackIssued = false;
				_groundWeaponFallbackRetryUsed = false;
				_groundWeaponProgressExtensionUsed = false;
				_groundWeaponDeadline = {};
				_groundWeaponActorFormId = 0;
				_groundWeaponPreEdgeState = -1;
				_groundWeaponSessionId = 0;
				_groundWeaponIdentityEpoch = 0;
				_groundWeaponEquipmentIdentity = {};
				_groundWeaponEquipmentIdentityCaptured = false;
			};
			const auto clearTransition = [&]() {
				_weaponTransitionPending = false;
				_weaponTransitionNativeFallbackArmed = false;
				_weaponTransitionExpiryRecoveryAttempted = false;
				_weaponTransitionNativeFallbackRetryUsed = false;
				_weaponTransitionProgressExtensionUsed = false;
				_weaponTransitionPostFlight = false;
				_weaponTransitionDeadline = {};
				_weaponTransitionNativeFallbackAt = {};
				_weaponTransitionPreRequestState = -1;
				_weaponTransitionActorFormId = 0;
				_weaponTransitionSessionId = 0;
				_weaponTransitionIdentityEpoch = 0;
				_weaponTransitionEquipmentIdentity = {};
				_weaponTransitionEquipmentIdentityCaptured = false;
			};
			if (!revalidation.commit) {
				// A strict post-call state check can reject a current request even when
				// its session/identity/transition gates still match.  Keep that exact
				// logical target alive and allow one delayed directional retry; the retry
				// marker is separate for flight and ground observers so this can never
				// become an unbounded native-call loop.
				const bool currentPostCallRequest = !revalidation.sessionMismatch &&
					!revalidation.readyGenerationChanged && !revalidation.identityMismatch &&
					!revalidation.identityEpochMismatch && !revalidation.transitionMismatch &&
					actorLoaded;
				if (revalidation.weaponStatePending && currentPostCallRequest &&
					State::IsNativeFallbackWeaponStateTowardTarget(
						postWeaponStateObservation,
						a_targetDrawn)) {
					if (a_groundObserver) {
						if (!_groundWeaponFallbackRetryUsed) {
							_groundWeaponFallbackRetryUsed = true;
							_groundWeaponFallbackIssued = false;
							_groundWeaponProgressExtensionUsed = true;
							_groundWeaponDeadline = std::chrono::steady_clock::now() +
								GroundWeaponFallbackObservationTimeout;
							recoveryOutcome = "transitional_retry_armed";
							recoveryReason = "post_call_transitional_state";
						} else {
							// The bounded retry was consumed.  Mark this ground observer as
							// having issued its final request so a later timeout cannot issue
							// an unbounded third native call.
							_groundWeaponFallbackIssued = true;
							recoveryOutcome = "transitional_retry_exhausted";
							recoveryReason = "post_call_transitional_state";
						}
					} else if (!_weaponTransitionNativeFallbackRetryUsed) {
						_weaponTransitionNativeFallbackRetryUsed = true;
						_weaponTransitionNativeFallbackArmed = true;
						_weaponTransitionProgressExtensionUsed = true;
						_weaponTransitionNativeFallbackAt = std::chrono::steady_clock::now() +
							WeaponNativeFallbackRetryDelay;
						_weaponTransitionDeadline = std::chrono::steady_clock::now() +
							GroundWeaponFallbackObservationTimeout;
						recoveryOutcome = "transitional_retry_armed";
						recoveryReason = "post_call_transitional_state";
					} else {
						// The bounded retry was consumed.  Disarm the flight fallback so
						// the next observation can only retire the transition, never issue
						// a third native request.
						_weaponTransitionNativeFallbackArmed = false;
						_weaponTransitionNativeFallbackAt = {};
						recoveryOutcome = "transitional_retry_exhausted";
						recoveryReason = "post_call_transitional_state";
					}
				}

				// The old request must never remain executable after a stale native
				// call.  Preserve only newer state; otherwise clear the exact old
				// transaction before installing a current-identity replacement.
				const bool oldGroundTargetDrawn = _groundWeaponTargetDrawn;
				const bool oldGroundExact = a_groundObserver &&
					_groundWeaponObservationPending &&
					_groundWeaponEdgeSequence == a_request.sequence &&
					_groundWeaponSessionId == a_request.session;
				const bool oldTransitionExact = !a_groundObserver &&
					_weaponTransitionPending &&
					_weaponTransitionSequence == a_request.sequence &&
					_weaponTransitionSessionId == a_request.session;
				if (oldGroundExact &&
					(recoveryDecision.rebased || recoveryDecision.failed ||
						recoveryDecision.preserveNewerObserver)) {
					clearGroundObserver();
					observerAborted = true;
				}
				if (oldTransitionExact &&
					(recoveryDecision.rebased || recoveryDecision.failed ||
						recoveryDecision.preserveNewerObserver)) {
					clearTransition();
					transitionDisarmed = true;
				}

				// If the engine callback exposed a new equipment identity without the
				// normal observer tick seeing it first, route that observation through
				// the same authoritative producer used by input/update snapshots.
				if (actorLoaded && postIdentityCaptured) {
					(void)ObserveWeaponEquipmentIdentityLocked(postIdentity, true);
					currentIdentityEpoch = _weaponEquipmentIdentityEpoch;
				}

				switch (recoveryDecision.disposition) {
				case State::NativeFallbackStaleRecoveryDisposition::kPreserveNewerObserver:
					recoveryOutcome = "preserved_newer_observer";
					recoveryReason = newerReady ? "newer_ready_generation" : "newer_observer";
					break;
				case State::NativeFallbackStaleRecoveryDisposition::kRebaseFlightTransition:
					++_weaponTransitionSequence;
					_weaponTransitionPending = true;
					_weaponTransitionTargetDrawn = _flightCombatActive;
					_weaponTransitionNativeFallbackArmed = false;
					_weaponTransitionExpiryRecoveryAttempted = false;
					_weaponTransitionNativeFallbackRetryUsed = false;
					_weaponTransitionProgressExtensionUsed = false;
					_weaponTransitionPostFlight = false;
					_weaponTransitionDeadline = std::chrono::steady_clock::now() + WeaponTransitionTimeout;
					_weaponTransitionNativeFallbackAt = {};
					_weaponTransitionPreRequestState = postWeaponStateValue;
					_weaponTransitionActorFormId = static_cast<std::uint32_t>(a_player->GetFormID());
					_weaponTransitionSessionId = _flightSessionId;
					_weaponTransitionIdentityEpoch = _weaponEquipmentIdentityEpoch;
					_weaponTransitionEquipmentIdentity = postIdentity;
					_weaponTransitionEquipmentIdentityCaptured = postIdentityCaptured;
					recoveryOutcome = "rebased_flight_transition";
					recoveryReason = revalidation.identityMismatch ? "equipment_identity_changed" :
						(revalidation.readyGenerationChanged ? "ready_generation_changed" : "stale_transition");
					break;
				case State::NativeFallbackStaleRecoveryDisposition::kRebaseGroundObserver:
					_groundWeaponObservationPending = true;
					_groundWeaponFallbackIssued = false;
					_groundWeaponFallbackRetryUsed = false;
					_groundWeaponProgressExtensionUsed = false;
					_groundWeaponTargetDrawn = a_groundObserver ?
						oldGroundTargetDrawn : WeaponStateIntendsDrawn(postWeaponState);
					_groundWeaponActorFormId = static_cast<std::uint32_t>(a_player->GetFormID());
					_groundWeaponPreEdgeState = postWeaponStateValue;
					_groundWeaponSessionId = _flightSessionId;
					++_groundWeaponEdgeSequence;
					_groundWeaponIdentityEpoch = _weaponEquipmentIdentityEpoch;
					_groundWeaponDeadline = std::chrono::steady_clock::now() + GroundWeaponObservationTimeout;
					_groundWeaponEquipmentIdentity = postIdentity;
					_groundWeaponEquipmentIdentityCaptured = postIdentityCaptured;
					recoveryOutcome = "rebased_ground_observer";
					recoveryReason = revalidation.sessionMismatch ? "stop_or_session_boundary" :
						(revalidation.identityEpochMismatch ? "identity_epoch_changed" : "stale_ground_observer");
					break;
				case State::NativeFallbackStaleRecoveryDisposition::kFailure:
					recoveryOutcome = "failure";
					recoveryReason = actorLoaded ? "stale_state_unrecoverable" : "actor_unloaded";
					break;
				case State::NativeFallbackStaleRecoveryDisposition::kNoStale:
					recoveryOutcome = "stale_unclassified";
					recoveryReason = "stale_recovery_contract_mismatch";
					break;
				}
			} else {
				recoveryOutcome = "success";
				recoveryReason = "post_call_current_identity";
			}

			recoveryIdentity = postIdentity;
			recoveryIdentityCaptured = postIdentityCaptured;
			recoverySession = _flightSessionId;
			recoverySequence = newerGroundObserver || a_groundObserver ?
				_groundWeaponEdgeSequence : _weaponTransitionSequence;
			recoveryIdentityEpoch = _weaponEquipmentIdentityEpoch;
			recoveryTargetDrawn = newerGroundObserver || a_groundObserver ?
				_groundWeaponTargetDrawn : _weaponTransitionTargetDrawn;
			recoveryWeaponState = postWeaponStateValue;
		}
		if (!revalidation.commit) {
			logger::info(
				"event=native_call_postcheck schema={} action_id={} session={} sequence={} ready_generation={} equipment_epoch={} committed=false post_weapon_state={} post_drawn={} reason={}",
				State::StructuredDiagnosticSchemaVersion, a_request.actionId, currentSession, a_request.sequence,
				currentReadyGeneration, currentIdentityEpoch, postWeaponStateValue,
				postActorState && postActorState->IsWeaponDrawn(), recoveryReason);
			logger::info(
				"event=weapon_native_fallback_gate session={} sequence={} ready_generation={} decision=stale_after_engine "
				"session_mismatch={} ready_generation_changed={} identity_mismatch={} identity_epoch_mismatch={} "
				"transition_mismatch={} observer_aborted={} reserved_identity_epoch={} current_identity_epoch={} "
				"engine_method=DrawWeaponMagicHands directional_side_effect_possible=true "
				"reconciliation_required=true",
				currentSession,
				a_request.sequence,
				currentReadyGeneration,
				revalidation.sessionMismatch,
				revalidation.readyGenerationChanged,
				revalidation.identityMismatch,
				revalidation.identityEpochMismatch,
				revalidation.transitionMismatch,
				observerAborted,
				reservedIdentityEpoch,
				currentIdentityEpoch);
			const auto recoveryEvent = recoveryDecision.failed ? "recovery_failure" : "recovery_result";
			logger::info(
				"event={} outcome={} stale=true rebased={} preserved_newer_observer={} "
				"session={} sequence={} identity_captured={} identity_epoch={} target_drawn={} "
				"weapon_state={} reason={} identity_right_form=0x{:08X} identity_left_form=0x{:08X} "
				"identity_right_type={} identity_left_type={} identity_family={} old_identity_epoch={} current_identity_epoch={} "
				"engine_method=DrawWeaponMagicHands recursive_native_call=false",
				recoveryEvent,
				recoveryOutcome,
				recoveryDecision.rebased,
				recoveryDecision.preserveNewerObserver,
				recoverySession,
				recoverySequence,
				recoveryIdentityCaptured,
				recoveryIdentityEpoch,
				recoveryTargetDrawn,
				recoveryWeaponState,
				recoveryReason,
				recoveryIdentity.rightFormId,
				recoveryIdentity.leftFormId,
				recoveryIdentity.rightWeaponType,
				recoveryIdentity.leftWeaponType,
				State::WeaponEquipmentFamilyName(recoveryIdentity.family),
				reservedIdentityEpoch,
				currentIdentityEpoch);
			return false;
		}
		logger::info(
			"event=native_call_postcheck schema={} action_id={} session={} sequence={} ready_generation={} equipment_epoch={} committed=true post_weapon_state={} post_drawn={} reason={}",
			State::StructuredDiagnosticSchemaVersion, a_request.actionId, currentSession, a_request.sequence,
			currentReadyGeneration, currentIdentityEpoch, postWeaponStateValue,
			postActorState && postActorState->IsWeaponDrawn(), recoveryReason);
		logger::info(
			"event=recovery_result outcome=success stale=false rebased=false preserved_newer_observer=false "
			"session={} sequence={} identity_captured={} identity_epoch={} target_drawn={} weapon_state={} "
			"reason={} identity_right_form=0x{:08X} identity_left_form=0x{:08X} identity_right_type={} identity_left_type={} identity_family={} "
			"old_identity_epoch={} current_identity_epoch={} engine_method=DrawWeaponMagicHands "
			"recursive_native_call=false",
			recoverySession,
			recoverySequence,
			recoveryIdentityCaptured,
			recoveryIdentityEpoch,
			recoveryTargetDrawn,
			recoveryWeaponState,
			recoveryReason,
			recoveryIdentity.rightFormId,
			recoveryIdentity.leftFormId,
			recoveryIdentity.rightWeaponType,
			recoveryIdentity.leftWeaponType,
			State::WeaponEquipmentFamilyName(recoveryIdentity.family),
			reservedIdentityEpoch,
			currentIdentityEpoch);
		logger::info(
			"event=weapon_native_fallback_gate session={} sequence={} ready_generation={} decision=execute "
			"kind={} engine_method=DrawWeaponMagicHands reservation_revalidated=true "
			"reservation_gate_held=false recursive_reentry_allowed=false directional_operation=true "
			"post_call_reconciliation_required=true residual_risk=stale_directional_side_effect_reconciled",
			currentSession,
			a_request.sequence,
			currentReadyGeneration,
			a_groundObserver ? "ground" : "flight");
		return true;
	}

	std::uint64_t FlightManager::ResetForLifecycle(std::string_view a_reason)
	{
		State::LifecycleInvalidationDecision lifecycle{};
		State::ReadyGenerationBarrierState oldBarrier{};
		bool hadFlying = false;
		bool hadTransition = false;
		bool hadFallback = false;
		bool hadGroundObserver = false;
		bool hadSwapPending = false;
		bool hadSwapPin = false;
		bool hadRegenObservation = false;
		bool hadReadyBarrier = false;
		bool hadThread = false;
		{
			std::unique_lock nativeGate(_readyNativeActionMutex);
			bool clearOwnedBlock = false;
			{
				std::shared_lock lock(_mutex);
				clearOwnedBlock = _flightBlockRequested || _flightBlockLease.active;
			}
			if (clearOwnedBlock) {
				// Keep lifecycle cleanup on the same guarded release path while the
				// actor is still available.  If it has unloaded, SetFlightBlockRequested
				// consumes only the logical/latch metadata and performs no native write.
				(void)SetFlightBlockRequested(false);
			}
			std::unique_lock lock(_mutex);
			lifecycle = State::DecideLifecycleInvalidation(
				_flightSessionId,
				_readyGenerationCounter.load(std::memory_order_relaxed));
			oldBarrier = _readyGenerationBarrier;
			hadFlying = _isFlying;
			hadTransition = _weaponTransitionPending;
			hadFallback = _weaponTransitionNativeFallbackArmed;
			hadGroundObserver = _groundWeaponObservationPending;
			hadSwapPending = _weaponEquipmentSwap.pending;
			hadSwapPin = State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap);
			hadRegenObservation = _magickaRegenObservationPending;
			hadReadyBarrier = State::ReadyGenerationBarrierPending(_readyGenerationBarrier);
			hadThread = _threadRunning.exchange(false, std::memory_order_acq_rel);

			_flightSessionId = lifecycle.newSession;
			_publishedFlightSessionId.store(_flightSessionId, std::memory_order_release);
			(void)AdvanceUpdateTaskGeneration();
			_readyGenerationBarrier = State::ResetReadyGenerationBarrier(
				_readyGenerationBarrier,
				lifecycle.newSession,
				lifecycle.readyGenerationFloor);
			ClearReadyGenerationLeaseLocked();
			_readyGenerationBlockedDiagnosticLogged = false;
			_isFlying = false;
			_isDescending = false;
			_flightCombatActive = false;
			_flightBlockRequested = false;
			_flightBlockNativeStateOwned = false;
			_flightBlockLease = {};
			_flightBlockRequestedIdentity = {};
			_flightBlockRequestedIdentityEpoch = 0;
			_flightBlockRequestedIdentityCaptured = false;
			_flightShoutHeld = false;
			_boostHeld = false;
			_useGeneratedCombatTopology = false;
			_aerialCombatUnsupportedNotified = false;
			_flightWorldStateOwned = false;
			_flightOwnedController = nullptr;
			_forwardInput = 0.0F;
			_strafeInput = 0.0F;
			_verticalInput = 0.0F;
			_pendingLaunchBoost = 0.0F;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kOff);
			_landingContactTicks = 0;
			_shoutGraphOverrideUntil = {};
			_whirlwindSprintUntil = {};
			_whirlwindSprintShoutPending = false;
			_smoothedFlightVelocity = RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F };
			_originalGravity = 0.0F;
			_originalNoFriction = false;

			_weaponTransitionPending = false;
			_weaponTransitionTargetDrawn = false;
			_weaponTransitionNativeFallbackArmed = false;
			_weaponTransitionExpiryRecoveryAttempted = false;
			_weaponTransitionNativeFallbackRetryUsed = false;
			_weaponTransitionProgressExtensionUsed = false;
			_weaponTransitionPostFlight = false;
			_weaponTransitionTerminalFailureHold = {};
			_weaponTransitionDeadline = {};
			_weaponTransitionNativeFallbackAt = {};
			_weaponTransitionPreRequestState = -1;
			_weaponTransitionActorFormId = 0;
			_weaponTransitionSessionId = 0;
			_weaponTransitionIdentityEpoch = 0;
			_weaponTransitionEquipmentIdentity = {};
			_weaponTransitionEquipmentIdentityCaptured = false;
			++_weaponTransitionSequence;

			_groundWeaponObservationPending = false;
			_groundWeaponFallbackIssued = false;
			_groundWeaponFallbackRetryUsed = false;
			_groundWeaponProgressExtensionUsed = false;
			_groundWeaponTargetDrawn = false;
			_groundWeaponActorFormId = 0;
			_groundWeaponPreEdgeState = -1;
			_groundWeaponSessionId = 0;
			_groundWeaponEdgeSequence = 0;
			_groundWeaponIdentityEpoch = 0;
			_groundWeaponDeadline = {};
			_groundWeaponEquipmentIdentity = {};
			_groundWeaponEquipmentIdentityCaptured = false;
			// Lifecycle/session invalidation already rejects delayed actions.  Keep the
			// equipment epoch stable here; only a live identity observer may advance it.
			SetWeaponEquipmentEpochBaselineLocked({}, false);

			_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
			_weaponEquipmentSwapPreviousIdentity = {};
			_weaponEquipmentSwapCurrentIdentity = {};
			_weaponEquipmentSwapIdentityCaptured = false;
			_weaponEquipmentSwapPinnedDiagnosticLogged = false;

			_magickaDrainCarrySeconds = 0.0F;
			_lastMagickaDrainAt = {};
			_magickaDrainSequence = 0;
			_magickaRegenObservationSessionId = 0;
			_magickaRegenObservationDrainSequence = 0;
			_magickaRegenObservationActorFormId = 0;
			_magickaRegenDelayBaseline = 0.0F;
			_magickaRegenDelayAfterDrain = 0.0F;
			_magickaRegenDelayOwned = false;
			_magickaRegenAwaitingDelayedIncrease = false;
			_magickaRegenObservationStableLogged = false;
			_magickaRegenObservationPending = false;
			_magickaRegenObservationSamples = 0;
			_magickaRegenObservationStarted = {};
			_magickaRegenObservationNextSample = {};
			_magickaDrainDiagnosticAggregate = {};
			_magickaDrainDiagnosticStarted = {};
			_magickaRegenDiagnosticPipeline = {};

			_flightSessionStartActionSequence = 0;
			_lastQueuedActionSequence = 0;
			_activeQueuedActionSequence = 0;
			_lastDiagnosticEquipmentSignature = ~std::uint64_t{ 0 };
			_lastWeaponRoutingSignature = ~std::uint64_t{ 0 };
			_lastDiagnosticStateSignature = ~std::uint64_t{ 0 };
			_lastInputDiagnosticSignature = ~std::uint64_t{ 0 };
			_lastInputDiagnosticAt = {};
			_suppressedInputDiagnosticCount = 0;
			_lastDiagnosticSnapshot = {};
		}
		// ResetForLifecycle invalidates the worker's state before releasing the
		// manager lock.  Join the old jthread now, while the singleton is still
		// alive; leaving a joinable worker for the next Start or static destruction
		// permits it to observe partially reset state.
		StopUpdateThread();

		logger::info(
			"event=lifecycle_reset reason={} old_session={} new_session={} "
			"ready_announced={} ready_applied={} ready_retired={} ready_floor={} ready_barrier_pending={} "
			"cleared_flying={} cleared_transition_pending={} cleared_native_fallback={} "
			"cleared_ground_observer={} cleared_swap_pending={} cleared_swap_pin={} "
			"cleared_regen_observation={} update_thread_stopped={}",
			a_reason,
			lifecycle.oldSession,
			lifecycle.newSession,
			oldBarrier.announced,
			oldBarrier.applied,
			oldBarrier.retired,
			lifecycle.readyGenerationFloor,
			hadReadyBarrier,
			hadFlying,
			hadTransition,
			hadFallback,
			hadGroundObserver,
			hadSwapPending,
			hadSwapPin,
			hadRegenObservation,
			hadThread);
		return lifecycle.newSession;
	}

	void FlightManager::LogInputDiagnostic(const State::InputDiagnosticSnapshot& a_snapshot) const
	{
		std::uint64_t session = 0;
		bool enabled = false;
		bool flying = false;
		bool descending = false;
		bool combatActive = false;
		bool blockRequested = false;
		bool transitionPending = false;
		bool transitionTargetDrawn = false;
		bool transitionNativeFallbackArmed = false;
		std::uint32_t suppressedCount = 0;
		bool emit = false;
		std::uint64_t diagnosticSignature = 1469598103934665603ULL;
		const auto mix = [&diagnosticSignature](std::uint64_t a_value) {
			diagnosticSignature ^= a_value;
			diagnosticSignature *= 1099511628211ULL;
		};
		const auto mixString = [&mix](std::string_view a_value) {
			for (const auto character : a_value) {
				mix(static_cast<std::uint8_t>(character));
			}
		};
		mixString(a_snapshot.action);
		mixString(a_snapshot.userEvent);
		mixString(a_snapshot.outcome);
		mix(a_snapshot.device);
		mix(a_snapshot.code);
		mix(static_cast<std::uint64_t>(a_snapshot.heldDuration * 1000.0F));
		mix(static_cast<std::uint8_t>(a_snapshot.phase));
		{
			std::unique_lock lock(_mutex);
			enabled = _detailedLogging;
			session = _flightSessionId;
			flying = _isFlying;
			descending = _isDescending;
			combatActive = _flightCombatActive;
			blockRequested = _flightBlockRequested;
			transitionPending = _weaponTransitionPending;
			transitionTargetDrawn = _weaponTransitionTargetDrawn;
			transitionNativeFallbackArmed = _weaponTransitionNativeFallbackArmed;
			mix(flying);
			mix(descending);
			mix(combatActive);
			mix(blockRequested);
			mix(transitionPending);
			mix(transitionTargetDrawn);
			mix(transitionNativeFallbackArmed);
			const auto now = std::chrono::steady_clock::now();
			const bool initialized = _lastInputDiagnosticAt.time_since_epoch().count() != 0;
			const float elapsed = initialized ?
				std::chrono::duration<float>(now - _lastInputDiagnosticAt).count() : 0.0F;
			const bool edge = a_snapshot.phase == State::InputButtonPhase::kDown ||
				a_snapshot.phase == State::InputButtonPhase::kUp ||
				a_snapshot.phase == State::InputButtonPhase::kPressed;
			const bool heartbeat = !initialized || elapsed >= 2.0F;
			const bool edgeDue = edge && (!initialized || elapsed >= 0.10F);
			const auto decision = State::ReduceInputDiagnostic(
				State::InputDiagnosticThrottleState{
					initialized,
					_lastInputDiagnosticSignature,
					_suppressedInputDiagnosticCount },
				diagnosticSignature,
				edgeDue,
				heartbeat);
			emit = decision.emit;
			suppressedCount = decision.flushedSuppressedCount;
			_lastInputDiagnosticSignature = decision.state.signature;
			_suppressedInputDiagnosticCount = decision.state.suppressedCount;
			if (emit) {
				_lastInputDiagnosticAt = now;
			}
		}
		if (!emit || !enabled) {
			return;
		}

		const char* phase = "other";
		switch (a_snapshot.phase) {
		case State::InputButtonPhase::kUp: phase = "up"; break;
		case State::InputButtonPhase::kDown: phase = "down"; break;
		case State::InputButtonPhase::kHeld: phase = "held"; break;
		case State::InputButtonPhase::kPressed: phase = "pressed"; break;
		default: break;
		}
		logger::info(
			"event=input_received schema={} action_id={} session={} input_sequence_domain={} manager_sequence=0 action={} user_event=\"{}\" device={} code=0x{:X} held_duration={:.2f} phase={} "
			"flying={} descending={} combat_active={} block_requested={} "
			"weapon_transition_pending={} weapon_transition_target_drawn={} "
			"weapon_transition_native_fallback_armed={} suppressed_count={} outcome={} reason=manager_diagnostic_snapshot",
			State::StructuredDiagnosticSchemaVersion,
			a_snapshot.actionId,
			session,
			a_snapshot.inputSequenceDomain,
			a_snapshot.action,
			a_snapshot.userEvent,
			a_snapshot.device,
			a_snapshot.code,
			a_snapshot.heldDuration,
			phase,
			flying,
			descending,
			combatActive,
			blockRequested,
			transitionPending,
			transitionTargetDrawn,
			transitionNativeFallbackArmed,
			suppressedCount,
			a_snapshot.outcome);
	}

	void FlightManager::LogDiagnosticSnapshot(RE::PlayerCharacter* a_player)
	{
		if (!a_player || !a_player->Is3DLoaded()) {
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		const auto equipment = GetEquipmentDiagnostic(a_player);
		std::uint64_t session = 0;
		std::uint64_t stateSignature = 0;
		float intervalSeconds = 2.0F;
		float forwardInput = 0.0F;
		float strafeInput = 0.0F;
		float verticalInput = 0.0F;
		bool phaseFlight = false;
		bool phasePostFlight = false;
		bool descending = false;
		bool combatActive = false;
		bool blockRequested = false;
		bool weaponTransitionPending = false;
		bool weaponTransitionTargetDrawn = false;
		bool weaponTransitionNativeFallbackArmed = false;
		bool weaponTransitionPostFlight = false;
		bool flightShoutHeld = false;
		bool magickaRegenDelayOwned = false;
		bool magickaRegenObservationPending = false;
		bool groundWeaponObservationPending = false;
		bool equipmentSwapPending = false;
		bool equipmentSwapPinned = false;
		bool equipmentSwapEdgeObserved = false;
		bool equipmentSwapTargetDrawn = false;
		bool equipmentSwapIdentityCaptured = false;
		State::WeaponEquipmentIdentity equipmentSwapIdentity{};
		std::uint32_t magickaRegenObservationSamples = 0;
		std::uint64_t magickaDrainSequence = 0;
		std::uint64_t magickaObservationSession = 0;
		std::uint64_t magickaObservationDrainSequence = 0;
		std::uint32_t magickaObservationActorFormId = 0;
		float magickaDrainCarrySeconds = 0.0F;
		float magickaRegenDelayBaseline = 0.0F;
		float magickaRegenDelayAfterDrain = 0.0F;
		std::uint64_t weaponTransitionSequence = 0;
		bool boostHeld = false;
		std::int32_t requestedGraphState = 0;
		const char* reason = "heartbeat";
		{
			std::unique_lock lock(_mutex);
			if (!_detailedLogging ||
				(!_isFlying && !_weaponTransitionPending && !_magickaRegenObservationPending &&
					!_groundWeaponObservationPending && !_weaponEquipmentSwap.pending &&
					!State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap))) {
				return;
			}

			intervalSeconds = _diagnosticSnapshotIntervalSeconds;
			stateSignature = State::ComputeDiagnosticStateSignature(
				_lastGraphState,
				_isDescending,
				_flightCombatActive,
				_boostHeld,
				_flightBlockRequested,
				_weaponTransitionPending,
				_weaponTransitionTargetDrawn,
				_weaponTransitionNativeFallbackArmed,
			_weaponTransitionPostFlight,
				_flightShoutHeld,
				_groundWeaponObservationPending,
				_weaponEquipmentSwap.pending,
				State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap),
				_weaponEquipmentSwap.transitionEdgeObserved,
				_flightCombatActive);
			const bool equipmentChanged = equipment.signature != _lastDiagnosticEquipmentSignature;
			const bool stateChanged = stateSignature != _lastDiagnosticStateSignature;
			const bool heartbeatDue = _lastDiagnosticSnapshot.time_since_epoch().count() == 0 ||
				std::chrono::duration<float>(now - _lastDiagnosticSnapshot).count() >= intervalSeconds;
			if (!equipmentChanged && !stateChanged && !heartbeatDue) {
				return;
			}

			reason = State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap) ? "equipment_swap_pinned" :
			(_weaponEquipmentSwap.pending ? "equipment_swap_pending" :
			(_weaponTransitionPostFlight ? "post_flight_transition" :
				(_magickaRegenObservationPending ? "magicka_regen_observation" :
				(equipmentChanged ? "equipment_change" : (stateChanged ? "state_change" : "heartbeat")))));
			_lastDiagnosticEquipmentSignature = equipment.signature;
			_lastDiagnosticStateSignature = stateSignature;
			_lastDiagnosticSnapshot = now;
			session = _flightSessionId;
			phaseFlight = _isFlying;
			phasePostFlight = !_isFlying && _weaponTransitionPostFlight;
			forwardInput = _forwardInput;
			strafeInput = _strafeInput;
			verticalInput = _verticalInput;
			descending = _isDescending;
			combatActive = _flightCombatActive;
			blockRequested = _flightBlockRequested;
			weaponTransitionPending = _weaponTransitionPending;
			weaponTransitionTargetDrawn = _weaponTransitionTargetDrawn;
			weaponTransitionNativeFallbackArmed = _weaponTransitionNativeFallbackArmed;
			weaponTransitionPostFlight = _weaponTransitionPostFlight;
			flightShoutHeld = _flightShoutHeld;
			magickaRegenDelayOwned = _magickaRegenDelayOwned;
			magickaRegenObservationPending = _magickaRegenObservationPending;
			groundWeaponObservationPending = _groundWeaponObservationPending;
			equipmentSwapPending = _weaponEquipmentSwap.pending;
			equipmentSwapPinned = State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap);
			equipmentSwapEdgeObserved = _weaponEquipmentSwap.transitionEdgeObserved;
			equipmentSwapTargetDrawn = _flightCombatActive;
			equipmentSwapIdentityCaptured = _weaponEquipmentSwapIdentityCaptured;
			equipmentSwapIdentity = _weaponEquipmentSwapCurrentIdentity;
			magickaRegenObservationSamples = _magickaRegenObservationSamples;
			magickaDrainSequence = _magickaDrainSequence;
			magickaObservationSession = _magickaRegenObservationSessionId;
			magickaObservationDrainSequence = _magickaRegenObservationDrainSequence;
			magickaObservationActorFormId = _magickaRegenObservationActorFormId;
			magickaDrainCarrySeconds = _magickaDrainCarrySeconds;
			magickaRegenDelayBaseline = _magickaRegenDelayBaseline;
			magickaRegenDelayAfterDrain = _magickaRegenDelayAfterDrain;
			weaponTransitionSequence = _weaponTransitionSequence;
			boostHeld = _boostHeld;
			requestedGraphState = _lastGraphState;
		}
		logger::info(
			"event=state_checkpoint schema={} session={} reason={} boundary=coalesced_state_change visibility=DAF_native_only",
			State::StructuredDiagnosticSchemaVersion,
			session,
			reason);

		bool graphDragonAspect = false;
		bool graphFlight = false;
		bool graphCombat = false;
		bool graphLaunch = false;
		bool graphShout = false;
		bool graphInJump = false;
		bool graphBlocking = false;
		std::int32_t graphState = -1;
		std::uint32_t graphReadMask = 0;
		graphReadMask |= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarDragonAspectActive), graphDragonAspect) ? 1U << 0U : 0;
		graphReadMask |= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarFlightActive), graphFlight) ? 1U << 1U : 0;
		graphReadMask |= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarFlightCombatActive), graphCombat) ? 1U << 2U : 0;
		graphReadMask |= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarLaunchBoost), graphLaunch) ? 1U << 3U : 0;
		graphReadMask |= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarFlightShout), graphShout) ? 1U << 4U : 0;
		graphReadMask |= a_player->GetGraphVariableInt(RE::BSFixedString(GraphVarFlightState), graphState) ? 1U << 5U : 0;
		graphReadMask |= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarVanillaInJumpState), graphInJump) ? 1U << 6U : 0;
		graphReadMask |= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarVanillaIsBlocking), graphBlocking) ? 1U << 7U : 0;

		const auto* actorState = a_player->AsActorState();
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const bool wantBlocking = actorState && actorState->actorState2.wantBlocking;
		const auto weaponState = actorState ? static_cast<std::int32_t>(actorState->actorState2.weaponState) : -1;
		const auto meleeAttackState = actorState ? static_cast<std::int32_t>(actorState->actorState1.meleeAttackState) : -1;
		const bool isBlocking = a_player->IsBlocking();
		const auto position = a_player->GetPosition();
		float magicka = 0.0F;
		float stamina = 0.0F;
		float magickaRegenDelay = 0.0F;
		if (const auto* avOwner = a_player->AsActorValueOwner()) {
			magicka = avOwner->GetActorValue(RE::ActorValue::kMagicka);
			stamina = avOwner->GetActorValue(RE::ActorValue::kStamina);
		}
		magickaRegenDelay = a_player->GetRegenDelay(RE::ActorValue::kMagicka);

		std::int32_t controllerCurrentState = -1;
		std::int32_t controllerWantState = -1;
		float gravity = 0.0F;
		RE::hkVector4 velocity{ 0.0F, 0.0F, 0.0F, 0.0F };
		if (auto* controller = a_player->GetCharController()) {
			controllerCurrentState = static_cast<std::int32_t>(controller->context.currentState);
			controllerWantState = static_cast<std::int32_t>(controller->wantState);
			gravity = controller->gravity;
			controller->GetLinearVelocityImpl(velocity);
		}

		logger::info(
			"event=state_snapshot session={} reason={} version={} expected_oar_family={} quarterstaff={} quarterstaff_keyword={} block_capable={} "
			"weapons_drawn={} right_form=0x{:08X} right_form_type={} right_weapon_type={} right_name=\"{}\" "
			"left_form=0x{:08X} left_form_type={} left_weapon_type={} left_name=\"{}\" "
			"phase_flight={} phase_postflight={} ground_weapon_observation_pending={} "
			"equipment_swap_pending={} equipment_swap_pinned={} equipment_swap_edge_observed={} "
			"equipment_swap_target_drawn={} equipment_swap_identity_captured={} "
			"equipment_swap_identity_right_form=0x{:08X} equipment_swap_identity_left_form=0x{:08X} "
			"equipment_swap_identity_right_type={} equipment_swap_identity_left_type={} equipment_swap_identity_family={} "
			"descending={} combat_active={} block_requested={} want_blocking={} is_blocking={} "
			"weapon_state={} attack_state={} weapon_transition_pending={} weapon_transition_target_drawn={} "
			"weapon_transition_native_fallback_armed={} weapon_transition_post_flight={} flight_shout_held={} "
			"weapon_transition_sequence={} boost_held={} input_fwd={:.3f} input_strafe={:.3f} input_vertical={:.3f} "
			"requested_graph_state={} graph_read_mask=0x{:02X} graph_da={} graph_flight={} graph_combat={} "
			"graph_launch={} graph_shout={} graph_state={} graph_in_jump={} graph_blocking={} "
			"controller_current={} controller_want={} gravity={:.3f} velocity=({:.3f},{:.3f},{:.3f}) "
			"position=({:.3f},{:.3f},{:.3f}) magicka={:.3f} stamina={:.3f} "
			"magicka_drain_seq={} magicka_observation_session={} magicka_observation_drain_seq={} "
			"magicka_observation_actor=0x{:08X} magicka_drain_carry_seconds={:.3f} magicka_regen_candidate={} "
			"magicka_regen_observation_pending={} "
			"magicka_regen_observation_samples={} magicka_regen_baseline={:.3f} magicka_regen_after_drain={:.3f} "
			"magicka_regen_delay={:.3f} magicka_regen_write_attempted=false magicka_regen_write_performed=false",
			session,
			reason,
			BuildVersion,
			equipment.expectedOarFamily,
			equipment.quarterstaffEquipped,
			equipment.quarterstaffKeyword,
			equipment.blockCapable,
			weaponsDrawn,
			equipment.right ? equipment.right->GetFormID() : 0,
			GetFormTypeValue(equipment.right),
			equipment.rightWeaponType,
			GetFormName(equipment.right),
			equipment.left ? equipment.left->GetFormID() : 0,
			GetFormTypeValue(equipment.left),
			equipment.leftWeaponType,
			GetFormName(equipment.left),
			phaseFlight,
			phasePostFlight,
			groundWeaponObservationPending,
			equipmentSwapPending,
			equipmentSwapPinned,
			equipmentSwapEdgeObserved,
			equipmentSwapTargetDrawn,
			equipmentSwapIdentityCaptured,
			equipmentSwapIdentity.rightFormId,
			equipmentSwapIdentity.leftFormId,
			equipmentSwapIdentity.rightWeaponType,
			equipmentSwapIdentity.leftWeaponType,
			State::WeaponEquipmentFamilyName(equipmentSwapIdentity.family),
			descending,
			combatActive,
			blockRequested,
			wantBlocking,
			isBlocking,
			weaponState,
			meleeAttackState,
			weaponTransitionPending,
			weaponTransitionTargetDrawn,
			weaponTransitionNativeFallbackArmed,
			weaponTransitionPostFlight,
			flightShoutHeld,
			weaponTransitionSequence,
			boostHeld,
			forwardInput,
			strafeInput,
			verticalInput,
			requestedGraphState,
			graphReadMask,
			graphDragonAspect,
			graphFlight,
			graphCombat,
			graphLaunch,
			graphShout,
			graphState,
			graphInJump,
			graphBlocking,
			controllerCurrentState,
			controllerWantState,
			gravity,
			velocity.quad.m128_f32[0],
			velocity.quad.m128_f32[1],
			velocity.quad.m128_f32[2],
			position.x,
			position.y,
			position.z,
			magicka,
			stamina,
			magickaDrainSequence,
			magickaObservationSession,
			magickaObservationDrainSequence,
			magickaObservationActorFormId,
			magickaDrainCarrySeconds,
			magickaRegenDelayOwned,
			magickaRegenObservationPending,
			magickaRegenObservationSamples,
			magickaRegenDelayBaseline,
			magickaRegenDelayAfterDrain,
			magickaRegenDelay);
	}

	std::uint64_t FlightManager::AdvanceUpdateTaskGeneration() noexcept
	{
		auto current = _updateTaskGeneration.load(std::memory_order_relaxed);
		for (;;) {
			const auto next = current == std::numeric_limits<std::uint64_t>::max() ?
				std::uint64_t{ 1 } : current + 1;
			if (_updateTaskGeneration.compare_exchange_weak(
					current,
					next,
					std::memory_order_acq_rel,
					std::memory_order_relaxed)) {
				// A callback from the prior generation may still be in SKSE's task
				// queue.  Its generation check prevents it from clearing this new
				// lease or mutating the new session.
				_queuedUpdateGeneration.store(0, std::memory_order_release);
				return next;
			}
		}
	}

	bool FlightManager::StartUpdateThread()
	{
		std::lock_guard lifecycleLock(_threadLifecycleMutex);
		if (_threadRunning.load(std::memory_order_acquire)) {
			return true;
		}
		if (_updateThread.joinable()) {
			_updateThread.request_stop();
			if (_updateThread.get_id() == std::this_thread::get_id()) {
				// A worker must never join or detach itself.  Leave the joinable
				// object for a later game/lifecycle caller to reap.
				logger::error("Flight update thread start refused from worker thread; join deferred");
				return false;
			}
			try {
				_updateThread.join();
			} catch (const std::system_error& e) {
				logger::error("Flight update thread stale join failed: {}; start deferred", e.what());
				return false;
			}
		}
		_threadRunning.store(true, std::memory_order_release);

		try {
			_updateThread = std::jthread([this](std::stop_token a_stopToken) {
				logger::info("Flight update thread started");

				while (!a_stopToken.stop_requested() && _threadRunning.load()) {
					bool flightActive = false;
					bool transitionPending = false;
					bool regenObservationPending = false;
					bool groundWeaponObservationPending = false;
					bool equipmentSwapPending = false;
					bool readyGenerationPending = false;
					{
						std::shared_lock lock(_mutex);
						flightActive = _isFlying;
						transitionPending = _weaponTransitionPending;
						regenObservationPending = _magickaRegenObservationPending;
						groundWeaponObservationPending = _groundWeaponObservationPending;
						equipmentSwapPending = _weaponEquipmentSwap.pending;
						readyGenerationPending = State::ReadyGenerationBarrierPending(_readyGenerationBarrier);
					}
					if (State::ShouldPumpWeaponTransition(
							flightActive,
							transitionPending,
							regenObservationPending,
							groundWeaponObservationPending,
							equipmentSwapPending,
							readyGenerationPending)) {
						QueueUpdate();
					}

					std::this_thread::sleep_for(std::chrono::milliseconds(16));
				}

				logger::info("Flight update thread stopped");
			});
		} catch (const std::exception& e) {
			_threadRunning.store(false, std::memory_order_release);
			logger::error("Flight update thread start failed: {}; worker state cleared", e.what());
			return false;
		} catch (...) {
			_threadRunning.store(false, std::memory_order_release);
			logger::error("Flight update thread start failed with an unknown exception; worker state cleared");
			return false;
		}
		return true;
	}

	void FlightManager::StopUpdateThread()
	{
		std::lock_guard lifecycleLock(_threadLifecycleMutex);
		_threadRunning.store(false, std::memory_order_release);
		if (!_updateThread.joinable()) {
			return;
		}

		_updateThread.request_stop();
		if (_updateThread.get_id() == std::this_thread::get_id()) {
			// Detaching is unsafe here: the worker captures this singleton and may
			// execute after lifecycle state or dependent engine code has gone away.
			// A later game/lifecycle call must reap the joinable thread.
			logger::error("Flight update thread stop requested from worker thread; join deferred");
			return;
		}
		try {
			_updateThread.join();
		} catch (const std::system_error& e) {
			// There is no timeout-based detach fallback.  Keep the jthread joinable
			// so the next lifecycle boundary can retry a safe join.
			logger::error("Flight update thread stop join failed: {}; join deferred", e.what());
		}
	}

	void FlightManager::QueueUpdate()
	{
		auto taskInterface = SKSE::GetTaskInterface();

		if (!taskInterface) {
			std::uint64_t expiredSession = 0;
			std::uint64_t expiredGeneration = 0;
			bool retiredReadyLease = false;
			{
				std::unique_lock nativeGate(_readyNativeActionMutex);
				std::unique_lock lock(_mutex);
				const auto leaseDecision = State::DecideReadyGenerationLease(
					_readyGenerationBarrier,
					_readyGenerationLeaseToken,
					State::kReadyGenerationLeaseMilliseconds);
				if (leaseDecision.pending) {
					const auto token = _readyGenerationLeaseToken;
					const auto retired = State::ReduceReadyGenerationBarrier(
						_readyGenerationBarrier,
						State::ReadyGenerationBarrierEvent::kRetire,
						token);
					if (retired.accepted) {
						_readyGenerationBarrier = retired.state;
						expiredSession = _flightSessionId;
						expiredGeneration = token.generation;
						ClearReadyProtectedStateLocked();
						retiredReadyLease = true;
					}
				}
			}
			if (retiredReadyLease) {
				logger::warn(
					"event=ready_barrier_expired reason=task_lease_timeout queue_failure=task_interface_unavailable "
					"session={} generation={} lease_ms={} barrier_retired=true late_task_policy=reject_stale",
					expiredSession,
					expiredGeneration,
					State::kReadyGenerationLeaseMilliseconds);
			}
			return;
		}

		const auto session = _publishedFlightSessionId.load(std::memory_order_acquire);
		const auto generation = _updateTaskGeneration.load(std::memory_order_acquire);
		std::uint64_t expectedGeneration = 0;
		if (!_queuedUpdateGeneration.compare_exchange_strong(
				expectedGeneration,
				generation,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			// The worker already has a game-thread task queued.  Coalescing here is
			// essential when the game thread stalls: one stale frame must not become
			// an unbounded queue of duplicate native updates.
			return;
		}

		// A lifecycle edge may have invalidated the generation between the worker
		// snapshot and this reservation.  Drop the reservation before returning;
		// the callback remains harmless if the edge races after this check.
		if (_updateTaskGeneration.load(std::memory_order_acquire) != generation ||
			_publishedFlightSessionId.load(std::memory_order_acquire) != session) {
			expectedGeneration = generation;
			_queuedUpdateGeneration.compare_exchange_strong(
				expectedGeneration,
				std::uint64_t{ 0 },
				std::memory_order_acq_rel,
				std::memory_order_acquire);
			return;
		}

		try {
			taskInterface->AddTask([this, session, generation]() {
				const auto releaseQueueLease = [this, generation]() noexcept {
					std::uint64_t expected = generation;
					_queuedUpdateGeneration.compare_exchange_strong(
						expected,
						std::uint64_t{ 0 },
						std::memory_order_acq_rel,
						std::memory_order_acquire);
				};
				const bool current =
					_updateTaskGeneration.load(std::memory_order_acquire) == generation &&
					_publishedFlightSessionId.load(std::memory_order_acquire) == session;
				try {
					if (current) {
						UpdateFlight();
					}
				} catch (const std::exception& e) {
					releaseQueueLease();
					logger::error(
						"Flight update task failed: {}; generation={} session={} lease released",
						e.what(),
						generation,
						session);
					return;
				} catch (...) {
					releaseQueueLease();
					logger::error(
						"Flight update task failed with an unknown exception; generation={} session={} lease released",
						generation,
						session);
					return;
				}
				releaseQueueLease();
			});
		} catch (const std::exception& e) {
			expectedGeneration = generation;
			_queuedUpdateGeneration.compare_exchange_strong(
				expectedGeneration,
				std::uint64_t{ 0 },
				std::memory_order_acq_rel,
				std::memory_order_acquire);
			logger::error("Flight update task queue failed: {}; reservation cleared", e.what());
		} catch (...) {
			expectedGeneration = generation;
			_queuedUpdateGeneration.compare_exchange_strong(
				expectedGeneration,
				std::uint64_t{ 0 },
				std::memory_order_acq_rel,
				std::memory_order_acquire);
			logger::error("Flight update task queue failed with an unknown exception; reservation cleared");
		}
	}

	void FlightManager::UpdateFlight()
	{
		float flightSpeed = 0.0F;
		float verticalSpeed = 0.0F;
		float liftScale = 1.0F;
		float forwardInput = 0.0F;
		float strafeInput = 0.0F;
		float verticalInput = 0.0F;
		float launchBoost = 0.0F;
		bool boostHeld = false;
		bool descending = false;
		bool combatActive = false;
		bool blockAutoCleared = false;
		bool blockAutoReleaseAccepted = false;
		bool useGeneratedCombatTopology = false;
		bool flightActive = false;
		bool postFlightTransition = false;
		bool regenObservationPending = false;
		bool groundWeaponObservationPending = false;
		bool equipmentSwapPending = false;
		bool flightShoutHeld = false;
		bool shoutOverrideActive = false;
		bool whirlwindSprintActive = false;
		bool transitionTargetDrawn = false;
		bool issueNativeWeaponFallback = false;
		bool logNativeFallbackDecision = false;
		std::int32_t transitionPreRequestState = -1;
		std::uint64_t transitionSequence = 0;
		std::uint64_t session = 0;
		std::string_view transitionOutcome;
		std::string_view nativeFallbackDecision;
		std::string_view nativeFallbackReason;
		std::string_view equipmentSwapPhase;
		bool equipmentSwapEdgeObserved = false;
		bool equipmentSwapSettled = false;
		bool equipmentSwapTimedOut = false;
		bool equipmentSwapPendingAfter = false;
		bool equipmentSwapEdgeAfter = false;
		bool equipmentSwapReadinessPreservedAfter = false;
		bool equipmentSwapPinnedAfter = false;
		State::WeaponEquipmentIdentity equipmentSwapPreviousIdentity{};
		State::WeaponEquipmentIdentity equipmentSwapCurrentIdentity{};
		bool equipmentSwapIdentityCaptured = false;
		State::NativeFallbackGateRequest nativeFallbackRequest{};
		State::WeaponEquipmentIdentity nativeFallbackIdentity{};
		bool nativeFallbackIdentityCaptured = false;
		bool readyBarrierExpired = false;
		std::uint64_t readyBarrierExpiredSession = 0;
		std::uint64_t readyBarrierExpiredGeneration = 0;
		std::uint32_t landingContactTicks = 0;
		FlightGraphState graphState = FlightGraphState::kOff;
		const auto now = std::chrono::steady_clock::now();
		auto* player = GetPlayer();
		if (!player || !player->Is3DLoaded()) {
			FlushDiagnosticAggregates("actor_unloaded");
			// Do not keep a native worker alive while its actor/3D is unavailable.
			// Abort any pending observation first so StopFlight cannot re-arm a
			// post-flight pump with a dead actor; the next valid input/lifecycle edge
			// may start a fresh session safely.
			ObserveDafMagickaRegen(nullptr, now, true);
			logger::warn(
				"event=flight_abort reason=actor_unloaded worker_cleanup=true graph_write=false "
				"controller_write=false weapon_fallback=false regen_write=false");
			StopFlight();
			return;
		}
		// Controller replacement is a lifecycle boundary even when the actor and
		// its 3D remain loaded. Never apply movement or graph updates under a
		// baseline captured from a different controller instance.
		auto* currentController = player->GetCharController();
		bool controllerOwnershipLost = false;
		{
			std::shared_lock lock(_mutex);
			controllerOwnershipLost = _isFlying && _flightWorldStateOwned &&
				(_flightOwnedController == nullptr || currentController != _flightOwnedController);
		}
		if (controllerOwnershipLost) {
			logger::error(
				"event=flight_abort reason=controller_replaced worker_cleanup=true "
				"physics_write=false graph_write=false weapon_fallback=false");
			StopFlight();
			return;
		}
		const auto equipment = GetEquipmentDiagnostic(player);
		LogWeaponRoutingDiagnostic(player, "equipment_family_change", "equipment_signature_changed", false);
		const auto* actorState = player ? player->AsActorState() : nullptr;
		const bool transitionActorLoaded = player && player->Is3DLoaded() && actorState;
		bool equipmentSwapDetected = false;
		const auto transitionActorFormId = transitionActorLoaded ?
			static_cast<std::uint32_t>(player->GetFormID()) : 0;
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
		State::ReadyGenerationUpdateDecision readyUpdateDecision{};
		// Serialize the barrier check with Ready announcement before allowing an
		// equipment callback to mutate/cancel the protected transition state.
		std::unique_lock readyMutationGate(_readyNativeActionMutex);
		// Refresh the identity under that same gate.  The diagnostic snapshot above
		// is useful for routing fields, but this tuple is the authoritative producer
		// observation used for epoch-bound invalidation.
		const auto updateEquipmentSnapshot = GetCurrentWeaponEquipmentSnapshot();

		{
			std::unique_lock lock(_mutex);

			flightActive = _isFlying;
			regenObservationPending = _magickaRegenObservationPending;
			groundWeaponObservationPending = _groundWeaponObservationPending;
			equipmentSwapPending = _weaponEquipmentSwap.pending;
			const auto leaseStarted = _readyGenerationLeaseStartedAt;
			const auto leaseElapsedMs = leaseStarted.time_since_epoch().count() != 0 && now >= leaseStarted ?
				static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - leaseStarted).count()) :
				std::uint64_t{ 0 };
			const auto leaseDecision = State::DecideReadyGenerationLease(
				_readyGenerationBarrier,
				_readyGenerationLeaseToken,
				leaseElapsedMs);
			if (leaseDecision.expired) {
				const auto expiredToken = _readyGenerationLeaseToken;
				const auto retired = State::ReduceReadyGenerationBarrier(
					_readyGenerationBarrier,
					State::ReadyGenerationBarrierEvent::kRetire,
					expiredToken);
				if (retired.accepted) {
					readyBarrierExpired = true;
					readyBarrierExpiredSession = _flightSessionId;
					readyBarrierExpiredGeneration = expiredToken.generation;
					_readyGenerationBarrier = retired.state;
					ClearReadyProtectedStateLocked();
				}
			}
			readyUpdateDecision = State::DecideReadyGenerationUpdate(
				_readyGenerationBarrier,
				_weaponTransitionPending,
				_weaponTransitionNativeFallbackArmed && now >= _weaponTransitionNativeFallbackAt,
				_groundWeaponObservationPending);
			if (readyUpdateDecision.barrierPending) {
				const bool emitBarrierDiagnostic = !_readyGenerationBlockedDiagnosticLogged;
				_readyGenerationBlockedDiagnosticLogged = true;
				const auto announced = _readyGenerationBarrier.announced;
				const auto applied = _readyGenerationBarrier.applied;
				const auto retired = _readyGenerationBarrier.retired;
				const auto barrierSession = _flightSessionId;
				lock.unlock();
				if (emitBarrierDiagnostic) {
					logger::info(
						"event=ready_generation session={} phase=update-blocked announced={} applied={} retired={} "
						"preserve_readiness={} suppress_native_fallback={}",
						barrierSession,
						announced,
						applied,
						retired,
						readyUpdateDecision.preserveReadiness,
						readyUpdateDecision.suppressNativeFallback);
				}
				return;
			}
			if (!flightActive && !_weaponTransitionPending && !regenObservationPending &&
				!groundWeaponObservationPending && !equipmentSwapPending) {
				lock.unlock();
				if (readyBarrierExpired) {
					logger::warn(
						"event=ready_barrier_expired reason=task_lease_timeout session={} generation={} "
						"lease_ms={} barrier_retired=true late_task_policy=reject_stale",
						readyBarrierExpiredSession,
						readyBarrierExpiredGeneration,
						State::kReadyGenerationLeaseMilliseconds);
				}
				return;
			}
		}
		if (readyBarrierExpired) {
			logger::warn(
				"event=ready_barrier_expired reason=task_lease_timeout session={} generation={} "
				"lease_ms={} barrier_retired=true late_task_policy=reject_stale",
				readyBarrierExpiredSession,
				readyBarrierExpiredGeneration,
				State::kReadyGenerationLeaseMilliseconds);
		}

		// Ready generation is the first mutation barrier.  Do not let an
		// equipment identity callback cancel/rebase the state protected by a
		// queued Ready edge; the callback runs only after this barrier clears.
		if (readyUpdateDecision.allowEquipmentMutation && transitionActorLoaded &&
			updateEquipmentSnapshot.captured) {
			equipmentSwapDetected = HandleEquipmentIdentityChange(
			updateEquipmentSnapshot.identity,
			"update");
		}
		readyMutationGate.unlock();

		{
			std::unique_lock lock(_mutex);
			postFlightTransition = !flightActive && _weaponTransitionPending && _weaponTransitionPostFlight;

			if (flightActive && !HasDragonAspectActive()) {
				lock.unlock();
				StopFlight();
				return;
			}

			const auto swapBeforeObservation = _weaponEquipmentSwap;
			if (swapBeforeObservation.pending) {
				transitionTargetDrawn = _flightCombatActive;
				transitionSequence = _weaponTransitionSequence;
				const auto swapObservation =
					IsWeaponTransitionInProgress(weaponState, true) ||
						IsWeaponTransitionInProgress(weaponState, false) ?
						State::WeaponEquipmentSwapObservation::kTransitionEdge :
					(IsWeaponStateAtTarget(weaponState, _flightCombatActive) ?
						State::WeaponEquipmentSwapObservation::kStableAtLogicalTarget :
						State::WeaponEquipmentSwapObservation::kStableAwayFromTarget);
				_weaponEquipmentSwap = State::ObserveWeaponEquipmentSwap(
					_weaponEquipmentSwap,
					swapObservation);
				if (!swapBeforeObservation.transitionEdgeObserved &&
					_weaponEquipmentSwap.transitionEdgeObserved) {
					equipmentSwapEdgeObserved = true;
					equipmentSwapPhase = "edge";
				}
				if (swapBeforeObservation.pending && !_weaponEquipmentSwap.pending) {
					equipmentSwapSettled = true;
					equipmentSwapTimedOut = !swapBeforeObservation.transitionEdgeObserved;
					equipmentSwapPhase = equipmentSwapTimedOut ? "expired" : "settled";
				}
				equipmentSwapPreviousIdentity = _weaponEquipmentSwapPreviousIdentity;
				equipmentSwapCurrentIdentity = _weaponEquipmentSwapCurrentIdentity;
				equipmentSwapIdentityCaptured = _weaponEquipmentSwapIdentityCaptured;
				equipmentSwapPendingAfter = _weaponEquipmentSwap.pending;
				equipmentSwapEdgeAfter = _weaponEquipmentSwap.transitionEdgeObserved;
				equipmentSwapReadinessPreservedAfter = _weaponEquipmentSwap.readinessPreserved;
				equipmentSwapPinnedAfter = State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap);
			}

			const bool swapRetiredThisUpdate = swapBeforeObservation.pending &&
				!_weaponEquipmentSwap.pending;
			if ((_weaponEquipmentSwap.pending || swapRetiredThisUpdate) && _weaponTransitionPending) {
				// An identity swap owns this edge.  Never let a transition armed for
				// the previous item issue DrawWeaponMagicHands during the settle hold.
				_weaponTransitionPending = false;
				_weaponTransitionNativeFallbackArmed = false;
				_weaponTransitionExpiryRecoveryAttempted = false;
				_weaponTransitionNativeFallbackRetryUsed = false;
				_weaponTransitionProgressExtensionUsed = false;
				_weaponTransitionPostFlight = false;
				_weaponTransitionDeadline = {};
				_weaponTransitionNativeFallbackAt = {};
				_weaponTransitionPreRequestState = -1;
				_weaponTransitionActorFormId = 0;
				_weaponTransitionSessionId = 0;
				_weaponTransitionIdentityEpoch = 0;
				_weaponTransitionEquipmentIdentity = {};
				_weaponTransitionEquipmentIdentityCaptured = false;
				transitionOutcome = "equipment_swap";
			}

			// Expiry retires the old identity-bound transaction.  If the live actor
			// still differs from DAF's logical Ready target, immediately arm a fresh
			// bounded transition against the current identity/epoch.  No dormant
			// readiness pin is allowed to survive this point.
			if (!_weaponEquipmentSwap.pending &&
				State::IsWeaponEquipmentSwapReconciliationRequired(_weaponEquipmentSwap)) {
				if (flightActive && transitionActorLoaded && _flightCombatActive != weaponsDrawn) {
					++_weaponTransitionSequence;
					_weaponTransitionPending = true;
					_weaponTransitionTargetDrawn = _flightCombatActive;
					const bool rebaseFallbackArmed =
						State::ShouldArmNativeFallbackAfterEquipmentRebase(
							flightActive,
							transitionActorLoaded,
							true,
							_weaponTransitionTargetDrawn,
							weaponsDrawn);
					_weaponTransitionNativeFallbackArmed = rebaseFallbackArmed;
					_weaponTransitionProgressExtensionUsed = false;
					_weaponTransitionPostFlight = false;
					_weaponTransitionDeadline = now + WeaponTransitionTimeout;
					_weaponTransitionNativeFallbackAt = rebaseFallbackArmed ?
						now + WeaponNativeFallbackDelay : std::chrono::steady_clock::time_point{};
					_weaponTransitionPreRequestState = static_cast<std::int32_t>(weaponState);
					_weaponTransitionActorFormId = transitionActorFormId;
					_weaponTransitionSessionId = _flightSessionId;
					_weaponTransitionIdentityEpoch = _weaponEquipmentIdentityEpoch;
					_weaponTransitionEquipmentIdentity = equipment.identity;
					_weaponTransitionEquipmentIdentityCaptured = true;
					transitionTargetDrawn = _weaponTransitionTargetDrawn;
					transitionSequence = _weaponTransitionSequence;
					transitionOutcome = "equipment_swap_reconciliation_armed";
					// The fresh transition owns reconciliation from here onward.
					_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
					equipmentSwapPhase = "expired_rearm";
					equipmentSwapPendingAfter = false;
					equipmentSwapEdgeAfter = false;
					equipmentSwapReadinessPreservedAfter = false;
					equipmentSwapPinnedAfter = false;
				} else {
					_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
					equipmentSwapPhase = "expired_clear";
					equipmentSwapPendingAfter = false;
					equipmentSwapEdgeAfter = false;
					equipmentSwapReadinessPreservedAfter = false;
					equipmentSwapPinnedAfter = false;
				}
				_weaponEquipmentSwapPreviousIdentity = {};
				_weaponEquipmentSwapCurrentIdentity = {};
				_weaponEquipmentSwapIdentityCaptured = false;
				_weaponEquipmentSwapPinnedDiagnosticLogged = false;
			}

			if (_weaponTransitionPending) {
				transitionTargetDrawn = _weaponTransitionTargetDrawn;
				transitionSequence = _weaponTransitionSequence;
				transitionPreRequestState = _weaponTransitionPreRequestState;
				const auto clearWeaponTransition = [&]() {
					_weaponTransitionPending = false;
					_weaponTransitionNativeFallbackArmed = false;
					_weaponTransitionExpiryRecoveryAttempted = false;
					_weaponTransitionNativeFallbackRetryUsed = false;
					_weaponTransitionProgressExtensionUsed = false;
					_weaponTransitionPostFlight = false;
					_weaponTransitionDeadline = {};
					_weaponTransitionNativeFallbackAt = {};
					_weaponTransitionPreRequestState = -1;
					_weaponTransitionActorFormId = 0;
					_weaponTransitionSessionId = 0;
					_weaponTransitionIdentityEpoch = 0;
					_weaponTransitionEquipmentIdentity = {};
					_weaponTransitionEquipmentIdentityCaptured = false;
				};

				const bool transitionSessionMatches = _weaponTransitionSessionId == _flightSessionId;
				const bool transitionActorMatches = transitionActorLoaded &&
					transitionActorFormId == _weaponTransitionActorFormId;
				const bool transitionPhaseValid = flightActive || _weaponTransitionPostFlight;
				if (!transitionSessionMatches || !transitionActorLoaded || !transitionActorMatches ||
					!transitionPhaseValid) {
					const bool requestedTarget = _weaponTransitionTargetDrawn;
					clearWeaponTransition();
					_weaponTransitionTerminalFailureHold = {};
					_flightCombatActive = flightActive && requestedTarget;
					_weaponTransitionTargetDrawn = requestedTarget;
					transitionTargetDrawn = requestedTarget;
					transitionOutcome = !transitionSessionMatches ?
						"aborted_session_mismatch" :
						(!transitionActorLoaded ?
							"aborted_actor_unloaded" :
							(!transitionActorMatches ? "aborted_actor_changed" : "aborted_flight_phase_changed"));
				} else if (IsWeaponStateAtTarget(weaponState, _weaponTransitionTargetDrawn)) {
					const bool requestedTarget = _weaponTransitionTargetDrawn;
					clearWeaponTransition();
					_weaponTransitionTerminalFailureHold =
						State::ReduceWeaponTransitionTerminalFailureHold(
							_weaponTransitionTerminalFailureHold,
							State::WeaponTransitionTerminalFailureHoldEvent::kStableConvergence)
							.state;
					_flightCombatActive = flightActive && requestedTarget;
					_weaponTransitionTargetDrawn = requestedTarget;
					transitionTargetDrawn = requestedTarget;
					transitionOutcome = "completed";
				} else if (now >= _weaponTransitionDeadline) {
					const bool transitionIdentityMatches =
						_weaponTransitionEquipmentIdentityCaptured &&
						State::SameWeaponEquipmentIdentity(
							_weaponTransitionEquipmentIdentity,
							equipment.identity);
					const auto expiryDecision = State::DecideWeaponTransitionExpiry(
						true,
						false,
						transitionActorLoaded,
						transitionIdentityMatches,
						_weaponTransitionExpiryRecoveryAttempted);
					if (expiryDecision.disposition ==
						State::WeaponTransitionExpiryDisposition::kRequestCurrentIdentityRecovery) {
						// Keep the logical target and transition record alive while the
						// current identity receives the real recovery request.  The
						// fallback gate validates this same identity/epoch again before
						// entering DrawWeaponMagicHands.
						_weaponTransitionNativeFallbackArmed = true;
						_weaponTransitionNativeFallbackAt = now;
						_weaponTransitionExpiryRecoveryAttempted = true;
						_weaponTransitionDeadline = now + GroundWeaponFallbackObservationTimeout;
						issueNativeWeaponFallback = true;
						nativeFallbackRequest = State::NativeFallbackGateRequest{
							_weaponTransitionSessionId,
							_weaponTransitionSequence,
							_readyGenerationBarrier.announced,
							_weaponTransitionIdentityEpoch,
							_weaponTransitionActionId };
						nativeFallbackIdentity = equipment.identity;
						nativeFallbackIdentityCaptured = transitionActorLoaded;
						logNativeFallbackDecision = true;
						nativeFallbackDecision = "recovery_requested";
						nativeFallbackReason = "settle_expired";
						transitionOutcome = "settle_expired";
					} else {
						const auto terminalWeaponState =
							State::NormalizeNativeFallbackWeaponState(
								static_cast<std::int32_t>(weaponState));
						const auto terminalRecovery =
							State::DecideWeaponTransitionTerminalRecovery(
								_weaponTransitionTargetDrawn,
								transitionActorLoaded,
								transitionIdentityMatches,
								terminalWeaponState,
								_weaponTransitionNativeFallbackRetryUsed);
						if (terminalRecovery.retryNativeFallback) {
							// A current actor still reports a transitional raw state.  Keep
							// the requested target and spend the one bounded retry rather
							// than converting kSheathing into drawn through IsWeaponDrawn().
							_weaponTransitionNativeFallbackRetryUsed = true;
							_weaponTransitionNativeFallbackArmed = true;
							_weaponTransitionProgressExtensionUsed = true;
							_weaponTransitionNativeFallbackAt = now + WeaponNativeFallbackRetryDelay;
							_weaponTransitionDeadline = now + GroundWeaponFallbackObservationTimeout;
							transitionTargetDrawn = terminalRecovery.targetDrawn;
							transitionOutcome = "transitional_retry_armed";
							logNativeFallbackDecision = true;
							nativeFallbackDecision = "retry_armed";
							nativeFallbackReason = "transitional_raw_state";
						} else {
							// Every terminal failure keeps the requested logical target.  A
							// stable opposite state is still a failed recovery, not evidence
							// that the request changed direction.
							const bool requestedTarget = terminalRecovery.targetDrawn;
							clearWeaponTransition();
							_weaponTransitionTerminalFailureHold =
								State::ReduceWeaponTransitionTerminalFailureHold(
									_weaponTransitionTerminalFailureHold,
									State::WeaponTransitionTerminalFailureHoldEvent::kTerminalFailure,
									requestedTarget,
									_flightSessionId,
									_weaponEquipmentIdentityEpoch)
									.state;
							_flightCombatActive = flightActive && requestedTarget;
							_weaponTransitionTargetDrawn = requestedTarget;
							transitionTargetDrawn = requestedTarget;
							transitionOutcome = terminalRecovery.terminalFailure ?
								"terminal_failure_target_preserved" : "terminal_target_preserved";
							logNativeFallbackDecision = true;
							nativeFallbackDecision = "terminal_failure";
							nativeFallbackReason =
								State::IsNativeFallbackWeaponStateTransitional(terminalWeaponState) ?
									"transitional_raw_state" : "stable_opposite_state";
						}
					}
				} else {
					_flightCombatActive = flightActive && _weaponTransitionTargetDrawn;
					if (_weaponTransitionNativeFallbackArmed) {
							const auto motion = GetWeaponFallbackMotion(weaponState, _weaponTransitionTargetDrawn);
						const bool stateUnchanged =
							static_cast<std::int32_t>(weaponState) == _weaponTransitionPreRequestState;
						auto fallbackDecision = State::DecideWeaponFallbackObservation(
							now >= _weaponTransitionNativeFallbackAt,
							stateUnchanged,
							motion,
							_weaponTransitionProgressExtensionUsed,
							false);
						const bool enoughTimeToRetry =
							now + WeaponNativeFallbackRetryDelay + WeaponNativeFallbackSafetyMargin <
							_weaponTransitionDeadline;
						if (fallbackDecision == State::WeaponFallbackDecision::kDeferForProgress &&
							enoughTimeToRetry) {
							_weaponTransitionProgressExtensionUsed = true;
							_weaponTransitionNativeFallbackAt = std::max(
								_weaponTransitionNativeFallbackAt,
								now + WeaponNativeFallbackRetryDelay);
							logNativeFallbackDecision = true;
							nativeFallbackDecision = "defer";
							nativeFallbackReason = "transition_toward_target";
						} else if (fallbackDecision == State::WeaponFallbackDecision::kFallback) {
							issueNativeWeaponFallback = true;
							nativeFallbackRequest = State::NativeFallbackGateRequest{
								_weaponTransitionSessionId,
								_weaponTransitionSequence,
								_readyGenerationBarrier.announced,
								_weaponTransitionIdentityEpoch,
								_weaponTransitionActionId };
							nativeFallbackIdentity = equipment.identity;
							nativeFallbackIdentityCaptured = transitionActorLoaded;
							nativeFallbackDecision = "issue";
							nativeFallbackReason = "unchanged_stalled_state";
						} else if (fallbackDecision == State::WeaponFallbackDecision::kAbort ||
							fallbackDecision == State::WeaponFallbackDecision::kDeferForProgress) {
							_weaponTransitionNativeFallbackArmed = false;
							_weaponTransitionNativeFallbackAt = {};
							logNativeFallbackDecision = true;
							nativeFallbackDecision = "abort";
							nativeFallbackReason = motion == State::WeaponFallbackMotion::kOppositeTarget ?
								"opposite_transition" :
								(motion == State::WeaponFallbackMotion::kTowardTarget ?
									(enoughTimeToRetry ? "transition_progress_extension_exhausted" :
										"transition_progress_no_safe_retry_window") :
									"actor_state_changed");
						}
					}
				}
			} else {
				const auto terminalFailureHoldDecision =
					State::ReduceWeaponTransitionTerminalFailureHold(
						_weaponTransitionTerminalFailureHold,
						State::WeaponTransitionTerminalFailureHoldEvent::kPassiveObservation,
						_weaponTransitionTerminalFailureHold.targetDrawn,
						_flightSessionId,
						_weaponEquipmentIdentityEpoch,
						_weaponTransitionTerminalFailureHold.active &&
							IsWeaponStateAtTarget(
								weaponState,
								_weaponTransitionTerminalFailureHold.targetDrawn));
				_weaponTransitionTerminalFailureHold = terminalFailureHoldDecision.state;
				if (terminalFailureHoldDecision.preserveTarget) {
					// A terminal failure is a passive hold: preserve DAF's requested
					// logical target and do not let ActorState::IsWeaponDrawn() rearm
					// combat or the native fallback on the next update.
					const bool heldTarget = _weaponTransitionTerminalFailureHold.targetDrawn;
					_flightCombatActive = flightActive && heldTarget;
					transitionTargetDrawn = heldTarget;
					transitionSequence = _weaponTransitionSequence;
					transitionOutcome = "terminal_failure_quarantine_hold";
				} else {
					const bool shouldReconcileCombatReady = State::ShouldReconcileFlightCombatReady(
						flightActive,
						_flightCombatActive,
						weaponsDrawn,
						_weaponEquipmentSwap.pending,
						equipmentSwapTimedOut,
						State::IsWeaponEquipmentSwapPinned(_weaponEquipmentSwap),
						State::IsWeaponEquipmentSwapReconciliationRequired(_weaponEquipmentSwap));
					if (shouldReconcileCombatReady) {
						_flightCombatActive = weaponsDrawn;
						transitionTargetDrawn = weaponsDrawn;
						transitionSequence = _weaponTransitionSequence;
						transitionOutcome = "actor_state_reconciled";
					} else if (flightActive && _flightCombatActive != weaponsDrawn) {
						if (_weaponEquipmentSwap.pending) {
							transitionTargetDrawn = _flightCombatActive;
							transitionSequence = _weaponTransitionSequence;
							transitionOutcome = equipmentSwapDetected || equipmentSwapEdgeObserved ?
								"equipment_swap_wait" : "equipment_swap_rebased";
						} else if (equipmentSwapTimedOut) {
							// If the actor was unavailable, leave only a diagnostic outcome; the
							// next loaded update will rebase and arm a current-identity retry.
							transitionTargetDrawn = _flightCombatActive;
							transitionSequence = _weaponTransitionSequence;
							transitionOutcome = "equipment_swap_timeout_observer_deferred";
						}
					}
				}
			}

			if (flightActive && _flightBlockRequested && !equipment.blockCapable) {
				blockAutoCleared = true;
			}

			descending = _isDescending;
			combatActive = _flightCombatActive;
			useGeneratedCombatTopology = _useGeneratedCombatTopology;
			session = _flightSessionId;
			flightSpeed = _flightSpeed;
			verticalSpeed = _verticalSpeed;
			liftScale = _liftScale;
			forwardInput = _forwardInput;
			strafeInput = _strafeInput;
			verticalInput = _verticalInput;
			launchBoost = _pendingLaunchBoost;
			boostHeld = _boostHeld;
			landingContactTicks = _landingContactTicks;
			_pendingLaunchBoost = 0.0F;
			shoutOverrideActive = now < _shoutGraphOverrideUntil;
			flightShoutHeld = _flightShoutHeld;
			whirlwindSprintActive = now < _whirlwindSprintUntil;

			const bool hasMovementInput = !descending && HasFlightControlInput(forwardInput, strafeInput, verticalInput);
			const bool hasLaunchBoost = launchBoost > 0.0F;
			graphState = descending ?
				FlightGraphState::kDescent :
				(hasLaunchBoost ?
				FlightGraphState::kLaunch :
				((hasMovementInput || shoutOverrideActive) ? FlightGraphState::kMoving : FlightGraphState::kIdle));

			const auto nextGraphState = static_cast<std::int32_t>(graphState);
			_lastGraphState = nextGraphState;
		}

		if (blockAutoCleared) {
			// Reconcile unsupported equipment through the same ownership-gated release
			// as an input edge.  UpdateFlight must never write shared block state on its
			// own, especially after an identity epoch has changed.
			blockAutoReleaseAccepted = SetFlightBlockRequested(false);
			logger::info(
				"event=block_reconcile session={} decision=auto_release accepted={} reason=equipment_not_block_capable",
				session,
				blockAutoReleaseAccepted);
		}

		if (!equipmentSwapPhase.empty()) {
			logger::info(
				"event=weapon_transition_equipment_swap session={} sequence={} phase={} "
				"pending_after={} edge_observed={} settled={} timeout={} readiness_preserved_after={} pinned_after={} "
				"target_drawn={} identity_captured={} "
				"previous_right_form=0x{:08X} previous_left_form=0x{:08X} previous_right_type={} "
				"previous_left_type={} previous_family={} current_right_form=0x{:08X} "
				"current_left_form=0x{:08X} current_right_type={} current_left_type={} current_family={} "
				"combat_ready_preserved={} engine_operation=passthrough",
				session,
				transitionSequence,
				equipmentSwapPhase,
				equipmentSwapPendingAfter,
				equipmentSwapEdgeAfter,
				equipmentSwapSettled,
				equipmentSwapTimedOut,
				equipmentSwapReadinessPreservedAfter,
				equipmentSwapPinnedAfter,
				transitionTargetDrawn,
				equipmentSwapIdentityCaptured,
				equipmentSwapPreviousIdentity.rightFormId,
				equipmentSwapPreviousIdentity.leftFormId,
				equipmentSwapPreviousIdentity.rightWeaponType,
				equipmentSwapPreviousIdentity.leftWeaponType,
				State::WeaponEquipmentFamilyName(equipmentSwapPreviousIdentity.family),
				equipmentSwapCurrentIdentity.rightFormId,
				equipmentSwapCurrentIdentity.leftFormId,
				equipmentSwapCurrentIdentity.rightWeaponType,
				equipmentSwapCurrentIdentity.leftWeaponType,
				State::WeaponEquipmentFamilyName(equipmentSwapCurrentIdentity.family),
				equipmentSwapReadinessPreservedAfter);
		}

		if (logNativeFallbackDecision) {
			logger::info(
				"event=weapon_transition_native_fallback session={} sequence={} decision={} reason={} "
				"target_drawn={} actual_drawn={} pre_weapon_state={} actual_weapon_state={} "
				"engine_method=none engine_result=not_issued post_flight={}",
				session,
				transitionSequence,
				nativeFallbackDecision,
				nativeFallbackReason,
				transitionTargetDrawn,
				weaponsDrawn,
				transitionPreRequestState,
				static_cast<std::int32_t>(weaponState),
				postFlightTransition);
		}

		if (issueNativeWeaponFallback) {
			const bool issued = TryExecuteNativeWeaponFallback(
				player,
				false,
				transitionTargetDrawn,
				transitionActorFormId,
				nativeFallbackIdentity,
				nativeFallbackIdentityCaptured,
				nativeFallbackRequest);
			if (!issued) {
				return;
			}
			LogWeaponRoutingDiagnostic(
				player,
				"native_fallback_decision",
				"unchanged_actor_state_stalled",
				true,
				transitionTargetDrawn);
			logger::info(
				"event=weapon_transition_native_fallback session={} sequence={} decision={} reason={} target_drawn={} "
				"actual_drawn={} pre_weapon_state={} weapon_state={} post_flight={} engine_method=DrawWeaponMagicHands "
				"engine_result=issued expected_oar_family={} quarterstaff={} quarterstaff_keyword={}",
				session,
				transitionSequence,
				nativeFallbackDecision,
				nativeFallbackReason,
				transitionTargetDrawn,
				weaponsDrawn,
				transitionPreRequestState,
				static_cast<std::int32_t>(weaponState),
				postFlightTransition,
				equipment.expectedOarFamily,
				equipment.quarterstaffEquipped,
				equipment.quarterstaffKeyword);
		}

		if (!transitionOutcome.empty()) {
			logger::info(
				"event=weapon_transition_result session={} sequence={} outcome={} target_drawn={} actual_drawn={} weapon_state={} "
				"post_flight={} expected_oar_family={} quarterstaff={} quarterstaff_keyword={}",
				session,
				transitionSequence,
				transitionOutcome,
				transitionTargetDrawn,
				weaponsDrawn,
				static_cast<std::int32_t>(weaponState),
				postFlightTransition,
				equipment.expectedOarFamily,
				equipment.quarterstaffEquipped,
				equipment.quarterstaffKeyword);
		}

		// Probe the process-owned regen delay while airborne as well.  This
		// captures delayed DamageActorValue publication for diagnostics only;
		// no regen-delay write is permitted in flight or post-flight.
		if (flightActive) {
			ObserveDafMagickaRegen(player, now, false);
		}

		if (!flightActive) {
			// Flight graph/controller cleanup is complete.  Keep this tiny pump
			// alive to observe actor-owned weapon and regen transitions; never
			// re-arm DAF combat, shout, block, physics, or magicka state here.
			ObserveDafMagickaRegen(player, now, true);
			ObserveGroundWeaponTransition(player, now);
			LogDiagnosticSnapshot(player);
			if (!transitionOutcome.empty() || issueNativeWeaponFallback || regenObservationPending ||
				groundWeaponObservationPending) {
				bool regenStillPending = false;
				bool groundStillPending = false;
				bool equipmentSwapStillPending = false;
				{
					std::shared_lock lock(_mutex);
					regenStillPending = _magickaRegenObservationPending;
					groundStillPending = _groundWeaponObservationPending;
					equipmentSwapStillPending = _weaponEquipmentSwap.pending;
				}
				logger::info(
					"event=post_flight_observer session={} transition_pending={} regen_observation_pending={} "
					"ground_weapon_observation_pending={} equipment_swap_pending={} "
					"target_drawn={} actual_drawn={} weapon_state={} combat_active=false graph_active=false",
					session,
					postFlightTransition,
					regenStillPending,
					groundStillPending,
					equipmentSwapStillPending,
					transitionTargetDrawn,
					weaponsDrawn,
					static_cast<std::int32_t>(weaponState));
			}
			bool transitionStillPending = false;
			bool groundStillPending = false;
			bool equipmentSwapStillPending = false;
			{
				std::shared_lock lock(_mutex);
				transitionStillPending = _weaponTransitionPending;
				regenObservationPending = _magickaRegenObservationPending;
				groundStillPending = _groundWeaponObservationPending;
				equipmentSwapStillPending = _weaponEquipmentSwap.pending;
			}
			if (!transitionStillPending && !regenObservationPending && !groundStillPending &&
				!equipmentSwapStillPending) {
				// Terminal observer paths normally consume the aggregate themselves;
				// this final guard makes pump shutdown lossless if a lifecycle edge
				// raced the observer and left a diagnostic record buffered.
				FlushMagickaRegenDiagnostic("pump_stop", true);
				{
					std::unique_lock lock(_mutex);
					if (_weaponEquipmentSwap.pending) {
						_weaponEquipmentSwap = State::ResetWeaponEquipmentSwap();
						_weaponEquipmentSwapPreviousIdentity = {};
						_weaponEquipmentSwapCurrentIdentity = {};
						_weaponEquipmentSwapIdentityCaptured = false;
						_weaponEquipmentSwapPinnedDiagnosticLogged = false;
					}
					_magickaRegenDiagnosticPipeline.PumpStopped();
				}
				StopUpdateThread();
			}
			return;
		}

		if (!SetFlightGraphVariables(
			player,
			true,
			true,
			combatActive,
			useGeneratedCombatTopology,
			graphState == FlightGraphState::kLaunch,
			flightShoutHeld,
			graphState)) {
			// The two OAR gate variables are essential to the presentation contract.
			// Once either write is unavailable, continuing to move the controller
			// would produce invisible/grounded animation with no owner to repair it.
			logger::error(
				"event=flight_abort reason=graph_gate_lost worker_cleanup=true "
				"physics_write=false weapon_fallback=false graph_write_partial=true");
			StopFlight();
			return;
		}

		// Shared native block state is never written by DAF.  UpdateFlight only
		// reports the engine's values; it must not reconcile wantBlocking/IsBlocking
		// every frame or erase vanilla/MCO ownership.
		LogDiagnosticSnapshot(player);

		if (whirlwindSprintActive && PreserveWhirlwindSprintVelocity(player, _smoothedFlightVelocity)) {
			return;
		}

		if (descending) {
			if (MovePlayerWithControlledDescent(landingContactTicks)) {
				StopFlight();
			} else {
				std::unique_lock lock(_mutex);
				if (_isFlying && _isDescending) {
					_landingContactTicks = landingContactTicks;
				}
			}
			return;
		}

		if (DrainFlightMagicka(player, now)) {
			logger::info("Dragon Aspect Flight: magicka depleted, beginning controlled descent");
			RE::SendHUDMessage::ShowHUDMessage("Dragon Aspect Flight: magicka exhausted, descending");
			BeginDescent();
			return;
		}

		MovePlayerWithCharacterControllerVelocity(flightSpeed, verticalSpeed, liftScale, forwardInput, strafeInput, verticalInput, launchBoost, boostHeld, _smoothedFlightVelocity);
	}
}
