"""Unit tests for the Dragon Aspect Flight PE import validator."""

from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR_PATH = REPO_ROOT / "tools/validate_pe_imports.py"
SPEC = importlib.util.spec_from_file_location("daf_pe_import_validator", VALIDATOR_PATH)
if SPEC is None or SPEC.loader is None:  # pragma: no cover - import failure is fatal
    raise RuntimeError(f"Unable to import {VALIDATOR_PATH}")
VALIDATOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VALIDATOR
SPEC.loader.exec_module(VALIDATOR)


def make_pe64(imports: tuple[str, ...]) -> bytes:
    """Build a small PE32+ fixture containing only an import-name table."""

    pe_offset = 0x80
    optional_size = 0xF0
    section_rva = 0x1000
    section_raw = 0x200
    section_size = 0x1000

    data = bytearray(section_raw + section_size)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe_offset)
    data[pe_offset : pe_offset + 4] = b"PE\0\0"

    coff = pe_offset + 4
    struct.pack_into("<HHIIIHH", data, coff, 0x8664, 1, 0, 0, 0, optional_size, 0x2022)

    optional = coff + 20
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<I", data, optional + 108, 16)

    import_rva = section_rva
    import_size = (len(imports) + 1) * 20
    struct.pack_into("<II", data, optional + 112 + 8, import_rva, import_size)

    section = optional + optional_size
    data[section : section + 8] = b".rdata\0\0"
    struct.pack_into(
        "<IIIIIIHHI",
        data,
        section + 8,
        section_size,
        section_rva,
        section_size,
        section_raw,
        0,
        0,
        0,
        0,
        0x40000040,
    )

    name_offset = section_raw + import_size
    for index, name in enumerate(imports):
        encoded = name.encode("ascii") + b"\0"
        name_rva = section_rva + (name_offset - section_raw)
        struct.pack_into("<IIIII", data, section_raw + index * 20, 0, 0, 0, name_rva, 0)
        data[name_offset : name_offset + len(encoded)] = encoded
        name_offset += len(encoded)

    return bytes(data[:name_offset])


class PEImportValidatorTests(unittest.TestCase):
    def validate_fixture(self, imports: tuple[str, ...]):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "DragonAspectFlight.dll"
            path.write_bytes(make_pe64(imports))
            return VALIDATOR.validate_pe_imports(path)

    def test_expected_windows_and_msvc_imports_are_reported(self) -> None:
        report = self.validate_fixture(
            (
                "KERNEL32.dll",
                "MSVCP140.dll",
                "api-ms-win-crt-runtime-l1-1-0.dll",
                "USER32.dll",
            )
        )
        self.assertEqual(
            report.imports,
            (
                "api-ms-win-crt-runtime-l1-1-0.dll",
                "KERNEL32.dll",
                "MSVCP140.dll",
                "USER32.dll",
            ),
        )

    def test_spdlog_runtime_import_is_rejected(self) -> None:
        with self.assertRaisesRegex(VALIDATOR.PEImportValidationError, "spdlog.dll"):
            self.validate_fixture(("KERNEL32.dll", "spdlog.dll"))

    def test_fmt_runtime_import_is_rejected_case_insensitively(self) -> None:
        with self.assertRaisesRegex(VALIDATOR.PEImportValidationError, "FMT.DLL"):
            self.validate_fixture(("KERNEL32.dll", "FMT.DLL"))

    def test_unreviewed_third_party_import_is_rejected(self) -> None:
        with self.assertRaisesRegex(VALIDATOR.PEImportValidationError, "thirdparty.dll"):
            self.validate_fixture(("KERNEL32.dll", "thirdparty.dll"))

    def test_malformed_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "not-pe.dll"
            path.write_bytes(b"not a PE")
            with self.assertRaises(VALIDATOR.PEImportValidationError):
                VALIDATOR.validate_pe_imports(path)


if __name__ == "__main__":
    unittest.main()
