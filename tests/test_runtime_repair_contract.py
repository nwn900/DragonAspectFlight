#!/usr/bin/env python3
"""Static contracts for runtime ownership safeguards from the repair plan."""

from __future__ import annotations

import pathlib
import re
import unittest


SOURCE = pathlib.Path(__file__).resolve().parents[1] / "src/FlightManager.cpp"


def function_body(source: str, name: str) -> str:
    marker = re.search(rf"(?:void|bool|std::uint64_t)\s+(?:FlightManager::)?{name}\s*\([^)]*\)\s*\{{", source)
    if marker is None:
        raise AssertionError(f"function {name} not found")
    start = marker.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function {name}")


class RuntimeRepairContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_jump_graph_variable_is_only_touched_during_active_flight(self) -> None:
        body = function_body(self.source, "SetFlightGraphVariables")
        jump_write = body.index("GraphVarVanillaInJumpState")
        self.assertRegex(body[:jump_write], r"if\s*\(\s*a_flightActive\s*\)")
        self.assertNotIn("SetGraphVariableBool(RE::BSFixedString(GraphVarVanillaInJumpState), false)", body)

    def test_stop_does_not_fabricate_ground_controller_state(self) -> None:
        body = function_body(self.source, "StopFlight")
        self.assertNotRegex(body, r"wantState\s*=\s*RE::hkpCharacterStateType::kOnGround")
        self.assertNotRegex(body, r"context\.currentState\s*=\s*RE::hkpCharacterStateType::kOnGround")

    def test_start_refuses_missing_loaded_actor_controller_before_claiming_flight(self) -> None:
        body = function_body(self.source, "StartFlight")
        claim = body.index("_isFlying = true")
        preflight = body[:claim]
        self.assertRegex(preflight, r"!player\s*\|\|\s*!player->Is3DLoaded\(\)")
        self.assertRegex(preflight, r"!controller")
        self.assertRegex(preflight, r"graph.*gate|Probe.*Graph", re.IGNORECASE)

    def test_original_friction_is_stored_and_restored(self) -> None:
        header = SOURCE.parents[1] / "include/DragonAspectFlight/FlightManager.h"
        header_text = header.read_text(encoding="utf-8")
        self.assertIn("_originalNoFriction", header_text)
        self.assertRegex(self.source, r"_originalNoFriction\s*=\s*controller->flags\.all")
        self.assertRegex(self.source, r"if\s*\(\s*originalNoFriction\s*\)[\s\S]{0,180}controller->flags\.set")


if __name__ == "__main__":
    unittest.main()
