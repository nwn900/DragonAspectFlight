#!/usr/bin/env python3
"""Build DAF's flight-scoped OAR locomotion and aerial-combat stack."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass

from AerializeHkx import aerialize, load_backend


ROOT_SCOPE = pathlib.Path()


def names(value: str) -> set[str]:
    return {token.lower() for token in value.split() if token}


def standard_motion(prefix: str, *, idle: bool = True) -> set[str]:
    result: set[str] = set()
    if idle:
        result.add(f"{prefix}idle.hkx")
    for gait in ("run", "walk"):
        for direction in (
            "backward",
            "backwardleft",
            "backwardright",
            "forward",
            "forwardleft",
            "forwardright",
            "left",
            "right",
            "strafeleft",
            "straferight",
        ):
            result.add(f"{prefix}{gait}{direction}.hkx")
    for direction in ("left180", "left60", "right180", "right60"):
        result.add(f"{prefix}turn{direction}.hkx")
    result.update({f"{prefix}sprint.hkx", f"{prefix}sprintforward.hkx"})
    return result


GENERIC_MOTION = standard_motion("mt_") | names(
    """
    mt_jump.hkx mt_jumpfall.hkx mt_jumpfallleft.hkx mt_jumpfallright.hkx
    mt_jumpfast.hkx mt_jumpfastleft.hkx mt_jumpfastright.hkx
    mt_jumpland.hkx mt_jumplandhighimpact.hkx mt_jumplandimpact.hkx
    mt_jumplandleft.hkx mt_jumplandright.hkx mt_jumpleft.hkx mt_jumpright.hkx
    mt_sprintforwardsword.hkx
    sneakmtidle.hkx sneak_turnleft180.hkx sneak_turnleft60.hkx
    sneak_turnright180.hkx sneak_turnright60.hkx
    sneakrun_backward.hkx sneakrun_bckwrdleft.hkx sneakrun_bckwrdright.hkx
    sneakrun_forward.hkx sneakrun_frwrdleft.hkx sneakrun_frwrdright.hkx
    sneakrun_left.hkx sneakrun_right.hkx
    sneakwalk_bckward.hkx sneakwalk_bckwrdleft.hkx sneakwalk_bckwrdright.hkx
    sneakwalk_forward.hkx sneakwalk_fwrwrdleft.hkx sneakwalk_frwrdright.hkx
    sneakwalk_left.hkx sneakwalk_right.hkx
    """
)

UNARMED_MOTION = standard_motion("h2h_") | names("h2h_idle.hkx")
ONE_HANDED_MOTION = standard_motion("1hm_") | names(
    """
    1hm_idle.hkx 1hm_blockidle.hkx sneak1hm_idle.hkx shd_blockidle.hkx
    """
)
DUAL_WIELD_MOTION = ONE_HANDED_MOTION | names(
    """
    dw_shieldadjustment.hkx dw_sprintforwardsword.hkx
    dw1hm1hmidle.hkx dw1hm1hmblockidle.hkx dw1hm1hmmovingblockidle.hkx
    """
)
GREATSWORD_MOTION = standard_motion("2hm_") | names(
    """
    2hm_blockidle.hkx 2hm_runforward2.hkx 2hm_sprintforwardsword.hkx
    """
)
AXE_WARHAMMER_MOTION = standard_motion("2hw_") | names(
    """
    2hw_blockidle.hkx 2hw_runarmblend.hkx 2hw_runarmblend-1.hkx
    2hw_runarmblend2.hkx 2hw_walkarmblend.hkx 2hw_sprintforwardsword.hkx
    """
)
BOW_MOTION = standard_motion("bow_") | standard_motion("bowdrawn_", idle=False) | names(
    """
    bow_idleheld.hkx bow_idledrawn.hkx bow_blockidle.hkx
    xpe0_bow_idleheld.hkx xpe0_bow_idledrawn.hkx
    sneakbow_idledrawn.hkx xpe0_sneakbow_idledrawn.hkx
    """
)
CROSSBOW_MOTION = standard_motion("crossbow_") | names(
    """
    crossbowsprintforward.hkx crossbow_idleheld.hkx crossbow_idledrawn.hkx
    crossbow_idledrawndwarven.hkx crossbow_aim.hkx crossbow_aimdwarven.hkx
    sneakcrossbow_idledrawn.hkx sneakcrossbow_idledrawndwarven.hkx
    sneakcrossbow_aim.hkx sneakcrossbow_aimdwarven.hkx
    """
)
MAGIC_MOTION = standard_motion("mag_", idle=False) | standard_motion("magcast_", idle=False) | names(
    """
    dualmagic_idle.hkx magic_sprintforward.hkx mlh_idle.hkx mrh_idle.hkx
    magcast_runbackwrdleft.hkx magcast_runbckwrdright.hkx
    magcast_runfrwrdright.hkx magcast_walkbckwrdleft.hkx
    magcast_walkbckwrdrht.hkx magcast_walkforwrdleft.hkx
    magcast_walkfrwrdright.hkx mag_walkbckwrdright.hkx
    dmagaimconcharge.hkx dmagpreaimconcharge.hkx
    dmagpreselfconcharge.hkx dmagselfconcharge.hkx
    """
)
STAFF_MOTION = names(
    """
    staff_idle.hkx staffright_idle.hkx staffrightleft_sprint.hkx
    staffmagic_runarm.hkx staffmagic_walkarm.hkx
    staffmagicright_runarm.hkx staffmagicright_walkarm.hkx
    staffmagiccast_turnleft180.hkx staffmagiccast_turnleft60.hkx
    staffmagiccast_turnright180.hkx staffmagiccast_turnright60.hkx
    """
)

MAGIC_SOURCE_ALIASES = {
    "mlh_prewardcharge.hkx": "mlh_prewardloop.hkx",
    "mlh_selfchargeloop.hkx": "mlh_selfconcentration.hkx",
    "mlh_telekinesisloop.hkx": "mrh_telekinesisloop.hkx",
    "mlh_wardcharge.hkx": "mlh_wardloop.hkx",
    "mlhmrh_aimedconcentrationloop.hkx": "dualmagic_idle.hkx",
    "mrh_prewardcharge.hkx": "mrh_prewardloop.hkx",
    "mrh_selfchargeloop.hkx": "mrh_selfconcentration.hkx",
    "mrh_wardcharge.hkx": "mrh_wardloop.hkx",
    "ritualspell_aimrelease2.hkx": "ritualspell_aimrelease.hkx",
}

COMMON_MELEE_SUFFIXES = names(
    """
    attackforwardsprint.hkx attackleft.hkx attackleftintro.hkx
    attackpower.hkx attackpower3slashcombo.hkx attackpowerbwd.hkx
    attackpowerforwardsprint.hkx attackpowerforward.hkx attackpowerfwd.hkx
    attackpowerleft.hkx attackpowerright.hkx attackright.hkx attackrightintro.hkx
    blockbash.hkx blockbashintro.hkx blockbashpower.hkx
    runbwdattackleft.hkx runbwdattackleftintro.hkx
    runbwdattackright.hkx runbwdattackrightintro.hkx
    runfwdattackleft.hkx runfwdattackleftintro.hkx
    runfwdattackright.hkx runfwdattackrightintro.hkx
    runleftattackleft.hkx runleftattackleftintro.hkx
    runleftattackright.hkx runleftattackrightintro.hkx
    runrightattackleft.hkx runrightattackleftintro.hkx
    runrightattackrt.hkx runrightattackrtintro.hkx
    walkbwdattackleft.hkx walkbwdattackleftintro.hkx
    walkbwdattackright.hkx walkbwdattackrightintro.hkx
    walkfwdattackleft.hkx walkfwdattackleftintro.hkx
    walkfwdattackright.hkx walkfwdattackrightintro.hkx
    walkleftattackleft.hkx walkleftattackleftintro.hkx
    walkleftattackright.hkx walkleftattackrightintro.hkx
    walkrightattackleft.hkx walkrightattackleftintro.hkx
    walkrtattackright.hkx walkrtattackrightintro.hkx
    """
)


def prefixed(prefix: str, suffixes: set[str]) -> set[str]:
    return {f"{prefix}_{suffix}" for suffix in suffixes}


ONE_HANDED_ATTACKS = prefixed("1hm", COMMON_MELEE_SUFFIXES) | names(
    """
    1hm_sneakattackleft.hkx 1hm_sneakattackleftintro.hkx
    1hm_sneakattackright.hkx 1hm_sneakattackrightintro.hkx
    1hm_sneakattackpower.hkx 1hm_sneakattackpowerback.hkx
    1hm_sneakattackpowerforward.hkx 1hm_sneakattackpowerleft.hkx
    1hm_sneakattackpowerright.hkx
    1hmlefthand_attackforwardsprint.hkx 1hmlefthand_attackpowerforwardsprint.hkx
    mlh_1hm_attackforward.hkx mlh_1hm_attackforwardintro.hkx
    sneak_1hmattack.hkx sneak_1hmattackintro.hkx
    sneak_1hmattacklefthand.hkx sneak_1hmattackpowerlefthand.hkx
    """
)
GREATSWORD_ATTACKS = prefixed("2hm", COMMON_MELEE_SUFFIXES)
AXE_WARHAMMER_ATTACKS = prefixed("2hw", COMMON_MELEE_SUFFIXES)
DUAL_WIELD_ATTACKS = names(
    """
    dw_attackpowerback.hkx dw_attackpowerforward.hkx
    dw_attackpowerknifeslashcombo.hkx dw_attackpowerleft.hkx
    dw_attackpowerright.hkx dw_attackpowerstab.hkx
    dw1hm1hm_attackright.hkx dw1hm1hm_attackrightintro.hkx
    dw1hm1hm_powerattack.hkx dw1hm1hm_specialattackpower.hkx
    dw1hm1hmblockbash.hkx dw1hm1hmblockbashintro.hkx dw1hm1hmblockbashpower.hkx
    dwrunback_attackright.hkx dwrunforward_attackright.hkx
    dwrunleft_attackright.hkx dwrunright_attackright.hkx
    dwwalkback_attackright.hkx dwwalkforward_attackright.hkx
    dwwalkleft_attackright.hkx dwwalkright_attackright.hkx
    """
)
UNARMED_ATTACKS = names(
    """
    h2h_attackleft.hkx h2h_attackright.hkx
    h2h_attackpowerforwardlefthand.hkx h2h_attackpowerforwardrighthand.hkx
    h2h_attackrightpowerforward.hkx h2h_attackpowerforwardsprint.hkx
    h2h_mlh_attack.hkx
    h2h_runbackattackleft.hkx h2h_runbackattackright.hkx
    h2h_runforwardattackleft.hkx h2h_runforwardattackright.hkx
    h2h_runleftattackleft.hkx h2h_runleftattackright.hkx
    h2h_runrightattackleft.hkx h2h_runrightattackright.hkx
    """
)
MCO_ATTACKS = (
    {f"mco_attack{index}.hkx" for index in range(1, 11)}
    | {f"mco_powerattack{index}.hkx" for index in range(1, 11)}
    | names(
        """
        mco_dodge_attack1.hkx mco_dodge_powerattack1.hkx
        mco_sprintattack.hkx mco_sprintpowerattack.hkx
        mco_sprintpoweattackr.hkx mco_weaponart.hkx
        mco_powerattackloop1.hkx mco_powerattackoutro1.hkx
        mco_powerattackoutro4.hkx
        mco_dodge-b-1.hkx mco_dodge-f-1.hkx mco_dodge-l-1.hkx
        mco_dodge-lb-1.hkx mco_dodgeleft1.hkx mco_dodge-lf-1.hkx
        mco_dodge-r-1.hkx mco_dodge-rb-1.hkx mco_dodge-rf-1.hkx
        scar_1hmreadydummy.hkx sneakrun_forwardroll.hkx
        """
    )
)


def actor_base_condition() -> dict[str, object]:
    return {
        "condition": "IsActorBase",
        "requiredVersion": "1.0.0.0",
        "Actor base": {"pluginName": "Skyrim.esm", "formID": "7"},
    }


def graph_bool(name: str) -> dict[str, object]:
    return {
        "condition": "CompareValues",
        "requiredVersion": "1.0.0.0",
        "Value A": {"graphVariable": name, "graphVariableType": "Bool"},
        "Comparison": "==",
        "Value B": {"value": 1.0},
    }


def graph_state_active() -> dict[str, object]:
    return {
        "condition": "CompareValues",
        "requiredVersion": "1.0.0.0",
        "Value A": {"graphVariable": "iDAF_FlightState", "graphVariableType": "Int"},
        "Comparison": ">",
        "Value B": {"value": 0.0},
    }


def equipped_type(type_value: int, left_hand: bool) -> dict[str, object]:
    return {
        "condition": "IsEquippedType",
        "requiredVersion": "1.0.0.0",
        "Type": {"value": float(type_value)},
        "Left hand": left_hand,
    }


def equipped_any(types: tuple[int, ...], *, hands: tuple[bool, ...] = (False, True)) -> dict[str, object]:
    return {
        "condition": "OR",
        "requiredVersion": "1.0.0.0",
        "Conditions": [equipped_type(value, hand) for hand in hands for value in types],
    }


def flight_conditions(*equipment: dict[str, object]) -> list[dict[str, object]]:
    result = [actor_base_condition(), graph_bool("bDAF_FlightActive"), graph_state_active()]
    # Equipment transitions can request their OAR originals before the engine's
    # drawn-state signal settles. Flight state + equipped type are sufficient;
    # bDAF_FlightCombatActive remains diagnostic state, not a routing gate.
    result.extend(equipment)
    return result


@dataclass(frozen=True)
class Family:
    directory: str
    display_name: str
    description: str
    priority: int
    base_source: str
    motion_names: set[str]
    conditions: list[dict[str, object]]
    attack_names: set[str] = frozenset()
    attack_prefix: str | None = None
    magic_sources: bool = False
    staff_sources: bool = False
    scopes: tuple[pathlib.Path, ...] = (ROOT_SCOPE,)


FAMILIES = (
    Family(
        "Flight Base 00 - Fallback",
        "DAF Flight Base - Fallback",
        "Root-stable flight fallback for generic locomotion and landing paths.",
        2147483600,
        "Flying_Mod_Idle.hkx",
        GENERIC_MOTION,
        flight_conditions(),
        scopes=(ROOT_SCOPE, pathlib.Path("male")),
    ),
    Family(
        "Flight Base 10 - Unarmed",
        "DAF Flight Base - Unarmed",
        "Root-stable unarmed flight locomotion and aerial attacks.",
        2147483601,
        "Flying_Mod_Idle.hkx",
        UNARMED_MOTION,
        flight_conditions(
            equipped_any((0,), hands=(False,)),
            equipped_any((0,), hands=(True,)),
        ),
        UNARMED_ATTACKS | MCO_ATTACKS,
        "H2h",
    ),
    Family(
        "Flight Base 20 - One Handed",
        "DAF Flight Base - One Handed",
        "Root-stable one-handed flight locomotion and vanilla/MCO aerial attacks.",
        2147483602,
        "Flying_Mod_Idle.hkx",
        ONE_HANDED_MOTION,
        flight_conditions(equipped_any((1, 2, 3, 4))),
        ONE_HANDED_ATTACKS | MCO_ATTACKS,
        "1hm",
    ),
    Family(
        "Flight Base 30 - Dual Wield",
        "DAF Flight Base - Dual Wield",
        "Root-stable dual-wield flight locomotion and vanilla/MCO aerial attacks.",
        2147483603,
        "Flying_Mod_Idle.hkx",
        DUAL_WIELD_MOTION,
        flight_conditions(
            equipped_any((1, 2, 3, 4), hands=(False,)),
            equipped_any((1, 2, 3, 4), hands=(True,)),
        ),
        DUAL_WIELD_ATTACKS | MCO_ATTACKS,
        "Dw",
    ),
    Family(
        "Flight Base 40 - Greatsword",
        "DAF Flight Base - Greatsword",
        "Root-stable greatsword flight locomotion and vanilla/MCO aerial attacks.",
        2147483604,
        "Flying_Mod_Idle.hkx",
        GREATSWORD_MOTION,
        flight_conditions(equipped_any((5,))),
        GREATSWORD_ATTACKS | MCO_ATTACKS,
        "2hm",
    ),
    Family(
        "Flight Base 50 - Axe and Warhammer",
        "DAF Flight Base - Axe and Warhammer",
        "Root-stable battleaxe/warhammer flight locomotion and vanilla/MCO aerial attacks.",
        2147483605,
        "Flying_Mod_Idle.hkx",
        AXE_WARHAMMER_MOTION,
        flight_conditions(equipped_any((6, 10))),
        AXE_WARHAMMER_ATTACKS | MCO_ATTACKS,
        "2hw",
    ),
    Family(
        "Flight Base 60 - Bow",
        "DAF Flight Base - Bow",
        "Root-stable bow flight locomotion; functional draw/release clips remain intact.",
        2147483606,
        "Flying_Mod_Idle.hkx",
        BOW_MOTION,
        flight_conditions(equipped_any((7,))),
    ),
    Family(
        "Flight Base 70 - Crossbow",
        "DAF Flight Base - Crossbow",
        "Root-stable crossbow flight locomotion; functional release/reload clips remain intact.",
        2147483607,
        "Flying_Mod_Idle.hkx",
        CROSSBOW_MOTION,
        flight_conditions(equipped_any((9,))),
    ),
    Family(
        "Flight Base 80 - Magic",
        "DAF Flight Base - Magic",
        "Root-stable magic locomotion plus aerialized redistributable xp32 casting clips.",
        2147483608,
        "Flying_Mod_Idle.hkx",
        MAGIC_MOTION,
        flight_conditions(equipped_any((12, 13, 14, 15, 16, 17))),
        magic_sources=True,
    ),
    Family(
        "Flight Base 90 - Staff",
        "DAF Flight Base - Staff",
        "Root-stable staff locomotion plus aerialized redistributable xp32 staff actions.",
        2147483609,
        "Flying_Mod_Idle.hkx",
        STAFF_MOTION,
        flight_conditions(equipped_any((8,))),
        staff_sources=True,
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--hkxcmd", type=pathlib.Path, required=True)
    parser.add_argument("--pynifly-hkx-dir", type=pathlib.Path, required=True)
    return parser.parse_args()


def validate_hkx(hkxcmd: pathlib.Path, source: pathlib.Path, output: pathlib.Path) -> None:
    prefix = source.read_bytes()[:64]
    is_binary_packfile = prefix.startswith(bytes((0x57, 0xE0, 0xE0, 0x57)))
    is_xml_packfile = prefix.lstrip().startswith(b"<?xml") or b"<hkpackfile" in prefix
    if source.stat().st_size < 16 or not (is_binary_packfile or is_xml_packfile):
        raise RuntimeError(f"Animation is not a recognizable HKX packfile: {source}")

    completed = subprocess.run(
        [str(hkxcmd), "Convert", "-v:TAGXML", "-i", str(source), "-o", str(output)],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        diagnostic = "\n".join(part for part in (completed.stdout, completed.stderr) if part)
        raise RuntimeError(f"hkxcmd validation failed ({completed.returncode}): {source}\n{diagnostic}")
    if not output.is_file():
        print(f"hkxcmd could not deserialize 64-bit source; binary header accepted: {source.name}")


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def write_json(path: pathlib.Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")


def is_intro_or_recovery(name: str) -> bool:
    return any(token in name for token in ("intro", "outro", "loop")) or (
        name.startswith("mco_dodge-")
        or name == "mco_dodgeleft1.hkx"
        or name in {"scar_1hmreadydummy.hkx", "sneakrun_forwardroll.hkx"}
    )


def attack_source_name(family: Family, animation_name: str) -> str:
    if family.attack_prefix is None:
        raise ValueError(f"Family has no attack source: {family.display_name}")
    if is_intro_or_recovery(animation_name):
        return family.base_source
    if family.attack_prefix == "1hm" and ("lefthand" in animation_name or animation_name.startswith("mlh_")):
        prefix = "LH_1hm"
    else:
        prefix = family.attack_prefix
    is_power = any(token in animation_name for token in ("power", "weaponart"))
    match = re.search(r"mco_attack(\d+)", animation_name)
    is_left = "attackleft" in animation_name or "lefthand" in animation_name
    if match and int(match.group(1)) % 2 == 0:
        is_left = True
    if is_power:
        return f"{prefix}_Air_Pwr_Attack.hkx"
    if is_left:
        return f"{prefix}_Air_Attack_Left.hkx"
    return f"{prefix}_Air_Attack.hkx"


def write_hash_manifest(data_root: pathlib.Path) -> int:
    assets = sorted(
        (
            path
            for path in (data_root / "meshes").rglob("*")
            if path.is_file() and path.suffix.lower() in {".hkx", ".nif"}
        ),
        key=lambda path: path.relative_to(data_root).as_posix().lower(),
    )
    lines = [
        "Dragon Aspect Flight 1.8.0 bundled animation/effect asset SHA-256",
        "=================================================================",
        "",
    ]
    lines.extend(f"{sha256(path).lower()} *{path.relative_to(data_root).as_posix()}" for path in assets)
    manifest = data_root / "SKSE/Plugins/DragonAspectFlight-AnimationHashes.txt"
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return len(assets)


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    hkxcmd = args.hkxcmd.resolve()
    if not hkxcmd.is_file():
        raise FileNotFoundError(f"hkxcmd not found: {hkxcmd}")
    anim_skyrim = load_backend(args.pynifly_hkx_dir)

    data_root = repo_root / "Data"
    nicknak_root = repo_root / "third_party/nicknak/animations"
    flying_root = repo_root / "third_party/flying-mod"
    magic_root = repo_root / "third_party/xp32-magic"
    oar_root = data_root / "meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight"
    magic_sources = {path.name.lower(): path for path in magic_root.glob("*.hkx")}
    if len(magic_sources) != 79:
        raise RuntimeError(f"Expected 79 credited xp32/Neumeria magic sources, found {len(magic_sources)}")

    if oar_root.exists():
        shutil.rmtree(oar_root)
    oar_root.mkdir(parents=True)
    write_json(
        oar_root / "config.json",
        {
            "name": "Dragon Aspect Flight",
            "author": "nwn900",
            "description": "Flight-scoped, equipment-aware animation ownership for DAF.",
        },
    )

    required_sources = {family.base_source for family in FAMILIES}
    for family in FAMILIES:
        if family.attack_prefix:
            for attack_name in family.attack_names:
                required_sources.add(attack_source_name(family, attack_name))

    built_sources: dict[str, pathlib.Path] = {}
    coverage: dict[str, object] = {}
    expected_outputs: set[pathlib.Path] = set()
    with tempfile.TemporaryDirectory(prefix="daf-flight-stack-") as temporary:
        temp_root = pathlib.Path(temporary)
        for source_name in sorted(required_sources, key=str.lower):
            source_hkx = (
                flying_root / source_name
                if source_name == "Flying_Mod_Idle.hkx"
                else nicknak_root / source_name
            )
            if not source_hkx.is_file():
                raise FileNotFoundError(f"Bundled animation source missing: {source_hkx}")
            validate_hkx(hkxcmd, source_hkx, temp_root / f"{source_hkx.stem}-validation.xml")
            built_sources[source_name] = source_hkx
            print(f"source {source_name}: sha256={sha256(source_hkx)}")

        aerial_magic_sources: dict[str, pathlib.Path] = {}
        for name, source in magic_sources.items():
            validate_hkx(hkxcmd, source, temp_root / f"magic-{source.stem}-validation.xml")
            composite = temp_root / "aerialized-magic" / name
            replaced_tracks = aerialize(
                anim_skyrim,
                flying_root / "Flying_Mod_Idle.hkx",
                source,
                composite,
            )
            aerial_magic_sources[name] = composite
            print(f"aerialized {name}: replaced_tracks={len(replaced_tracks)} sha256={sha256(composite)}")

        for family in FAMILIES:
            mappings = {name: built_sources[family.base_source] for name in family.motion_names}
            for attack_name in family.attack_names:
                mappings[attack_name] = built_sources[attack_source_name(family, attack_name)]

            if family.magic_sources:
                for name, source in aerial_magic_sources.items():
                    if not name.startswith("staff"):
                        mappings[name] = source
                for target_name, source_name in MAGIC_SOURCE_ALIASES.items():
                    mappings[target_name] = aerial_magic_sources[source_name]
                for name in family.motion_names:
                    mappings[name] = built_sources[family.base_source]
            if family.staff_sources:
                for name, source in aerial_magic_sources.items():
                    if name.startswith("staff"):
                        mappings[name] = source
                for name in family.motion_names:
                    mappings[name] = built_sources[family.base_source]

            submod_root = oar_root / family.directory
            write_json(
                submod_root / "config.json",
                {
                    "name": family.display_name,
                    "description": family.description,
                    "priority": family.priority,
                    "interruptible": True,
                    "replaceOnLoop": True,
                    "conditions": family.conditions,
                },
            )
            for scope in family.scopes:
                for animation_name, source in sorted(mappings.items()):
                    target = submod_root / scope / animation_name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, target)
                    expected_outputs.add(target.resolve())

            coverage[family.directory] = {
                "priority": family.priority,
                "baseSource": family.base_source,
                "animationCountPerScope": len(mappings),
                "motionNames": sorted(family.motion_names),
                "attackNames": sorted(family.attack_names),
                "scopes": [scope.as_posix() or "." for scope in family.scopes],
                "aerializedMagicSources": family.magic_sources or family.staff_sources,
            }
            print(f"{family.directory}: {len(mappings)} originals x {len(family.scopes)} scopes")

    existing_outputs = {path.resolve() for path in oar_root.rglob("*.hkx")}
    unexpected = sorted(existing_outputs - expected_outputs)
    missing = sorted(expected_outputs - existing_outputs)
    if unexpected or missing:
        raise RuntimeError(f"Generated flight stack mismatch: unexpected={len(unexpected)}, missing={len(missing)}")

    coverage_path = data_root / "SKSE/Plugins/DragonAspectFlight-AnimationCoverage.json"
    write_json(
        coverage_path,
        {
            "version": "1.8.0",
            "scopes": sorted({scope.as_posix() or "." for family in FAMILIES for scope in family.scopes}),
            "families": coverage,
            "totalOarHkx": len(existing_outputs),
        },
    )
    manifest_count = write_hash_manifest(data_root)
    print(f"Built {len(existing_outputs)} DAF OAR aliases across {len(FAMILIES)} equipment families.")
    print(f"Wrote SHA-256 manifest for {manifest_count} bundled HKX/NIF assets.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
