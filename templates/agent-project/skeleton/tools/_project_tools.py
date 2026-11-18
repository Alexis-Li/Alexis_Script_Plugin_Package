"""Shared standard-library helpers for the project's maintenance commands."""

from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ABSOLUTE_PATH_RE = re.compile(
    r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/]|(?<![A-Za-z0-9_.-])/(?:Users|home|private/tmp|var/folders)/"
)


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
