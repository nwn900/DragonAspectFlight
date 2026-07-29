# Dragon Aspect Flight

Dragon Aspect Flight is an SKSE/CommonLibSSE-NG plugin that lets the player manually fly while the full-strength Dragon Aspect shout is active.

Flight starts only when the third word of Dragon Aspect is active. The plugin handles flight physics, ascent/descent controls, shout pass-through during flight, weapon and magic combat, and OAR graph variables for animation selection.

## Version

Current release candidate: `1.7.0`

## Requirements

- Skyrim Special Edition or Skyrim VR with a matching SKSE/CommonLibSSE-NG runtime build. The public release DLL is built as one SE, AE, and VR-compatible binary.
- SKSE64.
- Address Library compatible with the target runtime.
- Behavior Data Injector.
- Open Animation Replacer.
- Edmond's More Draconic Aspect - Become The Dragonborn.
- More Draconic Aspect Can Fly animation package, installed separately.
- [Jumping Attack](https://www.nexusmods.com/skyrimspecialedition/mods/68043), with its behavior patch generated for the user's own mod list by Pandora or Nemesis.

Dragon Aspect Flight still does not bundle More Draconic's core flight `.hkx` files. It bundles the openly reusable NickNak aerial-combat clips, with attribution, so every animation path used by the Jumping Attack topology resolves from this package.

MCO/ADXP, Precision, and Payload Interpreter are optional. MCO is not required: Jumping Attack has vanilla and MCO behavior variants, and DAF uses whichever variant the user generated. One Click Power Attack is not claimed compatible because Jumping Attack's author documents that it can prevent midair power attacks.

## Optional: In-Game Settings Panel

[SKSE Menu Framework 3](https://www.nexusmods.com/skyrimspecialedition/mods/120352) (3.13+) is an optional dependency. If installed, a "Dragon Aspect Flight > Settings" page appears in the Mod Control Panel (default toggle key: F1). Click a binding, then press a keyboard key or controller button to rebind it; keyboard Escape cancels. The rebinder ignores mouse input and uses a short arming delay after the UI click. Duplicate flight bindings are rejected with a clear message. Also edit flight physics, notifications, and magicka cost. Save writes back to the INI.

## Load Order

Install Dragon Aspect Flight after More Draconic Aspect Can Fly and Jumping Attack in your mod manager.

Do not enable `More Draconic Aspect - Flight Combat` or `Mid Air Shouts - Shout while falling` alongside this release. Their plugins, scripts, and generated behaviors implement competing airborne state ownership. Dragon Aspect Flight does not override Dragon Aspect's duration.

The release ships two config-only OAR submods under:

```text
meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly
```

Those patches use OAR's `overrideAnimationsFolder` to read core flight HKX files from More Draconic's installed `Flying Mod` and `Elegant Flying Animations` folders.

The bundled NickNak clips use the direct paths requested by Jumping Attack's behavior graph. DAF does not replace ordinary `1hm_*`, `mco_*`, Stances, or grounded movement filenames. When flight combat starts, the DLL holds `bInJumpState=true`, so the generated aerial graph requests NickNak paths instead of entering a grounded combat framework.

## What The Release Ships

- `SKSE\Plugins\DragonAspectFlight.dll`
- `SKSE\Plugins\DragonAspectFlight.ini`
- `SKSE\Plugins\BehaviorDataInjector\DragonAspectFlight_BDI.json`
- `SKSE\Plugins\DragonAspectFlight-ThirdParty.txt`
- `SKSE\Plugins\DragonAspectFlight-AnimationAliases.txt`
- `SKSE\Plugins\DragonAspectFlight-AnimationHashes.txt`
- OAR config-only patches for More Draconic's installed animation folders.
- 96 direct NickNak HKX paths: the 81 published clips plus 15 documented, byte-identical directional aliases required by the behavior source.
- Two NickNak landing-effect NIF files.

It does not ship:

- More Draconic core flight animation `.hkx` files.
- Pandora/Nemesis behavior-generator files.
- ESP/ESL/ESM plugins or Papyrus scripts.
- Pre-generated behavior HKX files that would overwrite the user's merged behavior stack.
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

Supported gamepad bindings are D-Pad Up/Down/Left/Right, Start/Menu, Back/View, Left/Right Stick, Left/Right Bumper, A/Cross, B/Circle, X/Square, Y/Triangle, and Left/Right Trigger. Configure them manually in the INI or through the SMF3 rebinder. Custom bindings take precedence over Skyrim's vanilla semantic input while flying, so controller A, Y, bumpers, and triggers can drive flight instead of their normal action.

While flying, Ready Weapon toggles the flight-combat state. Attack, power-attack, bow, crossbow, staff, and spell inputs also activate combat automatically and continue into the generated aerial behavior graph. Sheathing exits the combat pose without ending flight. Shouts continue through Dragon Aspect Flight's own midair shout path.

Before activating flight combat, the DLL probes for Jumping Attack's `jumpAttack` graph variable. If it is missing, DAF blocks the combat input and displays a behavior-generation warning instead of allowing a grounded/Stances animation to walk on air.

Flight activation is refused while the player is mounted. This keeps the flight controller and its player-only OAR state mutually exclusive with horse, dragon-riding, and custom mount animation stacks.

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

### New v1.7.0 Aerial Topology Update

- **Midair melee attacks** for one-handed, left-handed, dual-wield, greatsword, battleaxe/warhammer, and unarmed combat.
- **Bow, crossbow, staff, and magic support** through Jumping Attack's ranged/magic aerial branches and bundled aiming clips.
- **Vanilla and MCO in one package**: DAF follows the Jumping Attack variant generated for the user's behavior stack and does not require MCO.
- **Stances isolation**: flight combat keeps `bInJumpState=true`, causing the aerial topology to request `Animations\NickNak\...` rather than grounded/Stances filenames.
- **Safe capability gate**: if the `jumpAttack` variable is absent, DAF suppresses the attack and explains that behaviors must be generated.
- **Complete clip namespace**: all 96 NickNak paths referenced by the behavior source are present; 15 missing directional paths are documented byte-identical aliases.
- **No Mid Air Shouts dependency and no duration plugin**: the package contains no ESP or Papyrus scripts, so the live load order's Dragon Aspect duration remains authoritative.

The withdrawn v1.6.0 design attempted to keep grounded combat states reachable and replace their clips through OAR. That allowed high-complexity Stances/MCO graphs to retain state ownership and has been removed.

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
- `bDAF_FlightCombatActive`
- `bDAF_LaunchBoost`
- `bDAF_FlightShout`
- `iDAF_FlightState`

The SKSE plugin drives those variables while Dragon Aspect Flight is active. OAR uses them only to select More Draconic's core flight clips. Combat uses the generated Jumping Attack topology: DAF verifies `jumpAttack` exists and sets `bInJumpState=true` only while flight combat is active.

BDI and OAR cannot add behavior states. Users must generate Jumping Attack for their own behavior stack with Pandora or Nemesis. DAF intentionally does not ship final behavior HKX files because they would overwrite merged outputs for TK Dodge, Jump Behavior Overhaul, MCO, Stances, and other behavior mods.

## Installation

Install the release ZIP with MO2 or another mod manager. The ZIP root is already the game `Data` root, so it should expose `SKSE` and `meshes` at the top level after installation.

Make sure:

- Edmond's More Draconic Aspect - Become The Dragonborn is installed and enabled.
- More Draconic Aspect Can Fly is installed and enabled.
- Dragon Aspect Flight is enabled after More Draconic Aspect Can Fly.
- Behavior Data Injector and Open Animation Replacer are installed and enabled.
- Jumping Attack is installed and its patch is present in the profile's generated behavior output.
- `More Draconic Aspect - Flight Combat` and `Mid Air Shouts - Shout while falling` are disabled.

## Build From Source

This project expects `CommonLibSSE-NG` next to this folder by default:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build stages the DLL and deployable `Data` files under `build/bin`, including the hash-pinned NickNak combat assets. The release audit verifies the 98 hash-manifest entries and validates the 96 HKX clips with the headless Skyrim HKX toolchain.

For local diagnostics only, `DAF_MATERIALIZE_EXTERNAL_ANIMATION_LINKS=ON` can recreate the old hardlink staging layout from an installed More Draconic folder. Do not use that option for release packaging.
