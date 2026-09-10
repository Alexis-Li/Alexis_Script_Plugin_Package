# ruff: noqa: E402

import importlib.util
import pathlib
import sys
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import _repo_tools
import create_project
import package_maya_tool
import package_unreal_plugin
import update_versions
import validate_repository

CONFORMANCE_GENERATOR = (
    ROOT / "composite" / "MtoULiveLink" / "protocol" / "generate_unreal_corpus.py"
)


class RepositoryToolTests(unittest.TestCase):
    def test_mtou_unreal_conformance_data_is_current(self):
        spec = importlib.util.spec_from_file_location(
            "generate_mtou_unreal_corpus", str(CONFORMANCE_GENERATOR)
        )
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual([], module.check_generated())

    def test_mtou_conformance_validation_errors_are_deterministic(self):
        spec = importlib.util.spec_from_file_location(
            "generate_mtou_unreal_corpus_invalid", str(CONFORMANCE_GENERATOR)
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        invalid = {
            "schema_version": 1,
            "protocol_version": 4,            "cases": [
                {
                    "id": "duplicate",
                    "operation": "init",
                    "applies_to": ["unreal"],
                    "payload": {},
                    "expected": {"accepted": True, "close": False},
                },
                {
                    "id": "duplicate",
                    "operation": "unknown",
                    "applies_to": [],
                    "expected": {"accepted": False, "close": True},
                },
            ],
            "limits": {
                "max_message_bytes": 33554432,
                "max_cache_payload_bytes": 1073741824,
                "max_cache_frame_count": 20000,
            },
        }
        self.assertEqual(
            [
                "protocol_version must equal 6",
                "case 1 id is duplicated: duplicate",
                "case 1 operation is unsupported",
                "case 1 applies_to must contain maya and/or unreal",
                "case 1 must define exactly one input source",
                "case 1 rejected case needs a stable parser error_code",
            ],
            module.validate_corpus(invalid),
        )

    def _create_composite_fixture(self, root):
        project = root / "composite" / "SampleBridge"
        for metadata in ("README.md", "README_CN.md", "CHANGELOG.md", "LICENSE"):
            (project / metadata).parent.mkdir(parents=True, exist_ok=True)
            (project / metadata).write_text(metadata, encoding="utf-8")

        maya_component = project / "maya" / "SampleBridge"
        (maya_component / "scripts").mkdir(parents=True)
        (maya_component / "scripts" / "SampleBridge.py").write_text(
            '__version__ = "1.0.0"\n', encoding="utf-8"
        )

        unreal_component = project / "unreal" / "SampleBridge"
        unreal_component.mkdir(parents=True)
        (unreal_component / "SampleBridge.uplugin").write_text("{}", encoding="utf-8")
        return project, maya_component, unreal_component

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
        maya_result = package_maya_tool.package("MtoULiveLink", ROOT / "releases", apply=False)
        unreal_result = package_unreal_plugin.package(
            "MtoULiveLink", "5.7", ROOT / "releases", apply=False
        )
        self.assertEqual("releases/MtoULiveLink-0.5.0.zip", maya_result["archive"])
        self.assertEqual(
            "releases/MtoULiveLink-0.5.0-UE5.7.zip",
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

    def test_composite_components_inherit_root_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self._create_composite_fixture(root)
            errors = []
            validate_repository._validate_composite(root, errors)
            self.assertEqual([], errors)

    def test_composite_components_reject_duplicate_metadata(self):
        for host_name, component_index in (("maya", 1), ("unreal", 2)):
            for metadata in ("README.md", "README_CN.md", "CHANGELOG.md", "LICENSE"):
                with self.subTest(host=host_name, metadata=metadata):
                    with tempfile.TemporaryDirectory() as directory:
                        root = pathlib.Path(directory)
                        fixture = self._create_composite_fixture(root)
                        component = fixture[component_index]
                        (component / metadata).write_text("duplicate", encoding="utf-8")
                        errors = []
                        validate_repository._validate_composite(root, errors)
                        self.assertIn(
                            "Composite component SampleBridge/{0}/SampleBridge must not "
                            "contain {1}; metadata belongs at the project root".format(
                                host_name, metadata
                            ),
                            errors,
                        )

    def test_composite_components_allow_local_instructions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            _, maya_component, unreal_component = self._create_composite_fixture(root)
            for component in (maya_component, unreal_component):
                (component / "AGENTS.md").write_text("# Local host checks\n", encoding="utf-8")
            errors = []
            validate_repository._validate_composite(root, errors)
            self.assertEqual([], errors)

    def test_shelf_script_allows_tests_but_rejects_runtime_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            project = root / "maya" / "scripts" / "SampleScript"
            (project / "tests").mkdir(parents=True)
            (project / "SampleScript.py").write_text("def run(): pass\n", encoding="utf-8")
            for metadata in ("README.md", "README_CN.md"):
                (project / metadata).write_text("Usage\n", encoding="utf-8")
            (project / "tests" / "test_sample.py").write_text(
                "# Regression checks\n", encoding="utf-8"
            )
            errors = []
            validate_repository._validate_shelf_scripts(root, errors)
            self.assertEqual([], errors)
            (project / "runtime").mkdir()
            validate_repository._validate_shelf_scripts(root, errors)
            self.assertEqual(["shelf script SampleScript contains directories: runtime"], errors)

    def test_legacy_maya_tool_still_requires_its_own_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            project = root / "maya" / "tools" / "LegacyTool"
            (project / "scripts").mkdir(parents=True)
            (project / "scripts" / "LegacyTool.py").write_text(
                '__version__ = "1.0.0"\n', encoding="utf-8"
            )
            errors = []
            validate_repository._validate_maya_project(project, errors, root)
            for metadata in ("README.md", "README_CN.md", "CHANGELOG.md", "LICENSE"):
                self.assertIn("Maya tool LegacyTool is missing {0}".format(metadata), errors)

    def test_legacy_unreal_plugin_still_requires_its_own_readmes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            plugin = root / "unreal" / "Plugins" / "LegacyPlugin"
            plugin.mkdir(parents=True)
            (plugin / "LegacyPlugin.uplugin").write_text("{}", encoding="utf-8")
            errors = []
            validate_repository._validate_unreal_plugin(plugin, errors, root)
            self.assertIn("Unreal plugin LegacyPlugin is missing README.md", errors)
            self.assertIn("Unreal plugin LegacyPlugin is missing README_CN.md", errors)

    def test_composite_packages_contain_only_component_files(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            maya_result = package_maya_tool.package("MtoULiveLink", output, apply=True)
            unreal_result = package_unreal_plugin.package(
                "MtoULiveLink", "5.7", output, apply=True
            )

            with zipfile.ZipFile(output / pathlib.Path(maya_result["archive"]).name) as archive:
                self.assertEqual(["scripts/MtoULiveLink.py"], archive.namelist())

            with zipfile.ZipFile(output / pathlib.Path(unreal_result["archive"]).name) as archive:
                names = archive.namelist()
            self.assertIn("MtoULiveLink/MtoULiveLink.uplugin", names)
            self.assertIn(
                "MtoULiveLink/Source/MtoULiveLink/Private/Tests/"
                "MtoULiveLinkTests.cpp",
                names,
            )
            forbidden_names = {
                "README.md",
                "README_CN.md",
                "CHANGELOG.md",
                "LICENSE",
                "AGENTS.md",
                "ICON.md",
                ".gitkeep",
            }
            self.assertFalse(
                [name for name in names if pathlib.PurePosixPath(name).name in forbidden_names]
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
            ROOT / "maya" / "tools" / "FlattenMeshToUV" / "plug-ins" / "FlattenMeshToUV.py"
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
