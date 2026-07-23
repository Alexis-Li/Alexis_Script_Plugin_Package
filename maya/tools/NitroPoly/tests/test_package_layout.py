import pathlib
import unittest


class PackageLayoutTests(unittest.TestCase):
    def test_runtime_files_exist(self):
        project = pathlib.Path(__file__).resolve().parents[1]
        scripts = project / "scripts"
        self.assertTrue((scripts / "NitroPoly.py").is_file())


if __name__ == "__main__":
    unittest.main()
