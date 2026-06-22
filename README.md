# Dragon Aspect Flight

SKSE/CommonLibSSE-NG plugin that lets the player manually fly while Dragon Aspect is active.

Flight activation requires the full-strength Dragon Aspect shout: the third word of power must be active.

## What It Ships

- SKSE plugin source in `src/` and `include/`.
- Runtime animation state integration through `Data/SKSE/Plugins/BehaviorDataInjector/DragonAspectFlight_BDI.json`.
- User-configurable keyboard scan codes in `Data/SKSE/Plugins/DragonAspectFlight.ini`.
- Open Animation Replacer selector configs under `Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - Bundled Flight Animations`.
- An optional link manifest and materializer under `tools/` for local diagnostics against the installed More Draconic animation package.
- Optional Pandora and Nemesis support files under `Data/OptionalBehaviorGeneratorSupport`.

## Animation Dependency

Dragon Aspect Flight source control does not bundle animation HKX files, and normal builds do not stage animation HKX files. The player should install the More Draconic Aspect Can Fly animation package separately so its OAR animation files are present in the virtual `Data` tree:

```text
meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly\Flying Mod
```

The Dragon Aspect Flight package ships SKSE, BDI, OAR selector configs, optional behavior-generator support, and INI files only. It should not ship More Draconic `.hkx` animation files.

The default local build path is:

```text
C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\[NoDelete] [001.00202] More Draconic Aspect Can Fly (With Collisions)\meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly
```

For local diagnostics only, `DAF_MATERIALIZE_EXTERNAL_ANIMATION_LINKS=ON` recreates the old hardlink staging layout from the installed More Draconic folder. Do not use that option for the release package unless you intentionally want to ship a local animation payload.

## Behavior Architecture

Behavior Data Injector is the primary path. Users should not need to run Nemesis or Pandora for the normal package.

The plugin drives graph variables such as `bDAF_DragonAspectActive`, `bDAF_FlightActive`, `bDAF_LaunchBoost`, and `iDAF_FlightState`; OAR uses those variables to select More Draconic flight movement clips without hijacking vanilla jump/sprint loops. See `docs/BehaviorArchitecture.md` for the design notes and fallback generator support.

## Configuration

Keyboard hotkeys are read once from:

```text
Data\SKSE\Plugins\DragonAspectFlight.ini
```

The values are DirectInput scan codes and can be decimal or hexadecimal:

```ini
[Hotkeys]
Activation=0x30
Ascend=0x39
Descend=0x2A
```

Defaults are `B` for activation, `Space` for ascent, and `Left Shift` for descent. Jump input is still swallowed during flight so the player does not enter the vanilla jump state, but only the configured ascent key raises flight height.

## Build

This project expects `CommonLibSSE-NG` next to this folder by default:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build stages the DLL and deployable `Data` files under `build/bin` without HKX animation payloads.

## Deploy

Copy the staged `SKSE` and `Data` contents into a mod manager mod folder. For the local Nolvus setup used during development, the active deployment target was:

```text
C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\[NoDelete] Dragon Aspect Flight
```

