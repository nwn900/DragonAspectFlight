"""Unit tests for the external 1.7.104 Address Library validator."""

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR_PATH = REPO_ROOT / "tools/validate_address_library.py"
SPEC = importlib.util.spec_from_file_location("daf_address_library_validator", VALIDATOR_PATH)
if SPEC is None or SPEC.loader is None:  # pragma: no cover - import failure is fatal
    raise RuntimeError(f"Unable to import {VALIDATOR_PATH}")
VALIDATOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VALIDATOR
SPEC.loader.exec_module(VALIDATOR)


class AddressLibraryValidatorTests(unittest.TestCase):
    def make_fixture(
        self,
        *,
        format_value: int = 5,
        runtime: tuple[int, int, int, int] = (1, 7, 104, 0),
        count: int = 565_759,
        include_all_ids: bool = True,
    ) -> bytes:
        header = VALIDATOR.HEADER.pack(
            format_value,
            *runtime,
            b"SkyrimSE.exe\0".ljust(64, b"\0"),
            VALIDATOR.EXPECTED_POINTER_SIZE,
            VALIDATOR.EXPECTED_DATA_FORMAT,
            count,
        )
        data = bytearray(header)
        data.extend(b"\0" * (count * 4))
        if include_all_ids:
            for relocation_id in VALIDATOR.REQUIRED_RELOCATION_IDS:
                struct.pack_into("<I", data, VALIDATOR.HEADER.size + relocation_id * 4, 0x1000 + relocation_id)
        return bytes(data)

    def test_valid_dense_1_7_104_format5_fixture(self) -> None:
        payload = self.make_fixture()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "versionlib-1-7-104-0.bin"
            path.write_bytes(payload)
            report = VALIDATOR.validate_bin(path, expected_sha256=hashlib.sha256(payload).hexdigest())
        self.assertEqual(report.format, 5)
        self.assertEqual(report.header_size, 96)
        self.assertEqual(report.runtime, "1.7.104.0")
        self.assertEqual(report.image, "SkyrimSE.exe")
        self.assertEqual(report.offset_count, 565_759)
        self.assertEqual(report.present_id_count, len(VALIDATOR.REQUIRED_RELOCATION_IDS))

    def test_1_7_99_runtime_is_rejected(self) -> None:
        payload = self.make_fixture(runtime=(1, 7, 99, 0), count=565_073)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "versionlib-1-7-99-0.bin"
            path.write_bytes(payload)
            with self.assertRaisesRegex(VALIDATOR.AddressLibraryValidationError, "runtime mismatch"):
                VALIDATOR.validate_bin(path, expected_sha256=hashlib.sha256(payload).hexdigest())

    def test_missing_relocation_is_rejected(self) -> None:
        payload = self.make_fixture(include_all_ids=False)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "missing.bin"
            path.write_bytes(payload)
            with self.assertRaises(VALIDATOR.AddressLibraryValidationError):
                VALIDATOR.validate_bin(path, expected_sha256=hashlib.sha256(payload).hexdigest())

    def test_wrong_format_is_rejected(self) -> None:
        payload = self.make_fixture(format_value=2)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "old-format.bin"
            path.write_bytes(payload)
            with self.assertRaises(VALIDATOR.AddressLibraryValidationError):
                VALIDATOR.validate_bin(path, expected_sha256=hashlib.sha256(payload).hexdigest())


if __name__ == "__main__":
    unittest.main()
