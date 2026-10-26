# ruff: noqa: E402
"""Drag-and-drop installer: the `.mod`, the copy, conflicts and activation."""

import datetime
import os
import pathlib
import shutil
import sys
import tempfile
import types
import unittest

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
        install.apply_install(plan, str(self.app / "backup"))

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
        install.apply_install(plan, str(self.app / "backup"))

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
        install.apply_install(plan, str(self.app / "backup"))
        self.assertFalse((copy / "scripts" / "stale.py").exists())

    def test_ascii_install_removes_an_earlier_managed_copy(self):
        install.apply_install(self.plan(self.component("中文/MtoULiveLink")),
                              str(self.app / "backup"))
        root = self.component("plain/MtoULiveLink")
        plan = self.plan(root)
        self.assertTrue(plan.remove_copy)
        install.apply_install(plan, str(self.app / "backup"))
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

    def test_earlier_installs_are_conflicts_moved_to_a_backup(self):
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

        backup = install.apply_install(plan, str(self.app / "backup"),
                                       now=datetime.datetime(2026, 10, 26, 9, 30, 0))
        self.assertEqual(self.app / "backup" / "20261026-093000", pathlib.Path(backup))
        self.assertFalse((self.user_scripts / "MtoULiveLink.py").exists())
        self.assertFalse((other_modules / "Old.mod").exists())
        self.assertTrue((other_modules / "Other.mod").exists())
        sources = (pathlib.Path(backup) / "sources.txt").read_text(encoding="utf-8")
        self.assertIn(str(self.user_scripts / "MtoULiveLink.py"), sources)
        self.assertEqual(3, len(sources.splitlines()))
        self.assertEqual("old", (pathlib.Path(backup) / "01_MtoULiveLink.py").read_text())

    def test_backups_in_the_same_second_do_not_collide(self):
        now = datetime.datetime(2026, 10, 26, 9, 30, 0)
        root = self.component("again/MtoULiveLink")
        backups = []
        for _ in range(2):
            (self.user_scripts / "MtoULiveLink.py").write_text("old", encoding="utf-8")
            backups.append(install.apply_install(self.plan(root), str(self.app / "backup"),
                                                 now=now))
        self.assertEqual(["20261026-093000", "20261026-093000-2"],
                         [pathlib.Path(path).name for path in backups])

    def test_component_inside_the_copy_target_is_refused(self):
        root = self.component("maya-app/modules/MtoULiveLink/发布/MtoULiveLink")
        with self.assertRaisesRegex(install.InstallError, "Extract the Maya component outside"):
            self.plan(root)
        self.assertFalse((self.modules / "MtoULiveLink.mod").exists())

    def test_reinstall_has_no_conflict_with_its_own_mod(self):
        root = self.component("same/MtoULiveLink")
        install.apply_install(self.plan(root), str(self.app / "backup"))
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

    def test_activation_adds_the_module_scripts_or_requires_a_restart(self):
        plan = self.plan(self.component("act/MtoULiveLink"))
        path = ["other"]
        self.assertIsNone(install.activate(plan, modules={}, path=path))
        self.assertEqual(plan.scripts_dir, path[0])
        install.activate(plan, modules={}, path=path)
        self.assertEqual(2, len(path))

        same = types.SimpleNamespace(__file__=os.path.join(plan.scripts_dir, "MtoULiveLink.py"))
        self.assertIsNone(install.activate(plan, modules={"MtoULiveLink": same}, path=path))
        old = types.SimpleNamespace(__file__=str(self.user_scripts / "mtou_livelink"
                                                 / "__init__.py"))
        self.assertEqual(old.__file__, install.activate(
            plan, modules={"mtou_livelink": old}, path=["other"]))

    def test_install_mel_is_ascii_and_loads_the_packaged_installer(self):
        text = (COMPONENT / "install.mel").read_bytes().decode("ascii")
        self.assertIn("whatIs mtouInstallLocator", text)
        self.assertIn("global proc mtouInstallLocator", text)
        self.assertIn("'scripts', 'mtou_livelink', 'install.py'", text)
        self.assertIn("install_from_maya(root)", text)


if __name__ == "__main__":
    unittest.main()
