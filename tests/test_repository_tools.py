import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import create_project
import package_maya_tool
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
