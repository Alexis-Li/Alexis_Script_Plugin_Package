"""Scaffold and validate GitHub issue reports locally; never write to GitHub."""

from __future__ import annotations

import argparse
import json
import re
from datetime import date, datetime
from pathlib import Path
from urllib.parse import urlparse

from _repo_tools import emit

REPORTS = {
    "development": (
        "开发交付",
        ("待审核", "部分完成", "受阻"),
        ("交付结果", "验证与证据边界", "剩余事项", "下一步"),
    ),
    "review": (
        "审核结论",
        ("通过", "需修改", "证据不足"),
        ("结论", "审核问题", "验证与证据边界", "后续处理"),
    ),
    "summary": (
        "当前结论",
        ("进行中", "待审核", "修复中", "待验证", "受阻", "可关闭", "已关闭"),
        ("未闭环事项", "已验证成果与边界", "验证依据", "接手入口", "记录索引"),
    ),
}
FIELDS = ("状态", "基线", "范围", "依据", "阻塞项", "本次验证", "更新日期")
VERIFICATION = ("本轮执行", "历史复核", "混合", "未执行")
KEY_RE = re.compile(r"[a-z][a-z0-9-]{0,63}")
MARKER_RE = re.compile(
    r"<!-- issue-report:v1 kind=(development|review|summary) "
    r"issue=([1-9][0-9]*) key=([a-z][a-z0-9-]{0,63}) -->"
)
PLACEHOLDER_RE = re.compile(r"\{\{.*?\}\}|^[ \t]*(?:TODO|TBD)[ \t]*$", re.MULTILINE)


def template(kind: str, issue: int, key: str) -> str:
    """Return an intentionally incomplete draft that cannot pass validation."""
    if kind not in REPORTS or issue <= 0 or not KEY_RE.fullmatch(key):
        raise ValueError("use a supported kind, a positive issue, and a lowercase report key")
    if (kind == "summary") != (key == "current"):
        raise ValueError("key 'current' is reserved for the one summary per issue")
    title, statuses, sections = REPORTS[kind]
    values = (
        "{{选择：" + " / ".join(statuses) + "}}",
        "{{完整提交 SHA 及可访问性；未提交工作须注明差异快照；整合分别写实现和审核基线}}",
        "{{本轮范围、验收项编号；整合写本票范围及保持开放或关闭的原因}}",
        "{{需求及交付/审核来源链接；整合含原报告或归档条目}}",
        "{{本报告范围内未闭环的 R-编号，以逗号分隔；没有则填无}}",
        "{{选择：" + " / ".join(VERIFICATION) + "}}",
        "{{YYYY-MM-DD，使用任务所在时区}}",
    )
    lines = [
        f"<!-- issue-report:v1 kind={kind} issue={issue} key={key} -->",
        f"## {title}",
        "",
        *(f"- {field}：{value}" for field, value in zip(FIELDS, values)),
        "",
    ]
    for section in sections:
        lines.extend((f"### {section}", "", "{{按工单报告协议填写；无事项时明确写无}}", ""))
    return "\n".join(lines)


def _outside_fences(body: str) -> str:
    """Ignore example code when locating the actual report structure."""
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


def validate_report(body: str) -> dict:
    errors = []
    structure = _outside_fences(body)
    markers = MARKER_RE.findall(structure)
    if len(markers) != 1 or not MARKER_RE.fullmatch(structure.splitlines()[0].strip()):
        return {"ok": False, "errors": ["first line must be one valid issue-report:v1 marker"]}
    kind, issue, key = markers[0]
    if (kind == "summary") != (key == "current"):
        errors.append("key 'current' is reserved for the one summary per issue")
    if PLACEHOLDER_RE.search(structure):
        errors.append("replace {{placeholders}} and standalone TODO/TBD draft lines")
    title, statuses, sections = REPORTS[kind]
    # Only header fields count; a quoted/example field in a later section cannot satisfy it.
    header = structure.split("\n### ", 1)[0]
    fields = {}
    for field in FIELDS:
        values = re.findall(r"^- " + re.escape(field) + r"：[ \t]*(.*)$", header, re.MULTILINE)
        if len(values) != 1 or not values[0].strip():
            errors.append(f"header requires exactly one non-empty '- {field}：' field")
        else:
            fields[field] = values[0].strip()
    if fields.get("状态") not in statuses:
        errors.append("状态 must be one of: " + ", ".join(statuses))
    if fields.get("本次验证") not in VERIFICATION:
        errors.append("本次验证 must be one of: " + ", ".join(VERIFICATION))
    updated = fields.get("更新日期", "")
    try:
        if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", updated):
            raise ValueError
        date.fromisoformat(updated)
    except ValueError:
        errors.append("更新日期 must be a valid YYYY-MM-DD date")
    blockers = fields.get("阻塞项", "")
    if blockers != "无":
        ids = re.split(r"[,，]\s*", blockers)
        if not all(re.fullmatch(r"R-[0-9]{3,}", item) for item in ids):
            errors.append("阻塞项 must be 无 or comma-separated R-001 style IDs")
        elif len(ids) != len(set(ids)):
            errors.append("阻塞项 contains duplicate IDs")
    if fields.get("状态") in {"通过", "可关闭", "已关闭"} and blockers != "无":
        errors.append("通过/可关闭/已关闭 cannot declare unresolved blockers in its scope")
    headings = list(re.finditer(r"^(#{2,3}) (.+)$", structure, re.MULTILINE))
    if not headings or headings[0][0] != f"## {title}":
        errors.append(f"report must start with heading '## {title}'")
    actual_sections = [match[2] for match in headings if match[1] == "###"]
    if [name for name in actual_sections if name in sections] != list(sections):
        errors.append("required sections must occur once in order: " + ", ".join(sections))
    for index, heading in enumerate(headings):
        if heading[1] != "###" or heading[2] not in sections:
            continue
        end = headings[index + 1].start() if index + 1 < len(headings) else len(structure)
        content = re.sub(r"<!--.*?-->", "", structure[heading.end() : end], flags=re.DOTALL)
        if not content.strip():
            errors.append(f"section '{heading[2]}' is empty; state 无 if not applicable")
    return {
        "ok": not errors,
        "kind": kind,
        "issue": int(issue),
        "key": key,
        "errors": errors,
    }


def validate_archive(archive: object) -> list[str]:
    """Check source preservation, not the truth of historical comment bodies."""
    if not isinstance(archive, dict):
        return ["archive must be an object"]
    errors = []
    if archive.get("schema") != "github-issue-comment-archive/1":
        errors.append("unsupported archive schema")
    issue = archive.get("issue", "")
    parsed = urlparse(issue if isinstance(issue, str) else "")
    if (
        parsed.scheme != "https"
        or parsed.netloc != "github.com"
        or not re.fullmatch(r"/[^/]+/[^/]+/issues/[1-9][0-9]*", parsed.path)
        or parsed.query
        or parsed.fragment
    ):
        errors.append("issue must be a canonical GitHub issue URL")
    for field in ("archived_on", "purpose", "current_status_comment"):
        if not isinstance(archive.get(field), str) or not archive[field].strip():
            errors.append(f"missing archive {field}")
    try:
        date.fromisoformat(archive.get("archived_on", ""))
    except (TypeError, ValueError):
        errors.append("archived_on must be an ISO date")
    current = archive.get("current_status_comment", "")
    if not isinstance(current, str) or not re.fullmatch(
        re.escape(str(issue)) + r"#issuecomment-[1-9][0-9]*", current
    ):
        errors.append("current_status_comment must refer to this issue")
    comments = archive.get("comments")
    if not isinstance(comments, list) or not comments:
        return errors + ["archive must preserve at least one source comment"]
    seen = set()
    for index, comment in enumerate(comments):
        prefix = f"comment {index + 1}: "
        if not isinstance(comment, dict):
            errors.append(prefix + "must be an object")
            continue
        comment_id = comment.get("id")
        if type(comment_id) is not int or comment_id <= 0:
            errors.append(prefix + "id must be a positive integer")
        elif comment_id in seen:
            errors.append(prefix + "duplicate id")
        else:
            seen.add(comment_id)
        for field in ("author", "body", "created_at", "updated_at", "original_url"):
            if not isinstance(comment.get(field), str) or not comment[field].strip():
                errors.append(prefix + f"missing {field}")
        for field in ("created_at", "updated_at"):
            try:
                timestamp = datetime.fromisoformat(comment.get(field, "").replace("Z", "+00:00"))
                if timestamp.utcoffset() is None:
                    raise ValueError
            except (AttributeError, TypeError, ValueError):
                errors.append(prefix + f"{field} must include a timezone")
        if comment.get("original_url") != f"{issue}#issuecomment-{comment_id}":
            errors.append(prefix + "original_url does not match issue and comment id")
    return errors


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    scaffold = commands.add_parser("template", help="Write a draft; refuses to overwrite files.")
    scaffold.add_argument("kind", choices=REPORTS)
    scaffold.add_argument("--issue", required=True, type=int)
    scaffold.add_argument(
        "--key", required=True, help="Stable round key; summary must use current."
    )
    scaffold.add_argument("--output", required=True, type=Path, help="New UTF-8 Markdown file.")
    for command in ("check", "check-archive"):
        check = commands.add_parser(command, help="Validate a local file without network access.")
        check.add_argument("path", type=Path)
        check.add_argument("--json", action="store_true", help="Emit machine-readable diagnostics.")
    args = parser.parse_args(argv)
    try:
        if args.command == "template":
            body = template(args.kind, args.issue, args.key)
            with args.output.open("x", encoding="utf-8", newline="\n") as stream:
                stream.write(body)
            result = {"ok": True, "draft": str(args.output)}
        else:
            body = args.path.read_text(encoding="utf-8-sig")
            if args.command == "check":
                result = validate_report(body)
            else:
                errors = validate_archive(json.loads(body))
                result = {"ok": not errors, "errors": errors}
    except (OSError, UnicodeError, ValueError) as exc:
        result = {"ok": False, "errors": [str(exc)]}
    emit(result, getattr(args, "json", False))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
