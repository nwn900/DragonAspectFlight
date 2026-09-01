# Dragon Aspect Flight

Dragon Aspect Flight is an SKSE/CommonLibSSE-NG plugin that lets the player manually fly while the full-strength Dragon Aspect shout is active.

Flight starts only when the third word of Dragon Aspect is active. The plugin handles flight physics, ascent/descent controls, shout pass-through during flight, weapon/magic suppression, and OAR graph variables for animation selection.

## Version

Current release: `1.6.0`

## Requirements

- Skyrim Special Edition or Skyrim VR with a matching SKSE/CommonLibSSE-NG runtime build. The public release DLL is built as one SE, AE, and VR-compatible binary.
- SKSE64.
- Address Library compatible with the target runtime.
- Behavior Data Injector.
- Open Animation Replacer.
- Edmond's More Draconic Aspect - Become The Dragonborn.
- More Draconic Aspect Can Fly, with its `Flying Mod` and `Elegant Flying Animations` OAR folders installed.

Dragon Aspect Flight does not redistribute those animations. Its OAR config-only overlays share the exact virtual folders used by More Draconic Aspect Can Fly, so OAR reads the dependency's installed HKX files directly.

## Optional: In-Game Settings Panel

[SKSE Menu Framework 3](https://www.nexusmods.com/skyrimspecialedition/mods/120352) (3.13+) is an optional dependency. If installed, a "Dragon Aspect Flight > Settings" page appears in the Mod Control Panel (default toggle key: F1). Click a binding, then press a keyboard key or controller button to rebind it; keyboard Escape cancels. The rebinder ignores mouse input and uses a short arming delay after the UI click. Duplicate flight bindings are rejected with a clear message. Also edit flight physics, notifications, and magicka cost. Save writes back to the INI.

## Load Order

The release installs two config-only overlays at More Draconic Aspect Can Fly's exact OAR animation paths:

```text
meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly\Flying Mod\config.json
meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly\Elegant Flying Animations\config.json
```

The overlays contain no HKX files and preserve More Draconic's original magic-effect plus `IsInAir` activation as an alternative to DAF's graph-variable activation. Dragon Aspect Flight must load after More Draconic so only these two `config.json` files win; every animation remains supplied by More Draconic through MO2's merged virtual Data tree.

## What The Release Ships

- `SKSE\Plugins\DragonAspectFlight.dll`
- `SKSE\Plugins\DragonAspectFlight.ini`
- `SKSE\Plugins\BehaviorDataInjector\DragonAspectFlight_BDI.json`
- Two config-only OAR patches that reference More Draconic Aspect Can Fly's installed animation folders.

It does not ship:

- Pandora/Nemesis behavior-generator files.
- A nested `Data` folder inside the mod root.

## Controls

Default hotkeys and new INI sections:

```ini
[Hotkeys]
ActivationDevice=Keyboard
ActivationCode=0x30
AscendDevice=Keyboard
AscendCode=0x39
DescendDevice=Keyboard
DescendCode=0x2A

[Flight]
FlightSpeed=14.0
VerticalSpeed=24.0
LiftScale=1.0

[Notifications]
ShowReady=1
ShowExpired=1
ShowShoutRequired=1
SuppressInMenus=1

[Magicka]
Enabled=1
CostPerSecond=5.0
```

Defaults are `B` for activation, `Space` for ascent, and `Left Shift` for descent. Magicka drain is **on by default** at 5 points/sec. Every flight binding stores both a device (`Keyboard` or `Gamepad`) and a code. Keyboard codes are DirectInput scan codes; gamepad codes use CommonLib's gamepad button IDs. For compatibility, legacy `Activation=`, `Ascend=`, and `Descend=` INI entries still load as keyboard bindings.

While flying, the Favorites and Magic menus temporarily reopen only Skyrim's fighting-control gate so a different shout can be assigned. Closing the menu restores the compatibility port's normal weapon and magic suppression.

Supported gamepad bindings are D-Pad Up/Down/Left/Right, Start/Menu, Back/View, Left/Right Stick, Left/Right Bumper, A/Cross, B/Circle, X/Square, Y/Triangle, and Left/Right Trigger. Configure them manually in the INI or through the SMF3 rebinder. Custom bindings take precedence over Skyrim's vanilla semantic input while flying, so controller A, Y, bumpers, and triggers can drive flight instead of their normal action.

### New v1.2.0 Features

- **Click-to-rebind hotkeys** in the SMF3 Settings panel (no manual scan codes). Esc cancels.
- **Magicka cost on by default** at 5 magicka/sec (still configurable / disableable).

### New v1.3.0 Controller Binding Update

- **Keyboard and gamepad bindings** for activation, ascent, and descent.
- **SMF3 raw-input rebinding** captures controller buttons as well as keyboard input; Escape cancels a keyboard rebind.
- **Binding-aware readiness notification** shows the configured activation control rather than assuming `B`.

### New v1.4.0 Multi-Runtime Update

- **Single DLL for SE, AE, and VR**: the build enables all three CommonLibSSE-NG runtime targets.
- VR still requires SKSEVR and the VR Address Library matching Skyrim VR 1.4.15.

### New v1.5.0 Notification Update

- **Optional shout-required message**: set `ShowShoutRequired=0` in `[Notifications]` to silence the message shown when flight is activated before the full Dragon Aspect shout is active.

### New v1.6.0 Compatibility Update

- **Skyrim 1.7.99 and 1.7.104 support** in the unified SE, AE, and VR DLL.
- **Mid-flight shout reassignment** through the Favorites or Magic menu.
- **External More Draconic animations** remain referenced through exact OAR paths and are not bundled.

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

The SKSE plugin drives those variables while Dragon Aspect Flight is active. OAR uses them to select More Draconic Aspect Can Fly's installed flight clips without copying or redistributing the HKX files.

Users should not need to run Nemesis or Pandora for this mod.

## Installation

Install the release ZIP with MO2 or another mod manager. The ZIP root is already the game `Data` root, so it should expose `SKSE` and `meshes` at the top level after installation.

Make sure:

- Edmond's More Draconic Aspect - Become The Dragonborn is installed and enabled.
- More Draconic Aspect Can Fly is installed and enabled with its `Flying Mod` and `Elegant Flying Animations` OAR folders intact.
- Behavior Data Injector and Open Animation Replacer are installed and enabled.

## Build From Source

This project expects `CommonLibSSE-NG` next to this folder by default:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build stages the DLL and deployable config-only `Data` files under `build/bin`. No HKX animation payload is bundled.
