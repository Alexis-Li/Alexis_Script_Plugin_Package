import pathlib
import unittest


class PackageLayoutTests(unittest.TestCase):
    def test_module_file_exists(self):
        project = pathlib.Path(__file__).resolve().parents[1]
        self.assertTrue(any((project / "package").glob("*.mod")))


if __name__ == "__main__":
    unittest.main()
