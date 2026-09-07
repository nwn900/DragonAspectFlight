#!/usr/bin/env python3
"""Safely composite a Skyrim action HKX with DAF's flight lower body.

The old implementation guessed a binding from track position when names were
missing and wrote the destination before it had verified the round trip.  That
is unsafe for MCO, magic, additive, and upper-body-only clips: a valid clip can
have a different track order or no lower-body data at all.  This module now
requires the explicit transform-track-to-bone binding for every composite,
keeps additive/upper-body clips byte-for-byte, and replaces outputs atomically
only after structural and ownership checks pass.

PyNifly 25.12+ supplies the direct Skyrim HKX reader/writer used by the CLI.
The small public helpers are dependency-free so the contract can be tested in
CI with a fake backend.
"""

from __future__ import annotations

import argparse
import copy
import math
import os
import pathlib
import shutil
import sys
import struct
import tempfile
from typing import Any, Iterable


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

ROUND_TRIP_TOLERANCE = 1.0e-3
LOOP_SEAM_TOLERANCE = 0.5


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
    if str(resolved) not in sys.path:
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


def inspect_hkx_packfile(path: pathlib.Path) -> dict[str, Any]:
    """Inspect the hk_2010 header/sections before a codec is allowed to read it.

    XML sources and dependency-free test doubles are reported without binary
    decoding.  A file that advertises the Skyrim packfile magic must, however,
    have a complete header and three in-bounds sections; accepting a magic-only
    stub would make a failed decode look like a valid animation.
    """

    raw = path.read_bytes()
    if raw.lstrip().startswith(b"<?xml") or b"<hkpackfile" in raw[:512]:
        return {"kind": "xml", "size": len(raw)}
    if raw[:4] != b"\x57\xe0\xe0\x57":
        return {"kind": "opaque", "size": len(raw)}
    if len(raw) < 0xD0:
        raise ValueError(f"truncated hk_2010 packfile header: {path}")
    version = struct.unpack_from("<I", raw, 0x0C)[0]
    ptr_size = raw[0x10]
    if version != 8 or ptr_size not in (4, 8):
        raise ValueError(
            f"unsupported hk_2010 packfile header in {path}: "
            f"version={version} ptr_size={ptr_size}"
        )
    sections: list[dict[str, Any]] = []
    for index in range(3):
        offset = 0x40 + index * 0x30
        name = raw[offset : offset + 16].split(b"\0", 1)[0].decode("ascii", errors="replace")
        data_start = struct.unpack_from("<I", raw, offset + 0x14)[0]
        end_delta = struct.unpack_from("<I", raw, offset + 0x2C)[0]
        end = data_start + end_delta
        if data_start < 0xD0 or end < data_start or end > len(raw):
            raise ValueError(
                f"out-of-bounds hk_2010 section in {path}: {name!r} "
                f"start=0x{data_start:X} end=0x{end:X} size=0x{len(raw):X}"
            )
        sections.append({"name": name, "start": data_start, "end": end})
    names = {section["name"] for section in sections}
    if not {"__classnames__", "__types__", "__data__"}.issubset(names):
        raise ValueError(f"hk_2010 packfile is missing required sections: {sorted(names)}")
    return {"kind": "hk_2010", "size": len(raw), "version": version, "ptrSize": ptr_size, "sections": sections}


def inspect_hkx_animation_channels(path: pathlib.Path, structure: dict[str, Any]) -> dict[str, Any]:
    """Inspect float-track/root-motion fields without trusting the codec model.

    Skyrim ships both spline-compressed and interleaved-uncompressed
    ``hkaAnimation`` payloads.  The latter has no float-track stream; it is
    still a valid transform animation and must not be mistaken for a corrupt
    packfile simply because it lacks the compressed-animation class.
    """

    if structure.get("kind") != "hk_2010":
        return {"floatTracks": 0, "hasExtractedMotion": False}
    raw = path.read_bytes()
    ptr_size = int(structure["ptrSize"])
    data_section = next(section for section in structure["sections"] if section["name"] == "__data__")
    class_section = next(section for section in structure["sections"] if section["name"] == "__classnames__")
    data_abs = int(data_section["start"])
    class_abs = int(class_section["start"])
    class_end = int(class_section["end"])
    class_blob = raw[class_abs:class_end]
    if b"hkaSplineCompressedAnimation" not in class_blob:
        if b"hkaInterleavedUncompressedAnimation" in class_blob:
            # Interleaved animations contain only transform tracks.  Any
            # backend-exposed extracted-motion field is still checked by
            # _validate_animation_structure after decode; there is no
            # compressed float-track stream to inspect in this representation.
            return {
                "floatTracks": 0,
                "hasExtractedMotion": False,
                "animationType": "hkaInterleavedUncompressedAnimation",
            }
        raise ValueError(f"hk_2010 packfile has no supported animation object: {path}")
    header_offset = 0x40 + 2 * 0x30
    local_offset = data_abs + struct.unpack_from("<I", raw, header_offset + 0x18)[0]
    virtual_offset = data_abs + struct.unpack_from("<I", raw, header_offset + 0x20)[0]
    export_offset = data_abs + struct.unpack_from("<I", raw, header_offset + 0x24)[0]
    local_fixups: dict[int, int] = {}
    cursor = local_offset
    while cursor + 8 <= virtual_offset:
        source = struct.unpack_from("<I", raw, cursor)[0]
        destination = struct.unpack_from("<I", raw, cursor + 4)[0]
        if source == 0xFFFFFFFF:
            break
        local_fixups[source] = destination
        cursor += 8
    animation_rel: int | None = None
    cursor = virtual_offset
    while cursor + 12 <= export_offset:
        source = struct.unpack_from("<I", raw, cursor)[0]
        name_offset = struct.unpack_from("<I", raw, cursor + 8)[0]
        if source == 0xFFFFFFFF:
            break
        absolute_name = class_abs + name_offset
        end = raw.find(b"\0", absolute_name, min(len(raw), absolute_name + 256))
        if end != -1 and raw[absolute_name:end].decode("ascii", errors="replace") == "hkaSplineCompressedAnimation":
            animation_rel = source
            break
        cursor += 12
    if animation_rel is None:
        raise ValueError(f"hk_2010 packfile has no hkaSplineCompressedAnimation object: {path}")
    base = 2 * ptr_size
    arr_size = ptr_size + 8
    ann_offset = base + 16 + ptr_size
    spline_base = ann_offset + arr_size
    num_float_offset = base + 12
    extracted_motion_offset = base + 16
    animation_abs = data_abs + animation_rel
    if animation_abs + spline_base + 32 > len(raw):
        raise ValueError(f"truncated hkaSplineCompressedAnimation object: {path}")
    float_tracks = struct.unpack_from("<I", raw, animation_abs + num_float_offset)[0]
    extracted_field_rel = animation_rel + extracted_motion_offset
    has_extracted_motion = extracted_field_rel in local_fixups or any(
        raw[animation_abs + extracted_motion_offset : animation_abs + extracted_motion_offset + ptr_size]
    )
    if float_tracks:
        raise ValueError(f"unsupported float tracks in {path}: count={float_tracks}")
    if has_extracted_motion:
        raise ValueError(f"unsupported extracted root motion in {path}")
    return {
        "floatTracks": float_tracks,
        "hasExtractedMotion": has_extracted_motion,
        "animationType": "hkaSplineCompressedAnimation",
    }


def _tracks(animation: Any) -> list[Any]:
    return list(getattr(animation, "tracks", ()) or ())


def _names(animation: Any) -> list[str]:
    return [str(name or "") for name in (getattr(animation, "bone_names", ()) or ())]


def _num_tracks(animation: Any) -> int:
    return int(getattr(animation, "num_tracks", len(_tracks(animation))))


def _num_frames(animation: Any) -> int:
    return int(getattr(animation, "num_frames", 0))


def _binding_indices(animation: Any, label: str, *, required: bool) -> list[int] | None:
    """Return the explicit transform-track-to-bone binding.

    PyNifly has used both snake-case and camel-case spellings over time.  An
    empty/missing binding is never treated as skeleton order: that is the exact
    positional guess which caused the original animation corruption.
    """

    candidates = (
        "track_to_bone_indices",
        "transform_track_to_bone_indices",
        "transformTrackToBoneIndices",
    )
    value: Iterable[Any] | None = None
    for attribute in candidates:
        candidate = getattr(animation, attribute, None)
        if candidate is not None:
            value = candidate
            break
    if value is None:
        if required:
            raise ValueError(f"{label} is missing an explicit transform-track binding")
        return None
    try:
        result = [int(index) for index in value]
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{label} has an invalid transform-track binding") from exc
    if len(result) != _num_tracks(animation) or any(index < 0 for index in result):
        raise ValueError(
            f"{label} has an incomplete transform-track binding: "
            f"{len(result)} entries for {_num_tracks(animation)} tracks"
        )
    return result


def _skeleton_name(animation: Any, label: str) -> str:
    for attribute in ("original_skeleton_name", "skeleton_name", "skeletonName"):
        value = getattr(animation, attribute, None)
        if value:
            return str(value).strip()
    raise ValueError(f"{label} has no skeleton identity")


def _normalised_hint(animation: Any) -> object:
    value = getattr(animation, "blend_hint", 0)
    return getattr(value, "value", value)


def _is_additive(animation: Any) -> bool:
    value = _normalised_hint(animation)
    if value is None:
        return False
    if isinstance(value, str):
        lowered = value.strip().lower()
        return lowered not in {"", "0", "normal", "override", "absolute"}
    try:
        return int(value) != 0
    except (TypeError, ValueError):
        return bool(value)


def _annotation_values(animation: Any) -> list[tuple[float, str]]:
    result: list[tuple[float, str]] = []
    for annotation in getattr(animation, "annotations", ()) or ():
        result.append((float(getattr(annotation, "time")), str(getattr(annotation, "text"))))
    return result


def _extract_motion(animation: Any) -> object | None:
    for attribute in ("extracted_motion", "extractedMotion"):
        if hasattr(animation, attribute):
            return getattr(animation, attribute)
    return None


def _check_optional_skeleton_compatibility(base: Any, action: Any) -> None:
    """Reject an explicit skeleton mismatch even for preserved clip classes."""

    base_name = next(
        (getattr(base, attribute, None) for attribute in ("original_skeleton_name", "skeleton_name", "skeletonName")
         if getattr(base, attribute, None)),
        None,
    )
    action_name = next(
        (getattr(action, attribute, None) for attribute in ("original_skeleton_name", "skeleton_name", "skeletonName")
         if getattr(action, attribute, None)),
        None,
    )
    if base_name is not None and action_name is not None:
        if str(base_name).strip().casefold() != str(action_name).strip().casefold():
            raise ValueError(
                "skeleton identity mismatch: "
                f"base={str(base_name).strip()!r}, action={str(action_name).strip()!r}"
            )


def _validate_animation_structure(animation: Any, label: str) -> None:
    tracks = _tracks(animation)
    names = _names(animation)
    num_tracks = _num_tracks(animation)
    num_frames = _num_frames(animation)
    if num_tracks <= 0 or len(tracks) != num_tracks or len(names) != num_tracks:
        raise ValueError(
            f"{label} has inconsistent track metadata: names={len(names)} "
            f"tracks={len(tracks)} declared={num_tracks}"
        )
    if num_frames <= 0:
        raise ValueError(f"{label} has no transform frames")
    frame_duration = float(getattr(animation, "frame_duration", 0.0))
    if num_frames > 1 and (not math.isfinite(frame_duration) or frame_duration <= 0.0):
        raise ValueError(f"{label} has an invalid frame duration")
    float_tracks = getattr(animation, "float_tracks", None)
    if float_tracks:
        raise ValueError(f"{label} contains unsupported float tracks")
    extracted_motion = _extract_motion(animation)
    if extracted_motion not in (None, "", "null", "None", False):
        raise ValueError(f"{label} contains extracted root motion: {extracted_motion}")
    for index, track in enumerate(tracks):
        for attribute in ("translations", "rotations", "scales"):
            samples = getattr(track, attribute, None)
            if samples is None or len(samples) < num_frames:
                raise ValueError(f"{label} track {index} has incomplete {attribute} samples")


def _resolve_track_mapping(base: Any, action: Any) -> list[tuple[int, int, str]]:
    """Resolve action tracks to base tracks by explicit bone binding."""

    base_skeleton = _skeleton_name(base, "base animation")
    action_skeleton = _skeleton_name(action, "action animation")
    if base_skeleton.casefold() != action_skeleton.casefold():
        raise ValueError(
            "skeleton identity mismatch: "
            f"base={base_skeleton!r}, action={action_skeleton!r}"
        )

    base_binding = _binding_indices(base, "base animation", required=True)
    action_binding = _binding_indices(action, "action animation", required=True)
    assert base_binding is not None and action_binding is not None
    base_names = _names(base)
    action_names = _names(action)

    base_by_bone: dict[int, int] = {}
    for track_index, bone_index in enumerate(base_binding):
        if bone_index in base_by_bone:
            raise ValueError(f"base animation has duplicate binding for bone {bone_index}")
        base_by_bone[bone_index] = track_index

    # Names are an integrity check, not a fallback.  If both clips identify a
    # bound bone, disagreeing names indicate the wrong skeleton/clip pair.
    for action_index, bone_index in enumerate(action_binding):
        base_index = base_by_bone.get(bone_index)
        if base_index is None:
            raise ValueError(f"action animation binding {bone_index} is absent from base animation")
        action_name = action_names[action_index].strip()
        base_name = base_names[base_index].strip()
        if action_name and base_name and action_name.casefold() != base_name.casefold():
            raise ValueError(
                "contradictory bound bone names: "
                f"action={action_name!r}, base={base_name!r}, bone={bone_index}"
            )

    return [
        (
            action_index,
            base_by_bone[bone_index],
            action_names[action_index].strip() or base_names[base_by_bone[bone_index]].strip(),
        )
        for action_index, bone_index in enumerate(action_binding)
    ]


def _max_sample_delta(left: list[float], right: list[float]) -> float:
    if len(left) != len(right):
        return float("inf")
    return max((abs(float(a) - float(b)) for a, b in zip(left, right)), default=0.0)


def loop_seam_error(animation: Any, names: Iterable[str] | None = None) -> float:
    """Return the largest first/last sample delta for selected tracks."""

    selected = {name.casefold() for name in names} if names is not None else None
    maximum = 0.0
    for name, track in zip(_names(animation), _tracks(animation)):
        if selected is not None and name.casefold() not in selected:
            continue
        for attribute in ("translations", "rotations", "scales"):
            samples = getattr(track, attribute)
            if len(samples) > 1:
                maximum = max(maximum, _max_sample_delta(list(samples[0]), list(samples[-1])))
    return maximum


def _atomic_copy(input_path: pathlib.Path, output_path: pathlib.Path) -> None:
    if input_path.resolve() == output_path.resolve():
        raise ValueError("input and output HKX paths must be different")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{output_path.name}.",
            suffix=".tmp",
            dir=output_path.parent,
            delete=False,
        ) as stream:
            temporary = pathlib.Path(stream.name)
            with input_path.open("rb") as source:
                shutil.copyfileobj(source, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output_path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _verify_round_trip(action: Any, verified: Any, replaced_indices: set[int]) -> None:
    fields = (
        ("num_frames", _num_frames(action), _num_frames(verified)),
        ("num_tracks", _num_tracks(action), _num_tracks(verified)),
        ("bone_names", _names(action), _names(verified)),
        ("duration", float(getattr(action, "duration", 0.0)), float(getattr(verified, "duration", 0.0))),
        ("annotations", _annotation_values(action), _annotation_values(verified)),
    )
    for field, expected, actual in fields:
        if field == "duration":
            if abs(expected - actual) > 1.0e-5:
                raise RuntimeError(f"round-trip metadata mismatch ({field})")
        elif expected != actual:
            raise RuntimeError(f"round-trip metadata mismatch ({field})")

    expected_binding = _binding_indices(action, "action animation", required=False)
    actual_binding = _binding_indices(verified, "written animation", required=False)
    if expected_binding != actual_binding:
        raise RuntimeError("round-trip metadata mismatch (transform-track binding)")
    expected_skeleton = getattr(action, "original_skeleton_name", None)
    actual_skeleton = getattr(verified, "original_skeleton_name", None)
    if expected_skeleton is not None and actual_skeleton is not None:
        if str(expected_skeleton).casefold() != str(actual_skeleton).casefold():
            raise RuntimeError("round-trip metadata mismatch (skeleton identity)")
    if _normalised_hint(action) != _normalised_hint(verified):
        raise RuntimeError("round-trip metadata mismatch (blend hint)")

    expected_tracks = _tracks(action)
    actual_tracks = _tracks(verified)
    if len(expected_tracks) != len(actual_tracks):
        raise RuntimeError("round-trip metadata mismatch (track count)")
    for index, (expected_track, actual_track) in enumerate(zip(expected_tracks, actual_tracks)):
        if index in replaced_indices:
            continue
        for attribute in ("translations", "rotations", "scales"):
            expected_samples = getattr(expected_track, attribute)
            actual_samples = getattr(actual_track, attribute)
            if len(expected_samples) != len(actual_samples):
                raise RuntimeError(f"round-trip upper-body track mismatch ({index}/{attribute})")
            for expected, actual in zip(expected_samples, actual_samples):
                if _max_sample_delta(list(expected), list(actual)) > ROUND_TRIP_TOLERANCE:
                    raise RuntimeError(f"round-trip upper-body track mismatch ({index}/{attribute})")


def aerialize(anim_skyrim: Any, base_path: pathlib.Path, input_path: pathlib.Path, output_path: pathlib.Path) -> list[str]:
    base_path = base_path.resolve()
    input_path = input_path.resolve()
    output_path = output_path.resolve()
    if input_path == output_path:
        raise ValueError("input and output HKX paths must be different")
    base_structure = inspect_hkx_packfile(base_path)
    input_structure = inspect_hkx_packfile(input_path)
    inspect_hkx_animation_channels(base_path, base_structure)
    inspect_hkx_animation_channels(input_path, input_structure)
    base = anim_skyrim.load_skyrim_animation(str(base_path))
    action = anim_skyrim.load_skyrim_animation(str(input_path))
    _validate_animation_structure(base, "base animation")
    _validate_animation_structure(action, "action animation")
    _check_optional_skeleton_compatibility(base, action)

    # Additive and verified upper-body-only sources are owned by their original
    # behavior stack.  Copying bytes preserves compression, annotations, and
    # blend semantics exactly; no writer is allowed to normalize them.
    if _is_additive(action):
        _atomic_copy(input_path, output_path)
        return []

    names = _names(action)
    named_lower_body = any(should_replace_track(name) for name in names if name)
    if not named_lower_body and all(name.strip() for name in names):
        _atomic_copy(input_path, output_path)
        return []

    mapping = _resolve_track_mapping(base, action)
    replacements = [entry for entry in mapping if should_replace_track(entry[2])]
    if not replacements:
        _atomic_copy(input_path, output_path)
        return []
    seam_error = loop_seam_error(base, (entry[2] for entry in replacements))
    if seam_error > LOOP_SEAM_TOLERANCE:
        raise ValueError(f"base animation loop seam exceeds tolerance: {seam_error:.6f}")

    composite = copy.deepcopy(action)
    composite_tracks = _tracks(composite)
    base_tracks = _tracks(base)
    for action_index, base_index, _ in replacements:
        track = composite_tracks[action_index]
        base_track = base_tracks[base_index]
        track.translations = []
        track.rotations = []
        track.scales = []
        for frame in range(_num_frames(composite)):
            translation, rotation, scale = sample_track(
                base_track,
                base,
                frame * float(getattr(composite, "frame_duration", 0.0)),
            )
            track.translations.append(translation)
            track.rotations.append(rotation)
            track.scales.append(scale)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{output_path.name}.",
            suffix=".tmp",
            dir=output_path.parent,
            delete=False,
        ) as stream:
            temporary = pathlib.Path(stream.name)
        # PyNifly owns the binary writer and opens the path itself.
        anim_skyrim.write_skyrim_animation(str(temporary), composite, ptr_size=8)
        if not temporary.is_file() or temporary.stat().st_size == 0:
            raise RuntimeError(f"HKX writer produced no output for {output_path}")
        written_structure = inspect_hkx_packfile(temporary)
        inspect_hkx_animation_channels(temporary, written_structure)
        verified = anim_skyrim.load_skyrim_animation(str(temporary))
        _verify_round_trip(action, verified, {entry[0] for entry in replacements})
        os.replace(temporary, output_path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return [entry[2] for entry in replacements]


if __name__ == "__main__":
    args = parse_args()
    backend = load_backend(args.pynifly_hkx_dir)
    replaced = aerialize(backend, args.base, args.input, args.output)
    print(f"aerialized={args.output.resolve()} replaced_tracks={len(replaced)}")
