#!/usr/bin/env python3
"""Composite a Skyrim SE action HKX with DAF's root-stable flying lower body.

The action's upper-body tracks, timing, binding, and annotations are preserved.
Root, controller, pelvis, skirt, and leg tracks are sampled from the flight base.
PyNifly 25.12+ supplies the direct Skyrim HKX reader/writer used here.
"""

from __future__ import annotations

import argparse
import copy
import math
import pathlib
import sys
from typing import Any


LOWER_BODY_TOKENS = (
    "root [root]",
    "looknode",
    "translate [pos",
    "rotate [rot",
    "com [com",
    "pelvis",
    "thigh",
    "calf",
    "foot",
    "toe",
    "skirt",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", type=pathlib.Path, required=True)
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--pynifly-hkx-dir", type=pathlib.Path, required=True)
    return parser.parse_args()


def load_backend(pynifly_hkx_dir: pathlib.Path) -> Any:
    resolved = pynifly_hkx_dir.resolve()
    if not (resolved / "anim_skyrim.py").is_file():
        raise FileNotFoundError(f"PyNifly anim_skyrim.py not found: {resolved}")
    sys.path.insert(0, str(resolved))
    import anim_skyrim  # type: ignore

    return anim_skyrim


def linear(a: list[float], b: list[float], amount: float) -> list[float]:
    return [left + ((right - left) * amount) for left, right in zip(a, b)]


def quaternion_nlerp(a: list[float], b: list[float], amount: float) -> list[float]:
    # Quaternions q and -q encode the same rotation. Follow the shortest arc.
    if sum(left * right for left, right in zip(a, b)) < 0.0:
        b = [-value for value in b]
    result = linear(a, b, amount)
    length = math.sqrt(sum(value * value for value in result))
    if length <= 1.0e-8:
        return list(a)
    return [value / length for value in result]


def sample_track(track: Any, animation: Any, time_seconds: float) -> tuple[list[float], list[float], list[float]]:
    if animation.num_frames <= 1 or animation.frame_duration <= 0.0:
        return (
            list(track.translations[0]),
            list(track.rotations[0]),
            list(track.scales[0]),
        )

    # Exclude the duplicate loop endpoint when wrapping a looping flight base.
    loop_frames = max(1, animation.num_frames - 1)
    frame_position = (time_seconds / animation.frame_duration) % loop_frames
    frame_a = int(math.floor(frame_position))
    frame_b = (frame_a + 1) % loop_frames
    amount = frame_position - frame_a
    return (
        linear(track.translations[frame_a], track.translations[frame_b], amount),
        quaternion_nlerp(track.rotations[frame_a], track.rotations[frame_b], amount),
        linear(track.scales[frame_a], track.scales[frame_b], amount),
    )


def should_replace_track(name: str) -> bool:
    lowered = name.lower()
    return any(token in lowered for token in LOWER_BODY_TOKENS)


def aerialize(anim_skyrim: Any, base_path: pathlib.Path, input_path: pathlib.Path, output_path: pathlib.Path) -> list[str]:
    base = anim_skyrim.load_skyrim_animation(str(base_path))
    action = anim_skyrim.load_skyrim_animation(str(input_path))
    composite = copy.deepcopy(action)
    base_by_name = {name.lower(): track for name, track in zip(base.bone_names, base.tracks)}

    replaced: list[str] = []
    for index, (name, track) in enumerate(zip(composite.bone_names, composite.tracks)):
        resolved_name = name
        base_track = base_by_name.get(name.lower()) if name else None
        # Some valid Skyrim clips omit annotation-track names. The 99-track
        # vanilla binding uses the same skeleton order as Flying_Mod_Idle.
        if base_track is None and not name and composite.num_tracks == base.num_tracks:
            resolved_name = base.bone_names[index]
            base_track = base.tracks[index]
        if base_track is None or not should_replace_track(resolved_name):
            continue

        track.translations = []
        track.rotations = []
        track.scales = []
        for frame in range(composite.num_frames):
            translation, rotation, scale = sample_track(
                base_track,
                base,
                frame * composite.frame_duration,
            )
            track.translations.append(translation)
            track.rotations.append(rotation)
            track.scales.append(scale)
        replaced.append(resolved_name)

    if not replaced:
        raise RuntimeError(f"No compatible lower-body tracks found in {input_path}")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    anim_skyrim.write_skyrim_animation(str(output_path), composite, ptr_size=8)
    verified = anim_skyrim.load_skyrim_animation(str(output_path))
    expected_annotations = [(annotation.time, annotation.text) for annotation in action.annotations]
    actual_annotations = [(annotation.time, annotation.text) for annotation in verified.annotations]
    if (
        verified.num_frames != action.num_frames
        or verified.num_tracks != action.num_tracks
        or verified.bone_names != action.bone_names
        or abs(verified.duration - action.duration) > 1.0e-5
        or actual_annotations != expected_annotations
    ):
        raise RuntimeError(f"Round-trip verification failed for {output_path}")
    return replaced


def main() -> int:
    args = parse_args()
    backend = load_backend(args.pynifly_hkx_dir)
    replaced = aerialize(
        backend,
        args.base.resolve(),
        args.input.resolve(),
        args.output.resolve(),
    )
    print(f"aerialized={args.output.resolve()} replaced_tracks={len(replaced)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
