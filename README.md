# Dragon Aspect Flight

SKSE/CommonLibSSE-NG plugin that lets the player manually fly while Dragon Aspect is active.

## What It Ships

- SKSE plugin source in `src/` and `include/`.
- Runtime animation state integration through `Data/SKSE/Plugins/BehaviorDataInjector/DragonAspectFlight_BDI.json`.
- Optional Pandora and Nemesis support files under `Data/OptionalBehaviorGeneratorSupport`.

This package does not redistribute flight animation HKX files. Install `More Dragonic Dragon Aspect Can Fly` alongside this mod for the Dragon Aspect flight animation set. Dragon Aspect Flight ships config-only OAR overrides that use those installed HKX files while removing the original More Draconic Aspect ESP condition.

## End User Dependencies

- SKSE.
- Address Library for SKSE Plugins.
- Behavior Data Injector.
- Open Animation Replacer.
- `More Dragonic Dragon Aspect Can Fly` for the flight animation HKX files and OAR folder layout. Its ESP is not required for Dragon Aspect Flight and can remain disabled unless another mod needs it.
- `More Draconic Aspect - Become The Dragonborn` is optional. When present, its visual Dragon Aspect effects can still be used, but Dragon Aspect Flight no longer needs its ESP for flight animation selection.

Place Dragon Aspect Flight after `More Dragonic Dragon Aspect Can Fly` in the mod manager's loose-file priority so these config-only OAR overrides win the `config.json` conflict while the external mod continues to provide the HKX animation files.

## Behavior Architecture

Behavior Data Injector is the primary path. Users should not need to run Nemesis or Pandora for the normal package.

The plugin drives graph variables such as `bDAF_DragonAspectActive`, `bDAF_FlightActive`, `bDAF_LaunchBoost`, and `iDAF_FlightState`. The packaged runtime state is provided through Behavior Data Injector; animation files are supplied by the external `More Dragonic Dragon Aspect Can Fly` mod and selected through Dragon Aspect Flight's config-only OAR compatibility overrides. See `docs/BehaviorArchitecture.md` for the design notes and fallback generator support.

## Build

This project expects `CommonLibSSE-NG` next to this folder by default:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build stages the DLL and deployable `Data` files under `build/bin`.

## Deploy

Copy the staged `SKSE` and `Data` contents into a mod manager mod folder. For the local Nolvus setup used during development, the active deployment target was:

```text
C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\[NoDelete] Dragon Aspect Flight
```
