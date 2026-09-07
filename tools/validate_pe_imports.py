"""Validate that a Dragon Aspect Flight DLL has only reviewed runtime imports.

The release plugin must be self-contained apart from Windows and the release
MSVC runtime.  In particular, vcpkg's dynamic spdlog/fmt triplet is forbidden:
those DLLs are not part of the mod payload and would make SKSE fail to load the
plugin on another machine.
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


EXPECTED_MACHINE = 0x8664  # IMAGE_FILE_MACHINE_AMD64
EXPECTED_OPTIONAL_MAGIC = 0x20B  # PE32+

FORBIDDEN_IMPORTS = frozenset(
    name.casefold() for name in ("spdlog.dll", "spdlogd.dll", "fmt.dll", "fmtd.dll")
)

# Deliberately narrow. Add a Windows system DLL only after reviewing why the
# plugin needs it; third-party runtime DLLs belong in neither this list nor the
# release archive.
EXPECTED_IMPORTS = frozenset(
    name.casefold()
    for name in (
        "ADVAPI32.dll",
        "bcrypt.dll",
        "D3D11.dll",
        "D3DCOMPILER_47.dll",
        "dbghelp.dll",
        "DXGI.dll",
        "KERNEL32.dll",
        "MSVCP140.dll",
        "MSVCP140_ATOMIC_WAIT.dll",
        "ole32.dll",
        "SHELL32.dll",
        "USER32.dll",
        "VCRUNTIME140.dll",
        "VCRUNTIME140_1.dll",
        "VERSION.dll",
        "WS2_32.dll",
    )
)
EXPECTED_IMPORT_PREFIXES = ("api-ms-win-crt-",)


class PEImportValidationError(ValueError):
    """Raised when a DLL is malformed or imports an unreviewed runtime DLL."""


@dataclass(frozen=True)
class PEImportReport:
    path: str
    sha256: str
    file_size: int
    machine: str
    pe_format: str
    imports: tuple[str, ...]


def _unpack_from(fmt: str, data: bytes, offset: int, label: str) -> tuple[int, ...]:
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(data):
        raise PEImportValidationError(f"PE is truncated while reading {label}")
    return struct.unpack_from(fmt, data, offset)


def _read_c_string(data: bytes, offset: int, label: str) -> str:
    if offset < 0 or offset >= len(data):
        raise PEImportValidationError(f"PE {label} points outside the file")
    end = data.find(b"\0", offset, min(len(data), offset + 4096))
    if end < 0:
        raise PEImportValidationError(f"PE {label} is not NUL-terminated")
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise PEImportValidationError(f"PE {label} is not ASCII") from exc


def _parse_imports(data: bytes) -> tuple[int, int, tuple[str, ...]]:
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise PEImportValidationError("file does not have a valid DOS MZ header")

    (pe_offset,) = _unpack_from("<I", data, 0x3C, "DOS e_lfanew")
    if pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise PEImportValidationError("file does not have a valid PE signature")

    coff = pe_offset + 4
    machine, section_count, _, _, _, optional_size, _ = _unpack_from(
        "<HHIIIHH", data, coff, "COFF header"
    )
    if machine != EXPECTED_MACHINE:
        raise PEImportValidationError(
            f"machine mismatch: expected x64 0x{EXPECTED_MACHINE:04X}, got 0x{machine:04X}"
        )
    if not section_count:
        raise PEImportValidationError("PE has no sections")

    optional = coff + 20
    if optional + optional_size > len(data):
        raise PEImportValidationError("PE optional header extends beyond the file")
    (magic,) = _unpack_from("<H", data, optional, "optional-header magic")
    if magic != EXPECTED_OPTIONAL_MAGIC:
        raise PEImportValidationError(
            f"optional-header mismatch: expected PE32+ 0x{EXPECTED_OPTIONAL_MAGIC:03X}, "
            f"got 0x{magic:03X}"
        )

    number_of_rva_offset = optional + 108
    data_directories_offset = optional + 112
    (directory_count,) = _unpack_from(
        "<I", data, number_of_rva_offset, "data-directory count"
    )
    if directory_count < 2 or optional_size < 128:
        raise PEImportValidationError("PE does not contain an import data directory")
    import_rva, import_size = _unpack_from(
        "<II", data, data_directories_offset + 8, "import data directory"
    )

    section_table = optional + optional_size
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        entry = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = _unpack_from(
            "<IIII", data, entry + 8, f"section {index}"
        )
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_to_offset(rva: int, label: str) -> int:
        for virtual_address, span, raw_offset, raw_size in sections:
            if virtual_address <= rva < virtual_address + span:
                delta = rva - virtual_address
                if delta >= raw_size:
                    break
                offset = raw_offset + delta
                if offset < len(data):
                    return offset
                break
        raise PEImportValidationError(f"PE {label} RVA 0x{rva:X} is not backed by file data")

    if import_rva == 0 or import_size == 0:
        return machine, magic, ()

    descriptor = rva_to_offset(import_rva, "import directory")
    directory_end = descriptor + import_size
    imports: dict[str, str] = {}
    terminated = False
    while descriptor + 20 <= len(data) and descriptor < directory_end:
        fields = _unpack_from("<IIIII", data, descriptor, "import descriptor")
        if not any(fields):
            terminated = True
            break
        name_rva = fields[3]
        if not name_rva:
            raise PEImportValidationError("PE import descriptor has no DLL name RVA")
        name = _read_c_string(data, rva_to_offset(name_rva, "import name"), "import name")
        imports.setdefault(name.casefold(), name)
        descriptor += 20
    if not terminated:
        raise PEImportValidationError("PE import descriptor table is not terminated")

    return machine, magic, tuple(sorted(imports.values(), key=str.casefold))


def validate_pe_imports(path: os.PathLike[str] | str) -> PEImportReport:
    """Return a report for *path*, rejecting forbidden or unreviewed imports."""

    source = Path(path)
    if not source.is_file():
        raise PEImportValidationError(f"DLL does not exist: {source}")
    data = source.read_bytes()
    machine, magic, imports = _parse_imports(data)

    forbidden = [name for name in imports if name.casefold() in FORBIDDEN_IMPORTS]
    if forbidden:
        raise PEImportValidationError(
            "forbidden dynamic dependency import(s): " + ", ".join(forbidden)
        )

    unexpected = [
        name
        for name in imports
        if name.casefold() not in EXPECTED_IMPORTS
        and not name.casefold().startswith(EXPECTED_IMPORT_PREFIXES)
    ]
    if unexpected:
        raise PEImportValidationError(
            "unreviewed runtime import(s): " + ", ".join(unexpected)
        )

    return PEImportReport(
        path=str(source.resolve()),
        sha256=hashlib.sha256(data).hexdigest(),
        file_size=len(data),
        machine=f"0x{machine:04X}",
        pe_format=f"PE32+ (0x{magic:03X})",
        imports=imports,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", required=True, help="DragonAspectFlight.dll to validate")
    parser.add_argument("--json", action="store_true", help="emit the report as JSON")
    args = parser.parse_args(argv)

    try:
        report = validate_pe_imports(args.dll)
    except (OSError, PEImportValidationError) as exc:
        print(f"PE import validation: FAIL: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(asdict(report), indent=2, sort_keys=True))
    else:
        print("PE import validation: PASS")
        print(f"path={report.path}")
        print(f"sha256={report.sha256.upper()}")
        print(
            f"machine={report.machine} format={report.pe_format} "
            f"file_size={report.file_size}"
        )
        print("imports=" + ", ".join(report.imports))
        print("forbidden_imports=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
