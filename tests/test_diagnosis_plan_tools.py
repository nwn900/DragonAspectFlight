#!/usr/bin/env python3
"""Regression tests for the diagnosis-and-plan repair tools.

These tests use a tiny in-memory animation backend.  They deliberately do not
need PyNifly or hkxcmd, so the safety contracts can run in dependency-free CI.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest
from dataclasses import dataclass, field
from unittest import mock


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS_ROOT = REPO_ROOT / "tools"


def load_tool(name: str):
    path = TOOLS_ROOT / name
    module_name = f"daf_test_{path.stem}"
    if str(TOOLS_ROOT) not in sys.path:
        sys.path.insert(0, str(TOOLS_ROOT))
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


@dataclass
class FakeTrack:
    translations: list[list[float]]
    rotations: list[list[float]]
    scales: list[list[float]]


@dataclass
class FakeAnnotation:
    time: float
    text: str


@dataclass
class FakeAnimation:
    bone_names: list[str]
    tracks: list[FakeTrack]
    track_to_bone_indices: list[int]
    original_skeleton_name: str = "NPC"
    blend_hint: int = 0
    num_frames: int = 2
    frame_duration: float = 0.5
    duration: float = 1.0
    annotations: list[FakeAnnotation] = field(default_factory=list)

    @property
    def num_tracks(self) -> int:
        return len(self.tracks)


def track(value: float, frames: int = 2) -> FakeTrack:
    return FakeTrack(
        translations=[[value, 0.0, 0.0] for _ in range(frames)],
        rotations=[[0.0, 0.0, 0.0, 1.0] for _ in range(frames)],
        scales=[[1.0, 1.0, 1.0] for _ in range(frames)],
    )


class FakeBackend:
    def __init__(self, animations: dict[str, FakeAnimation]):
        self.animations = {str(pathlib.Path(key)): copy.deepcopy(value) for key, value in animations.items()}
        self.write_calls: list[pathlib.Path] = []
        self.corrupt_output = False

    def load_skyrim_animation(self, path: str) -> FakeAnimation:
        key = str(pathlib.Path(path))
        if key not in self.animations:
            raise KeyError(key)
        return copy.deepcopy(self.animations[key])

    def write_skyrim_animation(self, path: str, animation: FakeAnimation, ptr_size: int = 8) -> None:
        del ptr_size
        target = pathlib.Path(path)
        self.write_calls.append(target)
        stored = copy.deepcopy(animation)
        if self.corrupt_output:
            stored.bone_names = list(stored.bone_names) + ["corrupt"]
        self.animations[str(target)] = stored
        target.write_bytes(b"fake-hkx-output")


def make_base_and_action(*, action_names: list[str] | None = None, bindings: list[int] | None = None) -> tuple[FakeAnimation, FakeAnimation]:
    names = action_names or ["NPC Root [Root]", "NPC Pelvis", "NPC Spine"]
    indices = [0, 1, 2] if bindings is None else bindings
    base = FakeAnimation(
        bone_names=["NPC Root [Root]", "NPC Pelvis", "NPC Spine"],
        tracks=[track(10.0), track(20.0), track(30.0)],
        track_to_bone_indices=[0, 1, 2],
    )
    action = FakeAnimation(
        bone_names=names,
        tracks=[track(1.0), track(2.0), track(3.0)],
        track_to_bone_indices=indices,
        annotations=[FakeAnnotation(0.25, "attack")],
    )
    return base, action


class AerializeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.module = load_tool("AerializeHkx.py")

    def test_missing_binding_rejects_positional_guess(self) -> None:
        base, action = make_base_and_action(action_names=["", "", ""], bindings=[])
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_path, input_path, output_path = root / "base.hkx", root / "action.hkx", root / "out.hkx"
            base_path.write_bytes(b"base")
            input_path.write_bytes(b"input")
            backend = FakeBackend({str(base_path): base, str(input_path): action})
            with self.assertRaisesRegex(ValueError, "binding"):
                self.module.aerialize(backend, base_path, input_path, output_path)
            self.assertFalse(output_path.exists())
            self.assertEqual(backend.write_calls, [])

    def test_reordered_tracks_follow_binding_indices(self) -> None:
        base, action = make_base_and_action(
            action_names=["NPC Spine", "NPC Root [Root]", "NPC Pelvis"],
            bindings=[2, 0, 1],
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_path, input_path, output_path = root / "base.hkx", root / "action.hkx", root / "out.hkx"
            base_path.write_bytes(b"base")
            input_path.write_bytes(b"input")
            backend = FakeBackend({str(base_path): base, str(input_path): action})
            replaced = self.module.aerialize(backend, base_path, input_path, output_path)
            self.assertEqual(set(replaced), {"NPC Root [Root]", "NPC Pelvis"})
            result = backend.animations[str(backend.write_calls[-1])]
            # Track 1 is root (binding 0), track 2 pelvis (binding 1); the
            # spine/upper-body track remains action-owned.
            self.assertEqual(result.tracks[1].translations[0][0], 10.0)
            self.assertEqual(result.tracks[2].translations[0][0], 20.0)
            self.assertEqual(result.tracks[0].translations[0][0], 1.0)

    def test_additive_clip_is_preserved_byte_for_byte(self) -> None:
        base, action = make_base_and_action()
        action.blend_hint = 1
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_path, input_path, output_path = root / "base.hkx", root / "action.hkx", root / "out.hkx"
            base_path.write_bytes(b"base")
            input_bytes = b"exact-additive-source"
            input_path.write_bytes(input_bytes)
            backend = FakeBackend({str(base_path): base, str(input_path): action})
            self.assertEqual(self.module.aerialize(backend, base_path, input_path, output_path), [])
            self.assertEqual(output_path.read_bytes(), input_bytes)
            self.assertEqual(backend.write_calls, [])

    def test_upper_body_only_clip_is_preserved_byte_for_byte(self) -> None:
        base, action = make_base_and_action(
            action_names=["NPC Spine", "NPC Neck", "NPC Head"],
            bindings=[2, 3, 4],
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_path, input_path, output_path = root / "base.hkx", root / "action.hkx", root / "out.hkx"
            base_path.write_bytes(b"base")
            input_bytes = b"upper-body-source"
            input_path.write_bytes(input_bytes)
            backend = FakeBackend({str(base_path): base, str(input_path): action})
            self.assertEqual(self.module.aerialize(backend, base_path, input_path, output_path), [])
            self.assertEqual(output_path.read_bytes(), input_bytes)
            self.assertEqual(backend.write_calls, [])

    def test_preserved_clip_with_explicit_skeleton_mismatch_is_rejected(self) -> None:
        base, action = make_base_and_action(
            action_names=["NPC Spine", "NPC Neck", "NPC Head"],
            bindings=[2, 3, 4],
        )
        action.original_skeleton_name = "DifferentSkeleton"
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_path, input_path, output_path = root / "base.hkx", root / "action.hkx", root / "out.hkx"
            base_path.write_bytes(b"base")
            input_path.write_bytes(b"input")
            backend = FakeBackend({str(base_path): base, str(input_path): action})
            with self.assertRaisesRegex(ValueError, "skeleton"):
                self.module.aerialize(backend, base_path, input_path, output_path)
            self.assertFalse(output_path.exists())
            self.assertEqual(backend.write_calls, [])

    def test_skeleton_mismatch_fails_before_write(self) -> None:
        base, action = make_base_and_action()
        action.original_skeleton_name = "DifferentSkeleton"
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_path, input_path, output_path = root / "base.hkx", root / "action.hkx", root / "out.hkx"
            base_path.write_bytes(b"base")
            input_path.write_bytes(b"input")
            backend = FakeBackend({str(base_path): base, str(input_path): action})
            with self.assertRaisesRegex(ValueError, "skeleton"):
                self.module.aerialize(backend, base_path, input_path, output_path)
            self.assertEqual(backend.write_calls, [])

    def test_existing_output_survives_round_trip_failure(self) -> None:
        base, action = make_base_and_action()
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_path, input_path, output_path = root / "base.hkx", root / "action.hkx", root / "out.hkx"
            base_path.write_bytes(b"base")
            input_path.write_bytes(b"input")
            output_path.write_bytes(b"previous-good-output")
            backend = FakeBackend({str(base_path): base, str(input_path): action})
            backend.corrupt_output = True
            with self.assertRaisesRegex(RuntimeError, "round-trip"):
                self.module.aerialize(backend, base_path, input_path, output_path)
            self.assertEqual(output_path.read_bytes(), b"previous-good-output")
            self.assertEqual(list(root.glob(".*.tmp")), [])

    def test_loop_seam_failure_is_rejected(self) -> None:
        base, action = make_base_and_action()
        base.tracks[0].translations[-1][0] = 100.0
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            base_path, input_path, output_path = root / "base.hkx", root / "action.hkx", root / "out.hkx"
            base_path.write_bytes(b"base")
            input_path.write_bytes(b"input")
            backend = FakeBackend({str(base_path): base, str(input_path): action})
            with self.assertRaisesRegex(ValueError, "seam"):
                self.module.aerialize(backend, base_path, input_path, output_path)

    def test_magic_only_hkx_is_rejected_by_structural_inspector(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "truncated.hkx"
            path.write_bytes(b"\x57\xe0\xe0\x57" + b"\0" * 8)
            with self.assertRaisesRegex(ValueError, "truncated"):
                self.module.inspect_hkx_packfile(path)


class StackPolicyContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.builder = load_tool("BuildFlightAnimationStack.py")

    def test_intro_and_recovery_never_fall_back_to_flight_idle(self) -> None:
        family = next(family for family in self.builder.FAMILIES if family.attack_prefix == "2hm")
        with self.assertRaisesRegex(ValueError, "exact original"):
            self.builder.attack_source_name(family, "2hm_attackrightintro.hkx")

    def test_action_offset_slots_are_explicitly_classified(self) -> None:
        self.assertTrue(self.builder.is_action_or_offset_slot("dualmagic_idle.hkx"))
        self.assertTrue(self.builder.is_action_or_offset_slot("dmagpreaimconcharge.hkx"))
        self.assertTrue(self.builder.is_action_or_offset_slot("staffmagiccast_turnleft60.hkx"))
        self.assertFalse(self.builder.is_action_or_offset_slot("mag_runforward.hkx"))

    def test_missing_decoded_xml_is_a_validation_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = root / "source.hkx"
            source.write_bytes(bytes((0x57, 0xE0, 0xE0, 0x57)) + b"\0" * 32)
            with mock.patch.object(
                self.builder.subprocess,
                "run",
                return_value=mock.Mock(returncode=0, stdout="", stderr=""),
            ):
                with self.assertRaisesRegex(RuntimeError, "decoded XML"):
                    self.builder.validate_hkx(root / "hkxcmd.exe", source, root / "decoded.xml")

    def test_oar_directory_commit_replaces_only_completed_stage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            target = root / "oar"
            staged = root / "staged"
            target.mkdir()
            staged.mkdir()
            (target / "old.hkx").write_bytes(b"old")
            (staged / "new.hkx").write_bytes(b"new")
            self.builder._atomic_replace_directory(staged, target)
            self.assertEqual((target / "new.hkx").read_bytes(), b"new")
            self.assertFalse((target / "old.hkx").exists())
            self.assertFalse(staged.exists())


class DeliveryToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.rebuild = load_tool("rebuild_flight_actions.py")
        cls.delivery = load_tool("apply_daf_source_patch.py")

    def test_exact_original_rebuild_is_hash_pinned_and_atomic(self) -> None:
        base, action = make_base_and_action()
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source_root = root / "vanilla"
            source_root.mkdir()
            base_path = root / "base.hkx"
            source = source_root / "2hm_attackrightintro.hkx"
            base_path.write_bytes(b"base")
            source_bytes = b"original-action"
            source.write_bytes(source_bytes)
            backend = FakeBackend({str(base_path): base, str(source): action})
            manifest = {
                "version": 1,
                "clips": [
                    {
                        "target": "2hm_attackrightintro.hkx",
                        "source": "2hm_attackrightintro.hkx",
                        "source_sha256": self.rebuild.sha256(source),
                        "family": "greatsword",
                    }
                ],
            }
            output_root = root / "staged-actions"
            report = self.rebuild.rebuild_actions(backend, base_path, source_root, output_root, manifest)
            self.assertFalse(report["compiled"])
            self.assertFalse(report["rebuiltDataStack"])
            self.assertTrue((output_root / "exact-original-action-manifest.json").is_file())
            self.assertTrue((output_root / "2hm_attackrightintro.hkx").is_file())
            self.assertEqual(report["clips"][0]["sourceSha256"], self.rebuild.sha256(source))

    def test_source_delivery_copies_baseline_and_applies_only_pinned_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source_root, baseline_root = root / "source", root / "baseline"
            source_root.mkdir()
            baseline_root.mkdir()
            source_file = source_root / "src" / "Flight.cpp"
            baseline_file = baseline_root / "src" / "Flight.cpp"
            source_file.parent.mkdir()
            baseline_file.parent.mkdir()
            baseline_file.write_bytes(b"old\r\nline\r\n")
            source_file.write_bytes(b"new\r\nline\r\n")
            manifest = {
                "version": 1,
                "files": [
                    {
                        "path": "src/Flight.cpp",
                        "original_sha256": self.delivery.sha256(baseline_file),
                        "replacement_sha256": self.delivery.sha256(source_file),
                    }
                ],
            }
            target_root = root / "target-copy"
            diff_path = root / "review.diff"
            report = self.delivery.apply_source_patch(
                source_root, baseline_root, target_root, manifest, diff_path
            )
            self.assertEqual(baseline_file.read_bytes(), b"old\r\nline\r\n")
            self.assertEqual((target_root / "src/Flight.cpp").read_bytes(), b"new\r\nline\r\n")
            self.assertTrue(report["files"][0]["crlfPreserved"])
            self.assertIn("Flight.cpp", diff_path.read_text(encoding="utf-8"))
            delivery_report = json.loads((target_root / "source-delivery-report.json").read_text(encoding="utf-8"))
            self.assertFalse(delivery_report["compiled"])
            self.assertFalse(delivery_report["deployed"])

    def test_delivery_manifests_reject_windows_escape_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "manifest.json"
            path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "files": [
                            {
                                "path": "..\\outside.cpp",
                                "original_sha256": "0" * 64,
                                "replacement_sha256": "1" * 64,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "safe relative POSIX"):
                self.delivery.load_manifest(path)

    def test_action_manifests_reject_windows_escape_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "manifest.json"
            path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "clips": [
                            {
                                "target": "C:/outside.hkx",
                                "source": "action.hkx",
                                "source_sha256": "0" * 64,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "safe relative POSIX"):
                self.rebuild.load_manifest(path)


class LegacyBuilderContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.builder = load_tool("BuildFlightBaseAnimations.py")

    def test_legacy_hkxc_success_without_output_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            with mock.patch.object(
                self.builder.subprocess,
                "run",
                return_value=mock.Mock(returncode=0, stdout="", stderr=""),
            ):
                with self.assertRaisesRegex(RuntimeError, "declared output"):
                    self.builder.run_hkxc(root / "hkxc.exe", "convert", "-o", str(root / "missing.xml"))


if __name__ == "__main__":
    unittest.main()
