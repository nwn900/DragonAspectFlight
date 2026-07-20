#pragma once

namespace DragonAspectFlight::UI
{
	// Registers the "Dragon Aspect Flight" section + "Settings" page in the
	// SKSE Menu Framework Mod Control Panel. No-op if SMF is not installed.
	void Register();

	// SMF render callback (void __stdcall signature required by SKSEMenuFramework::AddSectionItem).
	void __stdcall Render();
}
