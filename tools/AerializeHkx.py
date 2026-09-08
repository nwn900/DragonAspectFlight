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
import hashlib
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


def backend_provenance(anim_skyrim: Any) -> dict[str, object]:
    """Return stable identity for the codec used to read/write release HKX files."""

    module_path = pathlib.Path(str(getattr(anim_skyrim, "__file__", ""))) if getattr(anim_skyrim, "__file__", None) else None
    result: dict[str, object] = {
        "module": module_path.name if module_path else type(anim_skyrim).__name__,
        "version": str(getattr(anim_skyrim, "__version__", "unknown")),
    }
    if module_path is not None and module_path.is_file():
        digest = hashlib.sha256()
        with module_path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        result["moduleSha256"] = digest.hexdigest().upper()
    return result


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
        sections.append({
            "name": name,
            "start": data_start,
            "end": end,
            "header": offset,
            "local": struct.unpack_from("<I", raw, offset + 0x18)[0],
            "global": struct.unpack_from("<I", raw, offset + 0x1C)[0],
            "virtual": struct.unpack_from("<I", raw, offset + 0x20)[0],
            "exports": struct.unpack_from("<I", raw, offset + 0x24)[0],
        })
    ordered_sections = sorted(sections, key=lambda section: (section["start"], section["end"]))
    for previous, current in zip(ordered_sections, ordered_sections[1:]):
        if current["start"] < previous["end"]:
            raise ValueError(
                f"overlapping hk_2010 sections in {path}: "
                f"{previous['name']!r} and {current['name']!r}"
            )
    for section in sections:
        section_size = section["end"] - section["start"]
        if any(section[key] > section_size for key in ("local", "global", "virtual", "exports")):
            raise ValueError(f"out-of-bounds hk_2010 fixup table in {path}: {section['name']!r}")
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
    local_offset = data_abs + int(data_section["local"])
    global_offset = data_abs + int(data_section["global"])
    virtual_offset = data_abs + int(data_section["virtual"])
    export_offset = data_abs + int(data_section["exports"])
    local_fixups: dict[int, int] = {}
    cursor = local_offset
    while cursor + 8 <= global_offset:
        source = struct.unpack_from("<I", raw, cursor)[0]
        destination = struct.unpack_from("<I", raw, cursor + 4)[0]
        if source == 0xFFFFFFFF:
            break
        local_fixups[source] = destination
        cursor += 8
    global_fixup_sources: set[int] = set()
    cursor = global_offset
    while cursor + 8 <= virtual_offset:
        source = struct.unpack_from("<I", raw, cursor)[0]
        if source == 0xFFFFFFFF:
            break
        global_fixup_sources.add(source)
        cursor += 8
    animation_rel: int | None = None
    animation_type: str | None = None
    cursor = virtual_offset
    while cursor + 12 <= export_offset:
        source = struct.unpack_from("<I", raw, cursor)[0]
        name_offset = struct.unpack_from("<I", raw, cursor + 8)[0]
        if source == 0xFFFFFFFF:
            break
        absolute_name = class_abs + name_offset
        end = raw.find(b"\0", absolute_name, min(len(raw), absolute_name + 256))
        if end != -1:
            candidate = raw[absolute_name:end].decode("ascii", errors="replace")
            if candidate in {"hkaSplineCompressedAnimation", "hkaInterleavedUncompressedAnimation"}:
                animation_rel = source
                animation_type = candidate
                break
        cursor += 12
    if animation_rel is None or animation_type is None:
        raise ValueError(f"hk_2010 packfile has no supported animation object: {path}")
    # Keep the representation on the structural record as well as in the
    # return value.  Callers use the record when reporting a later codec
    # failure; dropping this field made valid interleaved inputs appear as
    # ``unknown`` in those diagnostics.
    structure["animationType"] = animation_type
    base = 2 * ptr_size
    arr_size = ptr_size + 8
    ann_offset = base + 16 + ptr_size
    spline_base = ann_offset + arr_size
    num_float_offset = base + 12
    extracted_motion_offset = base + 16
    animation_abs = data_abs + animation_rel
    data_end = int(data_section["end"])
    if animation_rel < 0 or animation_abs + extracted_motion_offset + ptr_size > data_end:
        raise ValueError(f"truncated hkaAnimation object: {path}")
    float_tracks = struct.unpack_from("<I", raw, animation_abs + num_float_offset)[0]
    extracted_field_rel = animation_rel + extracted_motion_offset
    has_extracted_motion = (
        extracted_field_rel in local_fixups or
        extracted_field_rel in global_fixup_sources or any(
        raw[animation_abs + extracted_motion_offset : animation_abs + extracted_motion_offset + ptr_size]
        )
    )
    if float_tracks:
        raise ValueError(f"unsupported float tracks in {path}: count={float_tracks}")
    if has_extracted_motion:
        raise ValueError(f"unsupported extracted root motion in {path}")
    return {
        "floatTracks": float_tracks,
        "hasExtractedMotion": has_extracted_motion,
        "animationType": animation_type,
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
    value = _optional_skeleton_name(animation)
    if value is not None:
        return value
    raise ValueError(f"{label} has no skeleton identity")


def _optional_skeleton_name(animation: Any) -> str | None:
    """Read the skeleton identity across PyNifly naming variants."""

    for attribute in ("original_skeleton_name", "skeleton_name", "skeletonName"):
        value = getattr(animation, attribute, None)
        if value:
            return str(value).strip()
    return None


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


def _validate_annotations(animation: Any, label: str, duration: float) -> None:
    for index, annotation in enumerate(getattr(animation, "annotations", ()) or ()):
        try:
            time_value = float(getattr(annotation, "time"))
        except (AttributeError, TypeError, ValueError, OverflowError) as exc:
            raise ValueError(f"{label} annotation {index} has a non-numeric time") from exc
        if not math.isfinite(time_value) or time_value < 0.0:
            raise ValueError(f"{label} annotation {index} must contain a finite non-negative time")
        if time_value > duration + ROUND_TRIP_TOLERANCE:
            raise ValueError(f"{label} annotation {index} lies outside the animation duration")
        if not hasattr(annotation, "text"):
            raise ValueError(f"{label} annotation {index} has no text")


def _extract_motion(animation: Any) -> object | None:
    for attribute in ("extracted_motion", "extractedMotion"):
        if hasattr(animation, attribute):
            return getattr(animation, attribute)
    return None


def _check_optional_skeleton_compatibility(base: Any, action: Any) -> None:
    """Reject an explicit skeleton mismatch even for preserved clip classes."""

    base_name = _optional_skeleton_name(base)
    action_name = _optional_skeleton_name(action)
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
    try:
        frame_duration = float(getattr(animation, "frame_duration", 0.0))
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError(f"{label} has an invalid frame duration") from exc
    if not math.isfinite(frame_duration) or (num_frames > 1 and frame_duration <= 0.0) or frame_duration < 0.0:
        raise ValueError(f"{label} has an invalid frame duration")
    try:
        duration = float(getattr(animation, "duration", 0.0))
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError(f"{label} has a non-finite or negative duration") from exc
    if not math.isfinite(duration) or duration < 0.0:
        raise ValueError(f"{label} has a non-finite or negative duration")
    _validate_annotations(animation, label, duration)
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
            expected_dimensions = 4 if attribute == "rotations" else (3, 4)
            try:
                frame_samples = list(samples[:num_frames])
            except TypeError as exc:
                raise ValueError(f"{label} track {index} {attribute} samples are not iterable") from exc
            for frame, sample in enumerate(frame_samples):
                try:
                    values = [float(value) for value in sample]
                except (TypeError, ValueError) as exc:
                    raise ValueError(f"{label} track {index} {attribute}[{frame}] is not numeric") from exc
                valid_dimensions = (expected_dimensions,) if isinstance(expected_dimensions, int) else expected_dimensions
                if len(values) not in valid_dimensions:
                    raise ValueError(
                        f"{label} track {index} {attribute}[{frame}] has invalid dimension {len(values)}"
                    )
                if not all(math.isfinite(value) for value in values):
                    raise ValueError(f"{label} track {index} {attribute}[{frame}] must contain finite values")
                if attribute == "rotations" and math.sqrt(sum(value * value for value in values)) <= 1.0e-8:
                    raise ValueError(f"{label} track {index} rotation[{frame}] has zero length")


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
    deltas: list[float] = []
    for left_value, right_value in zip(left, right):
        try:
            left_float = float(left_value)
            right_float = float(right_value)
        except (TypeError, ValueError, OverflowError):
            return float("inf")
        if not math.isfinite(left_float) or not math.isfinite(right_float):
            return float("inf")
        deltas.append(abs(left_float - right_float))
    return max(deltas, default=0.0)


def _rotation_sample_delta(left: list[float], right: list[float]) -> float:
    """Compare equivalent quaternion signs as the same rotation."""

    direct = _max_sample_delta(left, right)
    negated = _max_sample_delta(left, [-value for value in right])
    return min(direct, negated)


def _metadata_equal(left: object, right: object) -> bool:
    if left is right:
        return True
    try:
        result = left == right
    except Exception:
        return repr(left) == repr(right)
    if isinstance(result, bool):
        return result
    try:
        return bool(result.all())
    except Exception:
        try:
            return bool(result)
        except Exception:
            return repr(left) == repr(right)


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
                delta = (
                    _rotation_sample_delta(list(samples[0]), list(samples[-1]))
                    if attribute == "rotations"
                    else _max_sample_delta(list(samples[0]), list(samples[-1]))
                )
                maximum = max(maximum, delta)
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


def _verify_round_trip(action: Any, expected: Any, verified: Any, replaced_indices: set[int]) -> None:
    fields = (
        ("num_frames", _num_frames(expected), _num_frames(verified)),
        ("num_tracks", _num_tracks(expected), _num_tracks(verified)),
        ("bone_names", _names(expected), _names(verified)),
        ("frame_duration", float(getattr(expected, "frame_duration", 0.0)), float(getattr(verified, "frame_duration", 0.0))),
        ("duration", float(getattr(expected, "duration", 0.0)), float(getattr(verified, "duration", 0.0))),
        ("annotations", _annotation_values(expected), _annotation_values(verified)),
    )
    for field, expected_value, actual_value in fields:
        if field == "duration":
            if abs(expected_value - actual_value) > 1.0e-5:
                raise RuntimeError(f"round-trip metadata mismatch ({field})")
        elif expected_value != actual_value:
            raise RuntimeError(f"round-trip metadata mismatch ({field})")

    expected_binding = _binding_indices(expected, "action animation", required=False)
    actual_binding = _binding_indices(verified, "written animation", required=False)
    if expected_binding != actual_binding:
        raise RuntimeError("round-trip metadata mismatch (transform-track binding)")
    expected_skeleton = _optional_skeleton_name(expected)
    actual_skeleton = _optional_skeleton_name(verified)
    if (expected_skeleton is None) != (actual_skeleton is None):
        raise RuntimeError("round-trip metadata mismatch (skeleton identity)")
    if expected_skeleton is not None and str(expected_skeleton).casefold() != str(actual_skeleton).casefold():
        raise RuntimeError("round-trip metadata mismatch (skeleton identity)")
    if _normalised_hint(expected) != _normalised_hint(verified):
        raise RuntimeError("round-trip metadata mismatch (blend hint)")
    if not _metadata_equal(_extract_motion(expected), _extract_motion(verified)):
        raise RuntimeError("round-trip metadata mismatch (extracted motion)")
    expected_float_tracks = getattr(expected, "float_tracks", None)
    actual_float_tracks = getattr(verified, "float_tracks", None)
    if not _metadata_equal(expected_float_tracks, actual_float_tracks):
        raise RuntimeError("round-trip metadata mismatch (float tracks)")

    expected_tracks = _tracks(expected)
    original_tracks = _tracks(action)
    actual_tracks = _tracks(verified)
    if len(expected_tracks) != len(actual_tracks):
        raise RuntimeError("round-trip metadata mismatch (track count)")
    for index, (expected_track, actual_track) in enumerate(zip(expected_tracks, actual_tracks)):
        expected_track = expected_tracks[index] if index in replaced_indices else original_tracks[index]
        for attribute in ("translations", "rotations", "scales"):
            expected_samples = getattr(expected_track, attribute)
            actual_samples = getattr(actual_track, attribute)
            if len(expected_samples) != len(actual_samples):
                raise RuntimeError(f"round-trip upper-body track mismatch ({index}/{attribute})")
            for expected, actual in zip(expected_samples, actual_samples):
                delta = (
                    _rotation_sample_delta(list(expected), list(actual))
                    if attribute == "rotations"
                    else _max_sample_delta(list(expected), list(actual))
                )
                if delta > ROUND_TRIP_TOLERANCE:
                    scope = "lower-body" if index in replaced_indices else "upper-body"
                    raise RuntimeError(f"round-trip {scope} track mismatch ({index}/{attribute})")


def aerialize(anim_skyrim: Any, base_path: pathlib.Path, input_path: pathlib.Path, output_path: pathlib.Path) -> list[str]:
    base_path = base_path.resolve()
    input_path = input_path.resolve()
    output_path = output_path.resolve()
    if input_path == output_path:
        raise ValueError("input and output HKX paths must be different")
    base_structure = inspect_hkx_packfile(base_path)
    input_structure = inspect_hkx_packfile(input_path)
    base_structure.update(inspect_hkx_animation_channels(base_path, base_structure))
    input_structure.update(inspect_hkx_animation_channels(input_path, input_structure))
    try:
        base = anim_skyrim.load_skyrim_animation(str(base_path))
        action = anim_skyrim.load_skyrim_animation(str(input_path))
    except Exception as exc:
        base_type = base_structure.get("animationType", "unknown")
        input_type = input_structure.get("animationType", "unknown")
        raise RuntimeError(
            "HKX codec decode failed; representation is unsupported or unverified "
            f"(base={base_type}, input={input_type}, backend={backend_provenance(anim_skyrim)})"
        ) from exc
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
        try:
            anim_skyrim.write_skyrim_animation(str(temporary), composite, ptr_size=8)
        except Exception as exc:
            raise RuntimeError(
                "HKX codec write failed; representation is unsupported or unverified "
                f"(backend={backend_provenance(anim_skyrim)})"
            ) from exc
        if not temporary.is_file() or temporary.stat().st_size == 0:
            raise RuntimeError(f"HKX writer produced no output for {output_path}")
        written_structure = inspect_hkx_packfile(temporary)
        written_structure.update(inspect_hkx_animation_channels(temporary, written_structure))
        try:
            verified = anim_skyrim.load_skyrim_animation(str(temporary))
        except Exception as exc:
            raise RuntimeError(
                "HKX codec verification decode failed; representation is unsupported or unverified "
                f"(backend={backend_provenance(anim_skyrim)})"
            ) from exc
        try:
            _validate_animation_structure(verified, "written animation")
        except ValueError as exc:
            raise RuntimeError(f"round-trip output structure invalid: {exc}") from exc
        _verify_round_trip(action, composite, verified, {entry[0] for entry in replacements})
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
