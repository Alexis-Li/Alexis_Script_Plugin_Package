# ruff: noqa: E402

import contextlib
import copy
import io
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import issue_report
import validate_repository

STATUS = """<!-- issue-status:v1 issue=65 -->
## 状态

- 状态：已关闭
- 基线：实现和审核为 0f1367ebceb4d57bb0deb02dfbea0fb6527b9998；已集成到开发分支。
- 更新日期：2026-10-19

### 简介

工单范围内的规则和工具已验收。

### 相关提交

PR: https://github.com/example/tools/pull/66
审核：0f1367ebceb4d57bb0deb02dfbea0fb6527b9998，无阻塞项。

### 已完成

AC-01 文档一致；AC-02 本轮工具测试通过。

### 未完成

无

### 开发边界

历史归档原文保留；产品运行时代码不在范围内。

### 决定

采用当前分支上的小文档修改与其余工作的 PR 流程。

### 宿主验收

无

### 下一步

无
"""


class IssueStatusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = ROOT.parent / ".tmp"
        try:
            cls.scratch.mkdir(exist_ok=True)
            cls.directory = tempfile.TemporaryDirectory(prefix="issue-status-", dir=cls.scratch)
        except OSError:
            cls.scratch = ROOT / ".tmp"
            cls.scratch.mkdir(exist_ok=True)
            cls.directory = tempfile.TemporaryDirectory(prefix="issue-status-", dir=cls.scratch)
        cls.addClassCleanup(cls.directory.cleanup)
        cls.work = pathlib.Path(cls.directory.name)

    def run_cli(self, arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = issue_report.main(arguments)
        return code, output.getvalue()

    def test_status_comments_support_open_and_closed_issues(self):
        for status in ("进行中", "待审核", "待验证", "受阻", "可关闭", "已关闭"):
            with self.subTest(status=status):
                result = issue_report.validate_status(
                    STATUS.replace("状态：已关闭", "状态：" + status)
                )
                self.assertEqual({"ok": True, "issue": 65, "errors": []}, result)

    def test_header_fields_cannot_be_satisfied_by_later_examples_or_hidden_text(self):
        baseline = next(line for line in STATUS.splitlines() if line.startswith("- 基线："))
        body = STATUS.replace(baseline, "")
        examples = (
            "\n```markdown\n" + baseline + "\n```\n",
            "\n> " + baseline + "\n",
            "\n" + baseline + "\n",
        )
        for example in examples:
            result = issue_report.validate_status(body + example)
            self.assertTrue(any("基线" in error for error in result["errors"]))
        body = STATUS.replace(baseline, "<!--\n" + baseline + "\n-->")
        self.assertFalse(issue_report.validate_status(body)["ok"])

    def test_code_examples_do_not_become_duplicate_comments_or_sections(self):
        body = STATUS + "\n````markdown\n" + STATUS + "\n```\n{{example}}\nTODO\n````\n"
        body += "\n源码中的 TODO 注释仍需落实对应行为。\n"
        self.assertTrue(issue_report.validate_status(body)["ok"])

    def test_invalid_structure_returns_diagnostics(self):
        cases = {
            "empty": "",
            "wrong version": STATUS.replace("issue-status:v1", "issue-status:v2"),
            "duplicate marker": STATUS + STATUS.splitlines()[0],
            "invalid issue": STATUS.replace("issue=65", "issue=0"),
            "missing marker": STATUS.split("\n", 1)[1],
            "fenced marker": "```markdown\n" + STATUS + "\n```",
            "wrong title": STATUS.replace("## 状态", "## 开发交付"),
            "duplicate title": STATUS + "\n## 状态\n",
            "duplicate status": STATUS.replace("- 状态：已关闭", "- 状态：已关闭\n- 状态：可关闭"),
            "invalid status": STATUS.replace("状态：已关闭", "状态：通过"),
            "invalid calendar date": STATUS.replace("2026-10-19", "2026-02-30"),
            "invalid date format": STATUS.replace("2026-10-19", "20261019"),
            "missing date": STATUS.replace("- 更新日期：2026-10-19", ""),
            "placeholder": STATUS + "\n{{填写下一步}}\n",
            "todo": STATUS + "\nTODO\n",
            "empty section": STATUS.split("### 下一步")[0] + "### 下一步\n<!-- note -->\n",
            "missing section": STATUS.replace("### 决定", "### 随意标题"),
            "duplicate section": STATUS + "\n### 决定\n无\n",
            "wrong order": STATUS.replace("### 已完成", "### 待交换")
                .replace("### 未完成", "### 已完成").replace("### 待交换", "### 未完成"),
            "old report": "<!-- issue-report:v1 kind=summary issue=65 key=current -->\n" + STATUS,
        }
        for name, body in cases.items():
            with self.subTest(name=name):
                self.assertFalse(issue_report.validate_status(body)["ok"])

    def test_machine_paths_are_rejected_even_in_examples_and_hidden_comments(self):
        paths = (
            "E" + ":/UE_5.7/Build.bat",
            "C" + ":\\Temp\\run.ps1",
            "/" + "Users/dev/project/out.log",
            "/" + "home/dev/evidence.log",
            "/" + "private/tmp/evidence.log",
            "/" + "var/folders/abc/evidence.log",
        )
        for path in paths:
            for suffix in (f"\n`{path}`", f"\n```text\n{path}\n```", f"\n<!-- {path} -->"):
                with self.subTest(path=path, suffix=suffix):
                    result = issue_report.validate_status(STATUS + suffix)
                    self.assertFalse(result["ok"])
                    self.assertTrue(any("machine-specific" in error for error in result["errors"]))
        self.assertTrue(issue_report.validate_status(
            STATUS + "\n`tools/issue_report.py`、`<scratch>/result.json`、`<Maya2024>`"
        )["ok"])

    def test_template_requires_completion_and_positive_issue_number(self):
        draft = issue_report.template(65)
        self.assertTrue(draft.startswith("<!-- issue-status:v1 issue=65 -->\n"))
        self.assertFalse(issue_report.validate_status(draft)["ok"])
        for issue in (0, -1, True, "65"):
            with self.subTest(issue=issue), self.assertRaises(ValueError):
                issue_report.template(issue)

    def test_cli_refuses_overwrite_and_reports_file_errors(self):
        output = self.work / "draft.md"
        args = ["template", "--issue", "65", "--output", str(output)]
        code, _ = self.run_cli(args)
        self.assertEqual(0, code)
        before = output.read_bytes()
        code, _ = self.run_cli(args)
        self.assertEqual(1, code)
        self.assertEqual(before, output.read_bytes())
        invalid = self.work / "invalid.md"
        invalid.write_bytes(b"\xff\xfe")
        for path in (output, invalid, self.work / "missing.md"):
            code, text = self.run_cli(["check", str(path), "--json"])
            self.assertEqual(1, code)
            self.assertTrue(json.loads(text)["errors"])
        code, _ = self.run_cli(["template", "--issue", "0", "--output", str(output)])
        self.assertEqual(1, code)
        self.assertEqual(before, output.read_bytes())

    def test_cli_valid_status_and_bom(self):
        path = self.work / "status.md"
        path.write_text(STATUS, encoding="utf-8-sig")
        code, text = self.run_cli(["check", str(path), "--json"])
        self.assertEqual(0, code)
        self.assertEqual({"ok": True, "issue": 65, "errors": []}, json.loads(text))

    def test_cli_rejects_removed_commands_and_report_options(self):
        cases = (
            ["check-archive", "archive.json"],
            ["template", "development", "--issue", "65", "--output", "unused.md"],
            ["template", "--issue", "65", "--key", "current", "--output", "unused.md"],
        )
        for args in cases:
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    issue_report.main(args)
                self.assertEqual(2, raised.exception.code)

    def test_archive_file_errors_fail_repository_gate(self):
        root = self.work / "broken-archive-repo"
        path = root / "docs" / "project-history" / "sample" / "issue-1-comment-archive.json"
        path.parent.mkdir(parents=True)
        for body in ("not JSON", '{"schema": "unknown", "comments": []}'):
            with self.subTest(body=body):
                path.write_text(body, encoding="utf-8")
                errors = []
                validate_repository._validate_issue_archives(root, errors)
                self.assertTrue(errors)
                self.assertIn("issue-1-comment-archive.json", errors[0])


class IssueArchiveTests(unittest.TestCase):
    def setUp(self):
        self.archive = {
            "schema": "github-issue-comment-archive/1",
            "issue": "https://github.com/example/tools/issues/53",
            "archived_on": "2026-09-30",
            "purpose": "Historical source only; superseded claims are not current status.",
            "current_status_comment": "https://github.com/example/tools/issues/53#issuecomment-99",
            "comments": [
                {
                    "id": 10,
                    "created_at": "2026-09-29T10:00:00Z",
                    "updated_at": "2026-09-29T11:00:00Z",
                    "author": "reviewer",
                    "original_url": "https://github.com/example/tools/issues/53#issuecomment-10",
                    "body": "Old unstructured review; retain this wording exactly.",
                }
            ],
        }

    def test_original_unstructured_comments_are_preserved(self):
        before = copy.deepcopy(self.archive)
        self.assertEqual([], validate_repository._issue_archive_errors(self.archive))
        self.assertEqual(before, self.archive)

    def test_missing_source_fields_or_mismatched_identity_fail(self):
        for field in ("id", "author", "created_at", "updated_at", "original_url", "body"):
            with self.subTest(field=field):
                archive = copy.deepcopy(self.archive)
                del archive["comments"][0][field]
                self.assertTrue(validate_repository._issue_archive_errors(archive))
        for field, value in (
            ("original_url", "https://github.com/example/tools/issues/54#issuecomment-10"),
            ("updated_at", "2026-09-29T11:00:00"),
            ("id", True),
        ):
            archive = copy.deepcopy(self.archive)
            archive["comments"][0][field] = value
            self.assertTrue(validate_repository._issue_archive_errors(archive))

    def test_invalid_archive_shapes_are_diagnostics_not_exceptions(self):
        for value in (None, [], "archive", {}, {"comments": [None]}):
            with self.subTest(value=value):
                self.assertTrue(validate_repository._issue_archive_errors(value))
        archive = copy.deepcopy(self.archive)
        archive["comments"].append(copy.deepcopy(archive["comments"][0]))
        self.assertTrue(validate_repository._issue_archive_errors(archive))

    def test_repository_gate_validates_archives(self):
        # Existing archives are source artifacts, not generated report templates.
        errors = []
        validate_repository._validate_issue_archives(ROOT, errors)
        self.assertEqual([], errors)

    def test_archive_path_exemption_covers_only_source_bodies(self):
        for path in ("Z" + ":/old-workspace/evidence", "/" + "home/dev/evidence"):
            archive = copy.deepcopy(self.archive)
            archive["comments"][0]["body"] = path
            self.assertEqual([], validate_repository._issue_archive_errors(archive))
            for owner, field in ((archive, "purpose"), (archive["comments"][0], "author")):
                before = owner[field]
                owner[field] = path
                errors = validate_repository._issue_archive_errors(archive)
                self.assertTrue(any("machine-specific" in error for error in errors))
                owner[field] = before
            archive["extra"] = {"local_path": path}
            self.assertTrue(validate_repository._issue_archive_errors(archive))

    def test_repository_path_policy_preserves_only_valid_archive_bodies(self):
        historical_path = "Z" + ":/old-workspace/evidence"
        relative = "docs/project-history/sample/issue-53-comment-archive.json"
        archive = copy.deepcopy(self.archive)
        archive["comments"][0]["body"] += " Evidence: " + historical_path
        metadata = copy.deepcopy(archive)
        metadata["purpose"] = historical_path
        invalid = copy.deepcopy(archive)
        invalid["schema"] = "unknown"
        cases = (
            (relative, json.dumps(archive), False),
            (relative, json.dumps(metadata), True),
            (relative, json.dumps(invalid), True),
            (relative, "broken JSON " + historical_path, True),
            ("config/issue-53-comment-archive.json", json.dumps(archive), True),
            ("docs/project-history/sample/notes.json", json.dumps(archive), True),
            ("docs/guide.md", historical_path, True),
        )
        scratch = ROOT.parent / ".tmp"
        scratch.mkdir(exist_ok=True)
        for path_name, content, rejected in cases:
            with self.subTest(path=path_name, content=content):
                with tempfile.TemporaryDirectory(prefix="archive-path-", dir=scratch) as directory:
                    root = pathlib.Path(directory)
                    path = root / path_name
                    path.parent.mkdir(parents=True)
                    path.write_text(content, encoding="utf-8")
                    errors = validate_repository.validate(root)["errors"]
                    self.assertEqual(rejected, any(
                        "machine-specific absolute path" in error for error in errors
                    ))
                    self.assertEqual(content, path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
