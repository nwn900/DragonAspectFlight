#!/usr/bin/env python3
"""Extend Skyrim's Havok skeleton to match every track in an animation.

This is a build-time bridge for hkxcmd, whose KF exporter rejects animations
that contain extra XPMSE-compatible tracks even though Skyrim safely ignores
those tracks when the active skeleton does not use them.
"""

from __future__ import annotations

import argparse
import copy
import pathlib
import xml.etree.ElementTree as ET


IDENTITY_TRANSFORM = "(0.000000 0.000000 0.000000)(0.000000 0.000000 0.000000 1.000000)(1.000000 1.000000 1.000000)"


def get_param(parent: ET.Element, name: str) -> ET.Element:
    for element in parent.findall("hkparam"):
        if element.attrib.get("name") == name:
            return element
    raise ValueError(f"Missing hkparam {name!r}")


def find_object(root: ET.Element, class_names: tuple[str, ...]) -> ET.Element:
    for element in root.iter("hkobject"):
        if element.attrib.get("class") in class_names:
            return element
    raise ValueError(f"Expected one of {class_names!r}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skeleton-xml", type=pathlib.Path, required=True)
    parser.add_argument("--animation-xml", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    skeleton_tree = ET.parse(args.skeleton_xml)
    animation_tree = ET.parse(args.animation_xml)

    skeleton = find_object(skeleton_tree.getroot(), ("hkaSkeleton",))
    animation = find_object(
        animation_tree.getroot(),
        ("hkaInterleavedUncompressedAnimation", "hkaSplineCompressedAnimation"),
    )

    bones = get_param(skeleton, "bones")
    reference_pose = get_param(skeleton, "referencePose")
    parent_indices = get_param(skeleton, "parentIndices")
    annotation_tracks = get_param(animation, "annotationTracks")

    bone_names = [(get_param(bone, "name").text or "").strip() for bone in bones.findall("hkobject")]
    track_names = [
        (get_param(track, "trackName").text or "").strip()
        for track in annotation_tracks.findall("hkobject")
    ]
    normalized_bones = [name.casefold() for name in bone_names]
    normalized_tracks = [name.casefold() for name in track_names]
    if normalized_tracks[: len(bone_names)] != normalized_bones:
        mismatch = next(
            (
                index
                for index, (bone, track) in enumerate(zip(normalized_bones, normalized_tracks))
                if bone != track
            ),
            min(len(bone_names), len(track_names)),
        )
        raise ValueError(
            "Animation track order does not begin with the base skeleton's bone order: "
            f"index={mismatch}, bone={bone_names[mismatch]!r}, track={track_names[mismatch]!r}"
        )

    pose_lines = [line.strip() for line in (reference_pose.text or "").splitlines() if line.strip()]
    parent_values = (parent_indices.text or "").split()
    if len(pose_lines) != len(bone_names) or len(parent_values) != len(bone_names):
        raise ValueError("Base skeleton bone, parent, and reference-pose counts disagree")

    bone_template = bones.find("hkobject")
    if bone_template is None:
        raise ValueError("Base skeleton contains no bone template")

    for track_name in track_names[len(bone_names) :]:
        bone = copy.deepcopy(bone_template)
        get_param(bone, "name").text = track_name
        get_param(bone, "lockTranslation").text = "false"
        bones.append(bone)
        pose_lines.append(IDENTITY_TRANSFORM)
        parent_values.append("0")

    total = len(track_names)
    bones.attrib["numelements"] = str(total)
    reference_pose.attrib["numelements"] = str(total)
    reference_pose.text = "\n\t\t\t\t" + "\n\t\t\t\t".join(pose_lines) + "\n\t\t\t"
    parent_indices.attrib["numelements"] = str(total)
    parent_indices.text = "\n\t\t\t\t" + " ".join(parent_values) + "\n\t\t\t"

    ET.indent(skeleton_tree, space="\t")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    skeleton_tree.write(
        args.output,
        encoding="ascii",
        xml_declaration=True,
        short_empty_elements=False,
    )
    print(f"Extended skeleton from {len(bone_names)} to {total} tracks: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
