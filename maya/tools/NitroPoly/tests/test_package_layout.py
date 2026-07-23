import pathlib
import unittest


class PackageLayoutTests(unittest.TestCase):
    def test_runtime_files_exist(self):
        project = pathlib.Path(__file__).resolve().parents[1]
        scripts = project / "scripts"
        runtime = scripts / "NitroPoly.py"
        self.assertTrue(runtime.is_file())
        self.assertTrue(
            runtime.read_text(encoding="utf-8").rstrip().endswith(
                'if __name__ == "__main__":\n    main()'
            )
        )
        runtime.read_bytes().decode("ascii")
        self.assertFalse((scripts / "NitroPolyStart.py").exists())


if __name__ == "__main__":
    unittest.main()
