#!/usr/bin/env python3
"""Build DAF-owned OAR flight loops from the bundled, redistributable NickNak clips."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET


ANIMATION_NAMES = (
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

FAMILIES = {
    "Flight Base 00 - Unarmed and Magic": "H2h_Fall.hkx",
    "Flight Base 10 - One Handed": "1hm_Fall.hkx",
    "Flight Base 20 - Greatsword": "2hm_Fall.hkx",
    "Flight Base 30 - Axe and Warhammer": "2hw_Fall.hkx",
    "Flight Base 40 - Aiming": "Aiming_Fall.hkx",
}

SCOPES = (pathlib.Path(), pathlib.Path("male"), pathlib.Path("female"))
TRANSFORM_RE = re.compile(
    r"\(([-0-9.eE+]+) ([-0-9.eE+]+) ([-0-9.eE+]+)\)"
    r"\(([-0-9.eE+]+) ([-0-9.eE+]+) ([-0-9.eE+]+) ([-0-9.eE+]+)\)"
    r"\(([-0-9.eE+]+) ([-0-9.eE+]+) ([-0-9.eE+]+)\)"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--hkxc", type=pathlib.Path, required=True)
    return parser.parse_args()


def run_hkxc(hkxc: pathlib.Path, *args: str) -> None:
    completed = subprocess.run(
        [str(hkxc), *args],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        output = "\n".join(part for part in (completed.stdout, completed.stderr) if part)
        raise RuntimeError(f"hkxc failed ({completed.returncode}): {' '.join(args)}\n{output}")


def get_param(parent: ET.Element, name: str) -> ET.Element:
    for element in parent.findall("hkparam"):
        if element.attrib.get("name") == name:
            return element
    raise ValueError(f"Missing hkparam {name!r}")


def sanitize_animation_xml(source: pathlib.Path, target: pathlib.Path) -> tuple[int, int, float]:
    tree = ET.parse(source)
    animation = next(
        (
            element
            for element in tree.iter("hkobject")
            if element.attrib.get("class") == "hkaInterleavedUncompressedAnimation"
        ),
        None,
    )
    if animation is None:
        raise ValueError(f"Expected hkaInterleavedUncompressedAnimation in {source}")

    track_count = int((get_param(animation, "numberOfTransformTracks").text or "0").strip())
    annotation_tracks = get_param(animation, "annotationTracks")
    names = []
    removed_annotations = 0
    for track in annotation_tracks.findall("hkobject"):
        names.append((get_param(track, "trackName").text or "").strip())
        annotations = get_param(track, "annotations")
        removed_annotations += len(annotations.findall("hkobject"))
        for child in list(annotations):
            annotations.remove(child)
        annotations.attrib["numelements"] = "0"
        annotations.text = None

    if len(names) != track_count:
        raise ValueError(f"Track-name count mismatch in {source}: {len(names)} != {track_count}")
    try:
        root_index = names.index("NPC Root [Root]")
    except ValueError as exc:
        raise ValueError(f"NPC Root [Root] track missing in {source}") from exc

    transforms = get_param(animation, "transforms")
    transform_text = transforms.text or ""
    matches = list(TRANSFORM_RE.finditer(transform_text))
    declared_count = int(transforms.attrib.get("numelements", "0"))
    if len(matches) != declared_count or declared_count % track_count:
        raise ValueError(
            f"Transform count mismatch in {source}: parsed={len(matches)} "
            f"declared={declared_count} tracks={track_count}"
        )

    frame_count = declared_count // track_count
    root_z = [float(matches[frame * track_count + root_index].group(3)) for frame in range(frame_count)]
    root_z_span = max(root_z) - min(root_z)
    if root_z_span > 0.001:
        raise ValueError(f"Root Z is not stable in {source}: span={root_z_span:.6f}")

    extracted_motion = (get_param(animation, "extractedMotion").text or "").strip()
    if extracted_motion != "null":
        raise ValueError(f"Extracted root motion is present in {source}: {extracted_motion}")

    ET.indent(tree, space="\t")
    tree.write(target, encoding="ascii", xml_declaration=True)
    return frame_count, removed_annotations, root_z_span


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


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
        "Dragon Aspect Flight 1.8.1 bundled animation/effect asset SHA-256",
        "=================================================================",
        "",
    ]
    lines.extend(
        f"{sha256(path).lower()} *{path.relative_to(data_root).as_posix()}" for path in assets
    )
    manifest = data_root / "SKSE/Plugins/DragonAspectFlight-AnimationHashes.txt"
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return len(assets)


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    hkxc = args.hkxc.resolve()
    if not hkxc.is_file():
        raise FileNotFoundError(f"hkxc not found: {hkxc}")

    data_root = repo_root / "Data"
    nicknak_root = data_root / "meshes/actors/character/animations/NickNak"
    oar_root = (
        data_root
        / "meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight"
    )

    expected_outputs: set[pathlib.Path] = set()
    built = 0
    with tempfile.TemporaryDirectory(prefix="daf-flight-base-") as temporary:
        temp_root = pathlib.Path(temporary)
        for submod_name, source_name in FAMILIES.items():
            source_hkx = nicknak_root / source_name
            if not source_hkx.is_file():
                raise FileNotFoundError(f"Bundled NickNak source missing: {source_hkx}")

            source_xml = temp_root / f"{source_hkx.stem}-source.xml"
            sanitized_xml = temp_root / f"{source_hkx.stem}-sanitized.xml"
            sanitized_hkx = temp_root / f"{source_hkx.stem}-sanitized.hkx"

            run_hkxc(hkxc, "convert", "-i", str(source_hkx), "-o", str(source_xml), "-v", "xml")
            frames, removed, root_z_span = sanitize_animation_xml(source_xml, sanitized_xml)
            run_hkxc(
                hkxc,
                "convert",
                "-i",
                str(sanitized_xml),
                "-o",
                str(sanitized_hkx),
                "-v",
                "amd64",
            )
            run_hkxc(hkxc, "verify", str(sanitized_hkx))

            submod_root = oar_root / submod_name
            for scope in SCOPES:
                for animation_name in ANIMATION_NAMES:
                    target = submod_root / scope / animation_name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(sanitized_hkx, target)
                    expected_outputs.add(target.resolve())
                    built += 1

            print(
                f"{submod_name}: source={source_name}, frames={frames}, "
                f"removed_annotations={removed}, root_z_span={root_z_span:.6f}, "
                f"sha256={sha256(sanitized_hkx)}"
            )

    existing_outputs = {
        path.resolve()
        for submod_name in FAMILIES
        for path in (oar_root / submod_name).rglob("*.hkx")
    }
    unexpected = sorted(existing_outputs - expected_outputs)
    missing = sorted(expected_outputs - existing_outputs)
    if unexpected or missing:
        raise RuntimeError(
            f"Generated flight stack mismatch: unexpected={len(unexpected)}, missing={len(missing)}"
        )

    manifest_count = write_hash_manifest(data_root)
    print(f"Built {built} DAF flight-base aliases across {len(FAMILIES)} weapon families.")
    print(f"Wrote SHA-256 manifest for {manifest_count} bundled HKX/NIF assets.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
