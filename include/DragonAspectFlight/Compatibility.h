#pragma once

// This header is deliberately a compile-time capability contract.  Dragon
// Aspect Flight uses Address-Library relocation independence for one DLL
// across SE, AE, and VR, and the 1.7.99 AE Address Library is format 5.  A
// stale CommonLib checkout must therefore fail during the build instead of
// producing a DLL that compiles but cannot load the new runtime.
#include "REL/IDDB.h"
#include "SKSE/Version.h"

namespace DragonAspectFlight::Compatibility
{
	// 1.7.104 is a DAF-local reference for the format-5 Address Library
	// capability check.  CommonLib does not expose this runtime in every
	// supported checkout, so do not make it an SKSE namespace requirement or a
	// global minimum for the older SE/AE/VR runtimes.
	inline constexpr REL::Version RuntimeSSE_1_7_104{ 1, 7, 104, 0 };

	// 1.7.99 remains the source-level floor for the format-5 capability exposed
	// by the existing CommonLib API.  Older supported game runtimes (including
	// 1.6.x, 1.5.97, and VR) continue to select their normal Address Library
	// database through CommonLib's runtime routing.
	inline constexpr REL::Version MinimumAddressLibraryRuntime = SKSE::RUNTIME_SSE_1_7_99;
	inline constexpr REL::IDDB::Format AddressLibraryFormat = REL::IDDB::Format::SSEv5;

	static_assert(
		RuntimeSSE_1_7_104 == REL::Version{ 1, 7, 104, 0 },
		"DAF's 1.7.104 format-5 reference must remain local and exact");

	static_assert(
		MinimumAddressLibraryRuntime == REL::Version{ 1, 7, 99, 0 },
		"Dragon Aspect Flight requires CommonLib capability for Skyrim AE 1.7.99");

	// Referencing the enumerator is intentional: CommonLib versions that do
	// not parse the dense format-5 Address Library fail here at compile time.
	static_assert(
		AddressLibraryFormat == REL::IDDB::Format::SSEv5,
		"Dragon Aspect Flight requires CommonLib Address Library format 5 support");
}
