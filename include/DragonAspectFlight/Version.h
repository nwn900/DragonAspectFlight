#pragma once

#include <cstdint>
#include <string_view>

#if !defined(DAF_VERSION_MAJOR) || !defined(DAF_VERSION_MINOR) || \
	!defined(DAF_VERSION_PATCH) || !defined(DAF_VERSION_STRING) || \
	!defined(DAF_BUILD_LABEL)
#	error "Dragon Aspect Flight version definitions are missing"
#endif

namespace DragonAspectFlight
{
	inline constexpr std::uint32_t VersionMajor = DAF_VERSION_MAJOR;
	inline constexpr std::uint32_t VersionMinor = DAF_VERSION_MINOR;
	inline constexpr std::uint32_t VersionPatch = DAF_VERSION_PATCH;
	inline constexpr std::uint32_t VersionTweak = 0;
	inline constexpr std::uint32_t LegacyPluginVersion =
		(VersionMajor << 24) | (VersionMinor << 16) | (VersionPatch << 8) | VersionTweak;

	inline constexpr std::string_view Version = DAF_VERSION_STRING;
	inline constexpr std::string_view BuildVersion = DAF_BUILD_LABEL;
	inline constexpr std::string_view DisplayVersion = "Dragon Aspect Flight v" DAF_VERSION_STRING;
}
