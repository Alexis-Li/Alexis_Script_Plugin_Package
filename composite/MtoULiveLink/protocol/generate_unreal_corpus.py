"""Validate the MtoU conformance corpus and generate Unreal test-only data."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS_PATH = HERE / "conformance-v11.json"
OUTPUT_PATH = (
    HERE.parent
    / "unreal"
    / "MtoULiveLink"
    / "Source"
    / "MtoULiveLink"
    / "Private"
    / "Tests"
    / "MtoUConformanceCorpus.inl"
)
# Every wire operation the frozen v11 contract defines: the v9 message set plus
# the linkage pairing, the exclusive time-control state, the complete pose
# group, the application acknowledgement, the scene-reference handoff and the
# removal of the second object of a set.
OPERATIONS = {
    "framing", "init", "frame", "ready", "error",
    "cache_enter",
    "cache_begin", "cache_frame", "cache_end", "cache_play",
    "cache_stop", "cache_clear",
    "cache_ready", "cache_playing", "cache_progress", "cache_complete",
    "cache_paused", "cache_resumed", "cache_seeked", "cache_looped",
    "cache_loop_changed",
    "cache_stopped", "cache_cleared",
    "hello", "hello_ack",
    "mode", "mode_state", "leave_linkage",
    "camera_time",
    "group",
    "applied",
    "reference_offer", "reference_result", "reference_removed",
    "reference_query",
    "remove_object", "object_removed",
    "resource_limit",
}
HOSTS = {"maya", "unreal"}
# The frozen v11 budgets. A resource case names one of these keys and compares
# its measured size against the value the corpus publishes, so an oversized
# case never carries a multi-megabyte payload of its own.
LIMIT_KEYS = (
    "max_message_bytes",
    "max_linkage_group_bytes",
    "max_control_queue",
    "max_reference_bytes",
    "max_reference_objects",
    "max_reference_triangles",
    "max_cache_payload_bytes",
    "max_cache_frame_count",
    "max_set_objects",
)
# Stable rejection codes from the v11 contract; the message text is diagnostic
# only, so a rejected corpus case must name one of these.
ERROR_CODES = {
    "PROTOCOL_VERSION_MISMATCH", "INVALID_MESSAGE",
    "PAIRING_REQUIRED", "TARGET_UNKNOWN", "TARGET_ALREADY_PAIRED",
    "WRITER_NOT_ISOLATABLE", "SHARED_WRITER", "MOVING_SOCKET",
    "ANCHOR_MISMATCH", "OBJECT_SET_UNSUPPORTED",
    "MODE_CONFLICT", "CACHE_NOT_CLEARED",
    "SET_REVISION_STALE", "SPACE_REVISION_STALE", "EVAL_STALE",
    "RESOURCE_LIMIT",
    "REFERENCE_STALE", "REFERENCE_DIGEST_MISMATCH", "REFERENCE_IMAGE_DATA",
    "REFERENCE_OVER_BUDGET", "CONTAINER_NOT_OWNED", "RECOVERY_REFUSED",
    "LINKAGE_NOT_ACTIVE",
    "CACHE_METADATA_INVALID", "CACHE_PAYLOAD_TOO_LARGE",
    "CACHE_FRAME_INDEX_INVALID", "CACHE_FRAME_CONTENTS_INVALID",
    "CACHE_NOT_READY", "CACHE_REVISION_MISMATCH", "CACHE_INVALID_STATE",
}
# Message types a linkage case may declare the pre-state of. Each one carries
# the identity a v11 adapter must match before it answers.
LINKAGE_OPERATIONS = {
    "camera_time", "applied", "mode_state", "reference_offer", "leave_linkage",
    "remove_object",
}
# Stable object ids of a set: the primary character and the second object.
OBJECT_IDS = {"character", "secondary"}


def load_corpus(path: Path = CORPUS_PATH) -> dict:
    try:
        corpus = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("cannot read conformance corpus: {0}".format(exc)) from exc
    errors = validate_corpus(corpus)
    if errors:
        raise ValueError("invalid conformance corpus:\n" + "\n".join(errors))
    return corpus


def validate_corpus(corpus: object) -> list[str]:
    errors = []
    if not isinstance(corpus, dict):
        return ["corpus must be a JSON object"]
    if corpus.get("schema_version") != 1:
        errors.append("schema_version must equal 1")
    if corpus.get("protocol_version") != 11:
        errors.append("protocol_version must equal 11")
    cases = corpus.get("cases")
    if not isinstance(cases, list) or not cases:
        return errors + ["cases must be a non-empty array"]
    limits = corpus.get("limits")
    if not isinstance(limits, dict):
        errors.append("limits must be an object")
        limits = {}
    else:
        for key in LIMIT_KEYS:
            value = limits.get(key)
            if isinstance(value, bool) or not isinstance(value, int) or value < 1:
                errors.append("limits." + key + " must be a positive integer")
    identifiers = set()
    for index, case in enumerate(cases):
        prefix = "case {0}".format(index)
        if not isinstance(case, dict):
            errors.append(prefix + " must be an object")
            continue
        identifier = case.get("id")
        if not isinstance(identifier, str) or not identifier:
            errors.append(prefix + " id must be a non-empty string")
        elif identifier in identifiers:
            errors.append(prefix + " id is duplicated: " + identifier)
        else:
            identifiers.add(identifier)
            prefix = "case " + identifier
        operation = case.get("operation")
        if operation not in OPERATIONS:
            errors.append(prefix + " operation is unsupported")
        hosts = case.get("applies_to")
        if not isinstance(hosts, list) or not hosts or any(host not in HOSTS for host in hosts):
            errors.append(prefix + " applies_to must contain maya and/or unreal")
        sources = [name for name in ("payload", "payloads", "raw_utf8", "raw_hex") if name in case]
        if len(sources) != 1:
            errors.append(prefix + " must define exactly one input source")
        if "raw_hex" in case:
            try:
                bytes.fromhex(case["raw_hex"])
            except (TypeError, ValueError):
                errors.append(prefix + " raw_hex must contain complete hexadecimal bytes")
        if "raw_utf8" in case and not isinstance(case["raw_utf8"], str):
            errors.append(prefix + " raw_utf8 must be a string")
        expected = case.get("expected")
        if not isinstance(expected, dict):
            errors.append(prefix + " expected must be an object")
            continue
        if not isinstance(expected.get("accepted"), bool):
            errors.append(prefix + " expected.accepted must be boolean")
        if not isinstance(expected.get("close"), bool):
            errors.append(prefix + " expected.close must be boolean")
        if not expected.get("accepted") and expected.get("error_code") not in ERROR_CODES:
            errors.append(prefix + " rejected case needs a stable v11 error_code")
        keywords = expected.get("keywords", [])
        if not isinstance(keywords, list) or any(not isinstance(value, str) for value in keywords):
            errors.append(prefix + " expected.keywords must be a string array")
        negotiated = case.get("negotiated_revision")
        if negotiated is not None and (
                isinstance(negotiated, bool) or not isinstance(negotiated, int)):
            errors.append(prefix + "negotiated_revision must be an integer")
        session = case.get("session")
        if session is not None and session not in {"fresh", "uploaded"}:
            errors.append(prefix + " session must be 'fresh' or 'uploaded'")
        if "negotiated_set" in case:
            errors.extend(validate_negotiated_set(prefix, case["negotiated_set"]))
        if operation in LINKAGE_OPERATIONS:
            errors.extend(validate_linkage_state(prefix, case))
        if operation == "resource_limit":
            errors.extend(validate_resource_case(prefix, case, limits))
    return errors


def validate_linkage_state(prefix: str, case: dict) -> list[str]:
    """A linkage case runs against a declared adapter state, so the identity it
    must match is part of the case instead of hidden in a test fixture."""
    errors = []
    state = case.get("linkage")
    if not isinstance(state, dict):
        return [prefix + " linkage case must declare the linkage state it runs against"]
    for name in ("session_id", "mode_revision", "set_revision", "space_revision"):
        value = state.get(name)
        if isinstance(value, bool) or not isinstance(value, int) or value < 1:
            errors.append(
                prefix + " linkage." + name + " must be a positive integer")
    if not isinstance(state.get("active"), bool):
        errors.append(prefix + " linkage.active must be boolean")
    return errors


def validate_negotiated_set(prefix: str, value: object) -> list[str]:
    """The object set a case runs against: the primary first, no repeats."""
    if (not isinstance(value, list) or not value
            or any(item not in OBJECT_IDS for item in value)):
        return [prefix + " negotiated_set must list known object ids"]
    if value[0] != "character" or len(set(value)) != len(value):
        return [prefix + " negotiated_set must start with character and repeat no id"]
    return []


def validate_resource_case(prefix: str, case: dict, limits: dict) -> list[str]:
    """An oversized case names its budget and its measured size structurally;
    the published limit and the measured value must agree with each other."""
    errors = []
    payload = case.get("payload")
    if not isinstance(payload, dict):
        return [prefix + " resource case must carry its budget as a payload"]
    budget = payload.get("budget")
    if budget not in LIMIT_KEYS:
        return [prefix + " resource case names an unknown budget: {0!r}".format(budget)]
    if payload.get("limit") != limits.get(budget):
        errors.append(
            prefix + " resource limit must equal limits." + budget)
    measured = payload.get("measured")
    limit = payload.get("limit")
    if isinstance(measured, bool) or not isinstance(measured, int) or measured < 1:
        errors.append(prefix + " resource case must measure a positive integer")
    elif measured <= limit:
        errors.append(
            prefix + " oversized case must measure more than limits." + budget)
    return errors


def generated_text(corpus_path: Path = CORPUS_PATH) -> str:
    corpus = load_corpus(corpus_path)
    canonical = json.dumps(corpus, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    rows = []
    for offset in range(0, len(canonical), 16):
        rows.append("    " + ", ".join(str(value) for value in canonical[offset:offset + 16]) + ",")
    return (
        "// Generated by protocol/generate_unreal_corpus.py; do not edit.\n"
        "static const uint8 GMtoUConformanceCorpus[] = {\n"
        + "\n".join(rows)
        + "\n};\n"
    )


def check_generated(
    corpus_path: Path = CORPUS_PATH, output_path: Path = OUTPUT_PATH
) -> list[str]:
    expected = generated_text(corpus_path)
    try:
        actual = output_path.read_text(encoding="utf-8")
    except OSError:
        return ["missing generated Unreal conformance data: {0}".format(output_path)]
    if actual != expected:
        return ["stale generated Unreal conformance data: {0}".format(output_path)]
    return []


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Fail instead of writing stale data.")
    args = parser.parse_args(argv)
    if args.check:
        errors = check_generated()
        for error in errors:
            print(error)
        return 1 if errors else 0
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text(generated_text(), encoding="utf-8", newline="\n")
    print(OUTPUT_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
