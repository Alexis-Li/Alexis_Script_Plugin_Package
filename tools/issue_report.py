"""Generate and check issue status comments for tidying or closure; no network writes."""

from __future__ import annotations

import argparse
import re
from datetime import date
from pathlib import Path

from _repo_tools import emit

STATUSES = ("进行中", "待审核", "待验证", "受阻", "可关闭", "已关闭")
FIELDS = ("状态", "基线", "更新日期")
SECTIONS = ("简介", "相关提交", "已完成", "未完成", "开发边界", "决定", "宿主验收", "下一步")
MARKER_RE = re.compile(r"<!-- issue-status:v1 issue=([1-9][0-9]*) -->")
PLACEHOLDER_RE = re.compile(r"\{\{.*?\}\}|^[ \t]*(?:TODO|TBD)[ \t]*$", re.MULTILINE)
ABSOLUTE_PATH_RE = re.compile(
    r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/]|(?<![A-Za-z0-9_.-])/(?:Users|home|private/tmp|var/folders)/"
)


def template(issue: int) -> str:
    """Return an intentionally incomplete draft that cannot pass validation."""
    if type(issue) is not int or issue <= 0:
        raise ValueError("issue must be a positive integer")
    values = (
        "{{选择：" + " / ".join(STATUSES) + "}}",
        "{{完整实现、最终审核及集成 SHA 或包版本，说明可访问性与尚未完成的阶段}}",
        "{{YYYY-MM-DD，使用任务所在时区}}",
    )
    guidance = (
        "{{工单目标及当前结论}}",
        "{{PR、完整提交 SHA 及各自用途；最终评审链接}}",
        "{{按 AC 或需求编号列出成果和证据链接，区分本轮执行与历史证据}}",
        "{{未解决问题、验收缺口及完成条件；无则写无}}",
        "{{范围、排除项、转交其他工单的事项及证据限制}}",
        "{{当前生效的决定及其维护文档或需求链接；无则写无}}",
        "{{宿主版本、基线、输入、复现步骤、测量与结果；不适用则写无}}",
        "{{具体下一步及负责人；完成后写无}}",
    )
    lines = [
        f"<!-- issue-status:v1 issue={issue} -->",
        "## 状态",
        "",
        *(f"- {field}：{value}" for field, value in zip(FIELDS, values)),
        "",
    ]
    for section, instruction in zip(SECTIONS, guidance):
        lines.extend((f"### {section}", "", instruction, ""))
    return "\n".join(lines)


def _outside_fences(body: str) -> str:
    """Ignore example code when locating the actual comment structure."""
    lines = []
    fence = None
    for line in body.splitlines():
        match = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line)
        if fence:
            if match and match[1][0] == fence[0] and len(match[1]) >= len(fence):
                if not match[2].strip():
                    fence = None
            continue
        if match:
            fence = match[1]
            continue
        lines.append(line)
    return "\n".join(lines)


def validate_status(body: str) -> dict:
    """Check structure and portable paths; the publisher verifies factual claims."""
    errors = []
    structure = _outside_fences(body)
    markers = MARKER_RE.findall(structure)
    first_line = body.splitlines()[0].strip() if body.splitlines() else ""
    if ABSOLUTE_PATH_RE.search(body):
        errors.append(
            "comment contains a machine-specific absolute path; "
            "use repository-relative paths or placeholders such as <Maya2024> or <scratch>"
        )
    if len(markers) != 1 or not MARKER_RE.fullmatch(first_line):
        errors.append("first line must be one valid issue-status:v1 marker")
        return {"ok": False, "errors": errors}
    structure = re.sub(r"<!--.*?-->", "", structure, flags=re.DOTALL)
    if PLACEHOLDER_RE.search(structure):
        errors.append("replace {{placeholders}} and standalone TODO/TBD draft lines")
    # Only header fields count; a quoted/example field cannot satisfy them.
    header = structure.split("\n### ", 1)[0]
    fields = {}
    for field in FIELDS:
        values = re.findall(r"^- " + re.escape(field) + r"：[ \t]*(.*)$", header, re.MULTILINE)
        if len(values) != 1 or not values[0].strip():
            errors.append(f"header requires exactly one non-empty '- {field}：' field")
        else:
            fields[field] = values[0].strip()
    if fields.get("状态") not in STATUSES:
        errors.append("状态 must be one of: " + ", ".join(STATUSES))
    updated = fields.get("更新日期", "")
    try:
        if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", updated):
            raise ValueError
        date.fromisoformat(updated)
    except ValueError:
        errors.append("更新日期 must be a valid YYYY-MM-DD date")
    headings = list(re.finditer(r"^(#{2,3}) (.+)$", structure, re.MULTILINE))
    if [match[2] for match in headings if match[1] == "##"] != ["状态"]:
        errors.append("comment must have one title '## 状态'")
    if not headings or headings[0][0] != "## 状态":
        errors.append("comment must start with heading '## 状态'")
    actual_sections = [match[2] for match in headings if match[1] == "###"]
    if actual_sections != list(SECTIONS):
        errors.append("required sections must occur once in order: " + ", ".join(SECTIONS))
    for index, heading in enumerate(headings):
        if heading[1] != "###" or heading[2] not in SECTIONS:
            continue
        end = headings[index + 1].start() if index + 1 < len(headings) else len(structure)
        if not structure[heading.end() : end].strip():
            errors.append(f"section '{heading[2]}' is empty; state 无 if not applicable")
    return {"ok": not errors, "issue": int(markers[0]), "errors": errors}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    scaffold = commands.add_parser("template", help="Write a draft; refuses to overwrite files.")
    scaffold.add_argument("--issue", required=True, type=int)
    scaffold.add_argument("--output", required=True, type=Path, help="New UTF-8 Markdown file.")
    check = commands.add_parser(
        "check", help="Check a local status comment without network access."
    )
    check.add_argument("path", type=Path)
    check.add_argument("--json", action="store_true", help="Emit machine-readable diagnostics.")
    args = parser.parse_args(argv)
    try:
        if args.command == "template":
            body = template(args.issue)
            with args.output.open("x", encoding="utf-8", newline="\n") as stream:
                stream.write(body)
            result = {"ok": True, "draft": str(args.output)}
        else:
            result = validate_status(args.path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, ValueError) as exc:
        result = {"ok": False, "errors": [str(exc)]}
    emit(result, getattr(args, "json", False))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
