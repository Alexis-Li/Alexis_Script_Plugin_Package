import hashlib
import pathlib
import sys
import tempfile
import unittest

TESTS = pathlib.Path(__file__).resolve().parent
if str(TESTS) not in sys.path:
    sys.path.insert(0, str(TESTS))

import maya_prefs_template as template  # noqa: E402

DEFAULT = (
    "//Maya Preference 2024 (Release 1)\n//\n//\n"
    'evalDeferred("autoLoadPlugin(\\"\\", \\"mayaHIK\\", \\"mayaHIK\\")");\n'
    'evalDeferred("autoLoadPlugin(\\"\\", \\"modelingToolkit\\", \\"modelingToolkit\\")");\n'
    'evalDeferred("autoLoadPlugin(\\"\\", \\"ufeSupport.py\\", \\"ufeSupport\\")");\n'
    'evalDeferred("autoLoadPlugin(\\"\\", \\"MASH\\", \\"MASH\\")");\n'
)


class PrefsTemplateTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def _write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        return path

    def test_autoload_names_follow_the_file_order(self):
        self.assertEqual(template.autoload_plugins(DEFAULT),
                         ["mayaHIK", "modelingToolkit", "ufeSupport", "MASH"])

    def test_describe_finds_a_language_folder_and_hashes_the_bytes(self):
        path = self._write("app/2024/zh_CN/prefs/pluginPrefs.mel", DEFAULT)
        info = template.describe(self.root / "app")
        self.assertEqual(info["plugin_prefs"], "2024/zh_CN/prefs/pluginPrefs.mel")
        self.assertEqual(info["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(len(info["autoload"]), 4)

    def test_describe_finds_the_english_folder(self):
        self._write("app/2024/prefs/pluginPrefs.mel", DEFAULT)
        self.assertEqual(template.describe(self.root / "app")["plugin_prefs"],
                         "2024/prefs/pluginPrefs.mel")

    def test_describe_reports_a_directory_without_a_saved_list(self):
        (self.root / "app").mkdir()
        info = template.describe(self.root / "app")
        self.assertIsNone(info["plugin_prefs"])
        self.assertIsNone(info["sha256"])
        self.assertIsNone(info["autoload"])

    def test_minimal_template_keeps_only_modeling_toolkit(self):
        default = self._write("work/2024/prefs/pluginPrefs.mel", DEFAULT)
        target = template.write_template(default, "2024/prefs/pluginPrefs.mel", "minimal",
                                         self.root / "minimal")
        self.assertEqual(template.autoload_plugins(target.read_text(encoding="utf-8")),
                         ["modelingToolkit"])
        self.assertEqual(sorted(p.name for p in (self.root / "minimal").rglob("*") if p.is_file()),
                         ["pluginPrefs.mel"])

    def test_default_template_copies_the_saved_list(self):
        default = self._write("work/2024/prefs/pluginPrefs.mel", DEFAULT)
        target = template.write_template(default, "2024/prefs/pluginPrefs.mel", "maya-default",
                                         self.root / "default")
        self.assertEqual(target.read_text(encoding="utf-8"), DEFAULT)

    def test_minimal_template_refuses_a_list_without_modeling_toolkit(self):
        default = self._write("work/2024/prefs/pluginPrefs.mel",
                              DEFAULT.replace("modelingToolkit", "other"))
        with self.assertRaises(ValueError):
            template.write_template(default, "2024/prefs/pluginPrefs.mel", "minimal",
                                    self.root / "minimal")

    def test_cli_refuses_an_existing_template(self):
        (self.root / "exists").mkdir()
        with self.assertRaises(SystemExit):
            template.main(["--maya", "maya.exe", "--kind", "minimal",
                           "--out", str(self.root / "exists")])


if __name__ == "__main__":
    unittest.main()
