# Dragon Aspect Flight

Dragon Aspect Flight is an SKSE/CommonLibVR-NG plugin that lets the player manually fly while the full-strength Dragon Aspect shout is active.

Flight starts only when the third word of Dragon Aspect is active. The plugin handles flight physics, ascent/descent controls, shout pass-through during flight, weapon and magic combat, and OAR graph variables for animation selection.

## Version

Current diagnostic candidate: `1.8.1`

## Requirements

- Skyrim Special Edition, Anniversary Edition, or Skyrim VR with matching SKSE. The candidate DLL is built once against the maintained `alandtse/CommonLibVR` `ng` branch with SE, AE, and VR enabled.
- SKSE64 for SE/AE, or SKSEVR for VR.
- Address Library compatible with the target runtime.
- Behavior Data Injector.
- Open Animation Replacer.
- Edmond's More Draconic Aspect - Become The Dragonborn.

[Jumping Attack](https://www.nexusmods.com/skyrimspecialedition/mods/68043) is not a runtime requirement. DAF deliberately keeps `bInJumpState` clear and lets the normal vanilla or MCO combat graph handle draw, sheathe, block, bash, shout, bow, crossbow, staff, and magic transitions. The behavior-free path does not require Nemesis or Pandora.

Dragon Aspect Flight bundles a credited Flying Mod Beta flight pose, aerial melee clips from NickNak's openly reusable Jumping Attack assets, credited xp32/Neumeria magic and staff clips, and a seven-file Animated Armoury quarterstaff donor subset. Flight-safe block, bash, draw, sheathe, and shout composites preserve the original upper-body timing and annotations while replacing only lower-body/root tracks. More Draconic Aspect Can Fly is no longer an external animation-file dependency. MCO/ADXP, Precision, Payload Interpreter, Jumping Attack, Stances, Nemesis, and Pandora are optional. MCO users and non-MCO users receive the same package.

## Optional: In-Game Settings Panel

[SKSE Menu Framework 3](https://www.nexusmods.com/skyrimspecialedition/mods/120352) (3.13+) is an optional dependency. If installed, a "Dragon Aspect Flight > Settings" page appears in the Mod Control Panel (default toggle key: F1). Click a binding, then press a keyboard key or controller button to rebind it; keyboard Escape cancels. The rebinder ignores mouse input and uses a short arming delay after the UI click. Duplicate flight bindings are rejected with a clear message. Also edit flight physics, notifications, and magicka cost. Save writes back to the INI.

## Load Order

Install Dragon Aspect Flight after other animation replacers. Its OAR priorities are deliberately above the enabled Nolvus animation stack, but normal mod-file precedence still matters if another mod writes into DAF's own folder.

Do not enable `More Draconic Aspect - Flight Combat` or `Mid Air Shouts - Shout while falling` alongside this release. Their plugins, scripts, and generated behaviors implement competing airborne state ownership. Dragon Aspect Flight does not override Dragon Aspect's duration.

The release ships eleven equipment-aware OAR submods under:

```text
meshes\actors\character\animations\OpenAnimationReplacer\Dragon Aspect Flight
```

The fallback, unarmed, one-handed, dual-wield, greatsword, axe/warhammer, quarterstaff, bow, crossbow, magic, and staff families replace the original filenames that the vanilla, Stances, and MCO graphs actually request. Quarterstaves receive an explicit `WeapTypeQtrStaff` keyword route above the general two-handed family. That filename coverage is what prevents Stances from retaining a grounded walk or idle just because it uses `1hm_*`, `2hw_*`, `dw*`, `mco_*`, `mag_*`, or another equipment-specific original instead of generic `mt_*`.

The equipment routes use equipped type/keyword plus active DAF flight state; `bDAF_FlightCombatActive` remains diagnostic rather than becoming a fragile timing gate. While flying, Ready Weapon first passes through to Skyrim and installed equipment-state/input mods. DAF polls the actor's real weapon state and calls Skyrim's relocated `DrawWeaponMagicHands` virtual only as a delayed fallback if no compatible transition completes before the bounded deadline. Draw/sheathe and shout clips are aerial composites rather than static loops, so their weapon-placement and shout-release annotations remain intact. Bow release and crossbow reload remain untouched.

All neutral and equipment locomotion uses the actual credited Flying Mod Beta idle pose rather than a falling or grounded-walking clip. DAF does not use the donor's jump/fall animation, avoiding the animation path suspected of producing the recurring invisible-platform jump.

## Compatibility Boundaries

DAF's OAR conditions are limited to the player and require DAF flight to be active, so they do not change NPC or grounded animation winners. The same archive supports vanilla combat and MCO and never takes ownership of a generated Jumping Attack behavior state.

The bundled namespace covers vanilla originals plus every combat original and all non-transition locomotion originals found in the enabled Nolvus Stances/MCO providers audited for v1.8.x. A custom moveset can still escape DAF if it requests a different original filename, and another replacer can win if it uses a priority above `2147483610`. Input or behavior mods that consume attacks before Skyrim's normal event path also require an explicit in-game check. SE, AE, and VR share one compiled binary, but each runtime still needs launch and gameplay validation; successful compilation is not proof of VR behavior.

## What The Release Ships

- `SKSE\Plugins\DragonAspectFlight.dll`
- `SKSE\Plugins\DragonAspectFlight.ini`
- `SKSE\Plugins\BehaviorDataInjector\DragonAspectFlight_BDI.json`
- `SKSE\Plugins\DragonAspectFlight-ThirdParty.txt`
- `SKSE\Plugins\DragonAspectFlight-AnimationAliases.txt`
- `SKSE\Plugins\DragonAspectFlight-AnimationHashes.txt`
- `SKSE\Plugins\DragonAspectFlight-AnimationCoverage.json`
- Eleven DAF-owned OAR families with 1,167 flight-scoped HKX aliases (about 107 MiB).
- One credited Flying Mod Beta flight-pose donor used for neutral and equipment locomotion.
- Credited NickNak aerial melee clips mapped into the OAR families; repository-only source paths are not duplicated into the installed mod.
- Credited xp32/Neumeria magic and staff clips used by the magic and staff families.
- Seven credited Animated Armoury quarterstaff donors; only their generated flight composites are installed.
- Generated Skyrim action composites for block, bash, draw, sheathe, and shout. Raw vanilla source files are not stored in the repository.

It does not ship:

- More Draconic's full animation set; only the credited Flying Mod Beta flight-idle donor is bundled.
- Pandora/Nemesis behavior-generator files.
- Raw Bethesda, Stances, MCO, or other installed-mod animation source sets.
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

While flying, Ready Weapon passes through normally while DAF observes the requested target; the relocated native draw/sheathe call is a delayed recovery path, not the first owner. Attack, power-attack, bow, crossbow, staff, and spell inputs also activate combat automatically. Sheathing clears any held block state and exits the combat pose without ending flight, including during controlled descent. Shouts and attacks remain pass-through inputs during descent.

DAF always keeps `bInJumpState=false` while its character controller owns flight. This is intentional: the generated Jumping Attack branch could attack but could not reliably transition to draw, sheathe, block, bash, or shout. Normal vanilla/MCO input therefore remains authoritative, while DAF's high-priority OAR families own only flight-scoped visual replacements.

If flight is activated while weapons or magic are already drawn, DAF keeps them drawn and activates the appropriate combat path immediately. Missing generated behavior no longer cancels flight.

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
- **Bow, crossbow, staff, and magic support** through the matching equipment-aware flight families.
- **Vanilla and MCO in one package**: MCO is optional and no separate binary is required.
- **Behavior-free topology**: DAF does not require or enter Jumping Attack's generated behavior branch.
- **Complete clip namespace**: all 96 NickNak paths referenced by the behavior source are present; 15 missing directional paths are documented byte-identical aliases.
- **No Mid Air Shouts dependency and no duration plugin**: the package contains no ESP or Papyrus scripts, so the live load order's Dragon Aspect duration remains authoritative.

The withdrawn v1.6.0 design tried to solve this with a small generic OAR set. It failed because Stances and MCO requested different equipment-specific original filenames, so DAF was not a candidate regardless of priority. v1.8.0 replaces that incomplete namespace with explicit per-family coverage.

### New v1.8.0 Event-Driven Topology Fix

- **Behavior-free combat path**: Jumping Attack, Nemesis, and Pandora are not requirements. Normal vanilla/MCO input always continues into DAF's flight-scoped animation namespace.
- **Normal transition ownership**: DAF keeps `bInJumpState=false` so Skyrim's combat graph remains able to draw, sheathe, block, bash, and shout.
- **Stances winner fix**: ten high-priority OAR families cover the equipment-specific originals actually requested by the enabled Stances and MCO stacks; priority alone was not sufficient when DAF only supplied generic `mt_*` filenames.
- **Open-permission magic assets**: credited xp32/Neumeria casting and staff clips are bundled for magic-specific paths.
- **No forced activation sheathe**: weapons, staves, and magic stay drawn when flight begins; already-drawn equipment enters the aerial topology immediately.
- **Working mid-flight sheathe**: Ready Weapon now passes through to Skyrim instead of DAF calling a draw helper directly. Sheathing and drawing remain available during controlled descent.
- **No artificial launch bump**: the old unconditional upward velocity injection was removed. Height changes now come only from explicit ascent, boost, or controller physics.
- **Stuck-ascent protection**: menu suppression clears held flight inputs, every event in an input chain is processed before consumption, and pending launch boost is cleared on stop.
- **True flight pose**: neutral/equipment locomotion now uses the credited Flying Mod Beta flight idle instead of NickNak fall clips, eliminating the RC4 stuck-falling presentation. The donor's jump/fall clip is not used.
- **Whirlwind Sprint handoff**: after shout release DAF temporarily stops writing controller velocity, allowing the shout's forward impulse to move the player.
- **Compact animation layout**: a single root scope and non-duplicated generic locomotion reduce the OAR tree from 4,209 HKX files (about 1.87 GiB) to 899 HKX files (about 90.46 MiB), a 95% reduction.
- **One authoritative version**: CMake, the modern SKSE export, the legacy SKSE query export, logs, and the settings UI all report `1.8.0`.
- **Maintained unified runtime base**: the DLL builds against `alandtse/CommonLibVR` `ng` with SE, AE, and VR enabled.

### New v1.8.1 Native State and Action Composite Fix

- **Restorable baseline**: the user-tested v1.8.0 candidate remains tagged as `v1.8.0-rc-user-tested-20260811`; v1.8.1 is a separate diagnostic candidate.
- **Actual weapon-state reconciliation**: DAF adopts the useful polling idea from More Draconic Aspect - Flight Combat 2.0.0, but not its Nemesis behavior edits, hard Mid Air Shouts dependency, control disabling, or invisible collision platform.
- **Cooperative draw/sheathe recovery**: vanilla and equipment-state/input mods see Ready Weapon first. DAF observes `ActorState::WEAPON_STATE`, replaces stale opposite-direction requests atomically, preserves an already-armed compatible fallback, delays while a draw/sheathe transition is progressing, and invokes the relocated SE/AE/VR actor virtual only as a one-shot recovery fallback before timeout.
- **Flight-owned block intent**: shields, two-handed weapons (including keyword-routed quarterstaves), and a one-handed weapon with an empty off hand receive synchronized `wantBlocking`, `IsBlocking`, `blockStart`, and `blockStop` state while vanilla/MCO gameplay input still passes through.
- **Aerial block and bash**: block, bash, hit, and transition originals use validated lower-body flight composites instead of static flight idle or grounded full-body clips.
- **Quarterstaff route**: `WeapTypeQtrStaff` has a dedicated priority-`2147483610` family and seven Animated Armoury-derived block/draw composites.
- **Installed-mod action aliases**: Maxsu block-hit and Dynamic Bow Animation `xpe0_*` originals map to the matching flight composites. They remain inert on modlists that do not request those names.
- **Aerial draw, sheathe, and shout**: vanilla upper-body timing and annotations are preserved, while the root and lower body come from the flight pose. The upper-body-only crossbow shout offset is intentionally left untouched.
- **Descent input fix**: attacks, draw/sheathe, and shouts are no longer swallowed during magicka-exhaustion descent. Whirlwind Sprint can temporarily own velocity during descent as well.
- **Persistent diagnostics**: the SKSE log appends and rotates at 5 MiB x 3 files instead of truncating on launch. Snapshots include weapon transition, attack, block, quarterstaff, graph, controller, velocity, and equipment state.
- **Compact package**: 1,167 installed HKX aliases occupy about 107 MiB—well below the former 1.87 GiB stack.

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

The SKSE plugin drives those variables while Dragon Aspect Flight is active. OAR uses them to select DAF's bundled root-stable base loop and the matching combat-ready equipment branch. The conditions are player-only and flight-only, so DAF does not replace NPC or grounded animation winners.

DAF keeps `bInJumpState=false` and the normal vanilla/MCO attack event continues. BDI and OAR cannot add behavior states, so the behavior-free route still needs in-game validation for every input family; the package intentionally does not ship final behavior HKX files that would overwrite merged outputs for TK Dodge, Jump Behavior Overhaul, MCO, Stances, or other behavior mods.

## Installation

Install the release ZIP with MO2 or another mod manager. The ZIP root is already the game `Data` root, so it should expose `SKSE` and `meshes` at the top level after installation.

Make sure:

- Edmond's More Draconic Aspect - Become The Dragonborn is installed and enabled.
- Behavior Data Injector and Open Animation Replacer are installed and enabled.
- `More Draconic Aspect - Flight Combat` and `Mid Air Shouts - Shout while falling` are disabled.

Jumping Attack may remain installed for other gameplay, but DAF does not enter its generated topology. Do not install another mod into DAF's own OAR folder.

## Build From Source

This project expects the maintained `alandtse/CommonLibVR` `ng` branch in a sibling folder named `CommonLibVR-NG` by default. Its CMake package dependencies must be available through vcpkg or `CMAKE_PREFIX_PATH`:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build stages the DLL and deployable `Data` files under `build/Data`, including the hash-pinned animation stack. The current candidate manifest contains 1,167 installed HKX assets. The generator checks packfile headers, attempts TAGXML deserialization with `hkxcmd`, and round-trips every generated composite through PyNifly while verifying duration, track count, bone binding, and annotations.

Regenerate the eleven DAF-owned equipment families with:

```powershell
python tools\BuildFlightAnimationStack.py `
  --hkxcmd <path-to-hkxcmd.exe> `
  --pynifly-hkx-dir <path-to-PyNifly-hkx> `
  --vanilla-animation-root <extracted-Data\meshes\actors\character\animations>
```

The vanilla source root is a local build input and is not committed. The tool creates flight-safe composites, records exact filename coverage, and refreshes the SHA-256 manifest.
