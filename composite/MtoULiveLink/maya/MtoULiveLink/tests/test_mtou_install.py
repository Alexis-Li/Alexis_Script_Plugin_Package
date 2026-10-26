# ruff: noqa: E402
"""Drag-and-drop installer: the `.mod`, the copy, conflicts and activation."""

import os
import pathlib
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

COMPONENT = pathlib.Path(__file__).resolve().parents[1]
SCRIPTS = COMPONENT / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from mtou_livelink import config as mtou_config
from mtou_livelink import install


class InstallTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = pathlib.Path(self._tmp.name)
        self.app = self.base / "maya-app"
        self.modules = self.app / "modules"
        self.user_scripts = self.app / "scripts"
        self.user_scripts.mkdir(parents=True)

    def tearDown(self):
        self._tmp.cleanup()

    def component(self, relative):
        root = self.base / relative
        shutil.copytree(SCRIPTS, root / "scripts",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copy2(COMPONENT / "install.mel", root / "install.mel")
        return root

    def plan(self, root):
        return install.plan_install(str(root), str(self.modules), [str(self.user_scripts)],
                                    [str(self.modules)])

    def test_ascii_path_registers_the_extracted_folder(self):
        root = self.component("Tools/MtoU 0.10")
        plan = self.plan(root)
        install.apply_install(plan)

        text = (self.modules / "MtoULiveLink.mod").read_text(encoding="ascii")
        self.assertEqual("+ MtoULiveLink {0} {1}\n".format(mtou_config.__version__,
                                                           root.as_posix()), text)
        self.assertIsNone(plan.copy_dir)
        self.assertEqual(os.path.normcase(str(root / "scripts")),
                         os.path.normcase(os.path.normpath(plan.scripts_dir)))
        self.assertFalse((self.modules / "MtoULiveLink").exists())

    def test_non_ascii_path_is_copied_into_modules_by_relative_path(self):
        root = self.component("发布 目录/MtoULiveLink")
        plan = self.plan(root)
        install.apply_install(plan)

        copy = self.modules / "MtoULiveLink"
        self.assertEqual("+ MtoULiveLink {0} MtoULiveLink\n".format(mtou_config.__version__),
                         (self.modules / "MtoULiveLink.mod").read_text(encoding="ascii"))
        self.assertTrue((copy / "scripts" / "MtoULiveLink.py").is_file())
        self.assertTrue((copy / "scripts" / "mtou_livelink" / "install.py").is_file())
        self.assertTrue((copy / install.COPY_MARKER).is_file())
        self.assertFalse((copy / "install.mel").exists())

        # Reinstalling replaces the copy instead of merging into it.
        (copy / "scripts" / "stale.py").write_text("", encoding="utf-8")
        plan = self.plan(root)
        self.assertEqual([], plan.conflicts)
        install.apply_install(plan)
        self.assertFalse((copy / "scripts" / "stale.py").exists())

    def test_ascii_install_removes_an_earlier_managed_copy(self):
        install.apply_install(self.plan(self.component("中文/MtoULiveLink")))
        root = self.component("plain/MtoULiveLink")
        plan = self.plan(root)
        self.assertTrue(plan.remove_copy)
        install.apply_install(plan)
        self.assertFalse((self.modules / "MtoULiveLink").exists())
        self.assertIn(root.as_posix(), (self.modules / "MtoULiveLink.mod").read_text())

    def test_folder_extracted_into_modules_uses_its_relative_name(self):
        self.modules.mkdir()
        app = self.base / "用户" / "maya"
        modules = app / "modules"
        root = self.component("用户/maya/modules/MtoULiveLink")
        plan = install.plan_install(str(root), str(modules), [], [str(modules)])
        self.assertIsNone(plan.copy_dir)
        self.assertEqual("+ MtoULiveLink {0} MtoULiveLink\n".format(mtou_config.__version__),
                         plan.mod_text)
        self.assertEqual([], plan.conflicts)

    def test_incomplete_component_is_refused_before_any_write(self):
        root = self.component("broken/MtoULiveLink")
        shutil.rmtree(root / "scripts" / "mtou_livelink")
        with self.assertRaisesRegex(install.InstallError, "scripts/mtou_livelink/__init__.py"):
            self.plan(root)
        self.assertFalse(self.modules.exists())

    def test_earlier_installs_are_conflicts_deleted_without_a_backup(self):
        (self.user_scripts / "MtoULiveLink.py").write_text("old", encoding="utf-8")
        (self.user_scripts / "mtou_livelink").mkdir()
        other_modules = self.app / "2024" / "modules"
        other_modules.mkdir(parents=True)
        (other_modules / "Old.mod").write_text(
            "+ MAYAVERSION:2024 PLATFORM:win64 MtoULiveLink 0.9.0 old/MtoULiveLink\n",
            encoding="utf-8")
        (other_modules / "Other.mod").write_text("+ OtherTool 1.0 other\n",
                                                 encoding="utf-8")
        root = self.component("new/MtoULiveLink")
        plan = install.plan_install(str(root), str(self.modules), [str(self.user_scripts)],
                                    [str(self.modules), str(other_modules)])
        self.assertEqual(
            sorted([self.user_scripts / "MtoULiveLink.py", self.user_scripts / "mtou_livelink",
                    other_modules / "Old.mod"]),
            sorted(pathlib.Path(path) for path in plan.conflicts))

        removed = install.apply_install(plan)
        self.assertEqual(plan.conflicts, removed)
        self.assertFalse((self.user_scripts / "MtoULiveLink.py").exists())
        self.assertFalse((self.user_scripts / "mtou_livelink").exists())
        self.assertFalse((other_modules / "Old.mod").exists())
        self.assertTrue((other_modules / "Other.mod").exists())
        self.assertEqual(["MtoULiveLink.mod"], sorted(path.name for path in self.modules.iterdir()))
        self.assertEqual([], sorted(self.app.glob("*backup*")))

    def test_component_inside_the_copy_target_is_refused(self):
        root = self.component("maya-app/modules/MtoULiveLink/发布/MtoULiveLink")
        with self.assertRaisesRegex(install.InstallError, "Extract the Maya component outside"):
            self.plan(root)
        self.assertFalse((self.modules / "MtoULiveLink.mod").exists())

    def test_reinstall_has_no_conflict_with_its_own_mod(self):
        root = self.component("same/MtoULiveLink")
        install.apply_install(self.plan(root))
        self.assertEqual([], self.plan(root).conflicts)

    def test_an_unmanaged_modules_folder_is_a_conflict_for_a_copy(self):
        (self.modules / "MtoULiveLink" / "scripts").mkdir(parents=True)
        plan = self.plan(self.component("中文/MtoULiveLink"))
        self.assertEqual([str(self.modules / "MtoULiveLink")], plan.conflicts)

    def test_declared_module_skips_conditions(self):
        self.assertEqual(["MtoULiveLink", "Other"], install.declared_module(
            "+ MAYAVERSION:2024 MtoULiveLink 1.0 tools/a\nscripts: s\n"
            "+ Other any tools/b\n- Disabled 1.0 tools/c\n"))

    def test_shelf_commands_that_open_the_tool(self):
        self.assertTrue(install.opens_tool(install.SHELF_COMMAND))
        self.assertTrue(install.opens_tool("import MtoULiveLink; MtoULiveLink.main()"))
        self.assertFalse(install.opens_tool("import MtoULiveLink"))
        self.assertFalse(install.opens_tool(None))
        shelves = self.app / "shelves"
        shelves.mkdir()
        (shelves / "shelf_Custom.mel").write_text("shelfButton -command \"print(1)\";",
                                                  encoding="utf-8")
        self.assertFalse(install.saved_shelf_has_button(str(shelves)))
        (shelves / "shelf_MtoU.mel").write_text(
            'shelfButton -command "import MtoULiveLink\\nMtoULiveLink.run()" '
            '-sourceType "python";', encoding="utf-8")
        self.assertTrue(install.saved_shelf_has_button(str(shelves)))
        self.assertFalse(install.saved_shelf_has_button(str(self.base / "missing")))

    def test_activation_adds_the_module_scripts_when_nothing_is_loaded(self):
        plan = self.plan(self.component("act/MtoULiveLink"))
        install.apply_install(plan)
        path = ["other"]
        self.assertIsNone(install.activate(plan, modules={}, path=path))
        self.assertEqual(plan.scripts_dir, path[0])
        install.activate(plan, modules={}, path=path)
        self.assertEqual(2, len(path))

    def test_restart_unless_the_loaded_code_matches_the_installed_content(self):
        plan = self.plan(self.component("中文/MtoULiveLink"))
        install.apply_install(plan)
        installed = install.content_digest(plan.scripts_dir)
        entry = os.path.join(plan.scripts_dir, "MtoULiveLink.py")

        def package(digest, folder=plan.scripts_dir):
            return types.SimpleNamespace(
                __file__=os.path.join(folder, "mtou_livelink", "__init__.py"),
                LOADED_DIGEST=digest)

        # The same content reinstalled into the same copy needs no restart.
        same = {"MtoULiveLink": types.SimpleNamespace(__file__=entry),
                "mtou_livelink": package(installed)}
        self.assertIsNone(install.activate(plan, modules=same, path=[]))
        # A new version written into the same copy is not what the session runs.
        older = {"MtoULiveLink": types.SimpleNamespace(__file__=entry),
                 "mtou_livelink": package("0" * 64)}
        self.assertEqual(entry, install.activate(plan, modules=older, path=[]))
        # A release without a recorded digest (0.9.0) is never trusted.
        legacy = {"MtoULiveLink": types.SimpleNamespace(__file__=entry)}
        self.assertEqual(entry, install.activate(plan, modules=legacy, path=[]))
        elsewhere = {"mtou_livelink": package(installed, str(self.user_scripts))}
        self.assertEqual(elsewhere["mtou_livelink"].__file__,
                         install.activate(plan, modules=elsewhere, path=[]))

    def test_the_package_records_the_digest_of_the_code_it_loaded(self):
        import mtou_livelink
        self.assertEqual(install.content_digest(str(SCRIPTS)), mtou_livelink.LOADED_DIGEST)

    def test_content_digest_covers_the_entry_and_package_only(self):
        root = self.component("digest/MtoULiveLink")
        scripts = root / "scripts"
        digest = install.content_digest(str(scripts))
        (scripts / "unrelated.py").write_text("x = 1\n", encoding="utf-8")
        (scripts / "mtou_livelink" / "__pycache__").mkdir()
        (scripts / "mtou_livelink" / "__pycache__" / "config.cpython-37.pyc").write_bytes(b"x")
        self.assertEqual(digest, install.content_digest(str(scripts)))
        with open(scripts / "mtou_livelink" / "linkage.py", "a", encoding="utf-8") as handle:
            handle.write("\n")
        self.assertNotEqual(digest, install.content_digest(str(scripts)))

    def test_package_missing_an_imported_module_is_refused_before_any_write(self):
        (self.user_scripts / "MtoULiveLink.py").write_text("old", encoding="utf-8")
        for relative in ("mtou_livelink/linkage.py", "mtou_livelink/ui/style.py",
                         "mtou_livelink/ui/__init__.py"):
            root = self.component("partial-" + relative.replace("/", "-") + "/MtoULiveLink")
            (root / "scripts" / relative).unlink()
            with self.assertRaisesRegex(install.InstallError, "scripts/" + relative):
                self.plan(root)
        root = self.component("syntax/MtoULiveLink")
        (root / "scripts" / "mtou_livelink" / "camera.py").write_text("def broken(:\n",
                                                                      encoding="utf-8")
        with self.assertRaisesRegex(install.InstallError, "scripts/mtou_livelink/camera.py"):
            self.plan(root)
        self.assertEqual("old", (self.user_scripts / "MtoULiveLink.py").read_text())
        self.assertFalse(self.modules.exists())

    def test_complete_package_has_no_missing_runtime_file(self):
        self.assertEqual([], install.missing_runtime_files(str(SCRIPTS)))

    def test_source_inside_a_conflict_is_refused_before_confirmation(self):
        legacy = self.user_scripts / "mtou_livelink"
        legacy.mkdir()
        (legacy / "__init__.py").write_text("", encoding="utf-8")
        for relative in ("scripts/mtou_livelink/release", "scripts/mtou_livelink/发布/MtoU"):
            root = self.component("maya-app/" + relative)
            before = sorted(str(path) for path in self.app.rglob("*"))
            with self.assertRaisesRegex(install.InstallError, "Extract the Maya component"):
                self.plan(root)
            self.assertEqual(before, sorted(str(path) for path in self.app.rglob("*")))
        self.assertFalse(self.modules.exists())

    def test_moving_a_managed_copy_to_an_ascii_path_is_no_conflict(self):
        copied = self.plan(self.component("中文/MtoULiveLink"))
        install.apply_install(copied)
        root = self.component("plain/MtoULiveLink")
        plan = install.plan_install(str(root), str(self.modules),
                                    [str(self.user_scripts), copied.scripts_dir],
                                    [str(self.modules)])
        self.assertEqual([], plan.conflicts)
        self.assertTrue(plan.remove_copy)

    def test_batch_install_refuses_to_delete_without_explicit_consent(self):
        legacy = self.user_scripts / "MtoULiveLink.py"
        legacy.write_text("old", encoding="utf-8")
        root = self.component("batch/MtoULiveLink")
        dialogs = []
        cmds = types.SimpleNamespace(
            about=lambda batch: True,
            confirmDialog=lambda **kwargs: dialogs.append(kwargs) or "OK",
            internalVar=lambda **kwargs: {
                "userAppDir": str(self.app) + "/",
                "userScriptDir": str(self.app / "2024" / "scripts") + "/"}[next(iter(kwargs))])
        maya = types.ModuleType("maya")
        maya.cmds = cmds
        maya.mel = types.SimpleNamespace(eval=lambda text: None)
        fake = {"maya": maya, "maya.cmds": cmds, "maya.mel": maya.mel}
        with mock.patch.dict(sys.modules, fake), \
                mock.patch.dict(os.environ, {"MAYA_MODULE_PATH": ""}), \
                mock.patch("builtins.print"):
            refused = install.install_from_maya(str(root))
            self.assertFalse(refused["installed"])
            self.assertEqual([str(legacy)], refused["conflicts"])
            self.assertEqual("old", legacy.read_text())
            self.assertFalse(self.modules.exists())

            accepted = install.install_from_maya(str(root), delete_conflicts=True)
        self.assertTrue(accepted["installed"])
        self.assertEqual([str(legacy)], accepted["removed"])
        self.assertFalse(legacy.exists())
        self.assertEqual([], dialogs)

    def test_install_mel_is_ascii_and_loads_the_packaged_installer(self):
        text = (COMPONENT / "install.mel").read_bytes().decode("ascii")
        self.assertIn("whatIs mtouInstallLocator", text)
        self.assertIn("global proc mtouInstallLocator", text)
        self.assertIn("'scripts', 'mtou_livelink', 'install.py'", text)
        self.assertIn("install_from_maya(root)", text)


if __name__ == "__main__":
    unittest.main()
