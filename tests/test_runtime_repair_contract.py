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

    def test_duplicate_start_is_checked_before_any_graph_probe(self) -> None:
        body = function_body(self.source, "StartFlight")
        self.assertLess(body.index("_isFlying"), body.index("ProbeFlightGraphGates"))

    def test_start_update_thread_failure_is_propagated_and_rolled_back(self) -> None:
        header = SOURCE.parents[1] / "include/DragonAspectFlight/FlightManager.h"
        header_text = header.read_text(encoding="utf-8")
        self.assertRegex(header_text, r"\[\[nodiscard\]\]\s+bool StartUpdateThread\(\)")
        body = function_body(self.source, "StartFlight")
        self.assertRegex(body, r"if\s*\(\s*!StartUpdateThread\(\)\s*\)")
        self.assertIn("StopFlight()", body[body.index("StartUpdateThread"):])

    def test_controller_baseline_is_restored_only_to_owned_controller(self) -> None:
        header = SOURCE.parents[1] / "include/DragonAspectFlight/FlightManager.h"
        header_text = header.read_text(encoding="utf-8")
        self.assertIn("_flightOwnedController", header_text)
        body = function_body(self.source, "StopFlight")
        self.assertRegex(body, r"controllerMatches")
        self.assertRegex(body, r"worldCleanupOwned\s*=\s*[\s\S]{0,240}controllerMatches")

    def test_stop_rechecks_controller_before_restoring_baseline(self) -> None:
        body = function_body(self.source, "StopFlight")
        restore = body.index("restoreController")
        restore_block = body[restore : restore + 900]
        self.assertIn("restoreController != currentController", restore_block)
        self.assertIn("controller_replaced_before_restore", restore_block)
        self.assertRegex(restore_block, r"restoreController->gravity\s*=\s*originalGravity")

    def test_update_aborts_when_essential_graph_gate_write_fails(self) -> None:
        body = function_body(self.source, "UpdateFlight")
        self.assertRegex(body, r"if\s*\(\s*!SetFlightGraphVariables\(")
        self.assertIn("graph_gate_lost", body)

    def test_update_aborts_before_engine_writes_when_controller_identity_changes(self) -> None:
        body = function_body(self.source, "UpdateFlight")
        guard = body.index("controllerOwnershipLost")
        self.assertLess(guard, body.index("GetEquipmentDiagnostic"))
        self.assertRegex(body[:guard + 300], r"_flightOwnedController")
        self.assertIn("reason=controller_replaced", body)

    def test_lifecycle_reset_clears_controller_owner(self) -> None:
        body = function_body(self.source, "ResetForLifecycle")
        self.assertRegex(body, r"_flightWorldStateOwned\s*=\s*false;[\s\S]{0,120}_flightOwnedController\s*=\s*nullptr")

    def test_original_friction_is_stored_and_restored(self) -> None:
        header = SOURCE.parents[1] / "include/DragonAspectFlight/FlightManager.h"
        header_text = header.read_text(encoding="utf-8")
        self.assertIn("_originalNoFriction", header_text)
        self.assertRegex(self.source, r"_originalNoFriction\s*=\s*controller->flags\.all")
        self.assertRegex(self.source, r"if\s*\(\s*originalNoFriction\s*\)[\s\S]{0,220}restoreController->flags\.set")


if __name__ == "__main__":
    unittest.main()
