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

    def test_link_destinations_support_parentheses_escapes_and_angle_brackets(self):
        text = (
            '[a](guide_(v2).md) [b](<path with spaces.md>) [c](plain.md "Title")\n'
            "[d](esc\\)aped.md)\n"
        )
        names = ["guide_(v2).md", "path with spaces.md", "plain.md", "esc)aped.md"]
        self.assertEqual(names, [target for _, target in validate_project.markdown_links(text)])
        self.write("docs/guide.md", text)
        self.assertEqual(4, len(self.errors()))
        for name in names:
            self.write("docs/" + name, "# Target\n")
        self.assertEqual([], self.errors())

    def test_angle_destination_containing_link_syntax_is_one_link(self):
        text = "[manual](<guide](v2).md>)\n"
        self.assertEqual(["guide](v2).md"], [t for _, t in validate_project.markdown_links(text)])
        self.write("docs/guide.md", text)
        self.assertEqual(1, len(self.errors()))
        self.write("docs/guide](v2).md", "# Target\n")
        self.assertEqual([], self.errors())

    def test_inline_and_reference_destinations_unescape_the_same_way(self):
        text = "[a](guide_\\(v2\\).md)\n[b](guide_(v2).md)\n\n[r]: guide_\\(v2\\).md\n"
        targets = [target for _, target in validate_project.markdown_links(text)]
        self.assertEqual(["guide_(v2).md"] * 3, targets)

    def targets(self, text):
        return [target for _, target in validate_project.markdown_links(text)]

    def test_link_inside_stray_bracket_text_is_found(self):
        text = "](#[manual](missing.md))\n"
        self.assertEqual(["missing.md"], self.targets(text))
        self.write("docs/guide.md", text)
        self.assert_one_error("docs/guide.md:1", "broken link: missing.md")

    def test_parenthesized_destinations_match_escaped_ones(self):
        text = "[a](guide_(v2).md) [b](guide_\\(v2\\).md)\n\n[r]: guide_\\(v2\\).md\n"
        self.assertEqual(["guide_(v2).md"] * 3, self.targets(text))

    def test_labels_pair_brackets_and_honor_escapes(self):
        self.assertEqual(["x.md"], self.targets("[a [b] c](x.md)\n"))
        self.assertEqual([], self.targets("\\[not](a.md)\n"))
        self.assertEqual(["img.png"], self.targets("![alt](img.png)\n"))
        self.assertEqual(["y.md"], self.targets("[outer [inner](y.md)]\n"))
        self.assertEqual(["a.png", "b.md"], self.targets("[![badge](a.png)](b.md)\n"))
        self.assertEqual([], self.targets("[a\n\nb](x.md)\n"))

    def test_inner_link_title_with_open_bracket_does_not_hide_outer_target(self):
        text = '[![badge](a.png "Build [status")](missing.md)\n'
        self.assertEqual([(1, "a.png"), (1, "missing.md")], validate_project.markdown_links(text))
        self.write("docs/a.png", b"\x89PNG")
        self.write("docs/guide.md", text)
        self.assert_one_error("docs/guide.md:1", "broken link: missing.md")
        self.assertEqual(["d.md", "x.md"], self.targets('[see [doc](d.md "a [b") here](x.md)\n'))

    def test_titles_and_unclosed_links(self):
        self.assertEqual(["z.md"], self.targets('[t](z.md "title")\n'))
        self.assertEqual(["q.md", "z.md"], self.targets("[t](z.md 'title') [s](q.md (title))\n"))
        self.assertEqual(["w.md"], self.targets("[t](z.md [u](w.md)\n"))

    def test_deeply_nested_brackets_do_not_recurse(self):
        depth = 1200
        self.assertEqual([(1, "ok.md")], validate_project.markdown_links("[" * depth + "x" + "]" * depth + "(ok.md)"))
        nested = "[" * depth + "x" + "](d.md)" * depth
        self.assertEqual(["d.md"] * depth, self.targets(nested))
        self.assertEqual(["ok.md"], self.targets("[" * 5000 + "[a](ok.md)"))

    def test_code_is_not_scanned_and_lines_are_counted(self):
        text = "Intro\n`[c](code.md)`\n```\n[d](fence.md)\n```\n[first\nlabel](one.md)\n\ntext [two](two.md)\n"
        self.assertEqual([(6, "one.md"), (9, "two.md")], validate_project.markdown_links(text))

    def test_title_may_follow_any_line_ending(self):
        for newline in ("\r\n", "\r", "\n"):
            with self.subTest(newline=repr(newline)):
                text = '[guide](guide.md{0} "Manual")'.format(newline)
                self.assertEqual([(1, "guide.md")], validate_project.markdown_links(text))

    def test_label_line_of_only_inline_code_is_not_blank(self):
        text = "[See\n`command`\nusage](missing.md)\n"
        self.assertEqual([(1, "missing.md")], validate_project.markdown_links(text))
        self.assertEqual([], self.targets("[See\n`command`\n\nusage](missing.md)\n"))

    def test_title_does_not_cross_a_blank_line(self):
        text = '[draft](old.md "unfinished\n\nRead [manual](missing.md) (")\n'
        self.assertEqual([(3, "missing.md")], validate_project.markdown_links(text))

    def test_trailing_backslash_does_not_join_paragraphs(self):
        self.assertEqual([], self.targets("[first\\\n\nsecond](ghost.md)\n"))
        self.assertEqual([(1, "ok.md")], validate_project.markdown_links("[first\\\nsecond](ok.md)\n"))

    def test_crlf_file_reports_only_the_broken_link(self):
        self.write("docs/workflow.md", "# Workflow\n")
        text = "Intro\r\n\r\n[flow](workflow.md)\r\n[gone](missing.md\r\n \"Title\")\r\n"
        self.write("docs/guide.md", text)
        self.assert_one_error("docs/guide.md:4", "broken link: missing.md")

    def test_reference_definition_label_is_not_an_inline_link(self):
        self.assertEqual(["a[x](y.md)"], self.targets("[r]: a[x](y.md)\n"))

    def test_output_survives_a_legacy_code_page(self):
        self.write("docs/guide.md", "[x](中.md)\n")
        stream = io.TextIOWrapper(io.BytesIO(), encoding="cp1252")
        with mock.patch.object(validate_project, "ROOT", self.root):
            with mock.patch.object(sys, "stdout", stream):
                code = validate_project.main([])
        stream.flush()
        self.assertEqual(1, code)
        output = stream.buffer.getvalue().decode("utf-8")
        self.assertIn("broken link", output)
        self.assertIn("中.md", output)

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
