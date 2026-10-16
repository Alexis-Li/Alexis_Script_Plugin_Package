"""Integration checks for project gitlinks and parent validation boundaries."""

import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import _repo_tools  # noqa: E402
import submodule_registry  # noqa: E402
import validate_repository  # noqa: E402


class SubmoduleRegistryTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT.parent / ".tmp"
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name).resolve()
        self.git(self.root, "init", "-q")
        self.path = "substance-painter/plugins/SampleSync"
        self.child = self.root / self.path
        self.child.mkdir(parents=True)
        self.git(self.child, "init", "-q")
        (self.child / "content.txt").write_text("independent product\n", encoding="utf-8")
        self.git(self.child, "add", ".")
        self.git(self.child, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.test",
                 "commit", "-qm", "initial")
        self.oid = self.git(self.child, "rev-parse", "HEAD").strip()

    @staticmethod
    def git(root, *args):
        return subprocess.run(
            ["git", "-C", str(root), *args], check=True, capture_output=True, text=True
        ).stdout

    def register(self, path=None, url="https://example.test/SampleSync.git"):
        path = self.path if path is None else path
        self.git(self.root, "config", "-f", ".gitmodules", "submodule.sample.path", path)
        self.git(self.root, "config", "-f", ".gitmodules", "submodule.sample.url", url)

    def pin(self):
        self.git(self.root, "update-index", "--add", "--cacheinfo",
                 f"160000,{self.oid},{self.path}")

    def inspect(self):
        errors, warnings = [], []
        excluded = submodule_registry.registered_submodules(self.root, errors, warnings)
        return excluded, errors, warnings

    def test_initialized_product_is_owned_by_its_repository(self):
        self.register()
        self.pin()
        (self.child / "README.md").write_text("C" + ":/local-user/private", encoding="utf-8")
        excluded, errors, warnings = self.inspect()
        self.assertEqual([], errors)
        self.assertEqual([], warnings)
        self.assertEqual(frozenset({self.child}), excluded)
        files = list(validate_repository._iter_files(self.root, excluded))
        self.assertFalse(any(path.is_relative_to(self.child) for path in files))
        validate_repository._validate_substance_painter(self.root, errors, excluded)
        validate_repository._validate_nested_git(self.root, errors, excluded)
        self.assertEqual([], errors)

    def test_uninitialized_checkout_keeps_parent_validation_available(self):
        self.register()
        self.pin()
        self.assertTrue(self.child.is_relative_to(self.root))
        saved = self.root / ".tmp"
        saved.mkdir()
        self.child.rename(saved / "checkout")
        excluded, errors, warnings = self.inspect()
        self.assertEqual([], errors)
        self.assertIn(self.child, excluded)
        self.assertEqual([f"submodule not initialized: {self.path}"], warnings)

    def test_gitlink_requires_registration(self):
        self.pin()
        _, errors, _ = self.inspect()
        self.assertIn(f"gitlink missing .gitmodules registration: {self.path}", errors)

    def test_registration_requires_a_gitlink(self):
        self.register()
        excluded, errors, _ = self.inspect()
        self.assertFalse(excluded)
        self.assertIn(f"submodule registration has no staged gitlink: {self.path}", errors)

    def test_moved_head_requires_explicit_pin_update(self):
        self.register()
        self.pin()
        self.git(self.child, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.test",
                 "commit", "--allow-empty", "-qm", "next")
        _, errors, _ = self.inspect()
        self.assertIn(f"submodule HEAD differs from staged pin: {self.path}", errors)

    def test_unsafe_paths_and_urls_are_rejected(self):
        self.pin()
        for path in ("../SampleSync", "composite/SampleSync/unreal/Component",
                     "substance-painter/plugins/Bad_Name"):
            with self.subTest(path=path):
                self.register(path=path)
                excluded, errors, _ = self.inspect()
                self.assertFalse(excluded)
                self.assertIn("submodule must occupy a complete project path: sample", errors)
        for url in ("file:///local/repo", "https://token@example.test/repo",
                    "https://example.test/repo?token=secret"):
            with self.subTest(url=url):
                self.register(url=url)
                excluded, errors, _ = self.inspect()
                self.assertFalse(excluded)
                self.assertTrue(any("credential-free HTTPS" in error for error in errors))

    def test_duplicate_paths_are_rejected(self):
        self.register()
        self.pin()
        self.git(self.root, "config", "-f", ".gitmodules", "submodule.other.path", self.path)
        self.git(self.root, "config", "-f", ".gitmodules", "submodule.other.url",
                 "https://example.test/Other.git")
        _, errors, _ = self.inspect()
        self.assertIn(f"duplicate submodule path: {self.path}", errors)

    def test_ordinary_directory_cannot_impersonate_a_submodule(self):
        self.register()
        self.pin()
        self.assertTrue((self.child / ".git").is_relative_to(self.root))
        saved = self.root / ".tmp"
        saved.mkdir()
        (self.child / ".git").rename(saved / "git-data")
        excluded, errors, _ = self.inspect()
        self.assertFalse(excluded)
        self.assertIn(f"invalid submodule checkout: {self.path}", errors)

    def test_unregistered_git_file_is_rejected(self):
        errors = []
        fake = self.root / "unregistered"
        fake.mkdir()
        (fake / ".git").write_text("gitdir: elsewhere\n", encoding="utf-8")
        validate_repository._validate_nested_git(self.root, errors)
        self.assertIn("nested Git repository: unregistered/.git", errors)

    def test_collection_tools_do_not_modify_independent_products(self):
        self.register()
        self.pin()
        with patch.object(_repo_tools, "ROOT", self.root):
            with self.assertRaisesRegex(ValueError, "independent submodule"):
                _repo_tools.resolve_component("SampleSync", (self.child,))

    def test_collection_tools_refuse_unregistered_nested_products(self):
        with patch.object(_repo_tools, "ROOT", self.root):
            with self.assertRaisesRegex(ValueError, "independent submodule"):
                _repo_tools.resolve_component("SampleSync", (self.child,))


if __name__ == "__main__":
    unittest.main()
