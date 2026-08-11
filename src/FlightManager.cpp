#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
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

namespace
{
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
	constexpr const char* GraphVarDragonAspectActive = "bDAF_DragonAspectActive";
	constexpr const char* GraphVarFlightActive = "bDAF_FlightActive";
	constexpr const char* GraphVarFlightCombatActive = "bDAF_FlightCombatActive";
	constexpr const char* GraphVarLaunchBoost = "bDAF_LaunchBoost";
	constexpr const char* GraphVarFlightShout = "bDAF_FlightShout";
	constexpr const char* GraphVarFlightState = "iDAF_FlightState";
	constexpr const char* GraphVarVanillaInJumpState = "bInJumpState";
	constexpr const char* GraphVarVanillaIsBlocking = "IsBlocking";
	constexpr const char* QuarterstaffKeyword = "WeapTypeQtrStaff";
	constexpr const char* BlockStartEvent = "blockStart";
	constexpr const char* BlockStopEvent = "blockStop";
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
		bool quarterstaffEquipped{ false };
		bool blockCapable{ false };
		std::uint64_t signature{ 0 };
	};

	std::int32_t GetWeaponType(RE::TESForm* a_form)
	{
		if (const auto* weapon = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr) {
			return static_cast<std::int32_t>(weapon->GetWeaponType());
		}
		return -1;
	}

	bool IsOneHandedWeaponType(std::int32_t a_type)
	{
		return a_type >= static_cast<std::int32_t>(RE::WEAPON_TYPE::kOneHandSword) &&
			a_type <= static_cast<std::int32_t>(RE::WEAPON_TYPE::kOneHandMace);
	}

	bool IsEquippedMagic(RE::TESForm* a_form)
	{
		return a_form && !a_form->As<RE::TESObjectWEAP>() && a_form->As<RE::MagicItem>();
	}

	bool IsQuarterstaff(RE::TESForm* a_form)
	{
		const auto* weapon = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr;
		return weapon && weapon->HasKeywordString(QuarterstaffKeyword);
	}

	bool IsShield(RE::TESForm* a_form)
	{
		const auto* armor = a_form ? a_form->As<RE::TESObjectARMO>() : nullptr;
		return armor && armor->IsShield();
	}

	bool IsTwoHandedWeaponType(std::int32_t a_type)
	{
		return a_type == static_cast<std::int32_t>(RE::WEAPON_TYPE::kTwoHandSword) ||
			a_type == static_cast<std::int32_t>(RE::WEAPON_TYPE::kTwoHandAxe);
	}

	bool IsBlockCapableEquipment(const EquipmentDiagnostic& a_equipment)
	{
		if (IsShield(a_equipment.left) || IsShield(a_equipment.right) ||
			IsTwoHandedWeaponType(a_equipment.rightWeaponType) ||
			IsTwoHandedWeaponType(a_equipment.leftWeaponType)) {
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
		result.quarterstaffEquipped = IsQuarterstaff(result.right) || IsQuarterstaff(result.left);

		if (result.quarterstaffEquipped) {
			result.expectedOarFamily = "quarterstaff";
		} else if (IsEquippedMagic(result.right) || IsEquippedMagic(result.left)) {
			result.expectedOarFamily = "magic";
		} else if (result.rightWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kBow) ||
			result.leftWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kBow)) {
			result.expectedOarFamily = "bow";
		} else if (result.rightWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kCrossbow) ||
			result.leftWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kCrossbow)) {
			result.expectedOarFamily = "crossbow";
		} else if (result.rightWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kStaff) ||
			result.leftWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kStaff)) {
			result.expectedOarFamily = "staff";
		} else if (result.rightWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kTwoHandSword) ||
			result.rightWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kTwoHandAxe) ||
			result.leftWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kTwoHandSword) ||
			result.leftWeaponType == static_cast<std::int32_t>(RE::WEAPON_TYPE::kTwoHandAxe)) {
			result.expectedOarFamily = "two_handed";
		} else if (IsOneHandedWeaponType(result.rightWeaponType) && IsOneHandedWeaponType(result.leftWeaponType)) {
			result.expectedOarFamily = "dual_wield";
		} else if (IsOneHandedWeaponType(result.rightWeaponType) || IsOneHandedWeaponType(result.leftWeaponType)) {
			result.expectedOarFamily = "one_handed";
		}
		result.blockCapable = IsBlockCapableEquipment(result);

		const auto rightFormID = result.right ? result.right->GetFormID() : 0;
		const auto leftFormID = result.left ? result.left->GetFormID() : 0;
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

	// Drain magicka while flying. Returns true if magicka is depleted (caller
	// should trigger controlled descent). Reads Settings under shared lock.
	bool DrainFlightMagicka(RE::PlayerCharacter* a_player)
	{
		bool enabled = false;
		float costPerSecond = 0.0F;
		{
			std::shared_lock lock(DragonAspectFlight::Settings::GetSingleton().mutex);
			enabled = DragonAspectFlight::Settings::GetSingleton().magickaCostEnabled;
			costPerSecond = DragonAspectFlight::Settings::GetSingleton().magickaCostPerSecond;
		}

		if (!enabled || costPerSecond <= 0.0F || !a_player) {
			return false;
		}

		auto* actorValueOwner = a_player->AsActorValueOwner();
		if (!actorValueOwner) return false;

		const float currentMagicka = actorValueOwner->GetActorValue(RE::ActorValue::kMagicka);
		const float cost = costPerSecond * TickSeconds;

		if (currentMagicka <= cost) {
			return true;
		}

		actorValueOwner->DamageActorValue(RE::ActorValue::kMagicka, cost);
		return false;
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

	void SetFlightGraphVariables(
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
			return;
		}

		bool customVariablesWritten = true;
		customVariablesWritten &= a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarDragonAspectActive), a_dragonAspectActive);
		customVariablesWritten &= a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarFlightActive), a_flightActive);
		customVariablesWritten &= a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarFlightCombatActive), a_flightCombatActive);
		customVariablesWritten &= a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarLaunchBoost), a_launchBoost);
		customVariablesWritten &= a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarFlightShout), a_flightShout);
		customVariablesWritten &= a_player->SetGraphVariableInt(
			RE::BSFixedString(GraphVarFlightState), static_cast<std::int32_t>(a_state));

		if (!customVariablesWritten && !GraphVariableWriteFailureLogged.exchange(true)) {
			logger::warn(
				"Dragon Aspect Flight: one or more DAF graph variables were unavailable; "
				"verify Behavior Data Injector and DragonAspectFlight_BDI.json");
		}

		// Keep the normal combat graph active while the controller remains airborne.
		// Jumping Attack's branch cannot reliably transition to block, bash, shout,
		// draw, or sheathe; DAF owns the corresponding visuals through OAR instead.
		(void)a_useGeneratedCombatTopology;
		a_player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarVanillaInJumpState),
			false);
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

	void FlightManager::StartFlight()
	{
		if (!HasDragonAspectActive()) {
			logger::info("Dragon Aspect not active; flight cancelled");
			return;
		}

		auto* player = GetPlayer();
		if (player && player->IsOnMount()) {
			logger::info("Dragon Aspect Flight: flight start refused while mounted");
			return;
		}

		const auto* actorState = player ? player->AsActorState() : nullptr;
		const bool startWithWeaponsDrawn = actorState && actorState->IsWeaponDrawn();

		{
			std::unique_lock lock(_mutex);

			if (_isFlying) {
				return;
			}

			_isFlying = true;
			_isDescending = false;
			_flightCombatActive = false;
			_flightBlockRequested = false;
			_weaponTransitionPending = false;
			_weaponTransitionTargetDrawn = startWithWeaponsDrawn;
			_weaponTransitionNativeFallbackArmed = false;
			_weaponTransitionDeadline = {};
			_weaponTransitionNativeFallbackAt = {};
			_useGeneratedCombatTopology = false;
			_aerialCombatUnsupportedNotified = false;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kIdle);
			_landingContactTicks = 0;
			_shoutGraphOverrideUntil = {};
			_whirlwindSprintUntil = {};
			_whirlwindSprintShoutPending = false;
			_smoothedFlightVelocity = RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F };
			++_flightSessionId;
			_lastDiagnosticSnapshot = {};
			_lastDiagnosticEquipmentSignature = ~std::uint64_t{ 0 };
			_lastDiagnosticStateSignature = ~std::uint64_t{ 0 };
			logger::info(
				"event=flight_start session={} version={} weapons_drawn={} dragon_aspect_active=true",
				_flightSessionId,
				BuildVersion,
				startWithWeaponsDrawn);
		}

		// Keep the physics state airborne for OAR without firing sprint/jump
		// animation graph events that can collide with Better Jumping.
		if (player && player->Is3DLoaded()) {
			if (auto* controller = player->GetCharController()) {
				_originalGravity = controller->gravity;
				ApplyControlledAirState(player, controller);
			}
			SetFlightGraphVariables(player, true, true, false, false, false, false, FlightGraphState::kIdle);
			player->SetGraphVariableBool(RE::BSFixedString(GraphVarVanillaInJumpState), false);

			if (startWithWeaponsDrawn && !SetFlightCombatActive(true)) {
				logger::warn(
					"Dragon Aspect Flight: flight began with equipment drawn, but combat state initialization failed");
				StopFlight();
				return;
			}
		}

		StartUpdateThread();
	}

	void FlightManager::BeginDescent()
	{
		bool combatActive = false;
		bool useGeneratedCombatTopology = false;

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
			_shoutGraphOverrideUntil = {};
			_whirlwindSprintUntil = {};
			_whirlwindSprintShoutPending = false;
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
				false,
				FlightGraphState::kDescent);
		}

		StartUpdateThread();
	}

	void FlightManager::CancelDescent()
	{
		bool combatActive = false;
		bool useGeneratedCombatTopology = false;
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
			_shoutGraphOverrideUntil = {};
			_whirlwindSprintUntil = {};
			_whirlwindSprintShoutPending = false;
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
				false,
				FlightGraphState::kIdle);
		}

		StartUpdateThread();
	}

	void FlightManager::StopFlight()
	{
		SetFlightBlockRequested(false);
		SetFlightCombatActive(false);

		{
			std::unique_lock lock(_mutex);

			if (!_isFlying) {
				return;
			}

			_isFlying = false;
			_isDescending = false;
			_flightCombatActive = false;
			_flightBlockRequested = false;
			_weaponTransitionPending = false;
			_weaponTransitionTargetDrawn = false;
			_weaponTransitionNativeFallbackArmed = false;
			_weaponTransitionDeadline = {};
			_weaponTransitionNativeFallbackAt = {};
			_useGeneratedCombatTopology = false;
			_forwardInput = 0.0F;
			_strafeInput = 0.0F;
			_verticalInput = 0.0F;
			_pendingLaunchBoost = 0.0F;
			_boostHeld = false;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kOff);
			_landingContactTicks = 0;
			_shoutGraphOverrideUntil = {};
			_whirlwindSprintUntil = {};
			_whirlwindSprintShoutPending = false;
			_smoothedFlightVelocity = RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F };
			logger::info(
				"event=flight_stop session={} version={} reason=controller_stop",
				_flightSessionId,
				BuildVersion);
		}

		ClampStopVelocityForSafeRelease(GetPlayer());

		// Restore gravity, friction, and ground state.
		if (auto* player = GetPlayer(); player && player->Is3DLoaded()) {
			SetFlightGraphVariables(
				player,
				HasDragonAspectActive(),
				false,
				false,
				false,
				false,
				false,
				FlightGraphState::kOff);
			if (auto* controller = player->GetCharController()) {
				controller->gravity = _originalGravity;
				controller->flags.reset(RE::CHARACTER_FLAGS::kNoFriction);
				controller->wantState = RE::hkpCharacterStateType::kOnGround;
				controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
			}
		}

		StopUpdateThread();
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

	bool FlightManager::SetFlightCombatActive(bool a_active)
	{
		auto* player = GetPlayer();
		if (!player || !player->Is3DLoaded()) {
			return false;
		}

		const auto* actorState = player->AsActorState();
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
		bool transitionPending = false;
		bool transitionTargetDrawn = false;
		std::uint64_t transitionSequence = 0;
		{
			std::unique_lock lock(_mutex);
			if (!_isFlying || (a_active && !HasDragonAspectActive())) {
				return false;
			}
			_flightCombatActive = a_active;
			_useGeneratedCombatTopology = false;
			if (IsWeaponStateAtTarget(weaponState, a_active)) {
				// The requested state is already authoritative. Cancel any stale
				// transition, including one that was targeting the opposite state.
				_weaponTransitionPending = false;
				_weaponTransitionTargetDrawn = a_active;
				_weaponTransitionNativeFallbackArmed = false;
				_weaponTransitionDeadline = {};
				_weaponTransitionNativeFallbackAt = {};
			} else {
				if (!_weaponTransitionPending || _weaponTransitionTargetDrawn != a_active) {
					++_weaponTransitionSequence;
					_weaponTransitionPending = true;
					_weaponTransitionTargetDrawn = a_active;
					_weaponTransitionNativeFallbackArmed = false;
					_weaponTransitionDeadline = std::chrono::steady_clock::now() + WeaponTransitionTimeout;
					_weaponTransitionNativeFallbackAt = {};
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
		player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarVanillaInJumpState),
			false);
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
			const auto equipment = GetEquipmentDiagnostic(player);
			logger::info(
				"event=combat_state session={} active={} graph_write_ok={} weapons_drawn={} weapon_state={} "
				"transition_pending={} transition_target_drawn={} transition_sequence={} "
				"expected_oar_family={} quarterstaff={} block_capable={}",
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
				equipment.blockCapable);
		}

		return true;
	}

	bool FlightManager::ToggleFlightCombatReady()
	{
		auto* player = GetPlayer();
		if (!player || !player->Is3DLoaded()) {
			return false;
		}

		const auto* actorState = player->AsActorState();
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;
		bool referenceDrawn = WeaponStateIntendsDrawn(weaponState);
		{
			std::shared_lock lock(_mutex);
			if (!_isFlying) {
				return false;
			}
			if (_weaponTransitionPending) {
				referenceDrawn = _weaponTransitionTargetDrawn;
			}
		}

		const bool nextCombatActive = !referenceDrawn;
		if (!nextCombatActive) {
			SetFlightBlockRequested(false);
		}
		if (!SetFlightCombatActive(nextCombatActive)) {
			return false;
		}

		std::uint64_t session = 0;
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
			"event=weapon_transition_request session={} sequence={} actual_drawn={} weapon_state={} reference_drawn={} "
			"target_drawn={} native_fallback_delay_ms={} timeout_ms={}",
			session,
			sequence,
			weaponsDrawn,
			static_cast<std::int32_t>(weaponState),
			referenceDrawn,
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

	bool FlightManager::SetFlightBlockRequested(bool a_requested)
	{
		auto* player = GetPlayer();
		if (a_requested && (!player || !player->Is3DLoaded())) {
			return false;
		}

		const auto equipment = GetEquipmentDiagnostic(player);
		const bool supported = !a_requested || equipment.blockCapable;
		bool effectiveRequest = a_requested && supported;
		bool requestChanged = false;
		bool detailedLogging = false;
		std::uint64_t session = 0;
		{
			std::unique_lock lock(_mutex);
			if (a_requested && (!_isFlying || !HasDragonAspectActive())) {
				return false;
			}
			requestChanged = _flightBlockRequested != effectiveRequest;
			_flightBlockRequested = effectiveRequest;
			detailedLogging = _detailedLogging;
			session = _flightSessionId;
		}

		if (!player || !player->Is3DLoaded()) {
			return !a_requested;
		}

		auto* actorState = player->AsActorState();
		const bool previousWantBlocking = actorState && actorState->actorState2.wantBlocking;
		const bool previousBlocking = player->IsBlocking();
		if (actorState) {
			actorState->actorState2.wantBlocking = effectiveRequest;
		}
		const bool graphWriteOk = player->SetGraphVariableBool(
			RE::BSFixedString(GraphVarVanillaIsBlocking), effectiveRequest);
		bool graphEventOk = true;
		if (requestChanged || previousWantBlocking != effectiveRequest || previousBlocking != effectiveRequest) {
			graphEventOk = player->NotifyAnimationGraph(RE::BSFixedString(
				effectiveRequest ? BlockStartEvent : BlockStopEvent));
		}

		if (effectiveRequest) {
			SetFlightCombatActive(true);
		}

		if (detailedLogging || !supported) {
			logger::info(
				"event=block_state session={} requested={} supported={} effective={} changed={} "
				"previous_want_blocking={} previous_is_blocking={} graph_write_ok={} graph_event_ok={} "
				"expected_oar_family={} quarterstaff={} right_weapon_type={} left_weapon_type={}",
				session,
				a_requested,
				supported,
				effectiveRequest,
				requestChanged,
				previousWantBlocking,
				previousBlocking,
				graphWriteOk,
				graphEventOk,
				equipment.expectedOarFamily,
				equipment.quarterstaffEquipped,
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
		bool releaseWhirlwindSprint = false;
		bool combatActive = false;
		bool useGeneratedCombatTopology = false;
		bool descending = false;
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
			_shoutGraphOverrideUntil = now + ShoutGraphOverrideDuration;
			if (!a_released) {
				_whirlwindSprintShoutPending =
					_whirlwindSprintShoutPending || currentShoutIsWhirlwindSprint;
			} else {
				releaseWhirlwindSprint =
					_whirlwindSprintShoutPending || currentShoutIsWhirlwindSprint;
				_whirlwindSprintShoutPending = false;
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
				true,
				graphState);
			logger::info(
				"event=shout_state session={} released={} descending={} whirlwind_selected={} "
				"whirlwind_release_window={} graph_state={}",
				session,
				a_released,
				descending,
				currentShoutIsWhirlwindSprint,
				releaseWhirlwindSprint,
				static_cast<std::int32_t>(graphState));
			if (releaseWhirlwindSprint) {
				logger::info(
					"Dragon Aspect Flight: yielding controller velocity to Whirlwind Sprint for {} ms",
					std::chrono::duration_cast<std::chrono::milliseconds>(WhirlwindSprintControlWindow).count());
			}
		}
	}

	void FlightManager::SetBoostHeld(bool a_boostHeld)
	{
		std::unique_lock lock(_mutex);
		_boostHeld = _isDescending ? false : a_boostHeld;
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

	void FlightManager::LogInputDiagnostic(
		std::string_view a_action,
		const RE::ButtonEvent* a_event,
		std::string_view a_outcome) const
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
		{
			std::shared_lock lock(_mutex);
			enabled = _detailedLogging;
			session = _flightSessionId;
			flying = _isFlying;
			descending = _isDescending;
			combatActive = _flightCombatActive;
			blockRequested = _flightBlockRequested;
			transitionPending = _weaponTransitionPending;
			transitionTargetDrawn = _weaponTransitionTargetDrawn;
			transitionNativeFallbackArmed = _weaponTransitionNativeFallbackArmed;
		}
		if (!enabled || !a_event) {
			return;
		}

		const char* phase = a_event->IsUp() ? "up" :
			(a_event->IsDown() ? "down" :
				(a_event->IsHeld() ? "held" :
					(a_event->IsPressed() ? "pressed" : "other")));
		const auto userEvent = a_event->QUserEvent();
		logger::info(
			"event=input session={} action={} user_event=\"{}\" device={} code=0x{:X} phase={} "
			"flying={} descending={} combat_active={} block_requested={} "
			"weapon_transition_pending={} weapon_transition_target_drawn={} "
			"weapon_transition_native_fallback_armed={} outcome={}",
			session,
			a_action,
			userEvent.c_str(),
			static_cast<std::uint32_t>(a_event->GetDevice()),
			a_event->GetIDCode(),
			phase,
			flying,
			descending,
			combatActive,
			blockRequested,
			transitionPending,
			transitionTargetDrawn,
			transitionNativeFallbackArmed,
			a_outcome);
	}

	void FlightManager::LogDiagnosticSnapshot(RE::PlayerCharacter* a_player)
	{
		if (!a_player || !a_player->Is3DLoaded()) {
			return;
		}

		const auto equipment = GetEquipmentDiagnostic(a_player);
		const auto now = std::chrono::steady_clock::now();
		std::uint64_t session = 0;
		std::uint64_t stateSignature = 0;
		float intervalSeconds = 2.0F;
		float forwardInput = 0.0F;
		float strafeInput = 0.0F;
		float verticalInput = 0.0F;
		bool descending = false;
		bool combatActive = false;
		bool blockRequested = false;
		bool weaponTransitionPending = false;
		bool weaponTransitionTargetDrawn = false;
		bool weaponTransitionNativeFallbackArmed = false;
		std::uint64_t weaponTransitionSequence = 0;
		bool boostHeld = false;
		std::int32_t requestedGraphState = 0;
		const char* reason = "heartbeat";
		{
			std::unique_lock lock(_mutex);
			if (!_detailedLogging || !_isFlying) {
				return;
			}

			intervalSeconds = _diagnosticSnapshotIntervalSeconds;
			stateSignature = static_cast<std::uint64_t>(static_cast<std::uint32_t>(_lastGraphState));
			stateSignature |= _isDescending ? (std::uint64_t{ 1 } << 32U) : 0;
			stateSignature |= _flightCombatActive ? (std::uint64_t{ 1 } << 33U) : 0;
			stateSignature |= _boostHeld ? (std::uint64_t{ 1 } << 34U) : 0;
			stateSignature |= _flightBlockRequested ? (std::uint64_t{ 1 } << 35U) : 0;
			stateSignature |= _weaponTransitionPending ? (std::uint64_t{ 1 } << 36U) : 0;
			stateSignature |= _weaponTransitionTargetDrawn ? (std::uint64_t{ 1 } << 37U) : 0;
			stateSignature |= _weaponTransitionNativeFallbackArmed ? (std::uint64_t{ 1 } << 38U) : 0;
			const bool equipmentChanged = equipment.signature != _lastDiagnosticEquipmentSignature;
			const bool stateChanged = stateSignature != _lastDiagnosticStateSignature;
			const bool heartbeatDue = _lastDiagnosticSnapshot.time_since_epoch().count() == 0 ||
				std::chrono::duration<float>(now - _lastDiagnosticSnapshot).count() >= intervalSeconds;
			if (!equipmentChanged && !stateChanged && !heartbeatDue) {
				return;
			}

			reason = equipmentChanged ? "equipment_change" : (stateChanged ? "state_change" : "heartbeat");
			_lastDiagnosticEquipmentSignature = equipment.signature;
			_lastDiagnosticStateSignature = stateSignature;
			_lastDiagnosticSnapshot = now;
			session = _flightSessionId;
			forwardInput = _forwardInput;
			strafeInput = _strafeInput;
			verticalInput = _verticalInput;
			descending = _isDescending;
			combatActive = _flightCombatActive;
			blockRequested = _flightBlockRequested;
			weaponTransitionPending = _weaponTransitionPending;
			weaponTransitionTargetDrawn = _weaponTransitionTargetDrawn;
			weaponTransitionNativeFallbackArmed = _weaponTransitionNativeFallbackArmed;
			weaponTransitionSequence = _weaponTransitionSequence;
			boostHeld = _boostHeld;
			requestedGraphState = _lastGraphState;
		}

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
		if (const auto* avOwner = a_player->AsActorValueOwner()) {
			magicka = avOwner->GetActorValue(RE::ActorValue::kMagicka);
			stamina = avOwner->GetActorValue(RE::ActorValue::kStamina);
		}

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
			"event=state_snapshot session={} reason={} version={} expected_oar_family={} quarterstaff={} block_capable={} "
			"weapons_drawn={} right_form=0x{:08X} right_form_type={} right_weapon_type={} right_name=\"{}\" "
			"left_form=0x{:08X} left_form_type={} left_weapon_type={} left_name=\"{}\" "
			"descending={} combat_active={} block_requested={} want_blocking={} is_blocking={} "
			"weapon_state={} attack_state={} weapon_transition_pending={} weapon_transition_target_drawn={} "
			"weapon_transition_native_fallback_armed={} "
			"weapon_transition_sequence={} boost_held={} input_fwd={:.3f} input_strafe={:.3f} input_vertical={:.3f} "
			"requested_graph_state={} graph_read_mask=0x{:02X} graph_da={} graph_flight={} graph_combat={} "
			"graph_launch={} graph_shout={} graph_state={} graph_in_jump={} graph_blocking={} "
			"controller_current={} controller_want={} gravity={:.3f} velocity=({:.3f},{:.3f},{:.3f}) "
			"position=({:.3f},{:.3f},{:.3f}) magicka={:.3f} stamina={:.3f}",
			session,
			reason,
			BuildVersion,
			equipment.expectedOarFamily,
			equipment.quarterstaffEquipped,
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
			stamina);
	}

	void FlightManager::StartUpdateThread()
	{
		if (_threadRunning.exchange(true)) {
			return;
		}

		_updateThread = std::jthread([this](std::stop_token a_stopToken) {
			logger::info("Flight update thread started");

			while (!a_stopToken.stop_requested() && _threadRunning.load()) {
				if (!IsFlying()) {
					break;
				}

				QueueUpdate();

				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}

			logger::info("Flight update thread stopped");
		});
	}

	void FlightManager::StopUpdateThread()
	{
		if (!_threadRunning.exchange(false)) {
			return;
		}

		if (_updateThread.joinable()) {
			try {
				_updateThread.request_stop();
				_updateThread.join();
			} catch (const std::system_error& e) {
				logger::error("Flight update thread stop failed: {}", e.what());

				if (_updateThread.joinable()) {
					try {
						_updateThread.detach();
					} catch (const std::system_error& detachError) {
						logger::error("Flight update thread detach failed: {}", detachError.what());
					}
				}
			}
		}
	}

	void FlightManager::QueueUpdate()
	{
		auto taskInterface = SKSE::GetTaskInterface();

		if (!taskInterface) {
			return;
		}

		taskInterface->AddTask([this]() {
			UpdateFlight();
		});
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
		bool blockRequested = false;
		bool blockAutoCleared = false;
		bool useGeneratedCombatTopology = false;
		bool shoutOverrideActive = false;
		bool whirlwindSprintActive = false;
		bool transitionTargetDrawn = false;
		bool issueNativeWeaponFallback = false;
		std::uint64_t transitionSequence = 0;
		std::uint64_t session = 0;
		std::string_view transitionOutcome;
		std::uint32_t landingContactTicks = 0;
		FlightGraphState graphState = FlightGraphState::kOff;
		const auto now = std::chrono::steady_clock::now();
		auto* player = GetPlayer();
		const auto equipment = GetEquipmentDiagnostic(player);
		const auto* actorState = player ? player->AsActorState() : nullptr;
		const bool weaponsDrawn = actorState && actorState->IsWeaponDrawn();
		const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kSheathed;

		{
			std::unique_lock lock(_mutex);

			if (!_isFlying) {
				return;
			}

			if (!HasDragonAspectActive()) {
				lock.unlock();
				StopFlight();
				return;
			}

			if (_weaponTransitionPending) {
				transitionTargetDrawn = _weaponTransitionTargetDrawn;
				transitionSequence = _weaponTransitionSequence;
				if (IsWeaponStateAtTarget(weaponState, _weaponTransitionTargetDrawn)) {
					_weaponTransitionPending = false;
					_weaponTransitionNativeFallbackArmed = false;
					_weaponTransitionDeadline = {};
					_weaponTransitionNativeFallbackAt = {};
					_flightCombatActive = weaponsDrawn;
					transitionOutcome = "completed";
				} else if (now >= _weaponTransitionDeadline) {
					_weaponTransitionPending = false;
					_weaponTransitionNativeFallbackArmed = false;
					_weaponTransitionDeadline = {};
					_weaponTransitionNativeFallbackAt = {};
					_flightCombatActive = weaponsDrawn;
					transitionOutcome = "timed_out_reconciled_to_actor";
				} else {
					_flightCombatActive = _weaponTransitionTargetDrawn;
					if (_weaponTransitionNativeFallbackArmed && now >= _weaponTransitionNativeFallbackAt) {
						const bool transitionInProgress =
							IsWeaponTransitionInProgress(weaponState, _weaponTransitionTargetDrawn);
						const bool enoughTimeToRetry =
							now + WeaponNativeFallbackRetryDelay + WeaponNativeFallbackSafetyMargin <
							_weaponTransitionDeadline;
						if (transitionInProgress && enoughTimeToRetry) {
							_weaponTransitionNativeFallbackAt = now + WeaponNativeFallbackRetryDelay;
						} else {
							_weaponTransitionNativeFallbackArmed = false;
							_weaponTransitionNativeFallbackAt = {};
							issueNativeWeaponFallback = true;
						}
					}
				}
			} else if (_flightCombatActive != weaponsDrawn) {
				_flightCombatActive = weaponsDrawn;
				transitionTargetDrawn = weaponsDrawn;
				transitionSequence = _weaponTransitionSequence;
				transitionOutcome = "actor_state_reconciled";
			}

			if (_flightBlockRequested && !equipment.blockCapable) {
				_flightBlockRequested = false;
				blockAutoCleared = true;
			}

			descending = _isDescending;
			combatActive = _flightCombatActive;
			blockRequested = _flightBlockRequested;
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

		if (issueNativeWeaponFallback && player && player->Is3DLoaded()) {
			player->DrawWeaponMagicHands(transitionTargetDrawn);
			logger::info(
				"event=weapon_transition_native_fallback session={} sequence={} target_drawn={} "
				"actual_drawn={} weapon_state={} expected_oar_family={} quarterstaff={}",
				session,
				transitionSequence,
				transitionTargetDrawn,
				weaponsDrawn,
				static_cast<std::int32_t>(weaponState),
				equipment.expectedOarFamily,
				equipment.quarterstaffEquipped);
		}

		if (!transitionOutcome.empty()) {
			logger::info(
				"event=weapon_transition_result session={} sequence={} outcome={} target_drawn={} actual_drawn={} weapon_state={} "
				"expected_oar_family={} quarterstaff={}",
				session,
				transitionSequence,
				transitionOutcome,
				transitionTargetDrawn,
				weaponsDrawn,
				static_cast<std::int32_t>(weaponState),
				equipment.expectedOarFamily,
				equipment.quarterstaffEquipped);
		}

		SetFlightGraphVariables(
			player,
			true,
			true,
			combatActive,
			useGeneratedCombatTopology,
			graphState == FlightGraphState::kLaunch,
			shoutOverrideActive,
			graphState);

		if (player && player->Is3DLoaded()) {
			auto* mutableActorState = player->AsActorState();
			const bool previousWantBlocking = mutableActorState && mutableActorState->actorState2.wantBlocking;
			if (mutableActorState) {
				mutableActorState->actorState2.wantBlocking = blockRequested;
			}
			const bool blockGraphWriteOk = player->SetGraphVariableBool(
				RE::BSFixedString(GraphVarVanillaIsBlocking), blockRequested);
			bool blockGraphEventOk = true;
			if (blockAutoCleared || previousWantBlocking != blockRequested) {
				blockGraphEventOk = player->NotifyAnimationGraph(RE::BSFixedString(
					blockRequested ? BlockStartEvent : BlockStopEvent));
				logger::info(
					"event=block_reconcile session={} requested={} auto_cleared={} previous_want_blocking={} "
					"is_blocking={} graph_write_ok={} graph_event_ok={} expected_oar_family={} quarterstaff={}",
					session,
					blockRequested,
					blockAutoCleared,
					previousWantBlocking,
					player->IsBlocking(),
					blockGraphWriteOk,
					blockGraphEventOk,
					equipment.expectedOarFamily,
					equipment.quarterstaffEquipped);
			}
		}
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

		if (DrainFlightMagicka(player)) {
			logger::info("Dragon Aspect Flight: magicka depleted, beginning controlled descent");
			RE::SendHUDMessage::ShowHUDMessage("Dragon Aspect Flight: magicka exhausted, descending");
			BeginDescent();
			return;
		}

		MovePlayerWithCharacterControllerVelocity(flightSpeed, verticalSpeed, liftScale, forwardInput, strafeInput, verticalInput, launchBoost, boostHeld, _smoothedFlightVelocity);
	}
}
