"""Shared standard-library helpers for repository management commands."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:[-+][0-9A-Za-z.-]+)?$")
KEBAB_RE = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")


def emit(payload: dict, as_json: bool) -> None:
    if as_json:
        print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))
        return
    for key, value in payload.items():
        if isinstance(value, list):
            print("{0}:".format(key))
            for item in value:
                print("  - {0}".format(item))
        else:
            print("{0}: {1}".format(key, value))


def require_kebab_name(value: str) -> str:
    if not KEBAB_RE.fullmatch(value):
        raise ValueError("name must use lowercase kebab-case")
    return value


def require_semver(value: str) -> str:
    if not SEMVER_RE.fullmatch(value):
        raise ValueError("version must be Semantic Versioning, for example 1.2.3")
    return value


def pascal_case(kebab_name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in kebab_name.split("-"))


def snake_case(kebab_name: str) -> str:
    return kebab_name.replace("-", "_")


def relative(path: Path) -> str:
    return path.resolve().relative_to(ROOT).as_posix()
