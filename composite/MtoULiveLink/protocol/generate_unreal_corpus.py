"""Validate the MtoU conformance corpus and generate Unreal test-only data."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS_PATH = HERE / "conformance-v6.json"
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
OPERATIONS = {
    "framing", "init", "frame", "ready", "error",
    "cache_enter",
    "cache_begin", "cache_frame", "cache_end", "cache_play",
    "cache_stop", "cache_clear",
    "cache_ready", "cache_progress", "cache_complete",
    "cache_stopped", "cache_cleared",
}
HOSTS = {"maya", "unreal"}
LIMIT_KEYS = (
    "max_message_bytes",
    "max_cache_payload_bytes",
    "max_cache_frame_count",
)
PARSER_ERROR_CODES = {
    "INVALID_MESSAGE", "PROTOCOL_VERSION_MISMATCH",
    "CACHE_METADATA_INVALID", "CACHE_PAYLOAD_TOO_LARGE",
    "CACHE_FRAME_INDEX_INVALID", "CACHE_FRAME_CONTENTS_INVALID",
    "CACHE_NOT_READY", "CACHE_REVISION_MISMATCH", "CACHE_INVALID_STATE",
}


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
    if corpus.get("protocol_version") != 6:
        errors.append("protocol_version must equal 6")
    cases = corpus.get("cases")
    if not isinstance(cases, list) or not cases:
        return errors + ["cases must be a non-empty array"]
    limits = corpus.get("limits")
    if not isinstance(limits, dict):
        errors.append("limits must be an object")
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
        if case.get("operation") not in OPERATIONS:
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
        expected = case.get("expected")
        if not isinstance(expected, dict):
            errors.append(prefix + " expected must be an object")
            continue
        if not isinstance(expected.get("accepted"), bool):
            errors.append(prefix + " expected.accepted must be boolean")
        if not isinstance(expected.get("close"), bool):
            errors.append(prefix + " expected.close must be boolean")
        if not expected.get("accepted") and expected.get("error_code") not in PARSER_ERROR_CODES:
            errors.append(prefix + " rejected case needs a stable parser error_code")
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
