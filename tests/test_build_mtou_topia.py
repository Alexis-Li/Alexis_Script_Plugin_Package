import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "build_mtou_topia.ps1"


class BuildMtoUTopiaScriptTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("pwsh"), "PowerShell is required")
    def test_json_dry_run_validates_without_writing_binaries(self):
        self.check_dry_run("pwsh")

    @unittest.skipUnless(shutil.which("powershell.exe"), "Windows PowerShell is required")
    def test_windows_powershell_dry_run_without_powershell_7(self):
        self.check_dry_run("powershell.exe")

    def check_dry_run(self, executable):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            engine_root = root / "engine-root"
            engine = engine_root / "Engine"
            (engine / "Build" / "BatchFiles").mkdir(parents=True)
            (engine / "Build" / "BatchFiles" / "Build.bat").write_text("", encoding="utf-8")
            (engine / "Binaries" / "Win64").mkdir(parents=True)
            (engine / "Binaries" / "Win64" / "UnrealEditor.modules").write_text(
                '{"BuildId":"fixture","Modules":{}}', encoding="utf-8"
            )

            project = root / "project"
            project.mkdir()
            project_file = project / "Fixture.uproject"
            project_file.write_text('{"FileVersion":3}', encoding="utf-8")
            plugin = project / "Plugins" / "MtoULiveLink"
            plugin.mkdir(parents=True)
            (plugin / "MtoULiveLink.uplugin").write_text(
                '{"FileVersion":3}', encoding="utf-8"
            )

            completed = subprocess.run(
                [
                    executable,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(SCRIPT),
                    "-EngineRoot",
                    str(engine_root),
                    "-ProjectFile",
                    str(project_file),
                    "-Json",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(0, completed.returncode, completed.stderr)
            result = json.loads(completed.stdout)
            self.assertTrue(result["ok"])
            self.assertEqual("dry-run", result["mode"])
            self.assertEqual(str(plugin), result["plugin_root"])
            self.assertFalse((plugin / "Binaries").exists())


if __name__ == "__main__":
    unittest.main()
