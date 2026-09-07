"""Validate the external Address Library BIN used by a DAF 1.7.104 build.

The BIN is intentionally an external input and is never copied into the DAF
source or runtime package.  The validator understands the dense format-5
header used by the 1.7.104 AE Address Library and checks the relocation IDs
audited for the CommonLib surfaces DAF uses.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


EXPECTED_SHA256 = "8aab3dd251d135b849bd983f86a4a205c920fa3e81f8e30c0e63ccfef9423842"
EXPECTED_FORMAT = 5
EXPECTED_RUNTIME = (1, 7, 104, 0)
EXPECTED_POINTER_SIZE = 8
EXPECTED_DATA_FORMAT = 0
EXPECTED_OFFSET_COUNT = 565_759
EXPECTED_IMAGE = "SkyrimSE.exe"

# These are the 1.7.104 AE IDs identified while auditing the CommonLib APIs
# reached by DAF (UI/TES singletons, HUD, actor/value and input surfaces).
# Format 5 is a dense uint32 table, so a present ID is one whose entry is
# within the table and non-zero.
REQUIRED_RELOCATION_IDS = (
    400_327,
    402_776,
    400_863,
    403_521,
    400_802,
    403_450,
    400_269,
    52_933,
    37_952,
    32_885,
    32_886,
    34_517,
    13_344,
    400_507,
    400_517,
    27_202,
)

HEADER = struct.Struct("<I4I64siii")


class AddressLibraryValidationError(ValueError):
    """Raised when a supplied Address Library BIN is not the expected one."""


@dataclass(frozen=True)
class AddressLibraryReport:
    path: str
    sha256: str
    file_size: int
    header_size: int
    format: int
    runtime: str
    image: str
    pointer_size: int
    data_format: int
    offset_count: int
    required_id_count: int
    present_id_count: int
    offsets: dict[str, int]


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_bin(
    path: os.PathLike[str] | str,
    *,
    expected_sha256: str = EXPECTED_SHA256,
) -> AddressLibraryReport:
    """Validate *path* and return a machine-readable report.

    ``expected_sha256`` is injectable only for unit tests using a synthetic
    fixture.  The command-line entry point always uses the pinned 1.7.104
    digest above.
    """

    source = Path(path)
    if not source.is_file():
        raise AddressLibraryValidationError(f"Address Library BIN does not exist: {source}")

    data = source.read_bytes()
    digest = _sha256(data)
    if expected_sha256 and digest.lower() != expected_sha256.lower():
        raise AddressLibraryValidationError(
            f"SHA-256 mismatch: expected {expected_sha256.upper()}, got {digest.upper()}"
        )

    if len(data) < HEADER.size:
        raise AddressLibraryValidationError(
            f"BIN is truncated before the format-5 header ({len(data)} < {HEADER.size} bytes)"
        )

    file_format, *runtime_fields, raw_image, pointer_size, data_format, offset_count = HEADER.unpack_from(
        data
    )
    runtime = tuple(runtime_fields)
    image = raw_image.split(b"\0", 1)[0].decode("ascii", errors="replace")

    if file_format != EXPECTED_FORMAT:
        raise AddressLibraryValidationError(
            f"Address Library format mismatch: expected {EXPECTED_FORMAT}, got {file_format}"
        )
    if runtime != EXPECTED_RUNTIME:
        raise AddressLibraryValidationError(
            f"runtime mismatch: expected {'.'.join(map(str, EXPECTED_RUNTIME))}, "
            f"got {'.'.join(map(str, runtime))}"
        )
    if image != EXPECTED_IMAGE:
        raise AddressLibraryValidationError(
            f"image name mismatch: expected {EXPECTED_IMAGE!r}, got {image!r}"
        )
    if pointer_size != EXPECTED_POINTER_SIZE:
        raise AddressLibraryValidationError(
            f"pointer-size mismatch: expected {EXPECTED_POINTER_SIZE}, got {pointer_size}"
        )
    if data_format != EXPECTED_DATA_FORMAT:
        raise AddressLibraryValidationError(
            f"format-5 data-format mismatch: expected {EXPECTED_DATA_FORMAT}, got {data_format}"
        )
    if offset_count != EXPECTED_OFFSET_COUNT:
        raise AddressLibraryValidationError(
            f"offset-count mismatch: expected {EXPECTED_OFFSET_COUNT}, got {offset_count}"
        )

    expected_size = HEADER.size + offset_count * 4
    if len(data) != expected_size:
        raise AddressLibraryValidationError(
            f"BIN size mismatch: expected {expected_size} bytes from header, got {len(data)}"
        )

    offsets: dict[str, int] = {}
    missing: list[int] = []
    for relocation_id in REQUIRED_RELOCATION_IDS:
        if relocation_id >= offset_count:
            missing.append(relocation_id)
            continue
        offset = struct.unpack_from("<I", data, HEADER.size + relocation_id * 4)[0]
        if offset == 0:
            missing.append(relocation_id)
        else:
            offsets[str(relocation_id)] = offset

    if missing:
        missing_text = ", ".join(map(str, missing))
        raise AddressLibraryValidationError(
            f"required 1.7.104 relocation IDs are missing or zero: {missing_text}"
        )

    return AddressLibraryReport(
        path=str(source.resolve()),
        sha256=digest,
        file_size=len(data),
        header_size=HEADER.size,
        format=file_format,
        runtime=".".join(map(str, runtime)),
        image=image,
        pointer_size=pointer_size,
        data_format=data_format,
        offset_count=offset_count,
        required_id_count=len(REQUIRED_RELOCATION_IDS),
        present_id_count=len(offsets),
        offsets=offsets,
    )


def _resolve_path(cli_path: str | None) -> Path:
    value = cli_path or os.environ.get("DAF_ADDRESS_LIBRARY_BIN")
    if not value:
        raise AddressLibraryValidationError(
            "Supply --bin PATH or set DAF_ADDRESS_LIBRARY_BIN; the Address Library BIN is external."
        )
    return Path(value)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bin",
        dest="bin_path",
        help="explicit format-5 Address Library BIN path (or DAF_ADDRESS_LIBRARY_BIN)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit the validation report as JSON",
    )
    args = parser.parse_args(argv)

    try:
        report = validate_bin(_resolve_path(args.bin_path))
    except (OSError, AddressLibraryValidationError) as exc:
        print(f"Address Library validation: FAIL: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(asdict(report), indent=2, sort_keys=True))
    else:
        print("Address Library validation: PASS")
        print(f"path={report.path}")
        print(f"sha256={report.sha256.upper()}")
        print(
            f"format={report.format} runtime={report.runtime} image={report.image} "
            f"pointer_size={report.pointer_size} data_format={report.data_format} "
            f"entries={report.offset_count}"
        )
        print(
            f"header_size={report.header_size} "
            f"required_relocations={report.present_id_count}/{report.required_id_count} "
            f"status=present"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
