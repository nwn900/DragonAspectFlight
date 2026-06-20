# Dragon Aspect Flight Behavior Architecture

Dragon Aspect Flight uses a runtime-first animation contract:

- `BehaviorDataInjector` registers the player graph variables and events, so users do not need to run Nemesis or Pandora.
- The SKSE plugin sets `bDAF_DragonAspectActive`, `bDAF_FlightActive`, `bDAF_LaunchBoost`, and `iDAF_FlightState`.
- Open Animation Replacer selects bundled flight animations only when `bDAF_FlightActive == true` and `iDAF_FlightState > 0`.

The OAR folders intentionally do not require `IsInAir`. Flight start can spend a short time in a vanilla locomotion transition before the controller is fully airborne, so requiring `IsInAir` lets normal walk clips leak through. Dragon Aspect and the SKSE flight variables are the authoritative gates.

`Data/OptionalBehaviorGeneratorSupport/Pandora_Engine` contains a Pandora append patch that registers the same graph variables for users who explicitly prefer generated behavior files or who cannot use BDI.

`Data/OptionalBehaviorGeneratorSupport/Nemesis_Engine` is present as the Nemesis handoff location. Do not add handcrafted full-object `#NNNN` fragments unless they were generated from the exact target behavior graph. Nemesis patches replace serialized Havok objects by ID, so stale fragments can erase unrelated behavior changes from other mods. If a future Dragon Aspect Flight feature needs new topology that BDI cannot inject, generate those fragments from the current target `0_master` graph and place them there.
