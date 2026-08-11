import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import _repo_tools
import create_project
import package_maya_tool
import package_unreal_plugin
import update_versions
import validate_repository


class RepositoryToolTests(unittest.TestCase):
    def test_repository_is_structurally_valid(self):
        result = validate_repository.validate(ROOT)
        self.assertEqual([], result["errors"])

    def test_create_project_defaults_to_preview(self):
        result = create_project.create("maya-script", "PreviewScript", apply=False)
        self.assertEqual("dry-run", result["mode"])
        self.assertFalse((ROOT / "maya" / "scripts" / "PreviewScript").exists())
        self.assertIn("maya/scripts/PreviewScript/README_CN.md", result["files"])

    def test_all_project_templates_include_bilingual_readmes(self):
        cases = (
            ("maya-script", "SampleScript", "maya/scripts/SampleScript"),
            ("maya-tool", "SampleTool", "maya/tools/SampleTool"),
            ("unreal-plugin", "sample-plugin", "unreal/Plugins/SamplePlugin"),
            (
                "standalone-python-tool",
                "sample-standalone",
                "standalone/sample-standalone",
            ),
        )
        for kind, name, target in cases:
            with self.subTest(kind=kind):
                result = create_project.create(kind, name, apply=False)
                self.assertIn(target + "/README.md", result["files"])
                self.assertIn(target + "/README_CN.md", result["files"])

    def test_maya_packages_can_be_previewed(self):
        result = package_maya_tool.package("FlattenMeshToUV", ROOT / "releases", apply=False)
        self.assertEqual("dry-run", result["mode"])
        self.assertEqual("releases/FlattenMeshToUV-2.0.1.zip", result["archive"])
        self.assertGreater(result["file_count"], 0)

    def test_component_resolver_rejects_ambiguous_locations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            first = root / "legacy" / "MtoULiveLink"
            second = root / "composite" / "MtoULiveLink"
            first.mkdir(parents=True)
            second.mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "ambiguous Maya tool"):
                _repo_tools.resolve_component("Maya tool", (first, second))

    def test_mtou_components_resolve_from_composite_project(self):
        self.assertEqual(
            pathlib.Path("composite/MtoULiveLink/maya/MtoULiveLink"),
            _repo_tools.maya_tool_path("MtoULiveLink").relative_to(ROOT),
        )
        self.assertEqual(
            pathlib.Path("composite/MtoULiveLink/unreal/MtoULiveLink"),
            _repo_tools.unreal_plugin_path("MtoULiveLink").relative_to(ROOT),
        )

    def test_composite_mtou_packages_can_be_previewed(self):
        maya_result = package_maya_tool.package(
            "MtoULiveLink", ROOT / "releases", apply=False
        )
        unreal_result = package_unreal_plugin.package(
            "MtoULiveLink", "5.7", ROOT / "releases", apply=False
        )
        self.assertEqual("releases/MtoULiveLink-0.1.0.zip", maya_result["archive"])
        self.assertEqual(
            "releases/MtoULiveLink-0.1.0-UE5.7.zip",
            unreal_result["archive"],
        )
        self.assertGreater(maya_result["file_count"], 0)
        self.assertGreater(unreal_result["file_count"], 0)

    def test_composite_project_requires_root_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            project = root / "composite" / "SampleBridge"
            (project / "maya").mkdir(parents=True)
            (project / "unreal").mkdir()
            errors = []
            validate_repository._validate_composite(root, errors)
            for required in ("README.md", "README_CN.md", "CHANGELOG.md", "LICENSE"):
                self.assertIn(
                    "Composite project SampleBridge is missing {0}".format(required),
                    errors,
                )

    def test_git_worktree_file_is_not_a_nested_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / ".git").mkdir()
            worktree_git = root / ".worktrees" / "feature" / ".git"
            worktree_git.parent.mkdir(parents=True)
            worktree_git.write_text("gitdir: elsewhere", encoding="utf-8")
            nested_git = root / "vendor" / ".git"
            nested_git.mkdir(parents=True)
            errors = []
            validate_repository._validate_nested_git(root, errors)
            self.assertEqual(["nested Git repository: vendor/.git"], errors)

    def test_version_update_defaults_to_preview(self):
        runtime_file = (
            ROOT
            / "maya"
            / "tools"
            / "FlattenMeshToUV"
            / "plug-ins"
            / "FlattenMeshToUV.py"
        )
        before = runtime_file.read_text(encoding="utf-8")
        result = update_versions.update("maya", "FlattenMeshToUV", "2.0.2", apply=False)
        self.assertEqual("dry-run", result["mode"])
        self.assertEqual(before, runtime_file.read_text(encoding="utf-8"))
        self.assertIn(
            "maya/tools/FlattenMeshToUV/plug-ins/FlattenMeshToUV.py",
            result["files"],
        )

    def test_maya_tool_template_does_not_create_module_file(self):
        result = create_project.create("maya-tool", "SampleTool", apply=False)
        self.assertFalse(any(path.endswith(".mod") for path in result["files"]))


if __name__ == "__main__":
    unittest.main()
