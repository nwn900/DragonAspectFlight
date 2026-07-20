#include "PCH.h"

#include "DragonAspectFlight/Settings.h"
#include "DragonAspectFlight/FlightManager.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
	constexpr const char* SettingsIniPath = "Data\\SKSE\\Plugins\\DragonAspectFlight.ini";

	std::string_view Trim(std::string_view a_value)
	{
		while (!a_value.empty() && std::isspace(static_cast<unsigned char>(a_value.front())) != 0) {
			a_value.remove_prefix(1);
		}
		while (!a_value.empty() && std::isspace(static_cast<unsigned char>(a_value.back())) != 0) {
			a_value.remove_suffix(1);
		}
		return a_value;
	}

	bool EqualsIgnoreCase(std::string_view a_lhs, std::string_view a_rhs)
	{
		if (a_lhs.size() != a_rhs.size()) return false;
		for (std::size_t i = 0; i < a_lhs.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a_lhs[i])) !=
				std::tolower(static_cast<unsigned char>(a_rhs[i]))) {
				return false;
			}
		}
		return true;
	}

	std::uint32_t ParseScanCode(std::string_view a_value, std::uint32_t a_fallback)
	{
		a_value = Trim(a_value);
		if (a_value.empty()) return a_fallback;

		std::string value{ a_value };
		const char* begin = value.c_str();
		int base = 10;
		if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
			begin += 2;
			base = 16;
		}

		errno = 0;
		char* end = nullptr;
		const auto parsed = std::strtoul(begin, std::addressof(end), base);
		if (begin == end || errno != 0 || *end != '\0' ||
			parsed > std::numeric_limits<std::uint32_t>::max()) {
			return a_fallback;
		}
		return static_cast<std::uint32_t>(parsed);
	}

	float ParseFloat(std::string_view a_value, float a_fallback)
	{
		a_value = Trim(a_value);
		if (a_value.empty()) return a_fallback;
		std::string value{ a_value };
		errno = 0;
		char* end = nullptr;
		const auto parsed = std::strtof(value.c_str(), std::addressof(end));
		if (end == value.c_str() || *end != '\0') return a_fallback;
		return parsed;
	}

	bool ParseBool(std::string_view a_value, bool a_fallback)
	{
		a_value = Trim(a_value);
		if (a_value.empty()) return a_fallback;
		if (EqualsIgnoreCase(a_value, "1") || EqualsIgnoreCase(a_value, "true") ||
			EqualsIgnoreCase(a_value, "yes") || EqualsIgnoreCase(a_value, "on")) {
			return true;
		}
		if (EqualsIgnoreCase(a_value, "0") || EqualsIgnoreCase(a_value, "false") ||
			EqualsIgnoreCase(a_value, "no") || EqualsIgnoreCase(a_value, "off")) {
			return false;
		}
		return a_fallback;
	}
}

namespace DragonAspectFlight
{
	Settings& Settings::GetSingleton()
	{
		static Settings s;
		return s;
	}

	void Settings::Load()
	{
		std::ifstream file(SettingsIniPath);
		if (!file) {
			logger::info("Dragon Aspect Flight: INI not found at {}; using defaults", SettingsIniPath);
			ApplyToFlightManager();
			return;
		}

		std::string section;
		std::string line;
		while (std::getline(file, line)) {
			// Strip comments
			if (const auto c = line.find_first_of(";#"); c != std::string::npos) {
				line.erase(c);
			}

			auto trimmed = Trim(line);
			if (trimmed.empty()) continue;

			// Section header [Name]
			if (trimmed.front() == '[' && trimmed.back() == ']') {
				trimmed.remove_prefix(1);
				trimmed.remove_suffix(1);
				section = std::string(Trim(trimmed));
				continue;
			}

			const auto eq = trimmed.find('=');
			if (eq == std::string_view::npos) continue;

			const auto key = Trim(trimmed.substr(0, eq));
			const auto val = Trim(trimmed.substr(eq + 1));

			if (EqualsIgnoreCase(section, "Hotkeys")) {
				if (EqualsIgnoreCase(key, "Activation")) activation = ParseScanCode(val, activation);
				else if (EqualsIgnoreCase(key, "Ascend")) ascend = ParseScanCode(val, ascend);
				else if (EqualsIgnoreCase(key, "Descend")) descend = ParseScanCode(val, descend);
			} else if (EqualsIgnoreCase(section, "Flight")) {
				if (EqualsIgnoreCase(key, "FlightSpeed")) flightSpeed = ParseFloat(val, flightSpeed);
				else if (EqualsIgnoreCase(key, "VerticalSpeed")) verticalSpeed = ParseFloat(val, verticalSpeed);
				else if (EqualsIgnoreCase(key, "LiftScale")) liftScale = ParseFloat(val, liftScale);
			} else if (EqualsIgnoreCase(section, "Notifications")) {
				if (EqualsIgnoreCase(key, "ShowReady")) showReadyNotification = ParseBool(val, showReadyNotification);
				else if (EqualsIgnoreCase(key, "ShowExpired")) showExpiredNotification = ParseBool(val, showExpiredNotification);
				else if (EqualsIgnoreCase(key, "SuppressInMenus")) suppressInMenus = ParseBool(val, suppressInMenus);
			} else if (EqualsIgnoreCase(section, "Magicka")) {
				if (EqualsIgnoreCase(key, "Enabled")) magickaCostEnabled = ParseBool(val, magickaCostEnabled);
				else if (EqualsIgnoreCase(key, "CostPerSecond")) magickaCostPerSecond = ParseFloat(val, magickaCostPerSecond);
			}
		}

		// Clamp liftScale to the same range FlightManager uses
		if (liftScale < 0.25F) liftScale = 0.25F;
		if (liftScale > 2.50F) liftScale = 2.50F;

		logger::info(
			"Dragon Aspect Flight settings loaded: Activation=0x{:X} Ascend=0x{:X} Descend=0x{:X} "
			"FlightSpeed={} VerticalSpeed={} LiftScale={} ShowReady={} ShowExpired={} SuppressInMenus={} "
			"MagickaCostEnabled={} MagickaCostPerSecond={}",
			activation, ascend, descend, flightSpeed, verticalSpeed, liftScale,
			showReadyNotification, showExpiredNotification, suppressInMenus,
			magickaCostEnabled, magickaCostPerSecond);

		ApplyToFlightManager();
	}

	void Settings::Save()
	{
		std::ofstream file(SettingsIniPath, std::ios::trunc);
		if (!file) {
			logger::warn("Dragon Aspect Flight: could not write INI at {}", SettingsIniPath);
			return;
		}

		file << "[Hotkeys]\n";
		file << "; DirectInput keyboard scan codes. Decimal and hexadecimal values are both supported.\n";
		file << "Activation=0x" << std::hex << activation << "\n";
		file << "Ascend=0x" << ascend << "\n";
		file << "Descend=0x" << descend << std::dec << "\n\n";

		file << "[Flight]\n";
		file << "; Flight physics tuning.\n";
		file << "FlightSpeed=" << flightSpeed << "\n";
		file << "VerticalSpeed=" << verticalSpeed << "\n";
		file << "LiftScale=" << liftScale << "\n\n";

		file << "[Notifications]\n";
		file << "; 1=yes, 0=no\n";
		file << "ShowReady=" << (showReadyNotification ? 1 : 0) << "\n";
		file << "ShowExpired=" << (showExpiredNotification ? 1 : 0) << "\n";
		file << "SuppressInMenus=" << (suppressInMenus ? 1 : 0) << "\n\n";

		file << "[Magicka]\n";
		file << "; Drain magicka while flying. When depleted, flight descends safely.\n";
		file << "; 1=yes, 0=no\n";
		file << "Enabled=" << (magickaCostEnabled ? 1 : 0) << "\n";
		file << "CostPerSecond=" << magickaCostPerSecond << "\n";

		logger::info("Dragon Aspect Flight settings saved to {}", SettingsIniPath);
	}

	void Settings::ApplyToFlightManager()
	{
		auto& fm = FlightManager::GetSingleton();
		fm.SetFlightSpeed(flightSpeed);
		fm.SetVerticalSpeed(verticalSpeed);
		fm.SetLiftScale(liftScale);
	}
}
