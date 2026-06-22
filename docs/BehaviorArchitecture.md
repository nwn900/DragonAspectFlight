# Dragon Aspect Flight Behavior Architecture

Dragon Aspect Flight uses a runtime-first animation contract:

- `BehaviorDataInjector` registers the player graph variables and events, so users do not need to run Nemesis or Pandora.
- The SKSE plugin sets `bDAF_DragonAspectActive`, `bDAF_FlightActive`, `bDAF_LaunchBoost`, and `iDAF_FlightState`.
- Open Animation Replacer selects More Draconic's real animation folders only when `bDAF_FlightActive == true` and `iDAF_FlightState > 0`.

The OAR folders intentionally do not require `IsInAir`. Flight start can spend a short time in a vanilla locomotion transition before the controller is fully airborne, so requiring `IsInAir` lets normal walk clips leak through. Dragon Aspect and the SKSE flight variables are the authoritative gates.

The working 0.8.8 package depended on OAR seeing 351 specific replacement filenames. Several of those files were deliberate aliases of the same More Draconic clips, such as idle hover and midair shout slots. Release packages do not ship those HKX files. The installed More Draconic animation mod remains the animation provider in the player's virtual `Data` tree.

`tools/dragon-aspect-flight-animation-links.json` and `tools/MaterializeExternalAnimationLinks.ps1` are retained only for local diagnostics. CMake will materialize hardlinks only when `DAF_MATERIALIZE_EXTERNAL_ANIMATION_LINKS=ON` is set explicitly. Do not use that option for release packaging.

The Dragon Aspect Flight deployable provides the SKSE plugin, BDI graph variable registration, OAR config patches, and INI configuration. It does not ship optional Pandora/Nemesis behavior-generator support.

The OAR config patches are placed at More Draconic's virtual paths:

- `OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly\Flying Mod\config.json`
- `OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly\Elegant Flying Animations\config.json`

Dragon Aspect Flight must load after the More Draconic animation mod. In MO2's virtual filesystem, Dragon Aspect Flight's `config.json` files win the loose-file conflict while More Draconic's HKX files remain available in the same folders.
