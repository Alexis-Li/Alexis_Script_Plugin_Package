# ruff: noqa: E402

import contextlib
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import create_agent_project

# Built by concatenation: the collection's text checks must not see literal markers.
MARKER = "待定" + "制："
PLACEHOLDER = "{{" + "PROJECT_NAME" + "}}"
HAS_GIT = shutil.which("git") is not None
EXPECTED_FILES = (
    "AGENTS.md",
    ".gitignore",
    ".gitattributes",
    ".editorconfig",
    "CONTEXT.md",
    "README.md",
    "README_CN.md",
    "CHANGELOG.md",
    "docs/workflow.md",
    "docs/adr/README.md",
    ".github/pull_request_template.md",
    ".github/workflows/ci.yml",
    "tools/issue_report.py",
    "tools/_project_tools.py",
    "tools/validate_project.py",
    "tests/test_issue_report.py",
    "tests/test_validate_project.py",
)


def _run(project, *arguments):
    environment = {**os.environ, "PYTHONIOENCODING": "utf-8"}
    return subprocess.run(
        [sys.executable, *arguments], cwd=project, capture_output=True, encoding="utf-8",
        env=environment,
    )


class CreateAgentProjectTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="agent-project-")
        self.addCleanup(self.directory.cleanup)
        self.work = pathlib.Path(self.directory.name)

    def cli(self, *arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = create_agent_project.main(list(arguments))
        return code, output.getvalue()

    def generate(self, name="SampleProject"):
        destination = self.work / name
        code, _ = self.cli(name, "--destination", str(destination), "--apply")
        self.assertEqual(0, code)
        return destination

    @staticmethod
    def files(directory):
        return sorted(
            path.relative_to(directory).as_posix() for path in directory.rglob("*") if path.is_file()
        )

    def test_preview_lists_files_without_writing(self):
        destination = self.work / "SampleProject"
        result = create_agent_project.create("SampleProject", str(destination))
        self.assertEqual("dry-run", result["mode"])
        self.assertFalse(destination.exists())
        for name in EXPECTED_FILES:
            self.assertIn(name, result["files"])
        self.assertFalse(any(name.endswith(".tmpl") for name in result["files"]))
        self.assertTrue(result["next_steps"])

        destination.mkdir()
        code, text = self.cli("SampleProject", "--destination", str(destination), "--json")
        self.assertEqual(0, code)
        self.assertEqual(result["files"], json.loads(text)["files"])
        self.assertEqual([], list(destination.iterdir()))

    def test_apply_writes_renamed_lf_files_without_placeholders(self):
        destination = self.generate()
        written = self.files(destination)
        for name in EXPECTED_FILES:
            self.assertIn(name, written)
        self.assertFalse([name for name in written if name.endswith(".tmpl")])
        self.assertFalse([name for name in written if "__pycache__" in name])
        for name in written:
            data = (destination / name).read_bytes()
            self.assertNotIn(PLACEHOLDER.encode(), data, name)
            self.assertNotIn(b"\r\n", data, name)
        self.assertTrue((destination / "AGENTS.md").read_text(encoding="utf-8").startswith(
            "# SampleProject development rules\n"
        ))
        self.assertEqual(written, sorted(
            create_agent_project.create("SampleProject", str(self.work / "Other"))["files"]
        ))

    def test_untracked_skeleton_files_are_not_copied(self):
        repository = self.work / "collection"
        skeleton = repository / "templates" / "agent-project" / "skeleton"
        skeleton.mkdir(parents=True)
        (skeleton / "README.md").write_text("# {{PROJECT_NAME}}\n", encoding="utf-8")
        (skeleton / "AGENTS.md.tmpl").write_text("rules\n", encoding="utf-8")
        git = ["git", "-C", str(repository), "-c", "user.name=test", "-c", "user.email=test@example.invalid"]
        for command in (["init", "-q"], ["add", "."], ["commit", "-q", "-m", "skeleton"]):
            subprocess.run([*git, *command], check=True, capture_output=True)
        (skeleton / "HANDOFF.md").write_text("local notes\n", encoding="utf-8")
        with mock.patch.object(create_agent_project, "ROOT", repository), \
                mock.patch.object(create_agent_project, "SKELETON", skeleton):
            preview = create_agent_project.create("SampleProject", str(self.work / "Preview"))
            destination = self.work / "Applied"
            create_agent_project.create("SampleProject", str(destination), apply=True)
        self.assertEqual(["AGENTS.md", "README.md"], sorted(preview["files"]))
        self.assertEqual(["AGENTS.md", "README.md"], self.files(destination))

    def test_missing_tracked_file_and_git_failure_are_errors(self):
        missing = create_agent_project.SKELETON / "no-such-tracked-file.md"
        with mock.patch.object(create_agent_project, "_tracked_skeleton_files", return_value=[missing]):
            code, text = self.cli("SampleProject", "--destination", str(self.work / "A"), "--apply")
        self.assertEqual(1, code)
        self.assertIn("missing from the working tree", text)
        self.assertFalse((self.work / "A").exists())
        with mock.patch.object(create_agent_project.subprocess, "run", side_effect=OSError("no git")):
            code, text = self.cli("SampleProject", "--destination", str(self.work / "B"), "--apply")
        self.assertEqual(1, code)
        self.assertIn("cannot list tracked template files", text)
        self.assertFalse((self.work / "B").exists())

    def test_output_survives_a_legacy_code_page(self):
        destination = self.work / "Legacy"
        stream = io.TextIOWrapper(io.BytesIO(), encoding="cp1252")
        with contextlib.redirect_stdout(stream):
            code = create_agent_project.main(
                ["SampleProject", "--destination", str(destination), "--apply"]
            )
        stream.flush()
        self.assertEqual(0, code)
        self.assertTrue((destination / "AGENTS.md").is_file())
        self.assertIn(MARKER, stream.buffer.getvalue().decode("utf-8"))

    def test_next_steps_start_the_repository_on_main(self):
        steps = create_agent_project.create("SampleProject", str(self.work / "Steps"))["next_steps"]
        self.assertIn("git init -b main", " ".join(steps))

    def test_apply_creates_missing_parent_directories_and_accepts_empty_directory(self):
        nested = self.work / "a" / "b" / "SampleProject"
        self.assertEqual(0, self.cli("SampleProject", "--destination", str(nested), "--apply")[0])
        self.assertTrue((nested / "AGENTS.md").is_file())
        empty = self.work / "Empty"
        empty.mkdir()
        self.assertEqual(0, self.cli("SampleProject", "--destination", str(empty), "--apply")[0])
        self.assertTrue((empty / "AGENTS.md").is_file())

    def test_rejects_non_empty_or_file_destination(self):
        occupied = self.work / "Occupied"
        occupied.mkdir()
        (occupied / "keep.txt").write_text("user data", encoding="utf-8")
        a_file = self.work / "file"
        a_file.write_text("x", encoding="utf-8")
        for destination in (occupied, a_file):
            with self.subTest(destination=destination.name):
                code, text = self.cli("SampleProject", "--destination", str(destination), "--apply")
                self.assertEqual(1, code)
                self.assertIn("empty directory", text)
        self.assertEqual(["keep.txt"], self.files(occupied))

    def test_rejects_destination_inside_collection_repository(self):
        for destination in (ROOT, ROOT / "SampleProjectCopy", ROOT / "templates" / "SampleProject"):
            with self.subTest(destination=destination.name):
                code, text = self.cli("SampleProject", "--destination", str(destination), "--apply")
                self.assertEqual(1, code)
                self.assertIn("outside this collection", text)
        self.assertFalse((ROOT / "SampleProjectCopy").exists())
        self.assertFalse((ROOT / "templates" / "SampleProject").exists())

    def test_rejects_destination_inside_another_git_working_tree(self):
        repository = self.work / "other-repo"
        (repository / ".git").mkdir(parents=True)
        (repository / ".git" / "HEAD").write_text("ref: refs/heads/main" + chr(10), encoding="utf-8")
        linked = self.work / "linked-worktree"
        linked.mkdir()
        (linked / ".git").write_text("gitdir: elsewhere" + chr(10), encoding="utf-8")
        for parent in (repository, linked):
            destination = parent / "projects" / "SampleProject"
            with self.subTest(parent=parent.name):
                code, text = self.cli("SampleProject", "--destination", str(destination), "--apply")
                self.assertEqual(1, code)
                self.assertIn("another Git working tree", text)
                self.assertFalse(destination.exists())

    def test_empty_git_directory_is_not_a_repository(self):
        stale = self.work / "stale"
        (stale / ".git").mkdir(parents=True)
        destination = stale / "SampleProject"
        code, _ = self.cli("SampleProject", "--destination", str(destination), "--apply")
        self.assertEqual(0, code)
        self.assertTrue((destination / "AGENTS.md").is_file())

    def test_rejects_invalid_project_names_and_missing_destination(self):
        for name in ("sample", "Sample-Project", "1Sample", "Sample Project", ""):
            with self.subTest(name=name):
                code, text = self.cli(name, "--destination", str(self.work / "X"), "--apply")
                self.assertEqual(1, code)
                self.assertIn("PascalCase", text)
        self.assertFalse((self.work / "X").exists())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            create_agent_project.main(["SampleProject"])
        self.assertEqual(2, raised.exception.code)

    def test_generated_project_checks_itself_end_to_end(self):
        project = self.generate()

        failed = _run(project, "tools/validate_project.py", "--json")
        self.assertEqual(1, failed.returncode, failed.stdout)
        payload = json.loads(failed.stdout)
        self.assertFalse(payload["ok"])
        self.assertTrue(payload["errors"])
        for error in payload["errors"]:
            self.assertIn("template marker '" + MARKER + "'", error)

        tests = _run(project, "-m", "unittest", "discover", "-s", "tests")
        self.assertEqual(0, tests.returncode, tests.stderr)

        # Simulate first-time customization: remove every marker line.
        for path in project.rglob("*"):
            if path.is_file() and "__pycache__" not in path.parts:
                lines = path.read_bytes().decode("utf-8").split("\n")
                kept = [line for line in lines if MARKER not in line]
                if kept != lines:
                    path.write_bytes("\n".join(kept).encode("utf-8"))
        done = _run(project, "tools/validate_project.py")
        self.assertEqual(0, done.returncode, done.stdout)
        self.assertEqual("Project files: OK\n", done.stdout)

    @unittest.skipUnless(HAS_GIT, "git is not available")
    def test_customized_project_passes_inside_a_git_repository(self):
        project = self.generate()
        for path in project.rglob("*"):
            if path.is_file():
                lines = path.read_bytes().decode("utf-8").split("\n")
                path.write_bytes(
                    "\n".join(line for line in lines if MARKER not in line).encode("utf-8")
                )
        subprocess.run(["git", "-C", str(project), "init", "-q"], check=True, capture_output=True)
        _run(project, "-m", "unittest", "discover", "-s", "tests")
        done = _run(project, "tools/validate_project.py")
        self.assertEqual(0, done.returncode, done.stdout)


if __name__ == "__main__":
    unittest.main()
