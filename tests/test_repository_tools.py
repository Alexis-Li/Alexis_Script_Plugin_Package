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
        result = create_project.create("maya-script", "preview-script", apply=False)
        self.assertEqual("dry-run", result["mode"])
        self.assertFalse((ROOT / "maya" / "scripts" / "preview-script").exists())
        self.assertIn("maya/scripts/preview-script/README_CN.md", result["files"])

    def test_all_project_templates_include_bilingual_readmes(self):
        cases = (
            ("maya-script", "sample-script", "maya/scripts/sample-script"),
            ("maya-tool", "sample-tool", "maya/tools/sample-tool"),
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
        result = package_maya_tool.package("flatten-mesh-to-uv", ROOT / "releases", apply=False)
        self.assertEqual("dry-run", result["mode"])
        self.assertGreater(result["file_count"], 0)

    def test_version_update_defaults_to_preview(self):
        module_file = (
            ROOT
            / "maya"
            / "tools"
            / "flatten-mesh-to-uv"
            / "package"
            / "FlattenMeshToUV.mod"
        )
        before = module_file.read_text(encoding="utf-8")
        result = update_versions.update("maya", "flatten-mesh-to-uv", "2.0.2", apply=False)
        self.assertEqual("dry-run", result["mode"])
        self.assertEqual(before, module_file.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
