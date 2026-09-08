#!/usr/bin/env python3
"""Stage exact-original flight action composites without touching ``Data``.

The animation stack generator deliberately refuses to invent an intro,
recovery, MCO, or draw/commit source.  This tool consumes a reviewed manifest
that names each original clip and its expected source hash, composites it with
the flight base using :mod:`AerializeHkx`, and writes a separate atomic
staging directory plus provenance manifests.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import sys
import tempfile
from typing import Any

from AerializeHkx import aerialize, backend_provenance, load_backend


def sha256(path: pathlib.Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def _relative_path(value: object, field: str) -> pathlib.PurePosixPath:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"manifest {field} must be a non-empty relative POSIX path: {value!r}")
    if "\\" in value or "\x00" in value or ":" in value:
        raise ValueError(f"manifest {field} must use safe relative POSIX separators: {value!r}")
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise ValueError(f"manifest {field} must be a relative POSIX path: {value!r}")
    return path


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    _validate_manifest(payload)
    return payload


def _validate_manifest(payload: object) -> dict[str, Any]:
    """Validate a manifest whether it came from disk or an injected caller."""

    if not isinstance(payload, dict) or payload.get("version") != 1:
        raise ValueError("action manifest must be an object with version=1")
    clips = payload.get("clips")
    if not isinstance(clips, list) or not clips:
        raise ValueError("action manifest must contain a non-empty clips list")
    seen_targets: set[str] = set()
    for clip in clips:
        if not isinstance(clip, dict):
            raise ValueError("each action manifest clip must be an object")
        _relative_path(clip.get("target"), "target")
        _relative_path(clip.get("source"), "source")
        expected = clip.get("source_sha256")
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9A-Fa-f]{64}", expected):
            raise ValueError("each action clip requires a 64-character hexadecimal source_sha256")
        target_key = str(_relative_path(clip["target"], "target")).casefold()
        if target_key in seen_targets:
            raise ValueError(f"duplicate target in action manifest: {clip['target']!r}")
        if target_key in {"exact-original-action-manifest.json", "exact-original-action-sha256.txt"}:
            raise ValueError(f"action target collides with a generated report: {clip['target']!r}")
        seen_targets.add(target_key)
    return payload


def _join_under(root: pathlib.Path, relative: pathlib.PurePosixPath, field: str) -> pathlib.Path:
    """Resolve a manifest path and reject symlink/junction escapes."""

    root_resolved = root.resolve()
    candidate = root.joinpath(*relative.parts).resolve(strict=False)
    if candidate != root_resolved and root_resolved not in candidate.parents:
        raise ValueError(f"manifest {field} escapes its root: {relative.as_posix()!r}")
    return candidate


def _atomic_json(path: pathlib.Path, payload: object) -> None:
    temporary: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{path.name}.",
            suffix=".tmp",
            dir=path.parent,
            delete=False,
        ) as stream:
            temporary = pathlib.Path(stream.name)
            json.dump(payload, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def rebuild_actions(
    anim_skyrim: Any,
    base_path: pathlib.Path,
    source_root: pathlib.Path,
    output_root: pathlib.Path,
    manifest: dict[str, Any],
    manifest_path: pathlib.Path | None = None,
) -> dict[str, Any]:
    """Build a manifest-described action set into a new output directory."""

    base_path = base_path.resolve()
    source_root = source_root.resolve()
    output_root = output_root.resolve()
    if not source_root.is_dir():
        raise FileNotFoundError(f"exact-original source root not found: {source_root}")
    if output_root.exists():
        raise FileExistsError(f"refusing to overwrite existing action staging root: {output_root}")
    if output_root == source_root or source_root in output_root.parents:
        raise ValueError("output root must not be inside the exact-original source root")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    if not base_path.is_file():
        raise FileNotFoundError(f"flight base animation not found: {base_path}")
    manifest = _validate_manifest(manifest)
    base_sha256 = sha256(base_path)

    temporary_root: pathlib.Path | None = None
    result_clips: list[dict[str, Any]] = []
    try:
        temporary_root = pathlib.Path(tempfile.mkdtemp(prefix=f".{output_root.name}.", dir=output_root.parent))
        for clip in manifest["clips"]:
            source_rel = _relative_path(clip["source"], "source")
            target_rel = _relative_path(clip["target"], "target")
            source = _join_under(source_root, source_rel, "source")
            target = _join_under(temporary_root, target_rel, "target")
            if not source.is_file():
                raise FileNotFoundError(f"exact-original action source missing: {source}")
            observed_source_hash = sha256(source)
            if observed_source_hash.casefold() != str(clip["source_sha256"]).casefold():
                raise ValueError(
                    f"source hash mismatch for {source_rel}: "
                    f"expected={clip['source_sha256']} observed={observed_source_hash}"
                )
            replaced = aerialize(anim_skyrim, base_path, source, target)
            result_clips.append(
                {
                    "target": target_rel.as_posix(),
                    "source": source_rel.as_posix(),
                    "sourceSha256": observed_source_hash,
                    "outputSha256": sha256(target),
                    "replacedTracks": replaced,
                    "preservedByteForByte": not bool(replaced),
                    "family": clip.get("family"),
                }
            )

        report = {
            "version": 1,
            "tool": "rebuild_flight_actions.py",
            "base": str(base_path),
            "baseSha256": base_sha256,
            "sourceRoot": str(source_root),
            "provenance": {
                "toolVersion": "1",
                "backend": backend_provenance(anim_skyrim),
                "aerialize": "binding-safe composite with atomic output verification",
                "sourceHashPolicy": "SHA-256 exact-original inputs",
                "compiled": False,
                "rebuiltDataStack": False,
                "runtimeValidated": False,
            },
            "clips": result_clips,
            "compiled": False,
            "rebuiltDataStack": False,
            "runtimeValidated": False,
        }
        if manifest_path is not None:
            report["manifestInput"] = str(manifest_path.resolve())
            report["manifestSha256"] = sha256(manifest_path.resolve())
        _atomic_json(temporary_root / "exact-original-action-manifest.json", report)
        lines = [
            "Dragon Aspect Flight exact-original action staging SHA-256",
            "===========================================================",
            "",
        ]
        lines.extend(f"{clip['outputSha256'].lower()} *{clip['target']}" for clip in result_clips)
        (temporary_root / "exact-original-action-sha256.txt").write_text(
            "\n".join(lines) + "\n", encoding="utf-8", newline="\n"
        )
        os.replace(temporary_root, output_root)
        temporary_root = None
        return report
    finally:
        if temporary_root is not None:
            shutil.rmtree(temporary_root, ignore_errors=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", type=pathlib.Path, required=True)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--output-root", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--pynifly-hkx-dir", type=pathlib.Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    backend = load_backend(args.pynifly_hkx_dir)
    report = rebuild_actions(
        backend,
        args.base,
        args.source_root,
        args.output_root,
        load_manifest(args.manifest),
        manifest_path=args.manifest,
    )
    print(f"staged_exact_original_actions={len(report['clips'])} output={args.output_root.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
