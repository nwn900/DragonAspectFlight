# Dragon Aspect Flight

Dragon Aspect Flight is an SKSE/CommonLibSSE-NG plugin that lets the player manually fly while the full-strength Dragon Aspect shout is active.

Flight starts only when the third word of Dragon Aspect is active. The plugin handles flight physics, ascent/descent controls, shout pass-through during flight, weapon/magic suppression, and OAR graph variables for animation selection.

## Version

Current release: `1.0.0`

## Requirements

- Skyrim Special Edition with a matching SKSE/CommonLibSSE-NG runtime build. The public release DLL is built for the SE runtime target; rebuild from source if you need a different runtime target.
- SKSE64.
- Address Library compatible with the target runtime.
- Behavior Data Injector.
- Open Animation Replacer.
- Edmond's More Draconic Aspect - Become The Dragonborn.
- More Draconic Aspect Can Fly animation package, installed separately.

Dragon Aspect Flight does not bundle More Draconic `.hkx` animation files. More Draconic remains the owner/provider of the actual animation clips.

## Load Order

Install Dragon Aspect Flight after More Draconic Aspect Can Fly in your mod manager.

The release ships two config-only OAR submods under:

```text
meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly
```

Those patches use OAR's `overrideAnimationsFolder` to read HKX files from More Draconic's installed `Flying Mod` and `Elegant Flying Animations` folders. The Dragon Aspect Flight package should contain zero `.hkx` files.

## What The Release Ships

- `SKSE\Plugins\DragonAspectFlight.dll`
- `SKSE\Plugins\DragonAspectFlight.ini`
- `SKSE\Plugins\BehaviorDataInjector\DragonAspectFlight_BDI.json`
- OAR config-only patches for More Draconic's installed animation folders.

It does not ship:

- More Draconic animation `.hkx` files.
- Pandora/Nemesis behavior-generator files.
- A nested `Data` folder inside the mod root.

## Controls

Default hotkeys:

```ini
[Hotkeys]
Activation=0x30
Ascend=0x39
Descend=0x2A
```

Defaults are `B` for activation, `Space` for ascent, and `Left Shift` for descent. Values are DirectInput scan codes and can be written as decimal or hexadecimal. The release INI includes a commented key-code table for common keyboard keys.

Jump input is swallowed during flight so the player does not enter the vanilla jump state. If ascent is remapped away from `Space`, `Space` remains suppressed during flight but no longer raises flight height.

## Behavior And Animation Model

Behavior Data Injector registers these graph variables:

- `bDAF_DragonAspectActive`
- `bDAF_FlightActive`
- `bDAF_LaunchBoost`
- `bDAF_FlightShout`
- `iDAF_FlightState`

The SKSE plugin drives those variables while Dragon Aspect Flight is active. OAR uses them to select More Draconic's flight clips without depending on vanilla jump/sprint animation loops.

Users should not need to run Nemesis or Pandora for this mod.

## Installation

Install the release ZIP with MO2 or another mod manager. The ZIP root is already the game `Data` root, so it should expose `SKSE` and `meshes` at the top level after installation.

Make sure:

- Edmond's More Draconic Aspect - Become The Dragonborn is installed and enabled.
- More Draconic Aspect Can Fly is installed and enabled.
- Dragon Aspect Flight is enabled after More Draconic Aspect Can Fly.
- Behavior Data Injector and Open Animation Replacer are installed and enabled.

## Build From Source

This project expects `CommonLibSSE-NG` next to this folder by default:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build stages the DLL and deployable `Data` files under `build/bin` without HKX animation payloads.

For local diagnostics only, `DAF_MATERIALIZE_EXTERNAL_ANIMATION_LINKS=ON` can recreate the old hardlink staging layout from an installed More Draconic folder. Do not use that option for release packaging.
