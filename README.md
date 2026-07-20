# Dragon Aspect Flight

Dragon Aspect Flight is an SKSE/CommonLibSSE-NG plugin that lets the player manually fly while the full-strength Dragon Aspect shout is active.

Flight starts only when the third word of Dragon Aspect is active. The plugin handles flight physics, ascent/descent controls, shout pass-through during flight, weapon/magic suppression, and OAR graph variables for animation selection.

## Version

Current release: `1.2.0`

## Requirements

- Skyrim Special Edition with a matching SKSE/CommonLibSSE-NG runtime build. The public release DLL is built with SE and AE runtime support enabled.
- SKSE64.
- Address Library compatible with the target runtime.
- Behavior Data Injector.
- Open Animation Replacer.
- Edmond's More Draconic Aspect - Become The Dragonborn.
- More Draconic Aspect Can Fly animation package, installed separately.

Dragon Aspect Flight does not bundle More Draconic `.hkx` animation files. More Draconic remains the owner/provider of the actual animation clips.

## Optional: In-Game Settings Panel

[SKSE Menu Framework 3](https://www.nexusmods.com/skyrimspecialedition/mods/120352) (3.13+) is an optional dependency. If installed, a "Dragon Aspect Flight > Settings" page appears in the Mod Control Panel (default toggle key: F1). Click a hotkey button and press a key to rebind in-game (no scan-code lookup). Also edit flight physics, notifications, and magicka cost. Save writes back to the INI.

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

Default hotkeys and new INI sections:

```ini
[Hotkeys]
Activation=0x30
Ascend=0x39
Descend=0x2A

[Flight]
FlightSpeed=14.0
VerticalSpeed=24.0
LiftScale=1.0

[Notifications]
ShowReady=1
ShowExpired=1
SuppressInMenus=1

[Magicka]
Enabled=1
CostPerSecond=5.0
```

Defaults are `B` for activation, `Space` for ascent, and `Left Shift` for descent. Magicka drain is **on by default** at 5 points/sec. INI still accepts DirectInput scan codes (hex or decimal); with SMF3 you rebind by clicking the button and pressing a key.

### New v1.2.0 Features

- **Click-to-rebind hotkeys** in the SMF3 Settings panel (no manual scan codes). Esc cancels.
- **Magicka cost on by default** at 5 magicka/sec (still configurable / disableable).

### v1.1.0 Features

- **SKSE Menu Framework 3 integration**: optional in-game Settings panel for hotkeys, flight physics, notifications, and magicka cost. Changes can be saved to the INI.
- **No activation while typing in UI menus**: the flight hotkeys ignore key presses while the console, journal, inventory, magic, map, stats, book, MCM, or any text-input menu is open. Configurable via `[Notifications] SuppressInMenus`.
- **Dragon Aspect Shout cast notification**: a "Dragon Aspect Flight ready: press B to fly" notification fires the moment the player casts the full Dragon Aspect shout. An "exhausted" notification fires when the shout expires. Configurable via `[Notifications] ShowReady` and `ShowExpired`.
- **Magicka cost while flying**: drain magicka per second while airborne. When magicka runs out, the character descends safely to the ground instead of free-falling. Configurable via `[Magicka] Enabled` and `CostPerSecond`.

### Crash/Stutter Fixes (v1.1.0)

- Reset flight velocity smoothing state on flight start/stop. The previous `static` local persisted across sessions and carried stale velocity, causing a jerk on flight restart.
- Replace detached sheathe-wait thread with a `std::jthread` that respects stop tokens. The old thread had no shutdown control and could access freed memory on plugin unload.

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
