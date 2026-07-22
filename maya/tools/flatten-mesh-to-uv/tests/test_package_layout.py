import pathlib
import unittest


class PackageLayoutTests(unittest.TestCase):
    def test_runtime_files_exist(self):
        project = pathlib.Path(__file__).resolve().parents[1]
        self.assertTrue((project / "package" / "FlattenMeshToUV.mod").is_file())
        self.assertTrue((project / "package" / "FlattenMeshToUV" / "plug-ins" / "FlattenMeshToUV.py").is_file())
        self.assertTrue((project / "package" / "FlattenMeshToUV" / "scripts" / "flatten_mesh_to_uv" / "bootstrap.py").is_file())


if __name__ == "__main__":
    unittest.main()
