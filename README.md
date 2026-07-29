# Dragon Aspect Flight

Dragon Aspect Flight is an SKSE/CommonLibSSE-NG plugin that lets the player manually fly while the full-strength Dragon Aspect shout is active.

Flight starts only when the third word of Dragon Aspect is active. The plugin handles flight physics, ascent/descent controls, shout pass-through during flight, weapon and magic combat, and OAR graph variables for animation selection.

## Version

Current release: `1.6.0`

## Requirements

- Skyrim Special Edition or Skyrim VR with a matching SKSE/CommonLibSSE-NG runtime build. The public release DLL is built as one SE, AE, and VR-compatible binary.
- SKSE64.
- Address Library compatible with the target runtime.
- Behavior Data Injector.
- Open Animation Replacer.
- Edmond's More Draconic Aspect - Become The Dragonborn.
- More Draconic Aspect Can Fly animation package, installed separately.

Dragon Aspect Flight still does not bundle More Draconic's core flight `.hkx` files. It does bundle the openly reusable NickNak aerial-combat clips, with attribution, for the new combat layer.

MCO/ADXP, One Click Power Attack, Precision, and Payload Interpreter are supported but are not Dragon Aspect Flight requirements. The release includes both vanilla and MCO animation aliases. Users without MCO use Skyrim's normal attack behavior.

## Optional: In-Game Settings Panel

[SKSE Menu Framework 3](https://www.nexusmods.com/skyrimspecialedition/mods/120352) (3.13+) is an optional dependency. If installed, a "Dragon Aspect Flight > Settings" page appears in the Mod Control Panel (default toggle key: F1). Click a binding, then press a keyboard key or controller button to rebind it; keyboard Escape cancels. The rebinder ignores mouse input and uses a short arming delay after the UI click. Duplicate flight bindings are rejected with a clear message. Also edit flight physics, notifications, and magicka cost. Save writes back to the INI.

## Load Order

Install Dragon Aspect Flight after More Draconic Aspect Can Fly in your mod manager.

Do not enable the donor `More Draconic Aspect - Flight Combat` package alongside this release. Its `JumpAttack.esp`, `JumpAtkVioLens.esp`, `DragonAspect_AerialCombat_Patch.esp`, `MidAirShouts.esp`, Papyrus scripts, and generated behaviors implement a competing flight-combat stack. Dragon Aspect Flight 1.6.0 replaces that stack without an ESP and does not override Dragon Aspect's duration.

The release ships two config-only OAR submods under:

```text
meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly
```

Those patches use OAR's `overrideAnimationsFolder` to read core flight HKX files from More Draconic's installed `Flying Mod` and `Elegant Flying Animations` folders.

The separate `Dragon Aspect Flight - Flight Combat` OAR tree contains the bundled combat clips. Its conditions are player-only and require both `bDAF_FlightActive` and `bDAF_FlightCombatActive`, so it cannot replace normal grounded combat.

## What The Release Ships

- `SKSE\Plugins\DragonAspectFlight.dll`
- `SKSE\Plugins\DragonAspectFlight.ini`
- `SKSE\Plugins\BehaviorDataInjector\DragonAspectFlight_BDI.json`
- `SKSE\Plugins\DragonAspectFlight-ThirdParty.txt`
- OAR config-only patches for More Draconic's installed animation folders.
- 509 OAR combat HKX aliases for vanilla and MCO combat, generated from 18 hash-pinned NickNak source clips, including distinct regular and power attacks plus weapon-family ready stances.

It does not ship:

- More Draconic core flight animation `.hkx` files.
- Pandora/Nemesis behavior-generator files.
- ESP/ESL/ESM plugins or Papyrus scripts.
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

While flying, Ready Weapon toggles the flight-combat state. Attack, power-attack, bow, crossbow, staff, and spell inputs also activate combat automatically and continue to Skyrim's normal combat input sink. Sheathing exits the combat pose without ending flight. Shouts continue through Dragon Aspect Flight's own midair shout path.

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

### New v1.6.0 Flight Combat Update

- **Midair melee attacks** for one-handed, left-handed, dual-wield, greatsword, battleaxe/warhammer, and unarmed combat.
- **Bow, crossbow, staff, and magic support** using the bundled airborne aiming pose while Skyrim or the installed combat framework retains the actual shot/cast animation.
- **Vanilla and MCO in one package**: vanilla filenames use the original aerial clips; MCO aliases add verified combo, recovery, power-chain, Precision, and attack-stop annotations.
- **No new Nemesis or Pandora behavior output**: BDI registers the combat selector and the DLL keeps the existing combat state machine reachable while its controller owns airborne physics.
- **No Mid Air Shouts dependency and no duration plugin**: the package contains no ESP or Papyrus scripts, so the live load order's Dragon Aspect duration remains authoritative.

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

The SKSE plugin drives those variables while Dragon Aspect Flight is active. OAR uses them to select More Draconic's core flight clips and the bundled combat clips without depending on a new jump-attack behavior patch. During flight combat, the DLL keeps the vanilla/MCO combat graph reachable while maintaining the physical controller's in-air state.

Users should not need to run Nemesis or Pandora for this mod.

## Installation

Install the release ZIP with MO2 or another mod manager. The ZIP root is already the game `Data` root, so it should expose `SKSE` and `meshes` at the top level after installation.

Make sure:

- Edmond's More Draconic Aspect - Become The Dragonborn is installed and enabled.
- More Draconic Aspect Can Fly is installed and enabled.
- Dragon Aspect Flight is enabled after More Draconic Aspect Can Fly.
- Behavior Data Injector and Open Animation Replacer are installed and enabled.
- The donor `More Draconic Aspect - Flight Combat` mod and its four plugins are disabled.

## Build From Source

This project expects `CommonLibSSE-NG` next to this folder by default:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build stages the DLL and deployable `Data` files under `build/bin`, including the already-generated and verified combat HKX payloads.

To reproduce the release clips from the permitted source assets, download `serde-hkx` 2.0.0 and run:

```powershell
python tools\BuildFlightCombatAnimations.py --hkxc C:\path\to\hkxc.exe
```

The validated Windows archive is `serde-hkx-cli-x86_64-pc-windows-msvc-extra_fmt.zip` with SHA-256 `2AED0108E3EF3B445E169371DA470B1523DC770416012837E26CA1936256A891`.

The tool verifies every source SHA-256, reconstructs the donor behavior's hit and Precision timing inside the dedicated power-attack clips, injects the required MCO/Payload Interpreter annotations into MCO aliases, writes 64-bit Skyrim HKX files, and runs `hkxc verify` over the complete output. Regular vanilla aliases and ready poses remain byte-identical copies; generated power aliases preserve the source transforms exactly.

For local diagnostics only, `DAF_MATERIALIZE_EXTERNAL_ANIMATION_LINKS=ON` can recreate the old hardlink staging layout from an installed More Draconic folder. Do not use that option for release packaging.
