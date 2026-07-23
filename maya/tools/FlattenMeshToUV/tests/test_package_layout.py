import pathlib
import unittest


class PackageLayoutTests(unittest.TestCase):
    def test_runtime_files_exist(self):
        project = pathlib.Path(__file__).resolve().parents[1]
        self.assertTrue((project / "plug-ins" / "FlattenMeshToUV.py").is_file())
        self.assertTrue((project / "scripts" / "RunFlattenMeshToUV.py").is_file())


if __name__ == "__main__":
    unittest.main()
