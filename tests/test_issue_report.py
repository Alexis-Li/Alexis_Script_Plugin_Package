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

REVIEW = """<!-- issue-report:v1 kind=review issue=53 key=review-03 -->
## 审核结论

- 状态：需修改
- 基线：0f1367ebceb4d57bb0deb02dfbea0fb6527b9998，本地提交
- 范围：AC-05 容器替换失败时保护旧参考
- 依据：https://github.com/example/tools/issues/53
- 阻塞项：R-001
- 本次验证：本轮执行
- 更新日期：2026-09-30

### 结论

替换失败后的恢复不符合 AC-05，需修复。

### 审核问题

R-001：命名空间切换失败后旧参考被删除。阻塞 AC-05；修复后以 UUID 验证旧对象保留。

### 验证与证据边界

Maya 2024 故障注入返回失败，但旧对象 UUID 已不存在；其他路径未验证。

### 后续处理

修复 R-001 后独立复验；工单保持打开。
"""


class IssueReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = ROOT.parent / ".tmp"
        try:
            cls.scratch.mkdir(exist_ok=True)
            cls.directory = tempfile.TemporaryDirectory(prefix="issue-report-", dir=cls.scratch)
        except OSError:
            cls.scratch = ROOT / ".tmp"
            cls.scratch.mkdir(exist_ok=True)
            cls.directory = tempfile.TemporaryDirectory(prefix="issue-report-", dir=cls.scratch)
        cls.addClassCleanup(cls.directory.cleanup)
        cls.work = pathlib.Path(cls.directory.name)

    def run_cli(self, arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = issue_report.main(arguments)
        return code, output.getvalue()

    def test_review_preserves_concrete_blocker_and_baseline(self):
        result = issue_report.validate_report(REVIEW)
        self.assertTrue(result["ok"], result)
        self.assertEqual(
            ("review", 53, "review-03"), (result["kind"], result["issue"], result["key"])
        )

    def test_approval_cannot_have_declared_blockers(self):
        result = issue_report.validate_report(REVIEW.replace("状态：需修改", "状态：通过"))
        self.assertFalse(result["ok"])
        self.assertTrue(any("unresolved blockers" in error for error in result["errors"]))

    def test_cleared_blocker_can_pass_scoped_review(self):
        body = REVIEW.replace("状态：需修改", "状态：通过").replace("阻塞项：R-001", "阻塞项：无")
        # Content truth is the reviewer's responsibility; the validator checks declarations.
        self.assertTrue(issue_report.validate_report(body)["ok"])

    def test_header_fields_cannot_be_satisfied_by_later_example(self):
        body = REVIEW.replace("- 基线：0f1367ebceb4d57bb0deb02dfbea0fb6527b9998，本地提交\n", "")
        body += "\n```markdown\n- 基线：example\n```\n"
        result = issue_report.validate_report(body)
        self.assertTrue(any("基线" in error for error in result["errors"]))

    def test_code_examples_do_not_become_duplicate_reports_or_sections(self):
        body = REVIEW + "\n````markdown\n" + REVIEW + "\n```\n{{example}}\nTODO\n````\n"
        body += "\n源码中的 TODO 注释仍需落实对应行为。\n"
        self.assertTrue(issue_report.validate_report(body)["ok"])

    def test_invalid_reports_return_diagnostics(self):
        cases = {
            "empty": "",
            "wrong version": REVIEW.replace("issue-report:v1", "issue-report:v2"),
            "duplicate marker": REVIEW + REVIEW.splitlines()[0],
            "invalid issue": REVIEW.replace("issue=53", "issue=0"),
            "reserved key": REVIEW.replace("key=review-03", "key=current"),
            "missing source": REVIEW.replace(
                "- 依据：https://github.com/example/tools/issues/53\n", ""
            ),
            "duplicate status": REVIEW.replace("- 状态：需修改", "- 状态：需修改\n- 状态：通过"),
            "wrong kind status": REVIEW.replace("状态：需修改", "状态：待审核"),
            "invalid verification": REVIEW.replace("本次验证：本轮执行", "本次验证：全过"),
            "invalid calendar date": REVIEW.replace("2026-09-30", "2026-02-30"),
            "duplicate blocker": REVIEW.replace("阻塞项：R-001", "阻塞项：R-001, R-001"),
            "freeform blocker": REVIEW.replace("阻塞项：R-001", "阻塞项：有一个"),
            "placeholder": REVIEW.replace("工单保持打开", "{{填写下一步}}"),
            "empty section": REVIEW.split("### 后续处理")[0] + "### 后续处理\n<!-- note -->\n",
            "missing section": REVIEW.replace("### 审核问题", "### 随意标题"),
            "duplicate section": REVIEW + "\n### 审核问题\n无\n",
        }
        for name, body in cases.items():
            with self.subTest(name=name):
                self.assertFalse(issue_report.validate_report(body)["ok"])

    def test_drafts_require_completion_for_all_three_report_kinds(self):
        for kind in issue_report.REPORTS:
            with self.subTest(kind=kind):
                key = "current" if kind == "summary" else "round-01"
                draft = issue_report.template(kind, 53, key)
                self.assertFalse(issue_report.validate_report(draft)["ok"])
        with self.assertRaises(ValueError):
            issue_report.template("summary", 53, "another-summary")

    def test_summary_requires_whole_issue_blocker_resolution(self):
        body = """<!-- issue-report:v1 kind=summary issue=53 key=current -->
## 当前结论
- 状态：可关闭
- 基线：实现和审核均为 0f1367ebceb4d57bb0deb02dfbea0fb6527b9998
- 范围：本票全部验收项
- 依据：归档中的审核记录 5888512464
- 阻塞项：R-001
- 本次验证：未执行
- 更新日期：2026-09-30
### 未闭环事项
R-001：旧参考恢复，#53 实现者负责。
### 已验证成果与边界
FBX 基础样例通过。
### 验证依据
来自既有审核，本次仅整理。
### 接手入口
见原型 README。
### 记录索引
见原报告归档。
"""
        self.assertFalse(issue_report.validate_report(body)["ok"])
        self.assertTrue(
            issue_report.validate_report(body.replace("状态：可关闭", "状态：修复中"))["ok"]
        )

    def test_cli_refuses_overwrite_and_reports_invalid_utf8_or_missing_file(self):
        output = self.work / "draft.md"
        args = [
            "template",
            "development",
            "--issue",
            "53",
            "--key",
            "dev-01",
            "--output",
            str(output),
        ]
        code, _ = self.run_cli(args)
        self.assertEqual(0, code)
        before = output.read_bytes()
        code, _ = self.run_cli(args)
        self.assertEqual(1, code)
        self.assertEqual(before, output.read_bytes())
        code, text = self.run_cli(["check", str(output), "--json"])
        self.assertEqual(1, code)
        self.assertFalse(json.loads(text)["ok"])
        invalid = self.work / "invalid.md"
        invalid.write_bytes(b"\xff\xfe")
        for path in (invalid, self.work / "missing.md"):
            code, text = self.run_cli(["check", str(path), "--json"])
            self.assertEqual(1, code)
            self.assertTrue(json.loads(text)["errors"])

    def test_cli_valid_report_and_bom(self):
        path = self.work / "review.md"
        path.write_text(REVIEW, encoding="utf-8-sig")
        code, text = self.run_cli(["check", str(path), "--json"])
        self.assertEqual(0, code)
        self.assertTrue(json.loads(text)["ok"])

    def test_archive_file_errors_fail_cli_and_repository_gate(self):
        root = self.work / "broken-archive-repo"
        path = root / "docs" / "project-history" / "sample" / "issue-1-comment-archive.json"
        path.parent.mkdir(parents=True)
        for body in ("not JSON", '{"schema": "unknown", "comments": []}'):
            with self.subTest(body=body):
                path.write_text(body, encoding="utf-8")
                code, output = self.run_cli(["check-archive", str(path), "--json"])
                self.assertEqual(1, code)
                self.assertTrue(json.loads(output)["errors"])
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
        self.assertEqual([], issue_report.validate_archive(self.archive))
        self.assertEqual(before, self.archive)

    def test_missing_source_fields_or_mismatched_identity_fail(self):
        for field in ("id", "author", "created_at", "updated_at", "original_url", "body"):
            with self.subTest(field=field):
                archive = copy.deepcopy(self.archive)
                del archive["comments"][0][field]
                self.assertTrue(issue_report.validate_archive(archive))
        for field, value in (
            ("original_url", "https://github.com/example/tools/issues/54#issuecomment-10"),
            ("updated_at", "2026-09-29T11:00:00"),
            ("id", True),
        ):
            archive = copy.deepcopy(self.archive)
            archive["comments"][0][field] = value
            self.assertTrue(issue_report.validate_archive(archive))

    def test_invalid_archive_shapes_are_diagnostics_not_exceptions(self):
        for value in (None, [], "archive", {}, {"comments": [None]}):
            with self.subTest(value=value):
                self.assertTrue(issue_report.validate_archive(value))
        archive = copy.deepcopy(self.archive)
        archive["comments"].append(copy.deepcopy(archive["comments"][0]))
        self.assertTrue(issue_report.validate_archive(archive))

    def test_repository_gate_validates_archives(self):
        # Existing archives are source artifacts, not generated report templates.
        errors = []
        validate_repository._validate_issue_archives(ROOT, errors)
        self.assertEqual([], errors)


if __name__ == "__main__":
    unittest.main()
