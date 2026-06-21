# Dragon Aspect Flight

SKSE/CommonLibSSE-NG plugin that lets the player manually fly while Dragon Aspect is active.

## What It Ships

- SKSE plugin source in `src/` and `include/`.
- Runtime animation state integration through `Data/SKSE/Plugins/BehaviorDataInjector/DragonAspectFlight_BDI.json`.
- Open Animation Replacer selector configs under `Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - More Draconic External Selectors`.
- Optional Pandora and Nemesis support files under `Data/OptionalBehaviorGeneratorSupport`.

## Animation Dependency

Dragon Aspect Flight does not bundle animation HKX files. Its OAR selector configs load the active flight clips from the sibling load-order package:

```text
meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly\Flying Mod
```

Install the More Draconic Aspect Can Fly animation package so that folder is present in the player's virtual `Data` tree.

## Behavior Architecture

Behavior Data Injector is the primary path. Users should not need to run Nemesis or Pandora for the normal package.

The plugin drives graph variables such as `bDAF_DragonAspectActive`, `bDAF_FlightActive`, `bDAF_LaunchBoost`, and `iDAF_FlightState`; OAR uses those variables to select More Draconic flight movement clips without hijacking vanilla jump/sprint loops. See `docs/BehaviorArchitecture.md` for the design notes and fallback generator support.

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

