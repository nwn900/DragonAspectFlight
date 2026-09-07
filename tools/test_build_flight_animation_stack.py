"""Static routing regressions for the generated Dragon Aspect Flight stack."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
OAR_ROOT = (
    REPO_ROOT
    / "Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight"
)
COVERAGE_PATH = REPO_ROOT / "Data/SKSE/Plugins/DragonAspectFlight-AnimationCoverage.json"


def load_builder():
    path = pathlib.Path(__file__).with_name("BuildFlightAnimationStack.py")
    if str(path.parent) not in sys.path:
        sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location("daf_flight_stack_builder", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def walk_values(value: object, key: str):
    if isinstance(value, dict):
        if key in value:
            yield value[key]
        for child in value.values():
            yield from walk_values(child, key)
    elif isinstance(value, list):
        for child in value:
            yield from walk_values(child, key)


def walk_condition_dicts(value: object, condition: str):
    if isinstance(value, dict):
        if value.get("condition") == condition:
            yield value
        for child in value.values():
            yield from walk_condition_dicts(child, condition)
    elif isinstance(value, list):
        for child in value:
            yield from walk_condition_dicts(child, condition)


def read_config(directory: str) -> dict[str, object]:
    return json.loads((OAR_ROOT / directory / "config.json").read_text(encoding="utf-8"))


class FlightAnimationStackRoutingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.builder = load_builder()
        cls.coverage = json.loads(COVERAGE_PATH.read_text(encoding="utf-8"))

    def test_priorities_are_unique_and_ordered(self) -> None:
        configs = [
            read_config(path.name)
            for path in OAR_ROOT.iterdir()
            if path.is_dir() and (path / "config.json").is_file()
        ]
        priorities = [config["priority"] for config in configs]
        self.assertEqual(len(priorities), len(set(priorities)))
        self.assertGreater(
            read_config("Flight Base 40 - Greatsword")["priority"],
            read_config("Flight Base 50 - Axe and Warhammer")["priority"],
        )
        self.assertLess(
            read_config("Flight Base 40 - Greatsword")["priority"],
            read_config("Flight Base 55 - Quarterstaff")["priority"],
        )
        self.assertEqual(
            {
                path.name: read_config(path.name)["priority"]
                for path in OAR_ROOT.iterdir()
                if path.is_dir() and (path / "config.json").is_file()
            },
            {
                family.directory: family.priority
                for family in self.builder.FAMILIES
            },
        )

    def test_greatsword_routes_unknown_and_proven_polearms(self) -> None:
        config = read_config("Flight Base 40 - Greatsword")
        family = next(
            family
            for family in self.builder.FAMILIES
            if family.directory == "Flight Base 40 - Greatsword"
        )
        self.assertEqual(config["conditions"], family.conditions)
        types = {float(value) for value in walk_values(config["conditions"], "value")}
        editor_ids = set(walk_values(config["conditions"], "editorID"))
        plugin_names = set(walk_values(config["conditions"], "pluginName"))
        form_ids = set(walk_values(config["conditions"], "formID"))

        self.assertIn(5.0, types)
        self.assertIn(-1.0, types)
        self.assertTrue(
            {
                "WeapTypePike",
                "WeapTypeSpear",
                "WeapTypeHalberd",
                "OCF_WeapTypePike2H",
                "OCF_WeapTypeSpear2H",
                "OCF_WeapTypeHalberd2H",
                "OCF_WeapTypePole2H_Thrust",
                "OCF_WeapTypePole2H_Swing",
                "WeapTypeNodachi",
                "OCF_WeapTypeKatana2H",
            }.issubset(editor_ids)
        )
        self.assertTrue({"Spear of Skyrim.esp", "Spear of Omicron.esp"}.issubset(plugin_names))
        self.assertTrue({"80B", "815", "D62"}.issubset(form_ids))

    def test_quarterstaff_keywords_are_explicit_two_handed_only(self) -> None:
        config = read_config("Flight Base 55 - Quarterstaff")
        family = next(
            family
            for family in self.builder.FAMILIES
            if family.directory == "Flight Base 55 - Quarterstaff"
        )
        self.assertEqual(config["conditions"], family.conditions)
        keywords = {
            "WeapTypeQtrStaff",
            "WeapTypeQuarterstaff",
            "OCF_WeapTypeQuarterstaff2H",
        }
        keyword_conditions = list(
            walk_condition_dicts(config["conditions"], "IsEquippedHasKeyword")
        )
        self.assertEqual(len(keyword_conditions), len(keywords) * 2)
        keyword_hands = [
            (condition["Keyword"]["editorID"], condition["Left hand"])
            for condition in keyword_conditions
        ]
        self.assertEqual(len(keyword_hands), len(set(keyword_hands)))
        self.assertEqual(
            set(keyword_hands),
            {(keyword, hand) for keyword in keywords for hand in (False, True)},
        )
        self.assertTrue(all(isinstance(hand, bool) for _, hand in keyword_hands))
        self.assertNotIn(
            "OCF_WeapTypeQuarterstaff1H",
            set(walk_values(config["conditions"], "editorID")),
        )

    def test_one_handed_route_covers_shield_block_and_stance_aliases(self) -> None:
        generated = read_config("Flight Base 20 - One Handed")
        family = next(
            family
            for family in self.builder.FAMILIES
            if family.directory == "Flight Base 20 - One Handed"
        )
        types = {float(value) for value in walk_values(generated["conditions"], "value")}
        self.assertIn(11.0, types)
        self.assertEqual(generated["conditions"], family.conditions)
        files = {
            path.name for path in (OAR_ROOT / "Flight Base 20 - One Handed").rglob("*.hkx")
        }
        self.assertIn("1hm_sprintforwardsword.hkx", files)

    def test_unarmed_and_dual_routes_cover_stance_aliases(self) -> None:
        expected = {
            "Flight Base 10 - Unarmed": {
                "h2h_sprintforwardsword.hkx",
                "1hm_sprintforwardsword.hkx",
            },
            "Flight Base 30 - Dual Wield": "1hm_sprintforwardsword.hkx",
        }
        for directory, aliases in expected.items():
            files = {path.name for path in (OAR_ROOT / directory).rglob("*.hkx")}
            if isinstance(aliases, str):
                aliases = {aliases}
            self.assertTrue(set(aliases).issubset(files), directory)
        self.assertIn("h2h_sprintforwardsword.hkx", self.builder.UNARMED_MOTION)
        self.assertIn("1hm_sprintforwardsword.hkx", self.builder.UNARMED_MOTION)
        self.assertIn("1hm_sprintforwardsword.hkx", self.builder.DUAL_WIELD_MOTION)

    def test_coverage_counts_match_generated_hkx(self) -> None:
        total = 0
        magic_sources = {
            path.name.lower()
            for path in (REPO_ROOT / "third_party/xp32-magic").glob("*.hkx")
        }
        for directory, family in self.coverage["families"].items():
            count = len(list((OAR_ROOT / directory).rglob("*.hkx")))
            expected = family["animationCountPerScope"] * len(family["scopes"])
            self.assertEqual(count, expected, directory)
            aerialized_names = set(family.get("aerializedSourceNames", []))
            base_pose_override_names = set(family.get("basePoseOverrideNames", []))
            self.assertTrue(
                base_pose_override_names.issubset(set(family["motionNames"])),
                f"{directory}: base-pose overrides must be motion names",
            )
            self.assertTrue(
                aerialized_names.isdisjoint(base_pose_override_names),
                f"{directory}: base-pose overrides cannot remain aerialized",
            )
            covered_names = (
                set(family["motionNames"])
                | set(family["attackNames"])
                | set(family["actionNames"])
                | aerialized_names
                | base_pose_override_names
                | set(family.get("aerializedSourceAliases", {}))
            )
            for scope in family["scopes"]:
                scope_root = OAR_ROOT / directory / ("" if scope == "." else scope)
                scope_files = scope_root.rglob("*.hkx")
                if scope == ".":
                    scope_files = (
                        path
                        for path in scope_files
                        if path.relative_to(scope_root).parts[0] != "male"
                    )
                actual_names = {
                    path.relative_to(scope_root).as_posix().lower()
                    for path in scope_files
                }
                self.assertEqual(actual_names, covered_names, f"{directory}/{scope}")
            if directory == "Flight Base 80 - Magic":
                source_names = {
                    name for name in magic_sources if not name.startswith("staff")
                }
                self.assertEqual(
                    aerialized_names,
                    source_names - base_pose_override_names,
                )
                self.assertEqual(
                    base_pose_override_names,
                    source_names & set(family["motionNames"]),
                )
                self.assertEqual(
                    set(family["aerializedSourceAliases"]),
                    set(self.builder.MAGIC_SOURCE_ALIASES),
                )
            if directory == "Flight Base 90 - Staff":
                source_names = {name for name in magic_sources if name.startswith("staff")}
                self.assertEqual(
                    aerialized_names,
                    source_names - base_pose_override_names,
                )
                self.assertEqual(
                    base_pose_override_names,
                    source_names & set(family["motionNames"]),
                )
            total += count
        self.assertEqual(total, 1045)
        self.assertEqual(total, self.coverage["totalOarHkx"])

    def test_family_definitions_match_the_reviewed_output_tree(self) -> None:
        magic_sources = {
            path.name.lower()
            for path in (REPO_ROOT / "third_party/xp32-magic").glob("*.hkx")
        }
        for family in self.builder.FAMILIES:
            expected_names = self.builder.expected_family_animation_names(
                family,
                magic_source_names=magic_sources,
            )
            for scope in family.scopes:
                scope_root = OAR_ROOT / family.directory / scope
                actual_names = {
                    path.relative_to(scope_root).as_posix().lower()
                    for path in scope_root.rglob("*.hkx")
                }
                if not scope.parts:
                    actual_names = {
                        name for name in actual_names if not name.startswith("male/")
                    }
                self.assertEqual(actual_names, expected_names, family.directory)

    def test_reviewed_tree_validation_rejects_a_symmetric_deletion(self) -> None:
        family = self.builder.FAMILIES[0]
        with tempfile.TemporaryDirectory() as temporary:
            oar_root = pathlib.Path(temporary)
            expected_paths = self.builder.expected_family_output_paths(
                family,
                magic_source_names=set(),
            )
            for relative_path in expected_paths:
                path = oar_root / family.directory / relative_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"reviewed test clip")
            with mock.patch.object(self.builder, "FAMILIES", (family,)):
                self.builder.validate_reviewed_output_tree(oar_root, magic_source_names=set())
                (oar_root / family.directory / sorted(expected_paths)[0]).unlink()
                with self.assertRaisesRegex(RuntimeError, "missing=1"):
                    self.builder.validate_reviewed_output_tree(oar_root, magic_source_names=set())

    def test_magic_route_semantics_remain_unchanged(self) -> None:
        generated = read_config("Flight Base 80 - Magic")
        family = next(
            family
            for family in self.builder.FAMILIES
            if family.directory == "Flight Base 80 - Magic"
        )
        self.assertEqual(generated["priority"], 2147483608)
        self.assertEqual(generated["conditions"], family.conditions)
        self.assertEqual(self.coverage["families"]["Flight Base 80 - Magic"]["priority"], 2147483608)

    def test_flight_stack_leaves_combat_and_draw_commit_slots_to_their_owners(self) -> None:
        # MCO's normal-attack slots carry its attack-event contract.  DAF may
        # still own its power/bash presentation, but must not replace ordinary
        # mco_attackN slots with generic aerial clips.  Likewise, draw/sheathe
        # originals are state-commit paths and must remain with the behavior
        # stack that owns the active weapon graph.
        fallback_files = {path.name for path in (OAR_ROOT / "Flight Base 00 - Fallback").rglob("*.hkx")}
        self.assertTrue({"mt_shout_inhale.hkx", "mt_shout_exhale.hkx"}.issubset(fallback_files))

        generated_names = {path.name for path in OAR_ROOT.rglob("*.hkx")}
        protected_mco_names = {f"mco_attack{index}.hkx" for index in range(1, 11)}
        self.assertFalse(protected_mco_names & generated_names)
        self.assertFalse(
            any("equip" in name or "unequip" in name for name in generated_names),
            "DAF must not generate draw/equip/unequip commit-slot aliases",
        )
        self.assertFalse(
            protected_mco_names & set().union(*(family.attack_names for family in self.builder.FAMILIES))
        )
        self.assertFalse(
            any("equip" in name or "unequip" in name for name in self.builder.COMMON_VANILLA_ACTIONS)
        )
        self.assertFalse(
            any("equip" in name or "unequip" in name for name in self.builder.QUARTERSTAFF_AA_ACTIONS)
        )

        greatsword_files = {path.name for path in (OAR_ROOT / "Flight Base 40 - Greatsword").rglob("*.hkx")}
        self.assertTrue(
            {"2hm_attackright.hkx", "2hm_attackpower.hkx", "2hm_blockhit.hkx", "mco_powerattack1.hkx"}
            .issubset(greatsword_files)
        )


if __name__ == "__main__":
    unittest.main()
