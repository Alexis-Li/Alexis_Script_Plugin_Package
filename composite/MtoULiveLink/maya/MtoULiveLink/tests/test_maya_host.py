import importlib.util
import pathlib
import unittest
from unittest import mock

try:
    import maya.standalone
    MAYA_AVAILABLE = True
except ImportError:
    MAYA_AVAILABLE = False


@unittest.skipUnless(MAYA_AVAILABLE, "requires Maya mayapy")
class MayaHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        maya.standalone.initialize(name="python")
        global cmds, module
        import maya.cmds as cmds
        script = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
        spec = importlib.util.spec_from_file_location("MtoULiveLinkHost", str(script))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

    @classmethod
    def tearDownClass(cls):
        maya.standalone.uninitialize()

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentUnit(linear="cm")

    def _create_display(self, parent, entries="Clothes01:Clothes02", namespace=""):
        prefix = namespace + ":" if namespace else ""
        add_group = cmds.createNode(
            "transform", name=prefix + "Add_Ctrl_grp", parent=parent)
        display_group = cmds.createNode(
            "transform", name=prefix + "Display_ctrl_grp", parent=add_group
        )
        display = cmds.createNode(
            "transform", name=prefix + "Display_ctrl", parent=display_group)
        cmds.addAttr(display, longName="clothes", attributeType="enum", enumName=entries)
        return display

    def _create_character_group(self):
        group = cmds.createNode("transform", name="Group")
        motion = cmds.createNode("transform", name="MotionSystem", parent=group)
        display = self._create_display(motion)
        root = cmds.joint(name="root")
        cmds.parent(root, group)
        return group, root, display

    def test_character_scene_discovers_display_and_visible_skinned_meshes(self):
        group, root, display = self._create_character_group()
        geometry = cmds.createNode("transform", name="Geometry", parent=group)
        high_mesh = cmds.createNode("transform", name="HighMesh", parent=geometry)
        visible = cmds.polyCube(name="SM_C01_Clothes01")[0]
        hidden = cmds.polyCube(name="SM_C01_Clothes02")[0]
        cmds.parent(visible, hidden, high_mesh)
        cmds.skinCluster(root, visible, name="visibleSkin")
        cmds.skinCluster(root, hidden, name="hiddenSkin")
        cmds.setAttr(hidden + ".visibility", False)

        scene = module._CharacterScene.capture("|Group|root")
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()

        self.assertEqual("Clothes01", snapshot.outfit)
        self.assertEqual((("root", -1),), snapshot.bones)
        self.assertEqual(
            ("|Group|Geometry|HighMesh|SM_C01_Clothes01",),
            snapshot.mesh_paths)

    def test_capture_rejects_character_without_visible_skinned_mesh(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="hiddenBody")[0]
        cmds.skinCluster(root, mesh)
        cmds.setAttr(mesh + ".visibility", False)
        with self.assertRaises(module._CharacterSceneError) as caught:
            module._CharacterScene.capture(root)
        self.assertEqual("NO_VISIBLE_SKINNED_MESH", caught.exception.code)

    def test_duplicate_bone_action_selects_every_conflicting_dag_path(self):
        root = cmds.joint(name="duplicateRoot")
        left_parent = cmds.joint(name="leftHair")
        cmds.select(root)
        right_parent = cmds.joint(name="rightHair")
        cmds.select(left_parent)
        cmds.joint(name="duplicateTip")
        cmds.select(right_parent)
        cmds.joint(name="duplicateTip")
        paths = cmds.ls("duplicateTip", long=True, type="joint")

        class FakeScene(object):
            @staticmethod
            def snapshot():
                return module._CharacterSnapshot(
                    1, "|duplicateRoot", "Clothes01", [], [], paths)

        controller = module._Controller()
        controller._scene = FakeScene()
        controller.select_duplicate_bones()

        self.assertEqual(sorted(paths), sorted(cmds.ls(selection=True, long=True)))

    def test_captures_evaluated_joints_and_blendshape_alias(self):
        cmds.namespace(add="Hero")
        group = cmds.createNode("transform", name="Hero:Group")
        motion = cmds.createNode("transform", name="Hero:MotionSystem", parent=group)
        self._create_display(motion, namespace="Hero")
        root = cmds.joint(name="Hero:root", position=(1, 2, 3))
        cmds.parent(root, group)
        cmds.joint(name="Hero:spine", position=(1, 5, 3))
        base = cmds.polyCube(name="body")[0]
        target = cmds.duplicate(base, name="smileTarget")[0]
        blendshape = cmds.blendShape(target, base, name="faceBS")[0]
        cmds.aliasAttr("Smile", blendshape + ".weight[0]")
        cmds.skinCluster(root, base, name="bodySkin")
        cmds.setAttr(blendshape + ".Smile", 0.25)
        cmds.select(root, replace=True)

        scene = module._CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        frame = scene.sample()

        self.assertEqual((("root", -1), ("spine", 0)), snapshot.bones)
        self.assertEqual((1.0, 3.0, 2.0), frame.transforms[0][:3])
        self.assertEqual((0.0, 0.0, 3.0), frame.transforms[1][:3])
        self.assertEqual(("Smile",), snapshot.curve_names)
        self.assertEqual((0.25,), frame.curves)

    def test_same_alias_on_multiple_mesh_parts_streams_one_curve(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        for suffix in ("Face", "Teeth"):
            base = cmds.polyCube(name="body" + suffix)[0]
            target = cmds.duplicate(base, name="smile" + suffix)[0]
            blendshape = cmds.blendShape(
                target, base, name="blendShape" + suffix
            )[0]
            cmds.aliasAttr("Smile", blendshape + ".weight[0]")
            cmds.skinCluster(root, base, name="skin" + suffix)
            cmds.setAttr(blendshape + ".Smile", 0.25)
        cmds.select(root, replace=True)

        scene = module._CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        frame = scene.sample()

        self.assertEqual(("Smile",), snapshot.curve_names)
        self.assertEqual((0.25,), frame.curves)

    def test_same_alias_with_conflicting_values_is_rejected(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        for suffix, value in (("Face", 0.25), ("Teeth", 0.5)):
            base = cmds.polyCube(name="body" + suffix)[0]
            target = cmds.duplicate(base, name="smile" + suffix)[0]
            blendshape = cmds.blendShape(
                target, base, name="blendShape" + suffix
            )[0]
            cmds.aliasAttr("Smile", blendshape + ".weight[0]")
            cmds.skinCluster(root, base, name="skin" + suffix)
            cmds.setAttr(blendshape + ".Smile", value)
        cmds.select(root, replace=True)

        scene = module._CharacterScene.capture(root)
        self.addCleanup(scene.close)
        with self.assertRaises(module._CharacterSceneError) as caught:
            scene.sample()
        self.assertEqual("SAMPLING_FAILED", caught.exception.code)
        self.assertIn("conflicting values for BlendShape alias Smile", caught.exception.details)

    def test_streaming_session_uses_real_maya_callbacks_and_timer_cleanup(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, mesh)
        scene = module._CharacterScene.capture(root)
        self.addCleanup(scene.close)
        events = []

        class FakeWorker(object):
            instance = None

            def __init__(self, init_message):
                self.init_message = init_message
                self.stopped = False
                self.joined = None
                self.__class__.instance = self

            def start(self):
                pass

            def status(self):
                return ("connecting", "Connecting", None, None)

            def stop(self):
                self.stopped = True

            def join(self, timeout):
                self.joined = timeout

        with mock.patch.object(module, "_SenderWorker", FakeWorker):
            session = module._StreamingSession.start(scene, 24.0, events.append)
            session.change_rate(30.0)
            session.stop()
            session.stop()

        self.assertEqual(["stopped"], [event.kind for event in events])
        self.assertTrue(FakeWorker.instance.stopped)
        self.assertEqual(1.0, FakeWorker.instance.joined)
        self.assertEqual(2, FakeWorker.instance.init_message["version"])


if __name__ == "__main__":
    unittest.main()
