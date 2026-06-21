# Dragon Aspect Flight

SKSE/CommonLibSSE-NG plugin that lets the player manually fly while Dragon Aspect is active.

## What It Ships

- SKSE plugin source in `src/` and `include/`.
- Runtime animation state integration through `Data/SKSE/Plugins/BehaviorDataInjector/DragonAspectFlight_BDI.json`.
- Open Animation Replacer selector configs under `Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - Bundled Flight Animations`.
- A build-time link manifest and materializer under `tools/` that recreates the working 0.8.8 OAR animation filename layout from the installed More Draconic animation package.
- Optional Pandora and Nemesis support files under `Data/OptionalBehaviorGeneratorSupport`.

## Animation Dependency

Dragon Aspect Flight source control does not bundle animation HKX files. During build or local deployment, `tools/MaterializeExternalAnimationLinks.ps1` creates hardlinks for the exact 0.8.8 OAR replacement filenames, using byte-identical clips from the installed More Draconic package:

```text
meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly\Flying Mod
```

Install the More Draconic Aspect Can Fly animation package so that folder is present in the player's virtual `Data` tree.

The default local build path is:

```text
C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\[NoDelete] [001.00202] More Draconic Aspect Can Fly (With Collisions)\meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly
```

Override it with the CMake cache variable `DAF_MORE_DRACONIC_OAR_ROOT` if the More Draconic OAR root is installed elsewhere.

## Behavior Architecture

Behavior Data Injector is the primary path. Users should not need to run Nemesis or Pandora for the normal package.

The plugin drives graph variables such as `bDAF_DragonAspectActive`, `bDAF_FlightActive`, `bDAF_LaunchBoost`, and `iDAF_FlightState`; OAR uses those variables to select More Draconic flight movement clips without hijacking vanilla jump/sprint loops. See `docs/BehaviorArchitecture.md` for the design notes and fallback generator support.

## Build

This project expects `CommonLibSSE-NG` next to this folder by default:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build stages the DLL and deployable `Data` files under `build/bin`. It also materializes the 0.8.8 animation replacement filenames as hardlinks to the external More Draconic files, so Open Animation Replacer sees the same animation surface as the working 0.8.8 package without storing HKX binaries in this repository.

## Deploy

Copy the staged `SKSE` and `Data` contents into a mod manager mod folder. For the local Nolvus setup used during development, the active deployment target was:

```text
C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\[NoDelete] Dragon Aspect Flight
```

