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

from AerializeHkx import aerialize, backend_provenance, load_backend


ROOT_SCOPE = pathlib.Path()
RELEASE_VERSION = "1.8.3"
_STAGED_OUTPUT_ROOT: pathlib.Path | None = None


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

UNARMED_MOTION = standard_motion("h2h_") | names(
    "h2h_idle.hkx h2h_sprintforwardsword.hkx 1hm_sprintforwardsword.hkx"
)
ONE_HANDED_MOTION = standard_motion("1hm_") | names(
    """
    1hm_idle.hkx 1hm_blockidle.hkx 1hm_sprintforwardsword.hkx
    sneak1hm_idle.hkx shd_blockidle.hkx
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

COMMON_VANILLA_ACTIONS = names(
    """
    mt_shout_inhale.hkx mt_shout_exhale.hkx
    mt_shout_exhale_medium.hkx mt_shout_exhale_long.hkx
    1hm_shout_inhale.hkx 1hm_shout_exhale.hkx
    1hm_shout_exhale_medium.hkx 1hm_shout_exhale_long.hkx
    sneak1hm_shout_inhale.hkx sneak1hm_shout_exhale.hkx
    sneak1hm_shout_exhale_medium.hkx sneak1hm_shout_exhale_long.hkx
    """
)

BLOCK_ACTION_PROFILES: dict[str, set[str]] = {
    "one_handed": names(
        """
        1hm_blockanticipate.hkx 1hm_blockbash.hkx 1hm_blockbashintro.hkx
        1hm_blockbashpower.hkx 1hm_blockhit.hkx 1hm_blockhita.hkx
        1hm_blockhitb.hkx 1hm_blockidle.hkx
        shd_blockanticipate.hkx shd_blockbash.hkx shd_blockbashintro.hkx
        shd_blockbashpower.hkx shd_blockbashsprint.hkx shd_blockhit.hkx
        shd_blockhit_vara.hkx shd_blockhit_varb.hkx shd_blockidle.hkx
        shd_blocktimed.hkx
        tor_blockanticipate.hkx tor_blockanticipatemt.hkx tor_blockbash.hkx
        tor_blockbashintro.hkx tor_blockbashpower.hkx tor_blockhit.hkx
        tor_blockhitmt.hkx tor_blockidle.hkx
        """
    ),
    "dual_wield": names(
        """
        dw1hm1hmblockbash.hkx dw1hm1hmblockbashintro.hkx
        dw1hm1hmblockbashpower.hkx dw1hm1hmblockidle.hkx
        dw1hm1hmmovingblockidle.hkx
        """
    ),
    "greatsword": names(
        """
        2hm_blockanticipate.hkx 2hm_blockbash.hkx 2hm_blockbashintro.hkx
        2hm_blockbashpower.hkx 2hm_blockhit.hkx 2hm_blockhita.hkx
        2hm_blockhitb.hkx 2hm_blockidle.hkx
        """
    ),
    "axe_warhammer": names(
        """
        2hw_blockanticipate.hkx 2hw_blockbash.hkx 2hw_blockbashintro.hkx
        2hw_blockbashpower.hkx 2hw_blockhit.hkx 2hw_blockhita.hkx
        2hw_blockhitb.hkx 2hw_blockidle.hkx 2hw_blockstart.hkx
        2hw_blockstop.hkx
        """
    ),
    "bow": names(
        """
        bow_blockanticipate.hkx bow_blockbash.hkx bow_blockbashintro.hkx
        bow_blockbashpower.hkx bow_blockhit.hkx bow_blockidle.hkx
        bow_drawheavy.hkx bow_drawlight.hkx sneakbow_drawlight.hkx
        """
    ),
    "crossbow": {"dlc01/crossbow_blockbash.hkx"},
    "staff": names("staff_bash.hkx staff_bashintro.hkx"),
}

QUARTERSTAFF_AA_ACTIONS = names(
    """
    2hw_blockanticipate.hkx 2hw_blockbashintro.hkx 2hw_blockbashpower.hkx
    2hw_blockhit.hkx 2hw_blockidle.hkx
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
MCO_SPECIAL_ATTACKS = (
    {f"mco_powerattack{index}.hkx" for index in range(1, 11)}
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


def equipped_keyword(editor_id: str, left_hand: bool = False) -> dict[str, object]:
    return {
        "condition": "IsEquippedHasKeyword",
        "requiredVersion": "1.0.0.0",
        "Keyword": {"editorID": editor_id},
        "Left hand": left_hand,
    }


def equipped_keyword_any(
    editor_ids: tuple[str, ...], *, hands: tuple[bool, ...] = (False, True)
) -> dict[str, object]:
    """Build an OAR OR over proven keyword EditorIDs and hand scopes."""
    return {
        "condition": "OR",
        "requiredVersion": "1.0.0.0",
        "Conditions": [
            equipped_keyword(editor_id, left_hand=left_hand)
            for left_hand in hands
            for editor_id in editor_ids
        ],
    }


def equipped_keyword_any_for_types(
    editor_ids: tuple[str, ...],
    types: tuple[int, ...],
    *,
    hands: tuple[bool, ...] = (False, True),
) -> dict[str, object]:
    """Require a keyword and a two-handed/unknown OAR type on the same hand."""
    return {
        "condition": "OR",
        "requiredVersion": "1.0.0.0",
        "Conditions": [
            {
                "condition": "AND",
                "requiredVersion": "1.0.0.0",
                "Conditions": [
                    equipped_keyword(editor_id, left_hand=left_hand),
                    equipped_any(types, hands=(left_hand,)),
                ],
            }
            for left_hand in hands
            for editor_id in editor_ids
        ],
    }


def equipped_form(plugin_name: str, form_id: str, left_hand: bool = False) -> dict[str, object]:
    """Build an optional IsEquipped form condition.

    OAR evaluates an unresolved plugin/form link as false, so these optional
    active-load-order fallbacks do not make the stack depend on those plugins.
    """
    return {
        "condition": "IsEquipped",
        "requiredVersion": "1.0.0.0",
        "Form": {"pluginName": plugin_name, "formID": form_id},
        "Left hand": left_hand,
    }


def equipped_form_any(
    plugin_name: str, form_ids: tuple[str, ...], *, hands: tuple[bool, ...] = (False, True)
) -> dict[str, object]:
    """Build an OAR OR over optional active-load-order weapon forms."""
    return {
        "condition": "OR",
        "requiredVersion": "1.0.0.0",
        "Conditions": [
            equipped_form(plugin_name, form_id, left_hand=left_hand)
            for left_hand in hands
            for form_id in form_ids
        ],
    }


GENERIC_TWO_HANDED_POLEARM_KEYWORDS = (
    "WeapTypePike",
    "WeapTypeSpear",
    "WeapTypeHalberd",
)
EXPLICIT_TWO_HANDED_POLEARM_KEYWORDS = (
    "OCF_WeapTypePike2H",
    "OCF_WeapTypeSpear2H",
    "OCF_WeapTypeHalberd2H",
    "OCF_WeapTypePole2H_Thrust",
    "OCF_WeapTypePole2H_Swing",
)
EXPLICIT_TWO_HANDED_NODACHI_KEYWORDS = (
    "WeapTypeNodachi",
    "OCF_WeapTypeKatana2H",
)
ACTIVE_NO_KEYWORD_SPEAR_FORMS = (
    equipped_form_any(
        "Spear of Skyrim.esp",
        tuple(f"{form_id:X}" for form_id in range(0x80B, 0x816)),
        hands=(False,),
    ),
    equipped_form_any("Spear of Omicron.esp", ("D62",), hands=(False,)),
)


def greatsword_equipment_condition() -> dict[str, object]:
    """Route greatsword, unknown, nodachi, and proven polearms to 2hm.

    OAR's type 6 is indistinguishable from a real battleaxe when a spear has
    no keyword. The two active plugin/form branches above are therefore kept
    optional and narrowly scoped; generic type 6 remains Base 50.
    """
    return {
        "condition": "OR",
        "requiredVersion": "1.0.0.0",
        "Conditions": [
            equipped_any((5, -1)),
            equipped_keyword_any_for_types(
                GENERIC_TWO_HANDED_POLEARM_KEYWORDS,
                (5, 6, 10, -1),
            ),
            equipped_keyword_any(
                EXPLICIT_TWO_HANDED_POLEARM_KEYWORDS
                + EXPLICIT_TWO_HANDED_NODACHI_KEYWORDS
            ),
            *ACTIVE_NO_KEYWORD_SPEAR_FORMS,
        ],
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
    block_profile: str | None = None
    common_vanilla_actions: bool = False
    quarterstaff_actions: bool = False
    action_aliases: tuple[tuple[str, str], ...] = ()
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
        common_vanilla_actions=True,
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
        UNARMED_ATTACKS | MCO_SPECIAL_ATTACKS,
        "H2h",
    ),
    Family(
        "Flight Base 20 - One Handed",
        "DAF Flight Base - One Handed",
        "Root-stable one-handed/shield flight locomotion and vanilla/MCO special aerial attacks.",
        2147483602,
        "Flying_Mod_Idle.hkx",
        ONE_HANDED_MOTION,
        flight_conditions(equipped_any((1, 2, 3, 4, 11))),
        ONE_HANDED_ATTACKS | MCO_SPECIAL_ATTACKS,
        "1hm",
        block_profile="one_handed",
        action_aliases=(
            ("maxsu_shieldblockhit.hkx", "shd_blockhit.hkx"),
            ("maxsu_weaponblockhit.hkx", "1hm_blockhit.hkx"),
        ),
    ),
    Family(
        "Flight Base 30 - Dual Wield",
        "DAF Flight Base - Dual Wield",
        "Root-stable dual-wield flight locomotion and vanilla/MCO special aerial attacks.",
        2147483603,
        "Flying_Mod_Idle.hkx",
        DUAL_WIELD_MOTION,
        flight_conditions(
            equipped_any((1, 2, 3, 4), hands=(False,)),
            equipped_any((1, 2, 3, 4), hands=(True,)),
        ),
        DUAL_WIELD_ATTACKS | MCO_SPECIAL_ATTACKS,
        "Dw",
        block_profile="dual_wield",
        action_aliases=(("maxsu_weaponblockhit.hkx", "1hm_blockhit.hkx"),),
    ),
    Family(
        "Flight Base 40 - Greatsword",
        "DAF Flight Base - Greatsword",
        "Root-stable greatsword flight locomotion and vanilla/MCO special aerial attacks; explicit fallback for unknown/custom two-handed polearms and nodachi.",
        2147483605,
        "Flying_Mod_Idle.hkx",
        GREATSWORD_MOTION,
        flight_conditions(greatsword_equipment_condition()),
        GREATSWORD_ATTACKS | MCO_SPECIAL_ATTACKS,
        "2hm",
        block_profile="greatsword",
        action_aliases=(("maxsu_weaponblockhit.hkx", "2hm_blockhit.hkx"),),
    ),
    Family(
        "Flight Base 50 - Axe and Warhammer",
        "DAF Flight Base - Axe and Warhammer",
        "Root-stable battleaxe/warhammer flight locomotion and vanilla/MCO special aerial attacks.",
        2147483604,
        "Flying_Mod_Idle.hkx",
        AXE_WARHAMMER_MOTION,
        flight_conditions(equipped_any((6, 10))),
        AXE_WARHAMMER_ATTACKS | MCO_SPECIAL_ATTACKS,
        "2hw",
        block_profile="axe_warhammer",
        action_aliases=(("maxsu_weaponblockhit.hkx", "2hw_blockhit.hkx"),),
    ),
    Family(
        "Flight Base 55 - Quarterstaff",
        "DAF Flight Base - Quarterstaff",
        "Keyword-routed quarterstaff flight locomotion, attacks, block, bash, draw, and sheathe.",
        2147483610,
        "Flying_Mod_Idle.hkx",
        AXE_WARHAMMER_MOTION,
        flight_conditions(
            equipped_keyword_any(
                (
                    "WeapTypeQtrStaff",
                    "WeapTypeQuarterstaff",
                    "OCF_WeapTypeQuarterstaff2H",
                ),
                hands=(False, True),
            )
        ),
        AXE_WARHAMMER_ATTACKS | MCO_SPECIAL_ATTACKS,
        "2hw",
        block_profile="axe_warhammer",
        quarterstaff_actions=True,
        action_aliases=(("maxsu_weaponblockhit.hkx", "2hw_blockhit.hkx"),),
    ),
    Family(
        "Flight Base 60 - Bow",
        "DAF Flight Base - Bow",
        "Root-stable bow flight locomotion; functional draw/release clips remain intact.",
        2147483606,
        "Flying_Mod_Idle.hkx",
        BOW_MOTION,
        flight_conditions(equipped_any((7,))),
        block_profile="bow",
        action_aliases=(
            ("xpe0_bow_drawheavy.hkx", "bow_drawheavy.hkx"),
            ("xpe0_bow_drawlight.hkx", "bow_drawlight.hkx"),
            ("xpe0_sneakbow_drawlight.hkx", "sneakbow_drawlight.hkx"),
        ),
    ),
    Family(
        "Flight Base 70 - Crossbow",
        "DAF Flight Base - Crossbow",
        "Root-stable crossbow flight locomotion; functional release/reload clips remain intact.",
        2147483607,
        "Flying_Mod_Idle.hkx",
        CROSSBOW_MOTION,
        flight_conditions(equipped_any((9,))),
        block_profile="crossbow",
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
        block_profile="staff",
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--hkxcmd", type=pathlib.Path)
    parser.add_argument("--pynifly-hkx-dir", type=pathlib.Path)
    parser.add_argument(
        "--vanilla-animation-root",
        type=pathlib.Path,
        help="Extracted Skyrim Data/meshes/actors/character/animations directory",
    )
    parser.add_argument(
        "--refresh-metadata",
        action="store_true",
        help="Refresh coverage and hashes after a reviewed output-only alias removal.",
    )
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
        raise RuntimeError(
            "hkxcmd completed without producing decoded XML; refusing to accept a "
            f"header-only validation result: {source}"
        )


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


ACTION_OFFSET_SLOT_NAMES = {
    "dualmagic_idle.hkx",
    "mlh_idle.hkx",
    "mrh_idle.hkx",
    "dmagaimconcharge.hkx",
    "dmagpreaimconcharge.hkx",
    "dmagpreselfconcharge.hkx",
    "dmagselfconcharge.hkx",
}


def is_action_or_offset_slot(name: str) -> bool:
    """Return whether a source is an action/upper-body slot, not locomotion.

    This list is intentionally conservative.  Only the known magic/staff
    charge, concentration, idle, and cast-offset families are protected; a
    normal ``mag_run*`` locomotion source still receives the flight donor.
    """

    lowered = name.casefold()
    if lowered in ACTION_OFFSET_SLOT_NAMES:
        return True
    if lowered.startswith("staffmagiccast_"):
        return True
    if lowered.startswith(("dmag", "mlh_", "mrh_")) and any(
        token in lowered for token in ("charge", "concentration", "aim", "ward", "idle")
    ):
        return True
    return False


def attack_source_name(family: Family, animation_name: str) -> str:
    if family.attack_prefix is None:
        raise ValueError(f"Family has no attack source: {family.display_name}")
    if is_intro_or_recovery(animation_name):
        raise ValueError(
            f"{animation_name} requires an exact original action source; "
            "the flight base is not a valid intro/recovery donor"
        )
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


def write_hash_manifest(
    data_root: pathlib.Path,
    *,
    output_path: pathlib.Path | None = None,
    overlay_oar_root: pathlib.Path | None = None,
) -> int:
    """Write an asset manifest, optionally overlaying an unpublished OAR tree.

    The overlay lets the caller prepare metadata before publishing the new
    directory, so a failed metadata write cannot leave an installed tree with
    stale hashes.
    """

    oar_target = data_root / "meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight"
    assets: dict[str, pathlib.Path] = {}
    for path in (data_root / "meshes").rglob("*"):
        if not path.is_file() or path.suffix.lower() not in {".hkx", ".nif"}:
            continue
        if overlay_oar_root is not None and oar_target in path.parents:
            continue
        assets[path.relative_to(data_root).as_posix()] = path
    if overlay_oar_root is not None:
        for path in overlay_oar_root.rglob("*"):
            if path.is_file() and path.suffix.lower() in {".hkx", ".nif"}:
                relative = path.relative_to(overlay_oar_root)
                virtual = oar_target.relative_to(data_root) / relative
                assets[virtual.as_posix()] = path
    ordered_assets = sorted(assets.items(), key=lambda item: item[0].lower())
    lines = [
        f"Dragon Aspect Flight {RELEASE_VERSION} bundled animation/effect asset SHA-256",
        "=================================================================",
        "",
    ]
    lines.extend(f"{sha256(path).lower()} *{relative}" for relative, path in ordered_assets)
    manifest = output_path or (data_root / "SKSE/Plugins/DragonAspectFlight-AnimationHashes.txt")
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return len(ordered_assets)


def expected_family_animation_names(
    family: Family, *, magic_source_names: set[str]
) -> set[str]:
    """Return the exact reviewed original paths owned by one OAR family."""
    result = set(family.motion_names) | set(family.attack_names)
    if family.common_vanilla_actions:
        result.update(COMMON_VANILLA_ACTIONS)
    if family.block_profile:
        result.update(BLOCK_ACTION_PROFILES[family.block_profile])
    if family.quarterstaff_actions:
        result.update(QUARTERSTAFF_AA_ACTIONS)
    result.update(target for target, _ in family.action_aliases)
    if family.magic_sources:
        result.update(name for name in magic_source_names if not name.startswith("staff"))
        result.update(MAGIC_SOURCE_ALIASES)
    if family.staff_sources:
        result.update(name for name in magic_source_names if name.startswith("staff"))
    return {name.lower() for name in result}


def expected_family_output_paths(
    family: Family, *, magic_source_names: set[str]
) -> set[str]:
    """Return the exact reviewed scope/original paths owned by one family."""
    names_for_family = expected_family_animation_names(
        family,
        magic_source_names=magic_source_names,
    )
    return {
        (scope / name).as_posix().lower()
        for scope in family.scopes
        for name in names_for_family
    }


def coverage_entry_for_family(family: Family, *, magic_source_names: set[str]) -> dict[str, object]:
    """Generate metadata from the same contract used to validate OAR files."""
    source_names = {
        name
        for name in magic_source_names
        if (family.magic_sources and not name.startswith("staff"))
        or (family.staff_sources and name.startswith("staff"))
    }
    action_names: set[str] = set()
    if family.common_vanilla_actions:
        action_names.update(COMMON_VANILLA_ACTIONS)
    if family.block_profile:
        action_names.update(BLOCK_ACTION_PROFILES[family.block_profile])
    if family.quarterstaff_actions:
        action_names.update(QUARTERSTAFF_AA_ACTIONS)
    action_names.update(target for target, _ in family.action_aliases)
    base_pose_overrides = {
        name
        for name in source_names
        if name in family.motion_names and not is_action_or_offset_slot(name)
    }
    entry: dict[str, object] = {
        "priority": family.priority,
        "baseSource": family.base_source,
        "animationCountPerScope": len(
            expected_family_animation_names(family, magic_source_names=magic_source_names)
        ),
        "motionNames": sorted(family.motion_names),
        "attackNames": sorted(family.attack_names),
        "actionNames": sorted(action_names),
        "scopes": [scope.as_posix() or "." for scope in family.scopes],
        "aerializedMagicSources": family.magic_sources or family.staff_sources,
        "blockProfile": family.block_profile,
        "usesVanillaActionComposites": bool(action_names),
        "usesAnimatedArmouryQuarterstaffComposites": family.quarterstaff_actions,
        "actionSourceAliases": dict(sorted(family.action_aliases)),
    }
    if source_names - base_pose_overrides:
        entry["aerializedSourceNames"] = sorted(source_names - base_pose_overrides)
    if base_pose_overrides:
        entry["basePoseOverrideNames"] = sorted(base_pose_overrides)
    if family.magic_sources:
        entry["aerializedSourceAliases"] = dict(sorted(MAGIC_SOURCE_ALIASES.items()))
    return entry


def validate_reviewed_output_tree(
    oar_root: pathlib.Path, *, magic_source_names: set[str]
) -> set[str]:
    """Reject every addition or deletion outside the checked-in family contract."""
    expected_paths = {
        (pathlib.PurePosixPath(family.directory) / relative_path).as_posix().lower()
        for family in FAMILIES
        for relative_path in expected_family_output_paths(
            family,
            magic_source_names=magic_source_names,
        )
    }
    actual_paths = {
        path.relative_to(oar_root).as_posix().lower()
        for path in oar_root.rglob("*.hkx")
    }
    unexpected = sorted(actual_paths - expected_paths)
    missing = sorted(expected_paths - actual_paths)
    if unexpected or missing:
        raise RuntimeError(
            "Reviewed flight stack mismatch: "
            f"unexpected={len(unexpected)} ({unexpected[:3]}), "
            f"missing={len(missing)} ({missing[:3]})"
        )
    return actual_paths


def refresh_metadata(data_root: pathlib.Path) -> int:
    """Refresh generated metadata without manufacturing or altering HKX clips.

    The checked-in family definitions are the contract. This command never
    creates or deletes clips: it only rewrites metadata after the entire OAR
    tree exactly matches that contract.
    """
    oar_root = data_root / "meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight"
    coverage_path = data_root / "SKSE/Plugins/DragonAspectFlight-AnimationCoverage.json"
    magic_source_root = data_root.parent / "third_party/xp32-magic"
    magic_source_names = {path.name.lower() for path in magic_source_root.glob("*.hkx")}
    if len(magic_source_names) != 79:
        raise RuntimeError(f"Expected 79 credited xp32/Neumeria magic sources, found {len(magic_source_names)}")

    actual_paths = validate_reviewed_output_tree(
        oar_root,
        magic_source_names=magic_source_names,
    )

    coverage: dict[str, object] = {}
    for family in FAMILIES:
        coverage[family.directory] = coverage_entry_for_family(
            family,
            magic_source_names=magic_source_names,
        )
    with tempfile.TemporaryDirectory(prefix=".daf-refresh-metadata-", dir=data_root) as metadata_directory:
        metadata_root = pathlib.Path(metadata_directory)
        staged_coverage = metadata_root / coverage_path.name
        staged_hashes = metadata_root / "DragonAspectFlight-AnimationHashes.txt"
        write_json(
            staged_coverage,
            {
                "version": RELEASE_VERSION,
                "scopes": sorted({scope.as_posix() or "." for family in FAMILIES for scope in family.scopes}),
                "families": coverage,
                "totalOarHkx": len(actual_paths),
                "provenance": {
                    "tool": "BuildFlightAnimationStack.py",
                    "toolVersion": "1",
                    "mode": "metadata-refresh",
                    "codec": "not used; existing HKX tree validated by manifest",
                },
            },
        )
        manifest_count = write_hash_manifest(data_root, output_path=staged_hashes)
        _atomic_replace_files(
            [
                (staged_coverage, coverage_path),
                (staged_hashes, data_root / "SKSE/Plugins/DragonAspectFlight-AnimationHashes.txt"),
            ],
            data_root,
        )
    print(
        f"Refreshed output metadata for {len(actual_paths)} DAF OAR aliases "
        f"and {manifest_count} bundled HKX/NIF assets."
    )
    return 0


def _atomic_replace_directory(staged_root: pathlib.Path, target_root: pathlib.Path) -> None:
    """Replace an output directory only after every staged file validates.

    ``os.replace`` cannot portably overwrite a populated directory on Windows.
    Rename the old tree aside, install the completed staging tree, and restore
    the old tree if the second rename fails.  The caller owns the staged path
    and may remove it after this function returns.
    """

    target_root.parent.mkdir(parents=True, exist_ok=True)
    backup_root: pathlib.Path | None = None
    published = False
    try:
        if target_root.exists():
            backup_root = pathlib.Path(
                tempfile.mkdtemp(prefix=f".{target_root.name}.backup-", dir=target_root.parent)
            )
            backup_root.rmdir()
            target_root.rename(backup_root)
    except Exception:
        if backup_root is not None and backup_root.exists() and not any(backup_root.iterdir()):
            backup_root.rmdir()
        raise
    try:
        staged_root.rename(target_root)
        published = True
    except Exception as install_error:
        if backup_root is not None and not target_root.exists():
            try:
                backup_root.rename(target_root)
                backup_root = None
            except Exception as restore_error:
                # Keep the last known-good tree discoverable.  Never allow the
                # cleanup path to erase it after a double failure.
                message = (
                    "animation stack install and rollback both failed; "
                    f"intact backup retained at {backup_root}: {restore_error}"
                )
                if hasattr(install_error, "add_note"):
                    install_error.add_note(message)
                raise install_error from restore_error
        raise
    finally:
        if published and backup_root is not None and backup_root.exists():
            shutil.rmtree(backup_root)


def _atomic_replace_files(
    staged_files: list[tuple[pathlib.Path, pathlib.Path]],
    transaction_parent: pathlib.Path,
) -> None:
    """Replace metadata files together, retaining a transaction on rollback failure."""

    transaction_parent.mkdir(parents=True, exist_ok=True)
    transaction_root = pathlib.Path(
        tempfile.mkdtemp(prefix=".dragon-aspect-flight-metadata-", dir=transaction_parent)
    )
    previous_root = transaction_root / "previous"
    failed_root = transaction_root / "failed-candidate"
    previous_root.mkdir()
    failed_root.mkdir()
    backups: list[tuple[pathlib.Path, pathlib.Path]] = []
    installed: list[pathlib.Path] = []
    try:
        for index, (_, target) in enumerate(staged_files):
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                backup = previous_root / f"target-{index}"
                target.rename(backup)
                backups.append((target, backup))
        for index, (staged, target) in enumerate(staged_files):
            staged.rename(target)
            installed.append(target)
    except Exception as publish_error:
        rollback_error: Exception | None = None
        for index, target in reversed(list(enumerate(installed))):
            if not target.exists():
                continue
            try:
                target.rename(failed_root / f"target-{index}")
            except Exception as error:
                rollback_error = rollback_error or error
        for target, backup in reversed(backups):
            if not backup.exists():
                continue
            try:
                backup.rename(target)
            except Exception as error:
                rollback_error = rollback_error or error
        if rollback_error is not None:
            message = (
                "metadata replacement and rollback both failed; "
                f"transaction retained at {transaction_root}: {rollback_error}"
            )
            if hasattr(publish_error, "add_note"):
                publish_error.add_note(message)
            raise publish_error from rollback_error
        shutil.rmtree(transaction_root, ignore_errors=True)
        raise
    else:
        shutil.rmtree(transaction_root)


def _atomic_publish_candidate(
    staged_root: pathlib.Path,
    target_root: pathlib.Path,
    staged_files: list[tuple[pathlib.Path, pathlib.Path]],
    transaction_parent: pathlib.Path,
) -> None:
    """Publish an OAR tree and its metadata as one recoverable transaction.

    Every existing target is moved into a transaction directory first.  On a
    failure, newly installed targets are moved aside and the old set is
    restored.  A rollback failure leaves that transaction directory intact so
    the last good files remain recoverable instead of being deleted by a
    ``finally`` block.
    """

    transaction_parent.mkdir(parents=True, exist_ok=True)
    transaction_root = pathlib.Path(
        tempfile.mkdtemp(prefix=".dragon-aspect-flight-publish-", dir=transaction_parent)
    )
    backup_root = transaction_root / "previous"
    failed_root = transaction_root / "failed-candidate"
    backup_root.mkdir()
    failed_root.mkdir()
    targets = [(staged_root, target_root), *staged_files]
    backups: list[tuple[pathlib.Path, pathlib.Path]] = []
    installed: list[pathlib.Path] = []
    try:
        for index, (_, target) in enumerate(targets):
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                backup = backup_root / f"target-{index}"
                target.rename(backup)
                backups.append((target, backup))
        for staged, target in targets:
            staged.rename(target)
            installed.append(target)
    except Exception as publish_error:
        rollback_error: Exception | None = None
        for index, target in reversed(list(enumerate(installed))):
            if not target.exists():
                continue
            try:
                target.rename(failed_root / f"target-{index}")
            except Exception as error:
                rollback_error = rollback_error or error
        for target, backup in reversed(backups):
            if not backup.exists():
                continue
            try:
                backup.rename(target)
            except Exception as error:
                rollback_error = rollback_error or error
        if rollback_error is not None:
            message = (
                "candidate publication and rollback both failed; "
                f"transaction retained at {transaction_root}: {rollback_error}"
            )
            if hasattr(publish_error, "add_note"):
                publish_error.add_note(message)
            raise publish_error from rollback_error
        shutil.rmtree(transaction_root, ignore_errors=True)
        raise
    else:
        shutil.rmtree(transaction_root)


def _main() -> int:
    global _STAGED_OUTPUT_ROOT
    args = parse_args()
    repo_root = args.repo_root.resolve()
    data_root = repo_root / "Data"
    if args.refresh_metadata:
        return refresh_metadata(data_root)
    if args.hkxcmd is None or args.pynifly_hkx_dir is None or args.vanilla_animation_root is None:
        raise ValueError("--hkxcmd, --pynifly-hkx-dir, and --vanilla-animation-root are required to build clips")
    hkxcmd = args.hkxcmd.resolve()
    if not hkxcmd.is_file():
        raise FileNotFoundError(f"hkxcmd not found: {hkxcmd}")
    vanilla_root = args.vanilla_animation_root.resolve()
    if not vanilla_root.is_dir():
        raise FileNotFoundError(f"Vanilla animation root not found: {vanilla_root}")
    anim_skyrim = load_backend(args.pynifly_hkx_dir)

    nicknak_root = repo_root / "third_party/nicknak/animations"
    flying_root = repo_root / "third_party/flying-mod"
    magic_root = repo_root / "third_party/xp32-magic"
    animated_armoury_root = repo_root / "third_party/animated-armoury/quarterstaff"
    oar_root = data_root / "meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight"
    magic_sources = {path.name.lower(): path for path in magic_root.glob("*.hkx")}
    if len(magic_sources) != 79:
        raise RuntimeError(f"Expected 79 credited xp32/Neumeria magic sources, found {len(magic_sources)}")

    required_sources = {family.base_source for family in FAMILIES}
    for family in FAMILIES:
        if family.attack_prefix:
            for attack_name in family.attack_names:
                required_sources.add(attack_source_name(family, attack_name))

    required_vanilla_actions: set[str] = set()
    for family in FAMILIES:
        if family.common_vanilla_actions:
            required_vanilla_actions.update(COMMON_VANILLA_ACTIONS)
        if family.block_profile:
            required_vanilla_actions.update(BLOCK_ACTION_PROFILES[family.block_profile])
        for _, source_name in family.action_aliases:
            required_vanilla_actions.add(source_name)

    # Validate the complete source plan before touching the checked-in Data
    # tree.  In particular, attack_source_name deliberately fails closed for
    # intro/outro/loop/dummy/roll slots that need an exact original action
    # source.  A rejected plan must leave the existing output untouched.
    source_paths: dict[str, pathlib.Path] = {}
    for source_name in required_sources:
        source_path = flying_root / source_name if source_name == "Flying_Mod_Idle.hkx" else nicknak_root / source_name
        if not source_path.is_file():
            raise FileNotFoundError(f"Bundled animation source missing: {source_path}")
        source_paths[source_name] = source_path
    vanilla_paths: dict[str, pathlib.Path] = {}
    for action_name in required_vanilla_actions:
        source_path = vanilla_root.joinpath(*pathlib.PurePosixPath(action_name).parts)
        if not source_path.is_file():
            raise FileNotFoundError(f"Required vanilla animation missing: {source_path}")
        vanilla_paths[action_name] = source_path
    quarterstaff_paths = {
        action_name: animated_armoury_root / action_name for action_name in QUARTERSTAFF_AA_ACTIONS
    }
    for action_name, source_path in quarterstaff_paths.items():
        if not source_path.is_file():
            raise FileNotFoundError(f"Bundled Animated Armoury source missing: {source_path}")

    oar_root.parent.mkdir(parents=True, exist_ok=True)
    staged_oar_root = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{oar_root.name}.staging-", dir=oar_root.parent)
    )
    _STAGED_OUTPUT_ROOT = staged_oar_root
    oar_root = staged_oar_root
    oar_root.mkdir(parents=True, exist_ok=True)
    write_json(
        oar_root / "config.json",
        {
            "name": "Dragon Aspect Flight",
            "author": "nwn900",
            "description": "Flight-scoped, equipment-aware animation ownership for DAF.",
        },
    )

    built_sources: dict[str, pathlib.Path] = {}
    coverage: dict[str, object] = {}
    expected_outputs: set[pathlib.Path] = set()
    with tempfile.TemporaryDirectory(prefix="daf-flight-stack-") as temporary:
        temp_root = pathlib.Path(temporary)
        for source_name in sorted(required_sources, key=str.lower):
            source_hkx = source_paths[source_name]
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

        aerial_vanilla_actions: dict[str, pathlib.Path] = {}
        for action_name in sorted(required_vanilla_actions):
            source = vanilla_paths[action_name]
            safe_name = action_name.replace("/", "-")
            validate_hkx(hkxcmd, source, temp_root / f"vanilla-{safe_name}-validation.xml")
            composite = temp_root / "aerialized-vanilla" / pathlib.PurePosixPath(action_name)
            replaced_tracks = aerialize(
                anim_skyrim,
                flying_root / "Flying_Mod_Idle.hkx",
                source,
                composite,
            )
            aerial_vanilla_actions[action_name] = composite
            print(
                f"aerialized vanilla {action_name}: "
                f"replaced_tracks={len(replaced_tracks)} sha256={sha256(composite)}"
            )

        aerial_quarterstaff_actions: dict[str, pathlib.Path] = {}
        for action_name in sorted(QUARTERSTAFF_AA_ACTIONS):
            source = quarterstaff_paths[action_name]
            validate_hkx(hkxcmd, source, temp_root / f"aa-{action_name}-validation.xml")
            composite = temp_root / "aerialized-animated-armoury" / action_name
            replaced_tracks = aerialize(
                anim_skyrim,
                flying_root / "Flying_Mod_Idle.hkx",
                source,
                composite,
            )
            aerial_quarterstaff_actions[action_name] = composite
            print(
                f"aerialized Animated Armoury {action_name}: "
                f"replaced_tracks={len(replaced_tracks)} sha256={sha256(composite)}"
            )

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
                    # Magic idle/charge/concentration clips contain upper-body
                    # offsets and are not locomotion donors.  Keep their
                    # aerialized source instead of silently replacing it with
                    # the flight base.  Ordinary mag_* gait names still use
                    # the root-stable donor.
                    if name in aerial_magic_sources and is_action_or_offset_slot(name):
                        continue
                    mappings[name] = built_sources[family.base_source]

            action_names: set[str] = set()
            if family.common_vanilla_actions:
                for action_name in COMMON_VANILLA_ACTIONS:
                    mappings[action_name] = aerial_vanilla_actions[action_name]
                    action_names.add(action_name)
            if family.block_profile:
                for action_name in BLOCK_ACTION_PROFILES[family.block_profile]:
                    mappings[action_name] = aerial_vanilla_actions[action_name]
                    action_names.add(action_name)
            if family.quarterstaff_actions:
                for action_name, source in aerial_quarterstaff_actions.items():
                    mappings[action_name] = source
                    action_names.add(action_name)
            for target_name, source_name in family.action_aliases:
                mappings[target_name] = mappings.get(source_name, aerial_vanilla_actions[source_name])
                action_names.add(target_name)
            if family.staff_sources:
                for name, source in aerial_magic_sources.items():
                    if name.startswith("staff"):
                        mappings[name] = source
                for name in family.motion_names:
                    if name in aerial_magic_sources and is_action_or_offset_slot(name):
                        continue
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

            source_names = sorted(
                name
                for name in magic_sources
                if (family.magic_sources and not name.startswith("staff"))
                or (family.staff_sources and name.startswith("staff"))
            )
            # Ground locomotion names are deliberately remapped to the
            # root-stable flight donor after the credited magic/staff sources
            # are loaded. Keep provenance truthful: only names still mapped to
            # an aerialized source belong in aerializedSourceNames; the
            # intentional donor overrides are recorded separately.
            base_pose_overrides = sorted(
                name
                for name in source_names
                if name in family.motion_names
                and not (name in aerial_magic_sources and is_action_or_offset_slot(name))
            )
            aerialized_source_names = sorted(set(source_names) - set(base_pose_overrides))
            coverage_entry = {
                "priority": family.priority,
                "baseSource": family.base_source,
                "animationCountPerScope": len(mappings),
                "motionNames": sorted(family.motion_names),
                "attackNames": sorted(family.attack_names),
                "actionNames": sorted(action_names),
                "scopes": [scope.as_posix() or "." for scope in family.scopes],
                "aerializedMagicSources": family.magic_sources or family.staff_sources,
                "blockProfile": family.block_profile,
                "usesVanillaActionComposites": bool(action_names),
                "usesAnimatedArmouryQuarterstaffComposites": family.quarterstaff_actions,
                "actionSourceAliases": dict(sorted(family.action_aliases)),
            }
            if aerialized_source_names:
                coverage_entry["aerializedSourceNames"] = aerialized_source_names
            if base_pose_overrides:
                coverage_entry["basePoseOverrideNames"] = base_pose_overrides
            if family.magic_sources:
                coverage_entry["aerializedSourceAliases"] = dict(sorted(MAGIC_SOURCE_ALIASES.items()))
            coverage[family.directory] = coverage_entry
            print(f"{family.directory}: {len(mappings)} originals x {len(family.scopes)} scopes")

    existing_outputs = {path.resolve() for path in oar_root.rglob("*.hkx")}
    unexpected = sorted(existing_outputs - expected_outputs)
    missing = sorted(expected_outputs - existing_outputs)
    if unexpected or missing:
        raise RuntimeError(f"Generated flight stack mismatch: unexpected={len(unexpected)}, missing={len(missing)}")

    # Prepare metadata against the unpublished overlay before changing the
    # checked-in tree.  The directory and both metadata files are then moved as
    # one recoverable transaction; a late write failure cannot publish stale
    # coverage alongside a new OAR tree.
    coverage_path = data_root / "SKSE/Plugins/DragonAspectFlight-AnimationCoverage.json"
    hash_path = data_root / "SKSE/Plugins/DragonAspectFlight-AnimationHashes.txt"
    with tempfile.TemporaryDirectory(prefix=".daf-metadata-", dir=data_root) as metadata_directory:
        metadata_root = pathlib.Path(metadata_directory)
        staged_coverage = metadata_root / coverage_path.name
        staged_hashes = metadata_root / hash_path.name
        write_json(
            staged_coverage,
            {
                "version": RELEASE_VERSION,
                "scopes": sorted({scope.as_posix() or "." for family in FAMILIES for scope in family.scopes}),
                "families": coverage,
                "totalOarHkx": len(existing_outputs),
                "provenance": {
                    "tool": "BuildFlightAnimationStack.py",
                    "toolVersion": "1",
                    "releaseVersion": RELEASE_VERSION,
                    "codec": backend_provenance(anim_skyrim),
                    "hkxcmd": hkxcmd.name,
                    "sourcePolicy": "pinned third-party and exact-original inputs",
                },
            },
        )
        manifest_count = write_hash_manifest(
            data_root,
            output_path=staged_hashes,
            overlay_oar_root=oar_root,
        )
        _atomic_publish_candidate(
            oar_root,
            data_root / "meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight",
            [(staged_coverage, coverage_path), (staged_hashes, hash_path)],
            data_root,
        )
    _STAGED_OUTPUT_ROOT = None
    print(f"Built {len(existing_outputs)} DAF OAR aliases across {len(FAMILIES)} equipment families.")
    print(f"Wrote SHA-256 manifest for {manifest_count} bundled HKX/NIF assets.")
    return 0


def main() -> int:
    """Run the generator and remove an abandoned staging tree on failure."""

    global _STAGED_OUTPUT_ROOT
    try:
        return _main()
    finally:
        if _STAGED_OUTPUT_ROOT is not None and _STAGED_OUTPUT_ROOT.exists():
            shutil.rmtree(_STAGED_OUTPUT_ROOT, ignore_errors=True)
        _STAGED_OUTPUT_ROOT = None


if __name__ == "__main__":
    raise SystemExit(main())
