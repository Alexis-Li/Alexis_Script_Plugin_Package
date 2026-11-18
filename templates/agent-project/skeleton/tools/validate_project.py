"""Validate the project's required files, text encoding, links and generated output.

Run `python tools/validate_project.py [--json]` from anywhere; the repository root
is the parent of this script's directory. Exit codes: 0 success, 1 problems found,
2 usage error. Each check below is a constant or a function named in CHECKS, so a
project can add or remove checks in one place.
"""

from __future__ import annotations

import argparse
import functools
import json
import os
import re
import subprocess
from pathlib import Path
from urllib.parse import unquote

from _project_tools import ABSOLUTE_PATH_RE, ROOT, configure_output, emit

# --- Check inputs: edit these when the project grows ------------------------------

REQUIRED_FILES = (
    "AGENTS.md",
    "CONTEXT.md",
    "README.md",
    "README_CN.md",
    "CHANGELOG.md",
    "docs/workflow.md",
    "docs/architecture.md",
    "docs/requirements.md",
    "docs/roadmap.md",
    "docs/adr/README.md",
    ".gitignore",
    "tools/issue_report.py",
)
TEXT_SUFFIXES = {
    ".md", ".py", ".txt", ".json", ".yml", ".yaml", ".toml", ".ini", ".cfg",
    ".ps1", ".cmd", ".sh",
}
TEXT_NAMES = {".gitignore", ".gitattributes", ".editorconfig", "LICENSE", "NOTICE"}
# Directories never walked when the project is not a Git repository yet.
SKIPPED_DIRS = {".git", ".venv", ".tmp", "__pycache__", ".pytest_cache", ".ruff_cache", ".mypy_cache"}
# Template leftovers: built by concatenation so this file does not report itself.
CUSTOMIZE_MARKER = "待定" + "制："
NAME_PLACEHOLDER = "{{" + "PROJECT_NAME" + "}}"
RESIDUE_HINT = "fill it in or delete it during first setup"
# Generated output that must never be visible to Git (tracked or unignored).
FORBIDDEN_DIRS_ANYWHERE = {"__pycache__", ".tmp"}
FORBIDDEN_DIRS_AT_ROOT = {"samples", "reviews", "releases"}
FORBIDDEN_NAMES = {"HANDOFF.md"}
FORBIDDEN_SUFFIXES = (".pyc", ".zip", ".7z")

FENCE_RE = re.compile(r"^ {0,3}(`{3,}|~{3,})(.*)$")
HTML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
INLINE_CODE_RE = re.compile(r"(?<!`)(`+)(?!`)(?:(?!\n[ \t]*\n).)+?(?<!`)\1(?!`)", re.DOTALL)
ESCAPE_RE = re.compile(r"\\([!-/:-@\[-`{-~])")
REFERENCE_LINK_RE = re.compile(r"^ {0,3}(?P<label>\[)[^\]\n]+\]:[ \t]*(?P<dest><[^>\n]+>|\S+)", re.MULTILINE)
TITLE_CLOSERS = {'"': '"', "'": "'", "(": ")"}
SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


# --- File discovery ----------------------------------------------------------------


def _has_nested_checkout(root: Path, path: Path) -> bool:
    current = path.parent
    while current != root and root in current.parents:
        if (current / ".git").exists():
            return True
        current = current.parent
    return False


def _git_paths(root: Path, errors: list[str]) -> list[str]:
    command = [
        "git", "-c", "safe.directory={0}".format(root.as_posix()), "-C", str(root),
        "ls-files", "--cached", "--others", "--exclude-standard", "-z",
    ]
    try:
        completed = subprocess.run(command, check=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        errors.append("cannot list Git files: {0}".format(exc))
        return []
    names = completed.stdout.decode("utf-8", errors="surrogateescape").split("\0")
    return sorted({name for name in names if name})


def _walked_paths(root: Path) -> list[str]:
    found = []
    for current, directories, files in os.walk(root):
        base = Path(current)
        directories[:] = [
            name for name in directories
            if name not in SKIPPED_DIRS and not (base / name / ".git").exists()
        ]
        found.extend((base / name).relative_to(root).as_posix() for name in files)
    return sorted(found)


def project_files(root: Path, errors: list[str]) -> tuple[list[str], bool]:
    """Return project-relative file paths and whether the root is a Git repository."""
    in_git = (root / ".git").exists()
    names = _git_paths(root, errors) if in_git else _walked_paths(root)
    return names, in_git


def _is_text(relative: str) -> bool:
    path = Path(relative)
    return path.suffix.lower() in TEXT_SUFFIXES or path.name in TEXT_NAMES


# --- Checks ------------------------------------------------------------------------


def check_required_files(root: Path, names: list[str], texts: dict, errors: list[str]) -> None:
    for relative in REQUIRED_FILES:
        if not (root / relative).is_file():
            errors.append("{0}: missing required file".format(relative))


def check_absolute_paths(root: Path, names: list[str], texts: dict, errors: list[str]) -> None:
    for relative, text in texts.items():
        for number, line in enumerate(text.splitlines(), 1):
            if ABSOLUTE_PATH_RE.search(line):
                errors.append(
                    "{0}:{1}: machine-specific absolute path; use a relative path "
                    "or a placeholder such as <scratch>".format(relative, number)
                )


def check_template_residue(root: Path, names: list[str], texts: dict, errors: list[str]) -> None:
    for relative, text in texts.items():
        for number, line in enumerate(text.splitlines(), 1):
            for marker in (CUSTOMIZE_MARKER, NAME_PLACEHOLDER):
                if marker in line:
                    errors.append(
                        "{0}:{1}: template marker '{2}' remains; {3}".format(
                            relative, number, marker, RESIDUE_HINT
                        )
                    )


def check_json_files(root: Path, names: list[str], texts: dict, errors: list[str]) -> None:
    for relative, text in texts.items():
        if relative.lower().endswith(".json"):
            try:
                json.loads(text)
            except json.JSONDecodeError as exc:
                errors.append("{0}:{1}: invalid JSON: {2}".format(relative, exc.lineno, exc.msg))


def _blank(match: re.Match) -> str:
    return re.sub(r"[^\n]", " ", match.group(0))


def _visible_markdown(text: str) -> str:
    """Blank fenced code, HTML comments and inline code, keeping line numbers."""
    lines = []
    fence = None
    for line in text.split("\n"):
        match = FENCE_RE.match(line)
        if fence:
            if match and match[1][0] == fence[0] and len(match[1]) >= len(fence):
                if not match[2].strip():
                    fence = None
            lines.append("")
        elif match:
            fence = match[1]
            lines.append("")
        else:
            lines.append(line)
    visible = HTML_COMMENT_RE.sub(_blank, "\n".join(lines))
    return INLINE_CODE_RE.sub(_blank, visible)


def _skip_space(text: str, index: int, end: int) -> int:
    while index < end and text[index] in " \t\n":
        index += 1
    return index


@functools.lru_cache(maxsize=4096)
def _closing_bracket(text: str, start: int, end: int) -> int | None:
    """Return the index of the `]` matching the `[` at start.

    A `[` inside the label that starts a complete inline link is skipped as a whole, so
    brackets in that link's destination or title do not change the pairing.
    """
    depth = 0
    index = start
    while index < end:
        char = text[index]
        if char == "\\":
            index += 2
            continue
        if char == "[":
            if depth:
                inner = _closing_bracket(text, index, end)
                if inner is not None and inner + 1 < end and text[inner + 1] == "(":
                    parsed = _inline_link_end(text, inner + 1, end)
                    if parsed is not None:
                        index = parsed[1]
                        continue
            depth += 1
        elif char == "]":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    return None


def _destination(text: str, index: int, end: int) -> tuple[str, int] | None:
    """Parse `<angle form>` or a plain destination with balanced parentheses."""
    if index < end and text[index] == "<":
        begin = index + 1
        index = begin
        while index < end and text[index] not in "<>\n":
            index += 2 if text[index] == "\\" else 1
        if index < end and text[index] == ">":
            return text[begin:index], index + 1
        return None
    begin = index
    depth = 0
    while index < end and not text[index].isspace():
        char = text[index]
        if char == "\\":
            index += 2
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            if depth == 0:
                break
            depth -= 1
        index += 1
    return (text[begin:index], index) if depth == 0 else None


def _title_end(text: str, index: int, end: int) -> int | None:
    """Return the index past a `"..."`, `'...'` or `(...)` title starting at index."""
    closer = TITLE_CLOSERS[text[index]]
    index += 1
    while index < end:
        char = text[index]
        if char == "\\":
            index += 2
            continue
        if char == closer:
            return index + 1
        if closer == ")" and char == "(":
            return None
        index += 1
    return None


def _inline_link_end(text: str, index: int, end: int) -> tuple[str, int] | None:
    """Parse `(destination "title")` starting at the `(`; return the raw target and index past `)`."""
    index = _skip_space(text, index + 1, end)
    parsed = _destination(text, index, end)
    if parsed is None:
        return None
    target, after = parsed
    index = _skip_space(text, after, end)
    if index > after and index < end and text[index] in TITLE_CLOSERS:
        title_end = _title_end(text, index, end)
        if title_end is None:
            return None
        index = _skip_space(text, title_end, end)
    if index < end and text[index] == ")":
        return target, index + 1
    return None


def _scan_inline_links(text: str, start: int, end: int, skip: dict, found: list) -> None:
    """Pair each unescaped `[` with its `]`; a `(destination)` right after it makes a link.

    A link's label is scanned too, so an image inside a link is still found. A `[` that
    starts no link is plain text and scanning resumes just after it.
    """
    index = start
    while index < end:
        char = text[index]
        if char == "\\":
            index += 2
            continue
        if char != "[":
            index += 1
            continue
        if index in skip:
            index = skip[index]
            continue
        close = _closing_bracket(text, index, end)
        parsed = None
        if close is not None and close + 1 < end and text[close + 1] == "(":
            parsed = _inline_link_end(text, close + 1, end)
        if parsed is None:
            index += 1
            continue
        found.append((text.count("\n", 0, index) + 1, ESCAPE_RE.sub(r"\1", parsed[0])))
        _scan_inline_links(text, index + 1, close, skip, found)
        index = parsed[1]


def markdown_links(text: str) -> list[tuple[int, str]]:
    """Return (line, unescaped target) for inline links, images and reference definitions.

    Links never cross a paragraph boundary: a blank line of the source or a fenced
    code line. Each paragraph of the code-blanked text is scanned on its own.
    """
    source = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    visible = _visible_markdown("\n".join(source)).split("\n")
    found = []
    first = None
    for number, (line, shown) in enumerate(zip(source + [""], visible + [""])):
        if line.strip() and shown:
            if first is None:
                first = number
            continue
        if first is not None:
            _scan_paragraph("\n".join(visible[first:number]), first, found)
            first = None
    return sorted(found)


def _scan_paragraph(text: str, offset: int, found: list) -> None:
    """Collect the links of one paragraph, numbering lines from offset + 1."""
    links = []
    definitions = {}
    for match in REFERENCE_LINK_RE.finditer(text):
        definitions[match.start("label")] = match.end()
        links.append((text.count("\n", 0, match.start()) + 1, ESCAPE_RE.sub(r"\1", match["dest"].strip("<>"))))
    _scan_inline_links(text, 0, len(text), definitions, links)
    found.extend((line + offset, target) for line, target in links)


def check_markdown_links(root: Path, names: list[str], texts: dict, errors: list[str]) -> None:
    for relative, text in texts.items():
        if not relative.lower().endswith(".md"):
            continue
        for number, target in markdown_links(text):
            if not target or target.startswith("#") or SCHEME_RE.match(target):
                continue
            local = unquote(re.split(r"[#?]", target, maxsplit=1)[0])
            if not local:
                continue
            base = root if local.startswith("/") else (root / relative).parent
            destination = (base / local.lstrip("/")).resolve()
            if not destination.is_relative_to(root.resolve()):
                errors.append("{0}:{1}: link leaves the repository: {2}".format(relative, number, target))
            elif not destination.exists():
                errors.append("{0}:{1}: broken link: {2}".format(relative, number, target))


def check_generated_output(root: Path, names: list[str], texts: dict, errors: list[str]) -> None:
    """Only meaningful in a Git repository; see validate()."""
    for relative in names:
        parts = Path(relative).parts
        lowered = relative.lower()
        if (
            any(part in FORBIDDEN_DIRS_ANYWHERE for part in parts[:-1])
            or (parts[0] in FORBIDDEN_DIRS_AT_ROOT and len(parts) > 1)
            or parts[-1] in FORBIDDEN_NAMES
            or lowered.endswith(FORBIDDEN_SUFFIXES)
        ):
            errors.append("{0}: generated or private file visible to Git; ignore or remove it".format(relative))


# Checks that read decoded text and need no Git; check_generated_output is added in Git mode.
CHECKS = (
    check_required_files,
    check_absolute_paths,
    check_template_residue,
    check_json_files,
    check_markdown_links,
)
GIT_ONLY_CHECKS = (check_generated_output,)


def _read_texts(root: Path, names: list[str], errors: list[str]) -> dict[str, str]:
    texts = {}
    for relative in names:
        path = root / relative
        if not _is_text(relative) or not path.is_file() or _has_nested_checkout(root, path):
            continue
        try:
            texts[relative] = path.read_bytes().decode("utf-8-sig")
        except UnicodeError:
            errors.append("{0}: not valid UTF-8".format(relative))
        except OSError as exc:
            errors.append("{0}: cannot read file: {1}".format(relative, exc))
    return texts


def validate(root: Path = ROOT) -> dict:
    root = root.resolve()
    errors: list[str] = []
    names, in_git = project_files(root, errors)
    names = [name for name in names if not _has_nested_checkout(root, root / name)]
    texts = _read_texts(root, names, errors)
    checks = CHECKS + (GIT_ONLY_CHECKS if in_git else ())
    for check in checks:
        check(root, names, texts, errors)
    return {"ok": not errors, "errors": errors}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", action="store_true", help="Emit machine-readable output.")
    args = parser.parse_args(argv)
    configure_output()
    result = validate(ROOT)
    if args.json:
        emit(result, True)
    elif result["ok"]:
        print("Project files: OK")
    else:
        for error in result["errors"]:
            print(error)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
