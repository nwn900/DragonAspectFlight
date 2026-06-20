#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"

#include "RE/B/bhkCharacterController.h"
#include "RE/B/BSFixedString.h"
#include "RE/C/ControlMap.h"
#include "RE/H/hkVector4.h"
#include "RE/H/hkpCharacterState.h"
#include "RE/M/MagicTarget.h"

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
	constexpr float BoostVelocitySmoothing = 0.22F;
	constexpr float TurnVelocitySmoothing = 0.18F;
	constexpr float CollisionCatchUpBrake = 0.65F;
	constexpr float MinFlightHoverVelocity = 0.35F;
	constexpr float StaminaRestorePerUpdate = 6.0F;
	constexpr float MaxStopDownwardVelocity = -2.0F;
	constexpr float DescentVerticalVelocity = -4.5F;
	constexpr float DescentHorizontalDamping = 0.38F;
	constexpr float WaterLandingTolerance = 24.0F;
	constexpr float WaterLandingOffset = 4.0F;
	constexpr std::uint32_t MaxStartAfterSheatheAttempts = 8;
	constexpr auto StartAfterSheatheRetryDelay = 250ms;
	constexpr auto ShoutGraphOverrideDuration = 1400ms;
	constexpr std::string_view FlightBuildVersion = "v0.3.2-dragon-aspect";
	constexpr const char* GraphVarDragonAspectActive = "bDAF_DragonAspectActive";
	constexpr const char* GraphVarFlightActive = "bDAF_FlightActive";
	constexpr const char* GraphVarLaunchBoost = "bDAF_LaunchBoost";
	constexpr const char* GraphVarFlightState = "iDAF_FlightState";

	enum class FlightGraphState : std::int32_t
	{
		kOff = 0,
		kIdle = 1,
		kMoving = 2,
		kLaunch = 3,
		kDescent = 4
	};

	bool ResolveWaterLanding(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller);

	// Dragon Aspect magic effect form IDs from Dragonborn.esm.
	// We check for active magic effects rather than HasSpell() because
	// shout-applied temporary ability spells may not register as "known" spells.
	constexpr RE::FormID DA_BodyEffect1 = 0x01DF97;  // DLC2DragonAspectBody01Effect "Damage Resistance"
	constexpr RE::FormID DA_BodyEffect2 = 0x01DF98;  // DLC2DragonAspectBody02Effect "Fire/Frost Resist"
	constexpr RE::FormID DA_ArmsEffect  = 0x021730;  // DLC2DragonAspectArmsEffect02 "Dragon Aspect - Arms"
	constexpr const char* DragonbornPlugin = "Dragonborn.esm";

	// More Draconic Aspect wings magic effect (form 0x804 in the ESL)
	constexpr RE::FormID DA_WingsEffect = 0x00804;
	constexpr const char* MoreDraconicPlugin = "More Draconic Aspect - Become The Dragonborn ESL.esp";

	RE::PlayerCharacter* GetPlayer()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	// Check if any Dragon Aspect magic effect is active on the player.
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

		// Check the three vanilla Dragon Aspect body/arms effects
		constexpr RE::FormID ids[] = { DA_BodyEffect1, DA_BodyEffect2, DA_ArmsEffect };
		for (auto id : ids) {
			auto* effect = dh->LookupForm<RE::EffectSetting>(id, DragonbornPlugin);
			if (effect && magicTarget->HasMagicEffect(effect)) {
				return true;
			}
		}

		// Also check More Draconic Aspect wings effect if that mod is installed
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
		actorValueOwner->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, restoreAmount);
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

	void ApplyControlledAirState(RE::bhkCharacterController* a_controller)
	{
		if (!a_controller) {
			return;
		}

		a_controller->gravity = 0.0F;
		a_controller->flags.set(RE::CHARACTER_FLAGS::kNoFriction);
		a_controller->wantState = RE::hkpCharacterStateType::kInAir;
		a_controller->context.currentState = RE::hkpCharacterStateType::kInAir;
	}

	void HoldContinuousFlightAirState(RE::PlayerCharacter* a_player, RE::bhkCharacterController* a_controller)
	{
		ResetFlightFallState(a_player, a_controller);
		ApplyControlledAirState(a_controller);
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
		FlightGraphState a_state)
	{
		if (!a_player || !a_player->Is3DLoaded()) {
			return;
		}

		a_player->SetGraphVariableBool(RE::BSFixedString(GraphVarDragonAspectActive), a_dragonAspectActive);
		a_player->SetGraphVariableBool(RE::BSFixedString(GraphVarFlightActive), a_flightActive);
		a_player->SetGraphVariableBool(RE::BSFixedString(GraphVarLaunchBoost), a_launchBoost);
		a_player->SetGraphVariableInt(RE::BSFixedString(GraphVarFlightState), static_cast<std::int32_t>(a_state));
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

	void MovePlayerWithCharacterControllerVelocity(float a_horizontalSpeed, float a_verticalSpeed, float a_liftScale, float a_forwardInput, float a_strafeInput, float a_launchBoost, bool a_boostHeld)
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

		static RE::hkVector4 smoothedVelocity{ 0.0F, 0.0F, 0.0F, 0.0F };

		const float maxHorizontalForMode = a_boostHeld ? BoostedMaxHorizontalVelocity : MaxHorizontalVelocity;
		const float maxVerticalForMode = a_boostHeld ? BoostedMaxVerticalVelocity : MaxVerticalVelocity;
		const float activeHorizontalSpeed = a_boostHeld ? BoostHorizontalVelocity : a_horizontalSpeed;
		const float activeVerticalSpeed = a_boostHeld ? BoostVerticalVelocity : a_verticalSpeed;
		const float activeSmoothing = a_boostHeld ? BoostVelocitySmoothing : TurnVelocitySmoothing;

		if (a_horizontalSpeed <= 0.0F || !HasMovementInput(a_forwardInput, a_strafeInput)) {
			const RE::hkVector4 idleTargetVelocity{ 0.0F, 0.0F, std::clamp(std::max(a_launchBoost, MinFlightHoverVelocity), 0.0F, MaxVerticalVelocity), 0.0F };
			smoothedVelocity = LerpVelocity(smoothedVelocity, idleTargetVelocity);
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
			controller->SetLinearVelocityImpl(RE::hkVector4{ 0.0F, 0.0F, MinFlightHoverVelocity, 0.0F });
			return;
		}

		const float tunedHorizontalSpeed = std::min(activeHorizontalSpeed * inputMagnitude, maxHorizontalForMode);
		const float tunedVerticalSpeed = std::min(activeVerticalSpeed * inputMagnitude, maxVerticalForMode);

		const float targetVerticalVelocity = std::max(
			(desiredDirection.z * tunedVerticalSpeed * BaseVerticalVelocityScale * std::clamp(a_liftScale, 0.25F, 2.50F)) + a_launchBoost,
			MinFlightHoverVelocity);

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

		const RE::hkVector4 clampedVelocity{
			ClampMagnitude(smoothedVelocity.quad.m128_f32[0], maxHorizontalForMode),
			ClampMagnitude(smoothedVelocity.quad.m128_f32[1], maxHorizontalForMode),
			ClampMagnitude(smoothedVelocity.quad.m128_f32[2], maxVerticalForMode),
			0.0F
		};

		controller->SetLinearVelocityImpl(clampedVelocity);
	}

	bool MovePlayerWithControlledDescent()
	{
		auto* player = GetPlayer();

		if (!player || !player->Is3DLoaded()) {
			return false;
		}

		auto* controller = player->GetCharController();

		if (!controller) {
			return false;
		}

		if (ResolveWaterLanding(player, controller)) {
			logger::info("Flight descent resolved on water surface");
			return true;
		}

		if (IsControllerGrounded(controller)) {
			return true;
		}

		RestoreFlightStamina(player);
		ResetFlightFallState(player, controller);
		ApplyControlledAirState(controller);

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
		if (!a_player || !a_player->IsWeaponDrawn()) {
			return false;
		}

		a_player->DrawWeaponMagicHands(false);
		return true;
	}

	bool IsNearWaterSurface(RE::PlayerCharacter* a_player)
	{
		if (!a_player) {
			return false;
		}

		const float waterHeight = a_player->GetWaterHeight();

		if (!std::isfinite(waterHeight) || waterHeight < -100000.0F) {
			return false;
		}

		return a_player->GetPositionZ() <= waterHeight + WaterLandingTolerance;
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
			_startAfterSheathePending = false;
			_startAfterSheatheAttempts = 0;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kIdle);
			_shoutGraphOverrideUntil = {};
			logger::info("Flight started - {}", FlightBuildVersion);
		}

		SuppressFightingControls();

		// Keep the physics state airborne for OAR without firing sprint/jump
		// animation graph events that can collide with Better Jumping.
		if (player && player->Is3DLoaded()) {
			if (auto* controller = player->GetCharController()) {
				_originalGravity = controller->gravity;
				ApplyControlledAirState(controller);
				AddInitialFlightLift(controller);
			}
			SetFlightGraphVariables(player, true, true, false, FlightGraphState::kIdle);
		}

		StartUpdateThread();
	}

	void FlightManager::QueueStartAfterSheathe()
	{
		if (_startAfterSheathePending.exchange(true)) {
			return;
		}

		std::thread([this]() {
			std::this_thread::sleep_for(StartAfterSheatheRetryDelay);

			auto* taskInterface = SKSE::GetTaskInterface();
			if (!taskInterface) {
				_startAfterSheathePending = false;
				return;
			}

			taskInterface->AddTask([this]() {
				_startAfterSheathePending = false;
				StartFlight();
			});
		}).detach();
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
			_pendingLaunchBoost = 0.0F;
			_boostHeld = false;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kDescent);
			logger::info("Flight descent started - {}", FlightBuildVersion);
		}

		if (auto* player = GetPlayer(); player && player->Is3DLoaded()) {
			SetFlightGraphVariables(player, HasDragonAspectActive(), true, false, FlightGraphState::kDescent);
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
			_boostHeld = false;
			_lastGraphState = static_cast<std::int32_t>(FlightGraphState::kOff);
			logger::info("Flight stopped - {}", FlightBuildVersion);
		}

		ClampStopVelocityForSafeRelease(GetPlayer());
		RestoreFightingControls();

		// Restore gravity, friction, and ground state.
		if (auto* player = GetPlayer(); player && player->Is3DLoaded()) {
			SetFlightGraphVariables(player, HasDragonAspectActive(), false, false, FlightGraphState::kOff);
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

	bool FlightManager::IsDragonAspectActive() const
	{
		return HasDragonAspectActive();
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
			SetFlightGraphVariables(GetPlayer(), true, true, false, FlightGraphState::kMoving);
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
			controlMap->ToggleControls(RE::ControlMap::UEFlag::kFighting, false);
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
			controlMap->ToggleControls(RE::ControlMap::UEFlag::kFighting, true);
			logger::info("Fighting controls restored after Dragon Aspect flight");
		}
	}

	void FlightManager::EnforceFightingControlsSuppressed()
	{
		auto* controlMap = RE::ControlMap::GetSingleton();

		if (!controlMap) {
			return;
		}

		{
			std::shared_lock lock(_mutex);

			if (!_isFlying || !_fightingControlsSuppressed) {
				return;
			}
		}

		if (controlMap->IsFightingControlsEnabled()) {
			controlMap->ToggleControls(RE::ControlMap::UEFlag::kFighting, false);
			logger::info("Fighting controls re-suppressed during Dragon Aspect flight");
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
		float launchBoost = 0.0F;
		bool boostHeld = false;
		bool descending = false;
		bool shoutOverrideActive = false;
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
			launchBoost = _pendingLaunchBoost;
			boostHeld = _boostHeld;
			_pendingLaunchBoost = 0.0F;
			shoutOverrideActive = !descending && std::chrono::steady_clock::now() < _shoutGraphOverrideUntil;

			const bool hasMovementInput = !descending && HasMovementInput(forwardInput, strafeInput);
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

		SetFlightGraphVariables(player, true, true, graphState == FlightGraphState::kLaunch, graphState);

		if (descending) {
			if (MovePlayerWithControlledDescent()) {
				StopFlight();
			}
			return;
		}

		MovePlayerWithCharacterControllerVelocity(flightSpeed, verticalSpeed, liftScale, forwardInput, strafeInput, launchBoost, boostHeld);
	}
}
