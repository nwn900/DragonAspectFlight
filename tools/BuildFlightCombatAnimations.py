#!/usr/bin/env python3
"""Build Dragon Aspect Flight's bundled, no-Nemesis combat OAR package.

NickNak's permitted Skyrim SE clips are copied unchanged for vanilla combat
and ready poses.  MCO aliases are rebuilt with serde-hkx/hkxc after adding the
combo, recovery, power-chain, and attack-stop annotations MCO expects.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_SOURCE_ROOT = Path(
    r"C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods"
    r"\[NoDelete] More Draconic Aspect - Flight Combat"
    r"\meshes\actors\character\animations\NickNak"
)

SOURCE_HASHES = {
    "1hm_Air_Attack.hkx": "C8964A6A217669CC4BE29B5A98C2CA21EA2188435E754B49C0C5E7054B42B2B5",
    "1hm_Air_Attack_Left.hkx": "2DA12EC7611F2C88147DCB308FB2421EF13957A5DEA8903BA3B598F1C7AFC656",
    "1hm_Air_Pwr_Attack.hkx": "52EDFC8F19A3A18AC35BD2C3357326DDE58F698BDEF26FE144D203FCFBB3F7E5",
    "2hm_Air_Attack.hkx": "A71CFDDECA8A6558E1330A9EC0B63B76AD55A16310B179903AE6BA37728B0B91",
    "2hm_Air_Pwr_Attack.hkx": "900E13205E3AAA74CC037BD648F23BD6DF77D7659886E7AF97EAF62C73642832",
    "2hw_Air_Attack.hkx": "022DEC11FBDB7373619058B9B2AD10525DB6F7232670BD928650AE9C77239DA2",
    "2hw_Air_Pwr_Attack.hkx": "946DA738BF126E2D3557FC588F4EC7D020380C3099B3A7AB6DB72A5D385BD1F4",
    "Dw_Air_Attack.hkx": "E86BB5F4E8A631FA5448BD9B7A998EF8F4CEB8C176518616CB13BDC99CA446B8",
    "Dw_Air_Pwr_Attack.hkx": "0FAE44DEEB9B2EC001F5FD0BA3E8B4B175C77271D5CC8FC5784657F0C566BA68",
    "H2h_Air_Attack.hkx": "469E2B807A82847A43EE714C3325C2A8D0BD7AFD056965B3EEC688B32A018B05",
    "H2h_Air_Pwr_Attack.hkx": "CD2A67C5D7ABF50A9BFC34A5C7407EB43E9805A598533E103C1252529CDA03E3",
    "LH_1hm_Air_Attack.hkx": "C4865AB2F3F9FB08C9EE813792E1D7021B8700ECF476A6A0EA61103D087C8559",
    "LH_1hm_Air_Pwr_Attack.hkx": "7F3EF17E45F687E9EC40B36855950B17D2095A47E11ADA7EE96E33CBCD70BEAD",
    "1hm_Fall.hkx": "4075F885B9100D971775D88388E0FBC12AE875F2DE961A9774D239502645AB4F",
    "2hm_Fall.hkx": "9D118B2432BDD9B64E9962A52F7A5BCD278778DD513338195C05D6AFF5133DDB",
    "2hw_Fall.hkx": "4C3D503C8B604C08E7380CA7BD144A4D9E92B220F6F5A9222921F6363BB67A2F",
    "H2h_Fall.hkx": "8EC63F10237B0336A34B42963BB5A46EB82355AEAB7D7D965E5F68216DBC1693",
    "Aiming_Fall.hkx": "623991C3DE7546D151E7019032E799AEFE8CD2272A623D023B0A63666AA123B8",
}

LOCOMOTION_TARGETS = (
    "mt_idle.hkx",
    "mt_jump.hkx",
    "mt_jumpfall.hkx",
    "mt_jumpfallleft.hkx",
    "mt_jumpfallright.hkx",
    "mt_jumpfast.hkx",
    "mt_jumpfastleft.hkx",
    "mt_jumpfastright.hkx",
    "mt_runbackward.hkx",
    "mt_runbackwardleft.hkx",
    "mt_runbackwardright.hkx",
    "mt_runforward.hkx",
    "mt_runforwardleft.hkx",
    "mt_runforwardright.hkx",
    "mt_runleft.hkx",
    "mt_runright.hkx",
    "mt_runstrafeleft.hkx",
    "mt_runstraferight.hkx",
    "mt_sprintforward.hkx",
    "mt_walkbackward.hkx",
    "mt_walkbackwardleft.hkx",
    "mt_walkbackwardright.hkx",
    "mt_walkforward.hkx",
    "mt_walkforwardleft.hkx",
    "mt_walkforwardright.hkx",
    "mt_walkleft.hkx",
    "mt_walkright.hkx",
)

FULL_BODY_DIRECTIONAL_SUFFIXES = (
    "runbackward.hkx",
    "runbackwardleft.hkx",
    "runbackwardright.hkx",
    "runforward.hkx",
    "runforwardleft.hkx",
    "runforwardright.hkx",
    "runleft.hkx",
    "runright.hkx",
    "runstrafeleft.hkx",
    "runstraferight.hkx",
    "walkbackward.hkx",
    "walkbackwardleft.hkx",
    "walkbackwardright.hkx",
    "walkforward.hkx",
    "walkforwardleft.hkx",
    "walkforwardright.hkx",
    "walkleft.hkx",
    "walkright.hkx",
    "turnleft180.hkx",
    "turnleft60.hkx",
    "turnright180.hkx",
    "turnright60.hkx",
)


def prefixed(prefix: str, suffixes: Iterable[str]) -> tuple[str, ...]:
    return tuple(f"{prefix}_{suffix}" for suffix in suffixes)


ONE_HANDED_READY_TARGETS = (
    "1hm_idle.hkx",
    *prefixed("1hm", FULL_BODY_DIRECTIONAL_SUFFIXES),
)
TWO_HANDED_READY_TARGETS = (
    "2hm_idle.hkx",
    *prefixed("2hm", FULL_BODY_DIRECTIONAL_SUFFIXES),
    "2hm_sprintforwardsword.hkx",
)
TWO_HANDED_AXE_READY_TARGETS = (
    "2hw_idle.hkx",
    "2hw_sprintforwardsword.hkx",
)
UNARMED_READY_TARGETS = (
    "h2h_idle.hkx",
    "h2h_sprintforward.hkx",
)
BOW_READY_TARGETS = (
    "bow_idledrawn.hkx",
    "bow_idleheld.hkx",
    *prefixed("bow", FULL_BODY_DIRECTIONAL_SUFFIXES),
    "bow_sprint.hkx",
)
CROSSBOW_READY_TARGETS = (
    "crossbow_aim.hkx",
    "crossbow_idledrawn.hkx",
    "crossbow_idledrawndwarven.hkx",
    "crossbow_idleheld.hkx",
    *prefixed("crossbow", FULL_BODY_DIRECTIONAL_SUFFIXES),
)
MAGIC_STAFF_READY_TARGETS = (
    "dualmagic_idle.hkx",
    "magic_sprintforward.hkx",
    "staff_idle.hkx",
    "staffright_idle.hkx",
    "staffrightleft_sprint.hkx",
)
RANGED_MAGIC_READY_TARGETS = (
    *BOW_READY_TARGETS,
    *CROSSBOW_READY_TARGETS,
    *MAGIC_STAFF_READY_TARGETS,
)

MCO_ATTACK_TARGETS = tuple(f"mco_attack{index}.hkx" for index in range(1, 11))
MCO_POWER_TARGETS = tuple(f"mco_powerattack{index}.hkx" for index in range(1, 11))
MCO_TARGETS = (*MCO_ATTACK_TARGETS, *MCO_POWER_TARGETS)

RIGHT_POWER_COLLISION = (
    "Collision_Add.node(WEAPON)|DamageMult(1.2)|NoRecoil|"
    "GroundShake(30, 0.3, 40)|Scale(1.2)"
)
LEFT_POWER_COLLISION = (
    "Collision_Add.node(SHIELD)|DamageMult(1.2)|NoRecoil|"
    "GroundShake(30, 0.3, 40)|Scale(1.2)"
)
DUAL_LEFT_POWER_COLLISION = (
    "Collision_Add.node(SHIELD)|DamageMult(1.2)|NoRecoil|Scale(1.2)"
)
UNARMED_POWER_COLLISION = (
    "Collision_Add.node(NPC R Hand [RHnd])|Scale(3)|DamageMult(1.2)|"
    "NoRecoil|NoTrail|GroundShake(30, 0.3, 40)"
)

POWER_ATTACK_EVENTS = {
    "1hm_Air_Pwr_Attack.hkx": (
        (0.230000, "preHitFrame"),
        (0.260000, "weaponSwing"),
        (0.260000, RIGHT_POWER_COLLISION),
        (0.330000, "HitFrame"),
        (0.600000, "Collision_Remove.node(WEAPON)"),
        (0.700000, "Collision_AttackEnd"),
        (0.733333, "TDM_AttackStop"),
    ),
    "LH_1hm_Air_Pwr_Attack.hkx": (
        (0.230000, "preHitFrame"),
        (0.260000, "weaponLeftSwing"),
        (0.260000, LEFT_POWER_COLLISION),
        (0.330000, "HitFrame"),
        (0.600000, "Collision_Remove.node(SHIELD)"),
        (0.700000, "Collision_AttackEnd"),
        (0.733333, "TDM_AttackStop"),
    ),
    "Dw_Air_Pwr_Attack.hkx": (
        (0.230000, "preHitFrame"),
        (0.260000, "weaponSwing"),
        (0.260000, "weaponLeftSwing"),
        (0.260000, RIGHT_POWER_COLLISION),
        (0.260000, DUAL_LEFT_POWER_COLLISION),
        (0.330000, "HitFrame"),
        (0.330000, "HitFrame"),
        (0.600000, "Collision_Remove.node(WEAPON)"),
        (0.600000, "Collision_Remove.node(SHIELD)"),
        (0.700000, "Collision_AttackEnd"),
        (0.733333, "TDM_AttackStop"),
    ),
    "2hm_Air_Pwr_Attack.hkx": (
        (0.230000, "preHitFrame"),
        (0.260000, "weaponSwing"),
        (0.260000, RIGHT_POWER_COLLISION),
        (0.330000, "HitFrame"),
        (0.600000, "Collision_Remove.node(WEAPON)"),
        (0.700000, "Collision_AttackEnd"),
        (0.733333, "TDM_AttackStop"),
    ),
    "2hw_Air_Pwr_Attack.hkx": (
        (0.230000, "preHitFrame"),
        (0.260000, "weaponSwing"),
        (0.260000, RIGHT_POWER_COLLISION),
        (0.330000, "HitFrame"),
        (0.600000, "Collision_Remove.node(WEAPON)"),
        (0.700000, "Collision_AttackEnd"),
        (0.733333, "TDM_AttackStop"),
    ),
    "H2h_Air_Pwr_Attack.hkx": (
        (0.230000, "preHitFrame"),
        (0.260000, "SoundPlay.WPNSwingUnarmed"),
        (0.260000, "weaponSwing"),
        (0.260000, UNARMED_POWER_COLLISION),
        (0.330000, "HitFrame"),
        (0.600000, "Collision_Remove.node(NPC R Hand [RHnd])"),
        (0.700000, "Collision_AttackEnd"),
        (0.733333, "TDM_AttackStop"),
    ),
}

VANILLA_TARGETS = {
    "one_handed": (
        "1hm_attackleft.hkx",
        "1hm_attackright.hkx",
        "1hm_attackrightdiagonal.hkx",
        "1hm_attackpower.hkx",
        "1hm_attackpowerback.hkx",
        "1hm_attackpowerforward.hkx",
        "1hm_attackpowerleft.hkx",
        "1hm_attackpowerright.hkx",
        "1hm_attackpowerstanding.hkx",
    ),
    "dual_wield": (
        "dw1hm1hm_attackleft.hkx",
        "dw1hm1hm_attackright.hkx",
        "dw1hm1hm_attackpower.hkx",
        "dw1hm1hm_attackpowerforward.hkx",
        "dw1hm1hm_attackpowerstanding.hkx",
    ),
    "greatsword": (
        "2hm_attackleft.hkx",
        "2hm_attackright.hkx",
        "2hm_attackpower.hkx",
        "2hm_attackpowerback.hkx",
        "2hm_attackpowerforward.hkx",
        "2hm_attackpowerleft.hkx",
        "2hm_attackpowerright.hkx",
        "2hm_attackpowerstanding.hkx",
    ),
    "battleaxe_warhammer": (
        "2hw_attackleft.hkx",
        "2hw_attackright.hkx",
        "2hw_attackpower.hkx",
        "2hw_attackpowerback.hkx",
        "2hw_attackpowerforward.hkx",
        "2hw_attackpowerleft.hkx",
        "2hw_attackpowerright.hkx",
        "2hw_attackpowerstanding.hkx",
    ),
    "unarmed": (
        "h2h_attackleft.hkx",
        "h2h_attackright.hkx",
        "h2h_attackpower.hkx",
        "h2h_attackpowerback.hkx",
        "h2h_attackpowerforward.hkx",
        "h2h_attackpowerleft.hkx",
        "h2h_attackpowerright.hkx",
        "h2h_attackpowerstanding.hkx",
    ),
}


@dataclass(frozen=True)
class BuildSpec:
    folder: str
    priority: int
    attack_source: str | None
    power_source: str | None
    ready_source: str
    ready_targets: tuple[str, ...]
    equipment_condition: dict
    vanilla_family: str | None


def equipped_type(value: int, left_hand: bool) -> dict:
    return {
        "condition": "IsEquippedType",
        "requiredVersion": "1.0.0.0",
        "Type": {"value": float(value)},
        "Left hand": left_hand,
    }


def logical(name: str, conditions: Iterable[dict]) -> dict:
    return {
        "condition": name,
        "requiredVersion": "1.0.0.0",
        "Conditions": list(conditions),
    }


ONE_HAND_RIGHT = logical("OR", (equipped_type(value, False) for value in range(1, 5)))
ONE_HAND_LEFT = logical("OR", (equipped_type(value, True) for value in range(1, 5)))
RANGED_MAGIC = logical(
    "OR",
    (
        *(equipped_type(value, False) for value in (7, 8, 9, 12, 13, 14, 15, 16, 17)),
        *(equipped_type(value, True) for value in (8, 12, 13, 14, 15, 16, 17)),
    ),
)

BUILD_SPECS = (
    BuildSpec(
        "70 - Ranged and Magic",
        2_147_483_640,
        None,
        None,
        "Aiming_Fall.hkx",
        RANGED_MAGIC_READY_TARGETS,
        RANGED_MAGIC,
        None,
    ),
    BuildSpec(
        "60 - Dual Wield",
        2_147_483_639,
        "Dw_Air_Attack.hkx",
        "Dw_Air_Pwr_Attack.hkx",
        "1hm_Fall.hkx",
        ONE_HANDED_READY_TARGETS,
        logical("AND", (ONE_HAND_RIGHT, ONE_HAND_LEFT)),
        "dual_wield",
    ),
    BuildSpec(
        "50 - Left Handed",
        2_147_483_638,
        "LH_1hm_Air_Attack.hkx",
        "LH_1hm_Air_Pwr_Attack.hkx",
        "1hm_Fall.hkx",
        ONE_HANDED_READY_TARGETS,
        logical("AND", (equipped_type(0, False), ONE_HAND_LEFT)),
        "one_handed",
    ),
    BuildSpec(
        "40 - Greatsword",
        2_147_483_637,
        "2hm_Air_Attack.hkx",
        "2hm_Air_Pwr_Attack.hkx",
        "2hm_Fall.hkx",
        TWO_HANDED_READY_TARGETS,
        equipped_type(5, False),
        "greatsword",
    ),
    BuildSpec(
        "30 - Battleaxe and Warhammer",
        2_147_483_636,
        "2hw_Air_Attack.hkx",
        "2hw_Air_Pwr_Attack.hkx",
        "2hw_Fall.hkx",
        TWO_HANDED_AXE_READY_TARGETS,
        logical("OR", (equipped_type(6, False), equipped_type(10, False))),
        "battleaxe_warhammer",
    ),
    BuildSpec(
        "20 - One Handed",
        2_147_483_635,
        "1hm_Air_Attack.hkx",
        "1hm_Air_Pwr_Attack.hkx",
        "1hm_Fall.hkx",
        ONE_HANDED_READY_TARGETS,
        ONE_HAND_RIGHT,
        "one_handed",
    ),
    BuildSpec(
        "10 - Unarmed",
        2_147_483_634,
        "H2h_Air_Attack.hkx",
        "H2h_Air_Pwr_Attack.hkx",
        "H2h_Fall.hkx",
        UNARMED_READY_TARGETS,
        logical("AND", (equipped_type(0, False), equipped_type(0, True))),
        "unarmed",
    ),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def run(command: list[str]) -> None:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}\n{result.stderr}"
        )


def validate_sources(source_root: Path) -> None:
    for name, expected in SOURCE_HASHES.items():
        path = source_root / name
        if not path.is_file():
            raise FileNotFoundError(f"required source clip is missing: {path}")
        actual = sha256(path)
        if actual != expected:
            raise ValueError(
                f"{name}: expected SHA-256 {expected}, found {actual}; "
                "refusing to import an unreviewed asset"
            )
        data = path.read_bytes()
        if len(data) < 0x38 or data[:4] != b"\x57\xe0\xe0\x57" or data[0x10] != 8:
            raise ValueError(f"{name}: not a 64-bit Skyrim SE Havok packfile")


def common_conditions() -> list[dict]:
    return [
        {
            "condition": "IsActorBase",
            "requiredVersion": "1.0.0.0",
            "Actor base": {"pluginName": "Skyrim.esm", "formID": "7"},
        },
        {
            "condition": "CompareValues",
            "requiredVersion": "1.0.0.0",
            "Value A": {
                "graphVariable": "bDAF_FlightActive",
                "graphVariableType": "Bool",
            },
            "Comparison": "==",
            "Value B": {"value": 1.0},
        },
        {
            "condition": "CompareValues",
            "requiredVersion": "1.0.0.0",
            "Value A": {
                "graphVariable": "bDAF_FlightCombatActive",
                "graphVariableType": "Bool",
            },
            "Comparison": "==",
            "Value B": {"value": 1.0},
        },
    ]


def write_config(path: Path, spec: BuildSpec) -> None:
    config = {
        "name": f"Dragon Aspect Flight - {spec.folder.split(' - ', 1)[1]}",
        "description": (
            "Bundled NickNak aerial animation selected only for the player while "
            "Dragon Aspect Flight combat is active."
        ),
        "priority": spec.priority,
        "interruptible": True,
        "conditions": [*common_conditions(), spec.equipment_condition],
    }
    path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")


def source_for_vanilla(spec: BuildSpec, target: str) -> str:
    if "attackpower" in target:
        if spec.power_source is None:
            raise ValueError(f"{spec.folder}: no power-attack source for {target}")
        return spec.power_source
    if spec.folder == "20 - One Handed" and target == "1hm_attackleft.hkx":
        return "1hm_Air_Attack_Left.hkx"
    if spec.attack_source is None:
        raise ValueError(f"{spec.folder}: no attack source for {target}")
    return spec.attack_source


def convert_source_to_xml(hkxc: Path, source: Path, destination: Path) -> None:
    run(
        [
            str(hkxc),
            "convert",
            "--input",
            str(source),
            "--output",
            str(destination),
            "--format",
            "xml",
        ]
    )
    if not destination.is_file():
        raise RuntimeError(f"hkxc did not create {destination}")


def param(element: ET.Element, name: str) -> ET.Element:
    found = element.find(f".//hkparam[@name='{name}']")
    if found is None:
        raise ValueError(f"HKX XML has no {name} parameter")
    return found


def annotation_text(annotation: ET.Element) -> str:
    text = annotation.find("hkparam[@name='text']")
    return text.text if text is not None and text.text else ""


def add_annotation(parent: ET.Element, time: float, text: str) -> None:
    annotation = ET.SubElement(parent, "hkobject")
    time_param = ET.SubElement(annotation, "hkparam", {"name": "time"})
    time_param.text = f"{time:.6f}"
    text_param = ET.SubElement(annotation, "hkparam", {"name": "text"})
    text_param.text = text


def sort_annotations(annotations: ET.Element) -> None:
    ordered = sorted(
        annotations.findall("hkobject"),
        key=lambda annotation: (
            float((annotation.find("hkparam[@name='time']").text or "0")),
            annotation_text(annotation),
        ),
    )
    for annotation in list(annotations):
        annotations.remove(annotation)
    for annotation in ordered:
        annotations.append(annotation)
    annotations.set("numelements", str(len(ordered)))


def inject_power_attack_annotations(source_xml: Path) -> None:
    events = POWER_ATTACK_EVENTS.get(source_xml.stem + ".hkx")
    if events is None:
        return

    tree = ET.parse(source_xml)
    root = tree.getroot()
    animation = next(
        (
            item
            for item in root.iter("hkobject")
            if item.get("class")
            in {"hkaInterleavedUncompressedAnimation", "hkaSplineCompressedAnimation"}
        ),
        None,
    )
    if animation is None:
        raise ValueError(f"{source_xml}: no animation object")

    annotation_tracks = param(animation, "annotationTracks")
    first_track = annotation_tracks.find("hkobject")
    if first_track is None:
        raise ValueError(f"{source_xml}: animation has no annotation track")
    annotations = first_track.find("hkparam[@name='annotations']")
    if annotations is None:
        raise ValueError(f"{source_xml}: first track has no annotations parameter")

    # The donor behavior supplied these triggers externally. Strip its editor
    # markers and make the clip self-contained for vanilla and MCO behaviors.
    for annotation in list(annotations):
        annotations.remove(annotation)
    for time, text in events:
        add_annotation(annotations, time, text)
    sort_annotations(annotations)
    tree.write(source_xml, encoding="ascii", xml_declaration=True)


def inject_mco_annotations(source_xml: Path, target_xml: Path, attack_index: int) -> None:
    tree = ET.parse(source_xml)
    root = tree.getroot()
    animation = next(
        (
            item
            for item in root.iter("hkobject")
            if item.get("class")
            in {"hkaInterleavedUncompressedAnimation", "hkaSplineCompressedAnimation"}
        ),
        None,
    )
    if animation is None:
        raise ValueError(f"{source_xml}: no animation object")

    duration_element = param(animation, "duration")
    duration = float(duration_element.text or "0")
    annotation_tracks = param(animation, "annotationTracks")
    first_track = annotation_tracks.find("hkobject")
    if first_track is None:
        raise ValueError(f"{source_xml}: animation has no annotation track")
    annotations = first_track.find("hkparam[@name='annotations']")
    if annotations is None:
        raise ValueError(f"{source_xml}: first track has no annotations parameter")

    existing = list(annotations.findall("hkobject"))
    hit_time = duration * 0.45
    for annotation in existing:
        text = annotation_text(annotation)
        if text == "HitFrame":
            time_element = annotation.find("hkparam[@name='time']")
            if time_element is not None and time_element.text:
                hit_time = float(time_element.text)
        if (
            text.startswith("MCO_")
            or text.startswith("PIE.@SGV")
            or text == "attackStop"
        ):
            annotations.remove(annotation)

    window_open = min(duration * 0.78, max(hit_time + 0.10, duration * 0.62))
    window_close = min(duration - 0.02, max(window_open + 0.08, duration * 0.90))
    next_attack = 1 if attack_index >= 10 else attack_index + 1
    payloads = (
        (min(0.06, duration * 0.05), f"PIE.@SGVI|MCO_nextattack|{next_attack}"),
        (min(0.06, duration * 0.05), f"PIE.@SGVI|MCO_nextpowerattack|{next_attack}"),
        (min(0.06, duration * 0.05), "PIE.@SGVF|MCO_AttackSpeed|1"),
        (min(0.133333, duration * 0.10), "CastOKStart"),
        (window_open, "MCO_WinOpen"),
        (window_open, "MCO_PowerWinOpen"),
        (window_open, "MCO_Recovery"),
        (window_close, "MCO_WinClose"),
        (window_close, "MCO_PowerWinClose"),
        (duration, "attackStop"),
    )
    for time, text in payloads:
        add_annotation(annotations, time, text)

    sort_annotations(annotations)

    target_xml.parent.mkdir(parents=True, exist_ok=True)
    tree.write(target_xml, encoding="ascii", xml_declaration=True)


def convert_xml_to_amd64(hkxc: Path, source: Path, destination: Path) -> None:
    run(
        [
            str(hkxc),
            "convert",
            "--input",
            str(source),
            "--output",
            str(destination),
            "--format",
            "amd64",
        ]
    )
    if not destination.is_file():
        raise RuntimeError(f"hkxc did not create {destination}")
    data = destination.read_bytes()
    if len(data) < 0x38 or data[:4] != b"\x57\xe0\xe0\x57" or data[0x10] != 8:
        raise ValueError(f"{destination}: hkxc did not create a 64-bit HKX")


def build(args: argparse.Namespace) -> None:
    source_root = args.source_root.resolve()
    output_root = args.output_root.resolve()
    hkxc = args.hkxc.resolve()
    if not hkxc.is_file():
        raise FileNotFoundError(f"hkxc not found: {hkxc}")
    validate_sources(source_root)

    output_root.mkdir(parents=True, exist_ok=True)
    generated: list[Path] = []
    with tempfile.TemporaryDirectory(prefix="daf-flight-combat-") as temporary:
        temporary_root = Path(temporary)
        source_xml: dict[str, Path] = {}

        def prepared_source_xml(source_name: str) -> Path:
            if source_name not in source_xml:
                xml_path = temporary_root / f"{Path(source_name).stem}.xml"
                convert_source_to_xml(hkxc, source_root / source_name, xml_path)
                inject_power_attack_annotations(xml_path)
                source_xml[source_name] = xml_path
            return source_xml[source_name]

        for spec in BUILD_SPECS:
            submod = output_root / spec.folder
            submod.mkdir(parents=True, exist_ok=True)
            write_config(submod / "config.json", spec)

            for target in (*LOCOMOTION_TARGETS, *spec.ready_targets):
                target_path = submod / target
                shutil.copy2(source_root / spec.ready_source, target_path)
                generated.append(target_path)

            if spec.attack_source is None:
                continue

            for target in MCO_TARGETS:
                source_name = (
                    spec.power_source
                    if target in MCO_POWER_TARGETS
                    else spec.attack_source
                )
                if source_name is None:
                    raise ValueError(f"{spec.folder}: no source for {target}")
                attack_index = int(
                    "".join(character for character in target if character.isdigit())
                )
                target_xml = temporary_root / spec.folder / f"{target}.xml"
                inject_mco_annotations(
                    prepared_source_xml(source_name), target_xml, attack_index
                )
                target_path = submod / target
                convert_xml_to_amd64(hkxc, target_xml, target_path)
                generated.append(target_path)

            if spec.vanilla_family:
                for target in VANILLA_TARGETS[spec.vanilla_family]:
                    target_path = submod / target
                    source_name = source_for_vanilla(spec, target)
                    if source_name in POWER_ATTACK_EVENTS:
                        convert_xml_to_amd64(
                            hkxc, prepared_source_xml(source_name), target_path
                        )
                    else:
                        shutil.copy2(source_root / source_name, target_path)
                    generated.append(target_path)

    expected = {path.resolve() for path in generated}
    extras = {
        path.resolve()
        for path in output_root.rglob("*.hkx")
        if path.resolve() not in expected
    }
    if extras:
        raise ValueError(
            "stale HKX files exist under the generated output:\n"
            + "\n".join(str(path) for path in sorted(extras))
        )

    run([str(hkxc), "verify", str(output_root)])

    version = subprocess.run(
        [str(hkxc), "--version"], capture_output=True, text=True, check=False
    ).stdout.strip()
    manifest = {
        "source": (
            "More Draconic Aspect - Flight Combat/"
            "meshes/actors/character/animations/NickNak"
        ),
        "sourceHashes": SOURCE_HASHES,
        "tool": {
            "name": "serde-hkx hkxc",
            "version": version,
            "project": "https://github.com/SARDONYX-sard/serde-hkx",
        },
        "generatedClipCount": len(generated),
        "generatedClips": [
            {
                "path": str(path.relative_to(output_root)).replace("\\", "/"),
                "sha256": sha256(path),
                "bytes": path.stat().st_size,
            }
            for path in generated
        ],
    }
    (output_root / "animation-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Generated and independently verified {len(generated)} HKX clips")


def main() -> int:
    repository_root = Path(__file__).resolve().parents[1]
    default_output = (
        repository_root
        / "Data"
        / "meshes"
        / "actors"
        / "character"
        / "animations"
        / "OpenAnimationReplacer"
        / "Dragon Aspect Flight - Flight Combat"
    )
    default_hkxc = os.environ.get("DAF_HKXC") or shutil.which("hkxc")
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
    parser.add_argument("--output-root", type=Path, default=default_output)
    parser.add_argument(
        "--hkxc",
        type=Path,
        required=default_hkxc is None,
        default=Path(default_hkxc) if default_hkxc else None,
        help="serde-hkx hkxc.exe 2.0.0 or newer",
    )
    args = parser.parse_args()
    build(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
