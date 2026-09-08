"""Source-level contract for the DAF two-handed Ready diagnostic schema.

This deliberately does not pretend to exercise Skyrim.  It prevents a future
native refactor from silently dropping the fields needed to correlate an input
edge with its queued action and native fallback.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ACTION_EVENTS = {
    "action_applied",
    "action_enqueued",
    "flight_action_drop",
    "input_action_queue",
    "input_deferred_drop",
    "input_deferred_enqueue",
    "input_deferred_replay",
    "input_policy",
    "input_received",
    "flight_action_queue",
    "flight_action_result",
    "input_release_recovery",
    "input_suppressed",
    "shout_edge",
}
AGGREGATE_EVENTS = {
    "input_action_suppressed_summary",
    "input_deferred",
    "input_deferred_flush",
    "input_latches_reset",
    "input_lifecycle_reset",
    "input_runtime_boundary",
    "input_state_refresh",
}
NATIVE_EVENTS = {"native_call_begin", "native_call_return", "native_call_postcheck"}
REQUIRED_ACTION_KEYS = {
    "schema",
    "action_id",
    "session",
    "input_sequence_domain",
    "manager_sequence",
    "action",
    "outcome",
    "reason",
}
REQUIRED_AGGREGATE_KEYS = {
    "schema",
    "correlation",
    "first_action_id",
    "last_action_id",
    "first_session",
    "last_session",
    "first_input_sequence_domain",
    "last_input_sequence_domain",
    "manager_sequence",
}


def cpp_string_literals(text: str) -> list[str]:
    """Return each adjacent ordinary C++ string-literal run as one format string."""
    token = re.compile(r'"(?:\\.|[^"\\])*"')
    literals = list(token.finditer(text))
    runs: list[str] = []
    index = 0
    while index < len(literals):
        current = literals[index]
        value = bytes(current.group()[1:-1], "utf-8").decode("unicode_escape")
        end = current.end()
        index += 1
        while index < len(literals) and text[end:literals[index].start()].strip() == "":
            current = literals[index]
            value += bytes(current.group()[1:-1], "utf-8").decode("unicode_escape")
            end = current.end()
            index += 1
        runs.append(value)
    return runs


def named_event_formats(source: Path, names: set[str]) -> list[tuple[str, str]]:
    formats: list[tuple[str, str]] = []
    for value in cpp_string_literals(source.read_text(encoding="utf-8")):
        match = re.search(r"(?:^|\s)event=([a-z0-9_]+)(?=\s|$)", value)
        if match and match.group(1) in names:
            formats.append((match.group(1), value))
    return formats


def format_keys(value: str) -> set[str]:
    return {match.group(1) for match in re.finditer(r"(?:^|\s)([a-z_]+)=", value)}


def test_every_action_scoped_structured_record_has_complete_correlation_envelope() -> None:
    failures: list[str] = []
    for source in (ROOT / "src/InputHandler.cpp", ROOT / "src/FlightManager.cpp"):
        for event, value in named_event_formats(source, ACTION_EVENTS):
            keys = format_keys(value)
            missing = REQUIRED_ACTION_KEYS - keys
            if missing:
                failures.append(
                    f"{source.relative_to(ROOT)}:{event}: missing {sorted(missing)} in {value!r}"
                )
    assert not failures, "\\n".join(failures)


def test_aggregate_records_identify_their_full_correlation_range() -> None:
    failures: list[str] = []
    for source in (ROOT / "src/InputHandler.cpp", ROOT / "src/FlightManager.cpp"):
        for event, value in named_event_formats(source, AGGREGATE_EVENTS):
            missing = REQUIRED_AGGREGATE_KEYS - format_keys(value)
            if missing:
                failures.append(
                    f"{source.relative_to(ROOT)}:{event}: missing {sorted(missing)} in {value!r}"
                )
    assert not failures, "\\n".join(failures)


def test_native_draw_calls_preserve_the_transition_identity() -> None:
    manager = ROOT / "src/FlightManager.cpp"
    required = {"schema", "action_id", "session", "sequence", "ready_generation", "equipment_epoch"}
    failures: list[str] = []
    for event, value in named_event_formats(manager, NATIVE_EVENTS):
        missing = required - format_keys(value)
        if missing:
            failures.append(f"{event}: missing {sorted(missing)} in {value!r}")
    assert not failures, "\\n".join(failures)


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing {needle!r} in {source.relative_to(ROOT)}"


def test_structured_ready_diagnostics_preserve_action_correlation_contract() -> None:
    helpers = (ROOT / "include/DragonAspectFlight/FlightStateHelpers.h").read_text(encoding="utf-8")
    input_cpp = (ROOT / "src/InputHandler.cpp").read_text(encoding="utf-8")
    manager_cpp = (ROOT / "src/FlightManager.cpp").read_text(encoding="utf-8")
    main_cpp = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

    require(helpers, "StructuredDiagnosticSchemaVersion", ROOT / "include/DragonAspectFlight/FlightStateHelpers.h")
    require(helpers, "std::uint64_t actionId", ROOT / "include/DragonAspectFlight/FlightStateHelpers.h")
    require(input_cpp, "a_action.actionId", ROOT / "src/InputHandler.cpp")
    require(input_cpp, "event=input_received", ROOT / "src/InputHandler.cpp")
    require(input_cpp, "event=action_enqueued", ROOT / "src/InputHandler.cpp")
    require(manager_cpp, "event=action_applied", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "event=native_call_begin", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "event=native_call_return", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "event=native_call_postcheck", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "event=state_checkpoint", ROOT / "src/FlightManager.cpp")
    require(main_cpp, "event=diagnostic_schema", ROOT / "src/main.cpp")


def test_input_boundary_uses_one_immutable_correlation_id_for_diagnostic_and_actions() -> None:
    helpers = (ROOT / "include/DragonAspectFlight/FlightStateHelpers.h").read_text(encoding="utf-8")
    input_h = (ROOT / "include/DragonAspectFlight/InputHandler.h").read_text(encoding="utf-8")
    input_cpp = (ROOT / "src/InputHandler.cpp").read_text(encoding="utf-8")

    require(helpers, "std::uint64_t inputSequenceDomain", ROOT / "include/DragonAspectFlight/FlightStateHelpers.h")
    require(input_h, "std::uint64_t a_actionId", ROOT / "include/DragonAspectFlight/InputHandler.h")
    require(input_h, "std::uint64_t actionId{ 0 };", ROOT / "include/DragonAspectFlight/InputHandler.h")
    require(input_cpp, "AllocateInputCorrelationId", ROOT / "src/InputHandler.cpp")
    require(input_cpp, "a_action.actionId = _activeInputCorrelationId", ROOT / "src/InputHandler.cpp")
    require(input_cpp, "snapshot.actionId = a_actionId", ROOT / "src/InputHandler.cpp")
    require(input_cpp, "item.actionId", ROOT / "src/InputHandler.cpp")


def test_terminal_action_records_carry_action_session_and_both_sequence_domains() -> None:
    manager_cpp = (ROOT / "src/FlightManager.cpp").read_text(encoding="utf-8")

    for event in ("event=flight_action_queue", "event=flight_action_drop"):
        require(manager_cpp, f"{event} action_id=", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "event=action_applied schema={} action_id={}", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "_applyingActionId", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "input_sequence_domain={}", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "manager_sequence={}", ROOT / "src/FlightManager.cpp")
    require(manager_cpp, "_applyingInputSequenceDomain", ROOT / "src/FlightManager.cpp")


def test_checkpoint_is_emitted_only_after_snapshot_admission() -> None:
    manager_cpp = (ROOT / "src/FlightManager.cpp").read_text(encoding="utf-8")
    checkpoint = manager_cpp.index('"event=state_checkpoint')
    throttle_return = manager_cpp.index("if (!equipmentChanged && !stateChanged && !heartbeatDue)")
    assert checkpoint > throttle_return, "state_checkpoint must be emitted after snapshot throttle admission"


def test_startup_identity_is_a_compiled_candidate_not_a_claimed_runtime_hash() -> None:
    main_cpp = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    version_h = (ROOT / "include/DragonAspectFlight/Version.h").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    require(main_cpp, "compiled_candidate_identity", ROOT / "src/main.cpp")
    require(main_cpp, "__DATE__", ROOT / "src/main.cpp")
    require(main_cpp, "__TIME__", ROOT / "src/main.cpp")
    require(main_cpp, "dll_hash=not_computed", ROOT / "src/main.cpp")
    for needle in ("source_revision", "data_manifest_sha256", "commonlib_version", "commonlib_revision"):
        require(main_cpp, needle, ROOT / "src/main.cpp")
    for needle in (
        "DAF_SOURCE_REVISION",
        "DAF_DATA_MANIFEST_SHA256",
        "DAF_COMMONLIB_VERSION",
        "DAF_COMMONLIB_REVISION",
    ):
        require(cmake, needle, ROOT / "CMakeLists.txt")
        require(version_h, needle, ROOT / "include/DragonAspectFlight/Version.h")
    for needle in ("SourceRevision", "DataManifestSha256", "CommonLibVersion", "CommonLibRevision"):
        require(version_h, needle, ROOT / "include/DragonAspectFlight/Version.h")


def test_scoped_context_and_deferred_replay_preserve_correlation_contract() -> None:
    input_cpp = (ROOT / "src/InputHandler.cpp").read_text(encoding="utf-8")
    manager_cpp = (ROOT / "src/FlightManager.cpp").read_text(encoding="utf-8")

    for needle in ("ScopedInputCorrelation", "ScopedDeferredCorrelation", "item.actionId", "snapshot.inputSequenceDomain"):
        require(input_cpp, needle, ROOT / "src/InputHandler.cpp")
    for needle in ("ScopedApplyingCorrelation", "std::unique_lock lock(mutex)", "_applyingInputSequenceDomain", "event=input_deferred_replay schema={} action_id={}"):
        require(input_cpp if "deferred" in needle else manager_cpp, needle,
                ROOT / ("src/InputHandler.cpp" if "deferred" in needle else "src/FlightManager.cpp"))
