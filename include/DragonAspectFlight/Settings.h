#pragma once

#include <cstdint>
#include <shared_mutex>

namespace DragonAspectFlight
{
	enum class BindingDevice : std::uint8_t
	{
		Keyboard,
		Gamepad
	};

	struct InputBinding
	{
		BindingDevice device{ BindingDevice::Keyboard };
		std::uint32_t code{ 0 };
	};

	class Settings
	{
	public:
		static Settings& GetSingleton();

		// Device-aware hotkeys. Keyboard codes are DirectInput scan codes.
		// Defaults: B=0x30, Space=0x39, Left Shift=0x2A
		InputBinding activation{ BindingDevice::Keyboard, 0x30 };
		InputBinding ascend{ BindingDevice::Keyboard, 0x39 };
		InputBinding descend{ BindingDevice::Keyboard, 0x2A };

		// Flight physics (mirror FlightManager private fields, editable at runtime)
		float flightSpeed{ 14.0F };
		float verticalSpeed{ 24.0F };
		float liftScale{ 1.0F };

		// Notification + UX toggles
		bool showReadyNotification{ true };
		bool showExpiredNotification{ true };
		bool suppressInMenus{ true };

		bool magickaCostEnabled{ true };
		float magickaCostPerSecond{ 5.0F };

		// Thread safety - shared_mutex because reads (InputHandler/game thread)
		// far outnumber writes (UI render thread).
		mutable std::shared_mutex mutex;

		void Load();                   // Read from Data\SKSE\Plugins\DragonAspectFlight.ini
		void Save();                   // Write current values back to the INI
		void ApplyToFlightManager();   // Push flightSpeed/verticalSpeed/liftScale into FlightManager

	private:
		Settings() = default;
		Settings(const Settings&) = delete;
		Settings(Settings&&) = delete;
		Settings& operator=(const Settings&) = delete;
		Settings& operator=(Settings&&) = delete;
	};
}
