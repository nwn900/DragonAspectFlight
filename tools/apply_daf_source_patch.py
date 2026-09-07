#!/usr/bin/env python3
"""Apply reviewed DAF source edits to a separate copy with provenance checks.

The applier never edits the source checkout.  It copies a clean baseline into a
temporary sibling, verifies every manifest preimage (including optional Git
blob IDs), applies byte-preserving replacements, emits a binary diff, and
atomically renames the completed copy into place.  The report explicitly marks
the copy as uncompiled/unrebuilt/un-deployed.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
from typing import Any


def sha256(path: pathlib.Path) -> str:
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
    if not isinstance(payload, dict) or payload.get("version") != 1:
        raise ValueError("source patch manifest must be an object with version=1")
    files = payload.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("source patch manifest must contain a non-empty files list")
    for entry in files:
        if not isinstance(entry, dict):
            raise ValueError("each source patch entry must be an object")
        _relative_path(entry.get("path"), "path")
        for field in ("original_sha256", "replacement_sha256"):
            value = entry.get(field)
            if not isinstance(value, str) or len(value) != 64:
                raise ValueError(f"each source patch entry requires {field}")
    return payload


def git_blob_sha256(repo_root: pathlib.Path, relative_path: pathlib.PurePosixPath) -> str | None:
    """Return the repository's Git blob object ID, when the path is tracked."""

    completed = subprocess.run(
        ["git", "-C", str(repo_root), "hash-object", "--path", relative_path.as_posix(), "--", relative_path.as_posix()],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return None
    value = completed.stdout.strip()
    return value or None


_GENERATED_DIR_NAMES = {
    ".git",
    ".codebase-memory",
    ".vs",
    "artifacts",
    "build",
    "out",
    "__pycache__",
}


def _copy_tree(source: pathlib.Path, destination: pathlib.Path) -> None:
    """Copy source content while leaving VCS/build/generated state behind."""

    def ignore_generated(_directory: str, names: list[str]) -> set[str]:
        return {
            name
            for name in names
            if name.casefold() in _GENERATED_DIR_NAMES or name.casefold().startswith("build-")
        }

    shutil.copytree(
        source,
        destination,
        symlinks=True,
        dirs_exist_ok=True,
        ignore=ignore_generated,
    )


def _write_diff(baseline_root: pathlib.Path, output_root: pathlib.Path, paths: list[pathlib.PurePosixPath], destination: pathlib.Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    for relative in paths:
        before = baseline_root.joinpath(*relative.parts).read_bytes()
        after = output_root.joinpath(*relative.parts).read_bytes()
        before_lines = before.decode("utf-8", errors="surrogateescape").splitlines(keepends=True)
        after_lines = after.decode("utf-8", errors="surrogateescape").splitlines(keepends=True)
        lines.extend(
            difflib.unified_diff(
                before_lines,
                after_lines,
                fromfile=f"a/{relative.as_posix()}",
                tofile=f"b/{relative.as_posix()}",
            )
        )
    destination.write_text("".join(lines), encoding="utf-8", newline="\n")


def apply_source_patch(
    source_root: pathlib.Path,
    baseline_root: pathlib.Path,
    target_root: pathlib.Path,
    manifest: dict[str, Any],
    diff_path: pathlib.Path | None = None,
) -> dict[str, Any]:
    """Create an atomic patched copy from ``baseline_root`` and ``source_root``."""

    source_root = source_root.resolve()
    baseline_root = baseline_root.resolve()
    target_root = target_root.resolve()
    if not source_root.is_dir() or not baseline_root.is_dir():
        raise FileNotFoundError("source and baseline roots must be directories")
    if target_root.exists():
        raise FileExistsError(f"refusing to overwrite existing target root: {target_root}")
    for root in (source_root, baseline_root):
        if os.path.commonpath((str(target_root), str(root))) == str(root):
            raise ValueError("target root must be outside the source and baseline roots")
    target_root.parent.mkdir(parents=True, exist_ok=True)

    entries = manifest["files"]
    relative_paths = [_relative_path(entry["path"], "path") for entry in entries]
    temporary: pathlib.Path | None = None
    try:
        temporary = pathlib.Path(tempfile.mkdtemp(prefix=f".{target_root.name}.", dir=target_root.parent))
        _copy_tree(baseline_root, temporary)
        changed: list[dict[str, Any]] = []
        for entry, relative in zip(entries, relative_paths):
            baseline_file = baseline_root.joinpath(*relative.parts)
            source_file = source_root.joinpath(*relative.parts)
            output_file = temporary.joinpath(*relative.parts)
            if not baseline_file.is_file() or not source_file.is_file():
                raise FileNotFoundError(f"manifest path is missing in source/baseline: {relative}")
            original_hash = sha256(baseline_file)
            if original_hash.casefold() != str(entry["original_sha256"]).casefold():
                raise ValueError(
                    f"baseline preimage mismatch for {relative}: "
                    f"expected={entry['original_sha256']} observed={original_hash}"
                )
            expected_blob = entry.get("original_git_blob")
            if expected_blob:
                observed_blob = git_blob_sha256(baseline_root, relative)
                if observed_blob is None or observed_blob.casefold() != str(expected_blob).casefold():
                    raise ValueError(
                        f"Git blob preimage mismatch for {relative}: "
                        f"expected={expected_blob} observed={observed_blob}"
                    )
            replacement_hash = sha256(source_file)
            if replacement_hash.casefold() != str(entry["replacement_sha256"]).casefold():
                raise ValueError(
                    f"replacement hash mismatch for {relative}: "
                    f"expected={entry['replacement_sha256']} observed={replacement_hash}"
                )
            output_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_file, output_file)
            baseline_bytes = baseline_file.read_bytes()
            source_bytes = source_file.read_bytes()
            changed.append(
                {
                    "path": relative.as_posix(),
                    "originalSha256": original_hash,
                    "replacementSha256": replacement_hash,
                    "originalGitBlob": expected_blob,
                    "crlfPreserved": (b"\r\n" in baseline_bytes) == (b"\r\n" in source_bytes),
                }
            )

        if diff_path is None:
            diff_path = target_root.with_suffix(target_root.suffix + ".diff")
        _write_diff(baseline_root, temporary, relative_paths, diff_path.resolve())
        report = {
            "version": 1,
            "tool": "apply_daf_source_patch.py",
            "sourceRoot": str(source_root),
            "baselineRoot": str(baseline_root),
            "targetRoot": str(target_root),
            "files": changed,
            "generatedDiff": str(diff_path.resolve()),
            "compiled": False,
            "rebuiltDataStack": False,
            "deployed": False,
            "runtimeValidated": False,
        }
        (temporary / "source-delivery-report.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n"
        )
        os.replace(temporary, target_root)
        temporary = None
        return report
    finally:
        if temporary is not None:
            shutil.rmtree(temporary, ignore_errors=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--baseline-root", type=pathlib.Path, required=True)
    parser.add_argument("--target-root", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--diff", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = apply_source_patch(
        args.source_root,
        args.baseline_root,
        args.target_root,
        load_manifest(args.manifest),
        args.diff,
    )
    print(f"patched_files={len(report['files'])} target={args.target_root.resolve()} compiled=false deployed=false")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
