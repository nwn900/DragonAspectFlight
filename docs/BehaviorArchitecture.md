# Dragon Aspect Flight Behavior Architecture

Dragon Aspect Flight uses a runtime-first animation contract:

- `BehaviorDataInjector` registers the player graph variables and events, so users do not need to run Nemesis or Pandora.
- The SKSE plugin sets character graph variables `bDAF_DragonAspectActive`, `bDAF_FlightActive`, `bDAF_LaunchBoost`, and `iDAF_FlightState`.
- The SKSE plugin sends throttled vanilla animation graph pulses (`moveStart`, `moveStop`, `IdleForceDefaultState`, and `JumpFall`) when flight visual state changes. These pulses wake the normal `MT_*` animation slots that Open Animation Replacer replaces; the plugin does not return to repeated `SprintStart` refreshes.
- Open Animation Replacer animation files are intentionally external. Install `More Dragonic Dragon Aspect Can Fly` to supply the flight HKX files and folder layout.
- Dragon Aspect Flight ships config-only OAR compatibility overrides at the same `More Dragonic Dragon Aspect Can Fly` animation folder paths. In MO2/VFS, these override only the `config.json` files while the HKX files continue to come from the external animation mod.
- Dragon Aspect Flight must load after the external animation mod in loose-file priority for this merge to work. If the external animation mod's original `config.json` files win instead, their `More Draconic Aspect - Become The Dragonborn ESL.esp:000804` condition applies again.

Earlier development builds bundled graph-gated OAR folders. Public packages should not redistribute those HKX files. The plugin still maintains airborne controller state during flight, and the shipped config-only OAR overrides select the external `More Dragonic Dragon Aspect Can Fly` animations through either the original `More Draconic Aspect - Become The Dragonborn ESL.esp:000804`/`IsInAir` route or the Dragon Aspect Flight BDI state route using `iDAF_FlightState > 0`.

`Data/OptionalBehaviorGeneratorSupport/Pandora_Engine` contains a Pandora append patch that registers the same graph variables for users who explicitly prefer generated behavior files or who cannot use BDI.

`Data/OptionalBehaviorGeneratorSupport/Nemesis_Engine` is present as the Nemesis handoff location. Do not add handcrafted full-object `#NNNN` fragments unless they were generated from the exact target behavior graph. Nemesis patches replace serialized Havok objects by ID, so stale fragments can erase unrelated behavior changes from other mods. If a future Dragon Aspect Flight feature needs new topology that BDI cannot inject, generate those fragments from the current target `0_master` graph and place them there.
