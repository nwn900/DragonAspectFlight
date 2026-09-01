#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/Settings.h"
#include "SKSEMenuFramework.h"

#include "RE/B/bhkCharacterController.h"
#include "RE/B/BSFixedString.h"
#include "RE/C/ControlMap.h"
#include "RE/H/hkVector4.h"
#include "RE/H/hkpCharacterState.h"
#include "RE/M/MagicTarget.h"
#include "RE/T/TES.h"
#include "RE/U/UI.h"
#include "RE/U/UserEventEnabled.h"

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
	constexpr std::uint32_t MaxStartAfterSheatheAttempts = 8;
	constexpr auto StartAfterSheatheRetryDelay = 250ms;
	constexpr auto ShoutGraphOverrideDuration = 1400ms;
	constexpr auto ShoutControlsCloseDelay = 150ms;
	constexpr std::string_view FlightBuildVersion = "v1.5.0-compat-r7-midflight-shout-selection";
	constexpr const char* GraphVarDragonAspectActive = "bDAF_DragonAspectActive";
	constexpr const char* GraphVarFlightActive = "bDAF_FlightActive";
	constexpr const char* GraphVarLaunchBoost = "bDAF_LaunchBoost";
	constexpr const char* GraphVarFlightShout = "bDAF_FlightShout";
	constexpr const char* GraphVarFlightState = "iDAF_FlightState";
	std::atomic_bool GraphVariableFailureLogged{ false };
	std::atomic_int LastLoggedGraphSnapshot{ -1 };

	enum class FlightGraphState : std::int32_t
	{
		kOff = 0,
		kIdle = 1,
		kMoving = 2,
		kLaunch = 3,
		kDescent = 4
	};

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

	RE::PlayerCharacter* GetPlayer()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	bool IsShoutSelectionMenuOpen()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui &&
			(ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME) || ui->IsMenuOpen(RE::MagicMenu::MENU_NAME));
	}

	// The published v1.5 CommonLib helper changed both enabledControls and the
	// engine's stored snapshot. On current runtimes that snapshot is also used
	// by other input contexts, so repeated shout windows could preserve a stale
	// disabled fighting bit after landing. Only DAF's live bit is ours to edit.
	void SetControlFlagPreservingStored(
		RE::ControlMap* a_controlMap,
		RE::ControlMap::UEFlag a_flags,
		bool a_enable,
		std::string_view a_reason)
	{
		if (!a_controlMap) {
			return;
		}

		auto& runtimeData = a_controlMap->GetRuntimeData();
		const auto oldState = runtimeData.enabledControls;
		const auto storedBefore = runtimeData.storedControls;

		if (a_enable) {
			runtimeData.enabledControls.set(a_flags);
		} else {
			runtimeData.enabledControls.reset(a_flags);
		}

		RE::UserEventEnabled event{ runtimeData.enabledControls, oldState };
		a_controlMap->SendEvent(std::addressof(event));
		logger::info(
			"Fighting control transition: reason={} enable={} enabled=0x{:08X}->0x{:08X} stored=0x{:08X}->0x{:08X}",
			a_reason,
			a_enable,
			oldState.underlying(),
			runtimeData.enabledControls.underlying(),
			storedBefore.underlying(),
			runtimeData.storedControls.underlying());
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

	void AddInitialFlightLift(RE::bhkCharacterController* a_controller)
	{
		if (!a_controller) {
			return;
		}

		RE::hkVector4 currentVelocity{ 0.0F, 0.0F, 0.0F, 0.0F };
		a_controller->GetLinearVelocityImpl(currentVelocity);

		if (currentVelocity.quad.m128_f32[2] < 1.0F) {
			currentVelocity.quad.m128_f32[2] = 2.0F;
			a_controller->SetLinearVelocityImpl(currentVelocity);
		}
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

	void SetFlightGraphVariables(
		RE::PlayerCharacter* a_player,
		bool a_dragonAspectActive,
		bool a_flightActive,
		bool a_launchBoost,
		bool a_flightShout,
		FlightGraphState a_state)
	{
		if (!a_player || !a_player->Is3DLoaded()) {
			return;
		}

		bool allWritten = true;
		allWritten &= a_player->SetGraphVariableBool(RE::BSFixedString(GraphVarDragonAspectActive), a_dragonAspectActive);
		allWritten &= a_player->SetGraphVariableBool(RE::BSFixedString(GraphVarFlightActive), a_flightActive);
		allWritten &= a_player->SetGraphVariableBool(RE::BSFixedString(GraphVarLaunchBoost), a_launchBoost);
		allWritten &= a_player->SetGraphVariableBool(RE::BSFixedString(GraphVarFlightShout), a_flightShout);
		allWritten &= a_player->SetGraphVariableInt(RE::BSFixedString(GraphVarFlightState), static_cast<std::int32_t>(a_state));

		const auto requestedState = static_cast<std::int32_t>(a_state);
		const auto snapshotKey =
			(requestedState & 0xFF) |
			(a_dragonAspectActive ? 1 << 8 : 0) |
			(a_flightActive ? 1 << 9 : 0) |
			(a_launchBoost ? 1 << 10 : 0) |
			(a_flightShout ? 1 << 11 : 0);
		if (LastLoggedGraphSnapshot.exchange(snapshotKey) != snapshotKey) {
			bool observedDragonAspect = false;
			bool observedFlightActive = false;
			bool observedLaunchBoost = false;
			bool observedFlightShout = false;
			bool observedInJumpState = false;
			std::int32_t observedFlightState = -1;
			bool readbackSucceeded = true;
			readbackSucceeded &= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarDragonAspectActive), observedDragonAspect);
			readbackSucceeded &= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarFlightActive), observedFlightActive);
			readbackSucceeded &= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarLaunchBoost), observedLaunchBoost);
			readbackSucceeded &= a_player->GetGraphVariableBool(RE::BSFixedString(GraphVarFlightShout), observedFlightShout);
			readbackSucceeded &= a_player->GetGraphVariableInt(RE::BSFixedString(GraphVarFlightState), observedFlightState);
			const bool jumpStateReadable = a_player->GetGraphVariableBool(RE::BSFixedString("bInJumpState"), observedInJumpState);

			std::int32_t controllerCurrentState = -1;
			std::int32_t controllerWantedState = -1;
			if (auto* controller = a_player->GetCharController()) {
				controllerCurrentState = static_cast<std::int32_t>(controller->context.currentState);
				controllerWantedState = static_cast<std::int32_t>(controller->wantState);
			}

			logger::info(
				"Flight graph transition: requested dragon={} active={} launch={} shout={} state={}; "
				"write_ok={} read_ok={} observed dragon={} active={} launch={} shout={} state={}; "
				"jump_read_ok={} bInJumpState={} controller_current={} controller_wanted={}",
				a_dragonAspectActive,
				a_flightActive,
				a_launchBoost,
				a_flightShout,
				requestedState,
				allWritten,
				readbackSucceeded,
				observedDragonAspect,
				observedFlightActive,
				observedLaunchBoost,
				observedFlightShout,
				observedFlightState,
				jumpStateReadable,
				observedInJumpState,
				controllerCurrentState,
				controllerWantedState);
		}

		if (!allWritten && !GraphVariableFailureLogged.exchange(true)) {
			logger::error(
				"Dragon Aspect Flight: one or more v1.5 graph variables were unavailable; "
				"verify Behavior Data Injector and DragonAspectFlight_BDI.json");
		}
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

	bool ForceSheatheIfWeaponDrawn(RE::PlayerCharacter* a_player)
	{
		auto* actorState = a_player ? a_player->AsActorState() : nullptr;

		if (!actorState || !actorState->IsWeaponDrawn()) {
			return false;
		}

		a_player->DrawWeaponMagicHands(false);
		return true;
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

		if (ForceSheatheIfWeaponDrawn(player)) {
			const auto attempt = _startAfterSheatheAttempts.fetch_add(1);

			if (attempt >= MaxStartAfterSheatheAttempts) {
				_startAfterSheatheAttempts = 0;
				_startAfterSheathePending = false;
				logger::warn("Dragon Aspect Flight: cancelled delayed flight start because weapons stayed drawn");
				return;
			}

			logger::info("Dragon Aspect Flight: sheathing weapons before flight start");
			QueueStartAfterSheathe();
			return;
		}

		{
			std::unique_lock lock(_mutex);

			if (_isFlying) {
				return;
			}

			_isFlying = true;
			_isDescending = false;
			(void)detail::ResetFlightShoutControl(_flightShoutControls);
			_startAfterSheathePending = false;
			_startAfterSheatheAttempts = 0;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kIdle);
			_landingContactTicks = 0;
			_shoutGraphOverrideUntil = {};
			_smoothedFlightVelocity = RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F };
			_updateGeneration.fetch_add(1, std::memory_order_acq_rel);
			logger::info("Flight started - {}", FlightBuildVersion);
		}

		SuppressFightingControls();

		// Keep the physics state airborne for OAR without firing sprint/jump
		// animation graph events that can collide with Better Jumping.
		if (player && player->Is3DLoaded()) {
			if (auto* controller = player->GetCharController()) {
				_originalGravity = controller->gravity;
				ApplyControlledAirState(player, controller);
				AddInitialFlightLift(controller);
			}
			SetFlightGraphVariables(player, true, true, false, false, FlightGraphState::kIdle);
		}

		StartUpdateThread();
	}

	void FlightManager::QueueStartAfterSheathe()
	{
		if (_startAfterSheathePending.exchange(true)) {
			return;
		}

		if (_startAfterSheatheThreadRunning.exchange(true)) {
			return;
		}

		_startAfterSheatheThread = std::jthread([this](std::stop_token stopToken) {
			for (int i = 0; i < 25; ++i) {
				if (stopToken.stop_requested()) {
					_startAfterSheathePending = false;
					_startAfterSheatheThreadRunning = false;
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			if (stopToken.stop_requested()) {
				_startAfterSheathePending = false;
				_startAfterSheatheThreadRunning = false;
				return;
			}

			auto* taskInterface = SKSE::GetTaskInterface();
			if (!taskInterface) {
				_startAfterSheathePending = false;
				_startAfterSheatheThreadRunning = false;
				return;
			}

			taskInterface->AddTask([this]() {
				_startAfterSheathePending = false;
				_startAfterSheatheThreadRunning = false;
				StartFlight();
			});
		});
	}

	void FlightManager::BeginDescent()
	{
		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || _isDescending) {
				return;
			}

			_isDescending = true;
			_forwardInput = 0.0F;
			_strafeInput = 0.0F;
			_verticalInput = 0.0F;
			_pendingLaunchBoost = 0.0F;
			_boostHeld = false;
			(void)detail::ResetFlightShoutControl(_flightShoutControls);
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kDescent);
			_landingContactTicks = 0;
			logger::info("Flight descent started - {}", FlightBuildVersion);
		}

		if (auto* player = GetPlayer(); player && player->Is3DLoaded()) {
			SetFlightGraphVariables(player, HasDragonAspectActive(), true, false, false, FlightGraphState::kDescent);
		}

		StartUpdateThread();
	}

	void FlightManager::CancelDescent()
	{
		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || !_isDescending || !HasDragonAspectActive()) {
				return;
			}

			_isDescending = false;
			_forwardInput = 0.0F;
			_strafeInput = 0.0F;
			_verticalInput = 0.0F;
			_pendingLaunchBoost = 0.0F;
			_boostHeld = false;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kIdle);
			_landingContactTicks = 0;
			_shoutGraphOverrideUntil = {};
			logger::info("Flight descent cancelled - {}", FlightBuildVersion);
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

			SetFlightGraphVariables(player, true, true, false, false, FlightGraphState::kIdle);
		}

		StartUpdateThread();
	}

	void FlightManager::StopFlight()
	{
		{
			std::unique_lock lock(_mutex);

			if (!_isFlying) {
				return;
			}

			_isFlying = false;
			_isDescending = false;
			_forwardInput = 0.0F;
			_strafeInput = 0.0F;
			_verticalInput = 0.0F;
			_boostHeld = false;
			(void)detail::ResetFlightShoutControl(_flightShoutControls);
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kOff);
			_landingContactTicks = 0;
			_smoothedFlightVelocity = RE::hkVector4{ 0.0F, 0.0F, 0.0F, 0.0F };
			_updateGeneration.fetch_add(1, std::memory_order_acq_rel);
			logger::info("Flight stopped - {}", FlightBuildVersion);
		}

		ClampStopVelocityForSafeRelease(GetPlayer());
		RestoreFightingControls();

		// Restore gravity, friction, and ground state.
		if (auto* player = GetPlayer(); player && player->Is3DLoaded()) {
			SetFlightGraphVariables(player, HasDragonAspectActive(), false, false, false, FlightGraphState::kOff);
			if (auto* controller = player->GetCharController()) {
				controller->gravity = _originalGravity;
				controller->flags.reset(RE::CHARACTER_FLAGS::kNoFriction);
				controller->wantState = RE::hkpCharacterStateType::kOnGround;
				controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
			}
		}

		if (_startAfterSheatheThread.joinable()) {
			_startAfterSheatheThread.request_stop();
			_startAfterSheatheThread.join();
		}
		_startAfterSheatheThreadRunning = false;
		_startAfterSheathePending = false;

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

	bool FlightManager::IsDragonAspectActive() const
	{
		return HasDragonAspectActive();
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
		logger::info("Launch boost queued - {}", FlightBuildVersion);
	}

	void FlightManager::NotifyFlightShout()
	{
		bool applyImmediately = false;

		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || _isDescending || !HasDragonAspectActive()) {
				return;
			}

			_shoutGraphOverrideUntil = std::chrono::steady_clock::now() + ShoutGraphOverrideDuration;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kMoving);
			applyImmediately = true;
		}

		if (applyImmediately) {
			SetFlightGraphVariables(GetPlayer(), true, true, false, true, FlightGraphState::kMoving);
		}
	}

	void FlightManager::BeginFlightShoutInput()
	{
		auto* controlMap = RE::ControlMap::GetSingleton();

		if (!controlMap) {
			return;
		}

		bool shouldEnable = false;
		bool canceledPendingClose = false;
		bool wasOpen = false;

		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || _isDescending) {
				return;
			}

			if (_fightingControlsSuppressed) {
				wasOpen = _flightShoutControls.open;
				canceledPendingClose =
					_flightShoutControls.closeAfter != std::chrono::steady_clock::time_point{};
				(void)detail::BeginFlightShoutControl(_flightShoutControls);
				shouldEnable = true;
			}
		}

		logger::info(
			"Flight shout control window begin: was_open={} canceled_pending_close={} fighting_enabled={}",
			wasOpen,
			canceledPendingClose,
			controlMap->IsFightingControlsEnabled());

		if (shouldEnable && !controlMap->IsFightingControlsEnabled()) {
			SetControlFlagPreservingStored(controlMap, RE::ControlMap::UEFlag::kFighting, true, "shout_begin");
			logger::info("Fighting controls opened for Dragon Aspect flight shout");
		}
	}

	void FlightManager::QueueEndFlightShoutInput()
	{
		std::unique_lock lock(_mutex);

		const bool wasOpen = _flightShoutControls.open;
		detail::QueueFlightShoutControlClose(
			_flightShoutControls, std::chrono::steady_clock::now(), ShoutControlsCloseDelay);
		logger::info(
			"Flight shout control close queued: was_open={} deadline_armed={}",
			wasOpen,
			_flightShoutControls.closeAfter != std::chrono::steady_clock::time_point{});
	}

	void FlightManager::EndFlightShoutInput()
	{
		auto* controlMap = RE::ControlMap::GetSingleton();

		if (!controlMap) {
			return;
		}

		bool shouldDisable = false;

		{
			std::unique_lock lock(_mutex);

			if (detail::ResetFlightShoutControl(_flightShoutControls) ==
				detail::ShoutControlTransition::kClose) {
				shouldDisable = _isFlying && _fightingControlsSuppressed;
			}
		}

		if (shouldDisable && !IsShoutSelectionMenuOpen() && controlMap->IsFightingControlsEnabled()) {
			SetControlFlagPreservingStored(controlMap, RE::ControlMap::UEFlag::kFighting, false, "shout_reset");
			logger::info("Fighting controls closed after Dragon Aspect flight shout");
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
		logger::info("Lift scale set to {} - {}", _liftScale, FlightBuildVersion);
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
		_verticalInput = _isDescending ? 0.0F : std::clamp(a_verticalInput, -1.0F, 1.0F);
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

	void FlightManager::SuppressFightingControls()
	{
		auto* controlMap = RE::ControlMap::GetSingleton();

		if (!controlMap) {
			return;
		}

		bool shouldDisable = false;

		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || _fightingControlsSuppressed) {
				return;
			}

			_restoreFightingControls = controlMap->IsFightingControlsEnabled();
			shouldDisable = _restoreFightingControls;
			_fightingControlsSuppressed = true;
		}

		if (shouldDisable) {
			SetControlFlagPreservingStored(controlMap, RE::ControlMap::UEFlag::kFighting, false, "flight_start");
			logger::info("Fighting controls suppressed for Dragon Aspect flight");
		}
	}

	void FlightManager::RestoreFightingControls()
	{
		auto* controlMap = RE::ControlMap::GetSingleton();

		if (!controlMap) {
			return;
		}

		bool shouldRestore = false;

		{
			std::unique_lock lock(_mutex);
			shouldRestore = _fightingControlsSuppressed && _restoreFightingControls;
			_fightingControlsSuppressed = false;
			_restoreFightingControls = false;
		}

		if (shouldRestore) {
			SetControlFlagPreservingStored(controlMap, RE::ControlMap::UEFlag::kFighting, true, "flight_stop");
			logger::info("Fighting controls restored after Dragon Aspect flight");
		}
	}

	void FlightManager::EnforceFightingControlsSuppressed()
	{
		auto* controlMap = RE::ControlMap::GetSingleton();

		if (!controlMap) {
			return;
		}

		const bool shoutSelectionMenuOpen = IsShoutSelectionMenuOpen();
		bool shouldCloseQueuedShout = false;
		bool shouldEnableFightingControls = false;
		bool shoutWindowOpen = false;

		{
			std::unique_lock lock(_mutex);

			if (!_isFlying || !_fightingControlsSuppressed) {
				return;
			}

			if (_flightShoutControls.open) {
				shouldCloseQueuedShout = detail::PollFlightShoutControl(
										   _flightShoutControls, std::chrono::steady_clock::now()) ==
									   detail::ShoutControlTransition::kClose;
			}

			shouldEnableFightingControls =
				detail::ShouldEnableFlightFightingControls(_flightShoutControls, shoutSelectionMenuOpen);
			shoutWindowOpen = _flightShoutControls.open;
		}

		if (shouldEnableFightingControls && !controlMap->IsFightingControlsEnabled()) {
			SetControlFlagPreservingStored(
				controlMap,
				RE::ControlMap::UEFlag::kFighting,
				true,
				shoutSelectionMenuOpen ? "shout_selection_menu_open" : "shout_window_enforce");
			logger::info(
				"Fighting controls opened for mid-flight shout selection: menu_open={} shout_window_open={}",
				shoutSelectionMenuOpen,
				shoutWindowOpen);
		} else if (!shouldEnableFightingControls && controlMap->IsFightingControlsEnabled()) {
			SetControlFlagPreservingStored(
				controlMap,
				RE::ControlMap::UEFlag::kFighting,
				false,
				shouldCloseQueuedShout ? "shout_close_deadline" : "shout_selection_menu_close_or_flight_enforce");
			if (shouldCloseQueuedShout) {
				logger::info("Fighting controls closed after vanilla flight shout pass-through");
			} else {
				logger::info("Fighting controls re-suppressed during Dragon Aspect flight");
			}
		}
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

		bool expected = false;
		if (!_updateQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			return;
		}

		const auto generation = _updateGeneration.load(std::memory_order_acquire);
		try {
			taskInterface->AddTask([this, generation]() {
				try {
					if (_updateGeneration.load(std::memory_order_acquire) == generation) {
						UpdateFlight();
					}
				} catch (const std::exception& e) {
					logger::error("Dragon Aspect Flight: queued update failed: {}", e.what());
				} catch (...) {
					logger::error("Dragon Aspect Flight: queued update failed with an unknown exception");
				}
				_updateQueued.store(false, std::memory_order_release);
			});
		} catch (const std::exception& e) {
			_updateQueued.store(false, std::memory_order_release);
			logger::error("Dragon Aspect Flight: failed to queue update: {}", e.what());
		} catch (...) {
			_updateQueued.store(false, std::memory_order_release);
			logger::error("Dragon Aspect Flight: failed to queue update with an unknown exception");
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
		bool shoutOverrideActive = false;
		std::uint32_t landingContactTicks = 0;
		FlightGraphState graphState = FlightGraphState::kOff;

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

			descending = _isDescending;
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
			shoutOverrideActive = !descending && std::chrono::steady_clock::now() < _shoutGraphOverrideUntil;

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

		auto* player = GetPlayer();
		EnforceFightingControlsSuppressed();

		if (!descending && ForceSheatheIfWeaponDrawn(player)) {
			logger::info("Blocked weapon/magic draw during Dragon Aspect flight");
		}

		SetFlightGraphVariables(player, true, true, graphState == FlightGraphState::kLaunch, shoutOverrideActive, graphState);

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
