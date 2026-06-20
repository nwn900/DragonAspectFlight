#include "PCH.h"

#include "DragonAspectFlight/FlightManager.h"
#include "DragonAspectFlight/Papyrus.h"

namespace DragonAspectFlight::Papyrus
{
	namespace
	{
		constexpr auto SCRIPT_NAME = "DragonAspectFlight";

		void StartFlight(RE::StaticFunctionTag*) { FlightManager::GetSingleton().StartFlight(); }
		void StopFlight(RE::StaticFunctionTag*) { FlightManager::GetSingleton().StopFlight(); }
		bool IsFlying(RE::StaticFunctionTag*) { return FlightManager::GetSingleton().IsFlying(); }
		void SetFlightSpeed(RE::StaticFunctionTag*, float a_speed) { FlightManager::GetSingleton().SetFlightSpeed(a_speed); }
		void SetVerticalSpeed(RE::StaticFunctionTag*, float a_speed) { FlightManager::GetSingleton().SetVerticalSpeed(a_speed); }
		void SetLiftScale(RE::StaticFunctionTag*, float a_scale) { FlightManager::GetSingleton().SetLiftScale(a_scale); }
	}

	bool Register(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			logger::error("Papyrus VM was null; native registration skipped");
			return false;
		}

		a_vm->RegisterFunction("StartFlight", SCRIPT_NAME, StartFlight);
		a_vm->RegisterFunction("StopFlight", SCRIPT_NAME, StopFlight);
		a_vm->RegisterFunction("IsFlying", SCRIPT_NAME, IsFlying);
		a_vm->RegisterFunction("SetFlightSpeed", SCRIPT_NAME, SetFlightSpeed);
		a_vm->RegisterFunction("SetVerticalSpeed", SCRIPT_NAME, SetVerticalSpeed);
		a_vm->RegisterFunction("SetLiftScale", SCRIPT_NAME, SetLiftScale);

		logger::info("Registered Papyrus natives for {}", SCRIPT_NAME);
		return true;
	}
}
