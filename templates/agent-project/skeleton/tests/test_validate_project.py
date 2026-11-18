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

import validate_project

# Built by concatenation so these files never report themselves.
MARKER = "待定" + "制："
PLACEHOLDER = "{{" + "PROJECT_NAME" + "}}"
HAS_GIT = shutil.which("git") is not None


class ValidateProjectTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="validate-project-")
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        for relative in validate_project.REQUIRED_FILES:
            self.write(relative, "# Title\n")

    def write(self, relative, text, root=None):
        path = (root or self.root) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8") if isinstance(text, str) else text)
        return path

    def errors(self):
        return validate_project.validate(self.root)["errors"]

    def assert_one_error(self, *fragments):
        errors = self.errors()
        self.assertEqual(1, len(errors), errors)
        for fragment in fragments:
            self.assertIn(fragment, errors[0])

    def git(self, *args):
        subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True)

    def test_complete_project_passes(self):
        self.write("docs/extra.json", '{"a": 1}\n')
        self.write("docs/guide.md", "See [workflow](workflow.md#top) and [site](https://example.com).\n")
        self.write("docs/workflow.md", "# Workflow\n")
        self.assertEqual({"ok": True, "errors": []}, validate_project.validate(self.root))

    def test_missing_required_file_is_reported(self):
        (self.root / "docs" / "roadmap.md").unlink()
        self.assert_one_error("docs/roadmap.md", "missing required file")

    def test_invalid_utf8_is_reported(self):
        self.write("docs/notes.md", b"\xff\xfe broken")
        self.assert_one_error("docs/notes.md", "UTF-8")

    def test_utf8_bom_is_accepted(self):
        self.write("docs/notes.md", b"\xef\xbb\xbf# Notes\n")
        self.assertEqual([], self.errors())

    def test_machine_paths_are_reported_with_line_numbers(self):
        paths = ("C" + ":\\Users\\dev\\file.txt", "/" + "home/dev/file.txt", "/" + "Users/dev/file.txt")
        for path in paths:
            with self.subTest(path=path):
                self.write("docs/notes.md", "ok\n`{0}`\n".format(path))
                self.assert_one_error("docs/notes.md:2", "absolute path")
        self.write("docs/notes.md", "Use `<scratch>/out.txt` or https://example.com/a/b.\n")
        self.assertEqual([], self.errors())

    def test_template_markers_are_reported(self):
        self.write("docs/notes.md", "fine\n<!-- {0}fill this -->\n".format(MARKER))
        self.assert_one_error("docs/notes.md:2", MARKER, "first setup")
        self.write("docs/notes.md", "# {0} notes\n".format(PLACEHOLDER))
        self.assert_one_error("docs/notes.md:1", PLACEHOLDER)

    def test_other_double_braces_are_not_markers(self):
        self.write("tools/draft.md", "Fill {{description}} before publishing.\n")
        self.assertEqual([], self.errors())

    def test_markers_are_found_in_dotfiles(self):
        self.write(".gitattributes", "# {0}add LFS rules\n".format(MARKER))
        self.assert_one_error(".gitattributes:1", MARKER)

    def test_invalid_json_is_reported(self):
        self.write("config/data.json", '{"a": 1,}\n')
        self.assert_one_error("config/data.json:1", "invalid JSON")
        self.write("config/data.json", '{"a": 1}\n')
        self.assertEqual([], self.errors())

    def test_broken_relative_links_are_reported(self):
        self.write("docs/guide.md", "Intro\n\n[missing](missing.md#part)\n![image](img/none.png)\n")
        errors = self.errors()
        self.assertEqual(2, len(errors), errors)
        self.assertIn("docs/guide.md:3", errors[0])
        self.assertIn("docs/guide.md:4", errors[1])
        self.write("docs/missing.md", "# Missing\n")
        self.write("docs/img/none.png", b"\x89PNG")
        self.assertEqual([], self.errors())

    def test_links_ignore_external_anchors_code_and_comments(self):
        text = (
            "[a](https://example.com/x) [b](mailto:a@example.com) [c](#section)\n"
            "Inline `[d](nowhere.md)` is code.\n"
            "```text\n[e](nowhere.md)\n```\n"
            "<!-- [f](nowhere.md) -->\n"
            "[g](../README.md) and [h](/README.md)\n"
        )
        self.write("docs/guide.md", text)
        self.assertEqual([], self.errors())

    def test_reference_links_and_root_escape_are_checked(self):
        self.write("docs/guide.md", "[x][ref]\n\n[ref]: missing.md\n[out]: ../../outside.md\n")
        errors = self.errors()
        self.assertEqual(2, len(errors), errors)
        self.assertIn("docs/guide.md:3: broken link", errors[0])
        self.assertIn("docs/guide.md:4: link leaves the repository", errors[1])

    def test_non_git_directory_is_walked_and_skips_tool_directories(self):
        for skipped in (".venv", ".tmp", "__pycache__", ".pytest_cache", ".ruff_cache"):
            self.write(skipped + "/bad.md", "{0}\n".format(PLACEHOLDER))
        nested = self.root / "vendor" / "other"
        self.write("README.md", "# Nested\n{0}\n".format(PLACEHOLDER), root=nested)
        (nested / ".git").mkdir()
        self.assertEqual([], self.errors())
        # A generated-output name is only an error when Git can see it.
        self.write("samples/a.txt", "data\n")
        self.assertEqual([], self.errors())

    def test_non_text_files_are_not_decoded(self):
        self.write("docs/image.png", b"\xff\xfe\x00")
        self.assertEqual([], self.errors())

    @unittest.skipUnless(HAS_GIT, "git is not available")
    def test_git_repository_lists_tracked_and_unignored_files_only(self):
        self.git("init", "-q")
        self.write(".gitignore", "ignored/\nsamples/\n")
        self.write("ignored/bad.md", "{0}\n".format(PLACEHOLDER))
        self.write("docs/new.md", "{0}\n".format(MARKER))
        self.assert_one_error("docs/new.md:1", MARKER)

    @unittest.skipUnless(HAS_GIT, "git is not available")
    def test_generated_output_visible_to_git_is_reported(self):
        self.git("init", "-q")
        names = (
            "samples/set-a/input.txt", "reviews/pack.txt", "releases/a.txt", "HANDOFF.md",
            "dist/build.zip", "dist/pack.7z", "tools/__pycache__/x.cpython-313.pyc",
            ".tmp/scratch.txt", "docs/HANDOFF.md",
        )
        for name in names:
            self.write(name, b"x")
        errors = self.errors()
        self.assertEqual(len(names), len(errors), errors)
        for name in names:
            self.assertTrue(any(error.startswith(name + ":") for error in errors), name)

    @unittest.skipUnless(HAS_GIT, "git is not available")
    def test_ignored_generated_output_is_accepted(self):
        self.git("init", "-q")
        self.write(".gitignore", "samples/\nHANDOFF.md\n*.zip\n")
        self.write("samples/set-a/input.txt", b"x")
        self.write("HANDOFF.md", "note\n")
        self.write("dist/pack.zip", b"x")
        self.assertEqual([], self.errors())

    @unittest.skipUnless(HAS_GIT, "git is not available")
    def test_nested_git_checkout_is_skipped_in_git_mode(self):
        self.git("init", "-q")
        nested = self.root / "worktree-copy"
        self.write("docs/bad.md", "{0}\n".format(PLACEHOLDER), root=nested)
        (nested / ".git").write_text("gitdir: elsewhere\n", encoding="utf-8")
        self.assertEqual([], self.errors())

    def test_cli_reports_success_failure_and_json(self):
        project = self.root / "project"
        for relative in validate_project.REQUIRED_FILES:
            self.write(relative, "# Title\n", root=project)
        shutil.copytree(ROOT / "tools", project / "tools", dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("__pycache__"))
        script = str(project / "tools" / "validate_project.py")

        def run(*arguments):
            environment = {**os.environ, "PYTHONIOENCODING": "utf-8"}
            return subprocess.run([sys.executable, script, *arguments], capture_output=True,
                                  encoding="utf-8", env=environment)

        done = run()
        self.assertEqual((0, "Project files: OK\n"), (done.returncode, done.stdout))
        self.assertEqual({"errors": [], "ok": True}, json.loads(run("--json").stdout))

        self.write("docs/notes.md", "{0}\n".format(MARKER), root=project)
        failed = run()
        self.assertEqual(1, failed.returncode)
        self.assertIn("docs/notes.md:1", failed.stdout)
        failed_json = run("--json")
        self.assertEqual(1, failed_json.returncode)
        payload = json.loads(failed_json.stdout)
        self.assertFalse(payload["ok"])
        self.assertEqual(1, len(payload["errors"]))
        self.assertEqual(2, run("--unknown").returncode)

    def test_main_uses_module_root(self):
        output = io.StringIO()
        with mock.patch.object(validate_project, "ROOT", self.root), contextlib.redirect_stdout(output):
            self.assertEqual(0, validate_project.main([]))
        self.assertEqual("Project files: OK\n", output.getvalue())


if __name__ == "__main__":
    unittest.main()
