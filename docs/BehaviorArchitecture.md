# Dragon Aspect Flight Behavior Architecture

Dragon Aspect Flight uses a runtime-first animation contract:

- `BehaviorDataInjector` registers the player graph variables and events, so users do not need to run Nemesis or Pandora.
- The SKSE plugin sets `bDAF_DragonAspectActive`, `bDAF_FlightActive`, `bDAF_LaunchBoost`, and `iDAF_FlightState`.
- Open Animation Replacer animation files are intentionally external. Install `More Dragonic Dragon Aspect Can Fly` to supply the flight HKX files and its OAR conditions.

Earlier development builds bundled graph-gated OAR folders. Public packages should not redistribute those HKX files. The plugin still maintains airborne controller state during flight so the external `More Dragonic Dragon Aspect Can Fly` OAR package can select its installed animations without this repository carrying copied assets.

`Data/OptionalBehaviorGeneratorSupport/Pandora_Engine` contains a Pandora append patch that registers the same graph variables for users who explicitly prefer generated behavior files or who cannot use BDI.

`Data/OptionalBehaviorGeneratorSupport/Nemesis_Engine` is present as the Nemesis handoff location. Do not add handcrafted full-object `#NNNN` fragments unless they were generated from the exact target behavior graph. Nemesis patches replace serialized Havok objects by ID, so stale fragments can erase unrelated behavior changes from other mods. If a future Dragon Aspect Flight feature needs new topology that BDI cannot inject, generate those fragments from the current target `0_master` graph and place them there.
