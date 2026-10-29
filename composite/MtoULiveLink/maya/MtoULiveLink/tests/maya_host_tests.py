# ruff: noqa: E402
import contextlib
import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

try:
    import maya.standalone
    MAYA_AVAILABLE = True
except ImportError:
    MAYA_AVAILABLE = False

import sys

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from mtou_livelink import (  # noqa: E402
    cache as mtou_cache,
)
from mtou_livelink import (
    camera as mtou_camera,
)
from mtou_livelink import (
    config as mtou_config,
)
from mtou_livelink import (
    controller as mtou_controller,
)
from mtou_livelink import (
    diagnostics as mtou_diagnostics,
)
from mtou_livelink import (
    mayaapi as mtou_mayaapi,
)
from mtou_livelink import (
    scene as mtou_scene,
)
from mtou_livelink import (
    session as mtou_session,
)
from mtou_livelink import (
    timeline as mtou_timeline,
)

_PACKAGE_MODULES = (mtou_cache, mtou_config, mtou_controller, mtou_mayaapi,
                    mtou_scene, mtou_session, mtou_timeline)


class _GlobalPatcher(object):
    # Patch one late-bound global on every package module that binds it.

    def __init__(self, name, value, kwargs):
        self._name = name
        self._value = value
        self._kwargs = kwargs
        self._stack = None
        self._mock = None

    def _make_mock(self):
        if self._value is not mock.DEFAULT:
            return self._value
        new_callable = self._kwargs.pop("new_callable", None)
        if new_callable is not None:
            return new_callable()
        return mock.MagicMock(**self._kwargs)

    def start(self):
        self._mock = self._make_mock()
        self._stack = contextlib.ExitStack()
        for module in _PACKAGE_MODULES:
            if hasattr(module, self._name):
                self._stack.enter_context(
                    mock.patch.object(module, self._name, self._mock))
        return self._mock

    def stop(self):
        if self._stack is not None:
            self._stack.close()
            self._stack = None
            self._mock = None

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc_info):
        self.stop()
        return False


def patch_global(name, value=mock.DEFAULT, **kwargs):
    return _GlobalPatcher(name, value, kwargs)



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

        scene = mtou_scene.CharacterScene.capture("|Group|root")
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()

        self.assertEqual("Clothes01", snapshot.outfit)
        self.assertEqual((("root", -1),), snapshot.bones)
        self.assertEqual(
            ("|Group|Geometry|HighMesh|SM_C01_Clothes01",),
            snapshot.mesh_paths)

    def test_capture_rejects_character_without_visible_skinned_mesh(self):
        unused_group, root, unused_display = self._create_character_group()
        del unused_group, unused_display
        mesh = cmds.polyCube(name="hiddenBody")[0]
        cmds.skinCluster(root, mesh)
        cmds.setAttr(mesh + ".visibility", False)
        with self.assertRaises(mtou_scene.CharacterSceneError) as caught:
            mtou_scene.CharacterScene.capture(root)
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
                return mtou_scene.CharacterSnapshot(
                    1, "|duplicateRoot", "Clothes01", [], [], paths)

        controller = mtou_controller.Controller()
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

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        frame = scene.sample()

        self.assertEqual((("root", -1), ("spine", 0)), snapshot.bones)
        self.assertEqual((1.0, 3.0, 2.0), frame.transforms[0][:3])
        self.assertEqual((0.0, 0.0, 3.0), frame.transforms[1][:3])
        self.assertEqual(("Smile",), snapshot.curve_names)
        self.assertEqual((0.25,), frame.curves)

    def test_bind_pose_is_independent_of_current_frame_and_covers_non_influence_joint(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        cmds.select(root, replace=True)
        cmds.joint(name="middle", position=(1, 5, 3))
        child = cmds.joint(name="child", position=(1, 8, 3))
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, child, mesh, name="bodySkin")
        cmds.setAttr(root + ".rotateZ", 30.0)
        cmds.setAttr(child + ".rotateX", 20.0)

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        current = scene.sample()

        self.assertEqual((0.0, 0.0, 0.0), snapshot.bind_local_transforms[0][:3])
        for actual, expected in zip(
                snapshot.bind_local_transforms[1][:3], (1.0, 3.0, 5.0)):
            self.assertAlmostEqual(expected, actual)
        for actual, expected in zip(
                snapshot.bind_local_transforms[2][:3], (0.0, 0.0, 3.0)):
            self.assertAlmostEqual(expected, actual)
        self.assertNotEqual(snapshot.bind_local_transforms, current.transforms)
        self.assertEqual("middle", snapshot.bones[1][0])

    def test_hidden_outfit_bone_reads_bind_matrix_from_dagpose(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        cmds.select(root, replace=True)
        cmds.joint(name="spine", position=(0, 5, 0))
        visible = cmds.polyCube(name="SM_C01_Clothes01")[0]
        cmds.skinCluster(root, visible, name="visibleSkin")
        cmds.select(root, replace=True)
        skirt = cmds.joint(name="skirt", position=(0, -2, 0))
        hidden = cmds.polyCube(name="SM_C01_Clothes02")[0]
        cmds.skinCluster(skirt, hidden, name="hiddenSkin")
        cmds.setAttr(hidden + ".visibility", False)
        cmds.setAttr(skirt + ".rotateZ", 45.0)

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        current = scene.sample()

        bones = [name for name, _ in snapshot.bones]
        self.assertEqual(["root", "spine", "skirt"], bones)
        for actual, expected in zip(
                snapshot.bind_local_transforms[2][:3], (0.0, 0.0, -2.0)):
            self.assertAlmostEqual(expected, actual)
        self.assertNotEqual(snapshot.bind_local_transforms, current.transforms)

    def test_conflicting_bind_matrices_resolve_to_joint_dagpose(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        cmds.select(root, replace=True)
        hand = cmds.joint(name="hand", position=(0, 5, 0))
        first_mesh = cmds.polyCube(name="firstBody")[0]
        second_mesh = cmds.polyCube(name="secondBody")[0]
        cmds.skinCluster(root, hand, first_mesh, name="firstSkin")
        cmds.setAttr(hand + ".rotateZ", 30.0)
        cmds.skinCluster(root, hand, second_mesh, name="secondSkin")

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()

        self.assertEqual(1, snapshot.bind_conflict_count)
        for actual, expected in zip(
                snapshot.bind_local_transforms[1][3:7], (0.0, 0.0, 0.0, 1.0)):
            self.assertAlmostEqual(expected, actual, places=5)

    def test_conflicting_bind_matrices_prefer_largest_skin_cluster_without_dagpose(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        cmds.select(root, replace=True)
        hand = cmds.joint(name="hand", position=(0, 5, 0))
        first_mesh = cmds.polyCube(name="firstBody")[0]
        second_mesh = cmds.polyCube(name="secondBody")[0]
        cmds.skinCluster(root, hand, first_mesh, name="aSmallSkin")
        cmds.setAttr(hand + ".rotateZ", 30.0)
        cmds.select(root, replace=True)
        cmds.joint(name="extra", position=(0, 1, 0))
        cmds.skinCluster(
            root, hand, "|Group|root|extra", second_mesh, name="bBigSkin")
        for pose in cmds.ls(type="dagPose"):
            cmds.delete(pose)

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()

        self.assertEqual(1, snapshot.bind_conflict_count)
        self.assertAlmostEqual(
            -0.2588, snapshot.bind_local_transforms[1][4], places=4)

    def test_equal_size_conflicting_skin_clusters_without_dagpose_are_rejected(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        first_mesh = cmds.polyCube(name="firstBody")[0]
        second_mesh = cmds.polyCube(name="secondBody")[0]
        cmds.skinCluster(root, first_mesh, name="zFirstSkin")
        cmds.setAttr(root + ".translateX", 5.0)
        cmds.skinCluster(
            root, second_mesh, name="aSecondSkin", ignoreBindPose=True)
        for pose in cmds.ls(type="dagPose"):
            cmds.delete(pose)

        with self.assertRaises(mtou_scene.CharacterSceneError) as caught:
            mtou_scene.CharacterScene.capture(root)

        self.assertEqual("BIND_POSE_INVALID", caught.exception.code)
        self.assertIn("equally ranked bind matrices", caught.exception.details)
        self.assertIn("aSecondSkin.bindPreMatrix[0]", caught.exception.details)
        self.assertIn("zFirstSkin.bindPreMatrix[0]", caught.exception.details)

    def test_non_finite_bindpre_matrix_is_rejected_before_inverse(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, mesh, name="bodySkin")
        original_matrix_attr = mtou_scene.matrix_attr

        def matrix_attr(plug):
            if ".bindPreMatrix[" in plug:
                values = [1.0, 0.0, 0.0, 0.0,
                          0.0, 1.0, 0.0, 0.0,
                          0.0, 0.0, 1.0, 0.0,
                          0.0, 0.0, 0.0, float("inf")]
                return mtou_mayaapi.om.MMatrix(values)
            return original_matrix_attr(plug)

        with patch_global("matrix_attr", side_effect=matrix_attr):
            with self.assertRaises(mtou_scene.CharacterSceneError) as caught:
                mtou_scene.CharacterScene.capture(root)

        self.assertEqual("BIND_POSE_INVALID", caught.exception.code)
        self.assertIn("non-finite bind matrix", caught.exception.details)
        self.assertIn("bodySkin.bindPreMatrix[0]", caught.exception.details)

    def test_non_invertible_bindpre_matrix_is_rejected_before_inverse(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, mesh, name="bodySkin")
        original_matrix_attr = mtou_scene.matrix_attr

        def matrix_attr(plug):
            if ".bindPreMatrix[" in plug:
                return mtou_mayaapi.om.MMatrix([0.0] * 16)
            return original_matrix_attr(plug)

        with patch_global("matrix_attr", side_effect=matrix_attr):
            with self.assertRaises(mtou_scene.CharacterSceneError) as caught:
                mtou_scene.CharacterScene.capture(root)

        self.assertEqual("BIND_POSE_INVALID", caught.exception.code)
        self.assertIn("non-invertible bind matrix", caught.exception.details)
        self.assertIn("bodySkin.bindPreMatrix[0]", caught.exception.details)

    def test_ignore_bindpose_rebind_resolves_to_original_bind_pose(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        first_mesh = cmds.polyCube(name="firstBody")[0]
        second_mesh = cmds.polyCube(name="secondBody")[0]
        cmds.skinCluster(root, first_mesh, name="firstSkin")
        cmds.setAttr(root + ".translateX", 5.0)
        cmds.skinCluster(
            root, second_mesh, name="secondSkin", ignoreBindPose=True)

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()

        self.assertEqual(1, snapshot.bind_conflict_count)
        for actual, expected in zip(
                snapshot.bind_local_transforms[0][:3], (0.0, 0.0, 0.0)):
            self.assertAlmostEqual(expected, actual)

    def test_joint_added_after_bind_falls_back_to_capture_pose(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, mesh, name="bodySkin")
        cmds.select(root, replace=True)
        late = cmds.joint(name="addedAfterBind", position=(0, 5, 0))
        cmds.setAttr(late + ".rotateZ", 20.0)

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()

        self.assertEqual("addedAfterBind", snapshot.bones[1][0])
        for actual, expected in zip(
                snapshot.bind_local_transforms[1][:3], (0.0, 0.0, 5.0)):
            self.assertAlmostEqual(expected, actual)
        for actual, expected in zip(
                snapshot.bind_local_transforms[0][:3], (0.0, 0.0, 0.0)):
            self.assertAlmostEqual(expected, actual)

    def test_late_joint_fallback_keeps_parent_bind_frame(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        cmds.select(root, replace=True)
        spine = cmds.joint(name="spine", position=(0, 5, 0))
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, spine, mesh, name="bodySkin")
        cmds.select(spine, replace=True)
        late = cmds.joint(name="slider", position=(1, 0, 0))
        cmds.setAttr(spine + ".rotateZ", 30.0)
        cmds.setAttr(late + ".rotateX", 10.0)

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()

        # spine deviates from its bind pose at capture; the fallback slider
        # must anchor to spine's bind frame so its captured local offset
        # stays its current local translation (1, -5, 0).
        self.assertEqual("slider", snapshot.bones[2][0])
        for actual, expected in zip(
                snapshot.bind_local_transforms[2][:3], (1.0, 0.0, -5.0)):
            self.assertAlmostEqual(expected, actual, places=5)

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

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        frame = scene.sample()

        self.assertEqual(("Smile",), snapshot.curve_names)
        self.assertEqual((0.25,), frame.curves)

    def test_separately_named_head_part_publishes_its_own_blendshape(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        # A separated Head is its own visible skinned mesh of the same
        # character, so its BlendShapes join the one published manifest.
        body = cmds.polyCube(name="bodyMesh")[0]
        body_target = cmds.duplicate(body, name="smileBody")[0]
        body_blendshape = cmds.blendShape(
            body_target, body, name="blendShapeBody")[0]
        cmds.aliasAttr("Smile", body_blendshape + ".weight[0]")
        cmds.skinCluster(root, body, name="skinBody")
        cmds.setAttr(body_blendshape + ".Smile", 0.25)

        head = cmds.polyCube(name="headMesh")[0]
        cmds.setAttr(head + ".translateY", 12.0)
        head_target = cmds.duplicate(head, name="jawHead")[0]
        head_blendshape = cmds.blendShape(
            head_target, head, name="blendShapeHead")[0]
        cmds.aliasAttr("Jaw", head_blendshape + ".weight[0]")
        cmds.aliasAttr("Smile", head_blendshape + ".weight[1]")
        cmds.skinCluster(root, head, name="skinHead")
        cmds.setAttr(head_blendshape + ".Jaw", 0.75)
        cmds.setAttr(head_blendshape + ".Smile", 0.25)
        cmds.select(root, replace=True)

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        frame = scene.sample()

        # The Head's own name is transmitted once, and the name both meshes
        # own is transmitted once for every mesh that owns it.
        self.assertEqual(("Smile", "Jaw"), snapshot.curve_names)
        self.assertEqual((0.25, 0.75), frame.curves)

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

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        with self.assertRaises(mtou_scene.CharacterSceneError) as caught:
            scene.sample()
        self.assertEqual("SAMPLING_FAILED", caught.exception.code)
        self.assertIn("conflicting values for BlendShape alias Smile", caught.exception.details)

    def test_streaming_session_uses_real_maya_callbacks_and_timer_cleanup(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, mesh)
        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        events = []

        class FakeWorker(object):
            instance = None

            def __init__(self, init_messages, hello_message=None):
                # One init per object of the set; this session has one object.
                self.init_message = init_messages[0]
                self.init_count = len(init_messages)
                self.hello_message = hello_message
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

            def end_ordered(self, discard_pending=True):
                pass

        with patch_global("SenderWorker", FakeWorker):
            session = mtou_session.StreamingSession.start(scene, 24.0, events.append)
            session.change_rate(30.0)
            session.stop()
            session.stop()

        self.assertEqual(["stopped"], [event.kind for event in events])
        self.assertTrue(FakeWorker.instance.stopped)
        self.assertEqual(1.0, FakeWorker.instance.joined)
        self.assertEqual(mtou_config.PROTOCOL_VERSION,
                         FakeWorker.instance.init_message["version"])
        self.assertEqual(11, FakeWorker.instance.init_message["version"])
        self.assertEqual("character", FakeWorker.instance.init_message["object_id"])
        self.assertEqual(["character"], FakeWorker.instance.init_message["set_objects"])
        self.assertEqual(1, FakeWorker.instance.init_count)
        self.assertEqual(11, FakeWorker.instance.hello_message["version"])
        self.assertEqual("cm", FakeWorker.instance.hello_message["scene"]["linear_unit"])
        self.assertEqual("animation", FakeWorker.instance.init_message["workflow"])
        self.assertTrue(FakeWorker.instance.init_message["blendshapes_enabled"])
        self.assertEqual(3, len(FakeWorker.instance.init_message["bones"][0]))

    def test_termination_from_a_worker_thread_is_delivered_without_an_idle_loop(self):
        # mayapy never runs maya.utils.executeDeferred work; batch mode queues
        # it for the host loop instead, which drains it on the main thread.
        import threading
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, mesh)
        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        events = []

        class FakeWorker(object):
            def __init__(self, init_message, hello_message=None):
                pass

            def start(self):
                pass

            def status(self):
                return ("connecting", "Connecting", None, None)

            def stop(self):
                pass

            def join(self, timeout):
                pass

            def end_ordered(self, discard_pending=True):
                pass

        with patch_global("SenderWorker", FakeWorker):
            session = mtou_session.StreamingSession.start(scene, 24.0, events.append)
            self.addCleanup(session.stop)
            main_thread = threading.get_ident()
            ran_on = []
            mtou_mayaapi.run_deferred(
                lambda: ran_on.append(threading.get_ident()), cmds.evalDeferred)
            self.assertEqual([], ran_on)
            self.assertEqual(1, mtou_mayaapi.drain_deferred())
            self.assertEqual([main_thread], ran_on)

            worker = threading.Thread(target=lambda: session._request_failure(
                mtou_diagnostics.make_diagnostic(
                    "STREAM_INTERRUPTED", "connection lost"), False))
            worker.start()
            worker.join()
            self.assertEqual([], [event.kind for event in events])
            self.assertEqual(1, mtou_mayaapi.drain_deferred())

        self.assertEqual(["failed"], [event.kind for event in events])
        self.assertEqual(0, mtou_mayaapi.drain_deferred())

    def test_reference_container_in_the_anchor_frame_lines_up_with_the_character_and_camera(self):
        # The D003 fixture anchor: (60, 40, 0), roll 3, pitch -4, yaw 37. Real
        # Maya nodes: the imported level geometry sits at C(p), the character
        # stands at the Maya origin, the linked camera at C(A^-1 camera).
        import math
        import test_mtou_reference as fixtures
        from mtou_livelink import reference
        anchor, problem = reference.parse_anchor(fixtures.D003_ANCHOR)
        self.assertIsNone(problem)
        group = cmds.createNode("transform", name="reference_container")
        prop_world = (360.0, 140.0, 20.0)
        wall = cmds.createNode("transform", name="reference_wall", parent=group)
        cmds.xform(wall, translation=mtou_camera.ue_to_maya_point(*prop_world),
                   worldSpace=True)
        character = cmds.createNode("transform", name="reference_character")
        cmds.xform(group, matrix=reference.anchor_container_matrix(anchor))

        local_prop = fixtures.to_anchor_local(fixtures.D003_ANCHOR, prop_world)
        expected = mtou_camera.ue_to_maya_point(*local_prop)
        placed = cmds.xform(wall, query=True, worldSpace=True, translation=True)
        for index in range(3):
            self.assertAlmostEqual(expected[index], placed[index], places=4)
        # Rigid: the container never rescales or shears the reference.
        matrix = cmds.xform(group, query=True, matrix=True)
        for row in range(3):
            self.assertAlmostEqual(1.0, math.sqrt(sum(
                matrix[row * 4 + column] ** 2 for column in range(3))), places=6)

        # The character stays at the origin, where the anchor's own position
        # lands; the camera Unreal sends is relative to the anchor.
        probe = cmds.createNode("transform", name="reference_anchor_probe", parent=group)
        cmds.xform(probe, translation=mtou_camera.ue_to_maya_point(60.0, 40.0, 0.0),
                   worldSpace=False)
        at_anchor = cmds.xform(probe, query=True, worldSpace=True, translation=True)
        at_character = cmds.xform(character, query=True, worldSpace=True, translation=True)
        for index in range(3):
            self.assertAlmostEqual(at_character[index], at_anchor[index], places=4)

        camera_world = (160.0, -60.0, 170.0)
        camera = cmds.createNode("transform", name="reference_linked_camera")
        cmds.xform(camera, translation=mtou_camera.ue_to_maya_point(
            *fixtures.to_anchor_local(fixtures.D003_ANCHOR, camera_world)),
            worldSpace=True)
        camera_at = cmds.xform(camera, query=True, worldSpace=True, translation=True)
        wall_at = cmds.xform(wall, query=True, worldSpace=True, translation=True)
        distance = math.sqrt(sum((camera_at[i] - wall_at[i]) ** 2 for i in range(3)))
        world_distance = math.sqrt(sum(
            (camera_world[i] - prop_world[i]) ** 2 for i in range(3)))
        self.assertAlmostEqual(world_distance, distance, places=4)

    def test_cached_capture_uses_real_playback_range_and_restores_after_cancel(self):
        cmds.playbackOptions(min=10, max=12)
        cmds.currentTime(11, edit=True)
        playing = {"value": True}

        def play(**kwargs):
            if "state" in kwargs:
                playing["value"] = bool(kwargs["state"])

        with patch_global("maya_is_playing", side_effect=lambda: playing["value"]), \
                mock.patch.object(mtou_mayaapi.cmds, "play", side_effect=play):
            timeline = mtou_timeline.MayaTimeline()
            self.assertTrue(timeline.is_playing())
            original_frame = timeline.current_frame()
            snapshot = mtou_scene.CharacterSnapshot(
                1, "|root", "Clothes01", [("root", -1)], [],
                bind_local_transforms=[(0.0,) * 10])

            class FakeScene(object):
                @staticmethod
                def snapshot():
                    return snapshot

                @staticmethod
                def sample():
                    value = float(cmds.currentTime(query=True))
                    return mtou_scene.CharacterFrame(1, [[value] * 10], [])

            class FakeStream(object):
                is_ready = True
                revision = 1

                def __init__(self):
                    self.resumed = 0
                    self.reply_listener = None

                def pause_for_cached(self, reply_listener=None):
                    self.reply_listener = reply_listener

                def resume_from_cached(self):
                    self.resumed += 1

                def submit_cached(self, message):
                    if message["type"] == "cache_clear" and self.reply_listener:
                        self.reply_listener({"type": "cache_cleared",
                                             "upload_id": 0, "play_id": 0,
                                             "clear_id": message["clear_id"]})

                def end_cached_replay(self):
                    pass

            stream = FakeStream()
            progress = []

            class FakeTimer(object):
                def __init__(self):
                    self.callbacks = {}
                    self.next_id = 1
                    self.removed = []

                def addTimerCallback(self, unused_interval, callback):
                    timer_id = self.next_id
                    self.next_id += 1
                    self.callbacks[timer_id] = callback
                    return timer_id

                def removeCallback(self, timer_id):
                    self.removed.append(timer_id)
                    self.callbacks.pop(timer_id, None)

                def fire(self):
                    for callback in list(self.callbacks.values()):
                        callback()

            timer = FakeTimer()
            holder = {}

            def on_change(view):
                if view.state == mtou_cache.CachedPlayback.CAPTURING and view.current:
                    progress.append((view.current, view.total))
                    if view.current == 2:
                        holder["cached"].cancel_capture()

            with tempfile.TemporaryDirectory() as directory:
                cached = mtou_cache.CachedPlayback(
                    FakeScene(), stream, on_change=on_change, timeline=timeline,
                    temp_dir=directory,
                    disk_usage=lambda unused: (1, 1, 1 << 40), timer_api=timer)
                holder["cached"] = cached
                cached.enter()
                cached.capture(24.0, lambda unused_size, unused_frames: True)
                while cached.view.state == mtou_cache.CachedPlayback.CAPTURING:
                    timer.fire()
                self.assertEqual([(1, 3), (2, 3)], progress)
                self.assertFalse(timeline.is_playing())
                self.assertEqual(original_frame, timeline.current_frame())
                self.assertEqual(1, stream.resumed)
                self.assertIsNone(cached.view.cache_summary)
                self.assertEqual(mtou_cache.CachedPlayback.REALTIME, cached.view.state)
                self.assertEqual([1, 2], sorted(timer.removed))
                cached.discard()
            self.assertFalse(playing["value"])


    def test_playback_state_query_uses_maya_host_state_without_editing_scene(self):
        current_time = cmds.currentTime(query=True)
        with mock.patch.object(mtou_mayaapi.cmds, "play", return_value=True) as play:
            self.assertTrue(mtou_timeline.maya_is_playing())
        play.assert_called_once_with(query=True, state=True)
        self.assertEqual(current_time, cmds.currentTime(query=True))

    def test_structural_chain_preserves_trs_in_realtime_and_bind(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mid = cmds.createNode("transform", name="mid_grp", parent=root)
        cmds.setAttr(mid + ".translate", 1.0, 2.0, 3.0)
        cmds.setAttr(mid + ".rotate", 10.0, 20.0, 30.0)
        cmds.setAttr(mid + ".scale", 1.5, 2.0, 0.5)
        mid2 = cmds.createNode("transform", name="mid2_grp", parent=mid)
        cmds.setAttr(mid2 + ".translate", -2.0, 0.5, 4.0)
        cmds.setAttr(mid2 + ".rotate", -15.0, 45.0, 5.0)
        cmds.setAttr(mid2 + ".scale", 0.8, 1.2, 1.0)
        cmds.select(mid2, replace=True)
        tip = cmds.joint(name="tip", position=(4.0, 5.0, 6.0))
        unrelated = cmds.createNode("transform", name="unrelated_grp", parent=root)
        cmds.createNode("transform", name="geo_grp", parent=unrelated)
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, tip, mesh, name="bodySkin")

        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        frame = scene.sample()

        self.assertEqual(
            (("root", -1), ("mid_grp", 0), ("mid2_grp", 1), ("tip", 2)),
            snapshot.bones)
        self.assertEqual(4, len(frame.transforms))
        self.assertEqual(4, len(snapshot.bind_local_transforms))
        # Structural transforms fall back to the capture-time local frame,
        # so a static scene must agree between realtime and bind.
        for index in (1, 2, 3):
            for actual, expected in zip(
                    snapshot.bind_local_transforms[index], frame.transforms[index]):
                self.assertAlmostEqual(expected, actual, places=4)
        # The tip local must be relative to its structural parent, not the
        # nearest joint ancestor. Recompute both candidates from Maya world
        # matrices and compare in the transmitted UE basis.
        worlds = [bone["dag_path"].inclusiveMatrix()
                  for bone in scene._subject["bones"]]
        unit_scale = scene._subject["unit_scale"]
        correct = worlds[3] * worlds[2].inverse()
        wrong = worlds[3] * worlds[0].inverse()
        expected = mtou_scene.sample_matrix(correct, unit_scale)
        skipped = mtou_scene.sample_matrix(wrong, unit_scale)
        for actual, wanted in zip(frame.transforms[3], expected):
            self.assertAlmostEqual(wanted, actual, places=4)
        self.assertTrue(
            any(abs(actual - bad) > 1.0e-3
                for actual, bad in zip(frame.transforms[3], skipped)))

    def test_published_trs_reconstructs_bind_and_animated_worlds(self):
        # Independent reconstruction: convert wire UE (X,Z,Y) back to Maya,
        # build S*R*T from the transmitted quaternion, then multiply parents.
        # Do not call _sample_matrix or convert_transform in the oracle.
        om = mtou_mayaapi.om
        def reconstruct(transforms, bones):
            worlds = []
            for values, (_, parent) in zip(transforms, bones):
                tx, tz, ty, nx, nz, ny, qw, sx, sz, sy = values
                scale = om.MMatrix([sx, 0, 0, 0, 0, sy, 0, 0,
                                    0, 0, sz, 0, 0, 0, 0, 1])
                rotation = om.MQuaternion(-nx, -ny, -nz, qw).asMatrix()
                translation = om.MMatrix([1, 0, 0, 0, 0, 1, 0, 0,
                                          0, 0, 1, 0, tx, ty, tz, 1])
                local = scale * rotation * translation
                worlds.append(local if parent < 0 else local * worlds[parent])
            return worlds

        group, root, unused = self._create_character_group()
        mid = cmds.createNode("transform", name="offset", parent=root)
        inner = cmds.createNode("transform", name="inner", parent=mid)
        cmds.select(inner)
        tip = cmds.joint(name="tip")
        cmds.setAttr(tip + ".segmentScaleCompensate", False)
        for node, t, r, scale in (
                (root, (2, 3, 4), (10, -20, 15), (1.1, 1.2, 0.9)),
                (mid, (1, 2, 3), (15, 25, 35), (1.5, 2, 0.5)),
                (inner, (-2, 1, 4), (-20, 10, 5), (0.8, 1.2, 1.1)),
                (tip, (4, 5, 6), (8, 12, -15), (1, 1, 1))):
            cmds.setAttr(node + ".translate", *t)
            cmds.setAttr(node + ".rotate", *r)
            cmds.setAttr(node + ".scale", *scale)
        mesh = cmds.polyCube()[0]
        cmds.skinCluster(root, tip, mesh)
        scene = mtou_scene.CharacterScene.capture(root)
        self.addCleanup(scene.close)
        snapshot = scene.snapshot()
        paths = [bone["path"] for bone in scene._subject["bones"]]
        bind_worlds = [cmds.xform(path, q=True, ws=True, matrix=True) for path in paths]
        cmds.setAttr(mid + ".rotate", -35, 12, 21)
        cmds.setAttr(inner + ".translate", 3, -1, 8)
        cmds.setAttr(tip + ".rotate", 30, -10, 45)
        current_worlds = [cmds.xform(path, q=True, ws=True, matrix=True) for path in paths]
        for label, transforms, expected in (
                ("bind", snapshot.bind_local_transforms, bind_worlds),
                ("animated", scene.sample().transforms, current_worlds)):
            for path, actual, wanted in zip(paths, reconstruct(transforms, snapshot.bones), expected):
                with self.subTest(pose=label, path=path):
                    self.assertLess(max(abs(a - b) for a, b in zip(actual, wanted)), 1.e-6)

    def test_capture_subject_isolates_second_character(self):
        first_group = cmds.createNode("transform", name="CharA")
        cmds.select(clear=True)
        first_root = cmds.joint(name="rootA", position=(0, 0, 0))
        cmds.parent(first_root, first_group)
        first_root = cmds.ls(first_root, long=True)[0]
        first_mid = cmds.createNode(
            "transform", name="grpA", parent=first_root)
        first_mid = cmds.ls(first_mid, long=True)[0]
        cmds.select(first_mid, replace=True)
        first_tip = cmds.joint(name="tipA", position=(0, 5, 0))
        first_tip = cmds.ls(first_tip, long=True)[0]
        first_mesh = cmds.polyCube(name="bodyA")[0]
        cmds.skinCluster(first_root, first_tip, first_mesh, name="skinA")
        second_group = cmds.createNode("transform", name="CharB")
        cmds.select(clear=True)
        second_root = cmds.joint(name="rootB", position=(10, 0, 0))
        cmds.parent(second_root, second_group)
        second_root = cmds.ls(second_root, long=True)[0]
        second_mid = cmds.createNode(
            "transform", name="grpB", parent=second_root)
        second_mid = cmds.ls(second_mid, long=True)[0]
        cmds.select(second_mid, replace=True)
        second_tip = cmds.joint(name="tipB", position=(10, 5, 0))
        second_tip = cmds.ls(second_tip, long=True)[0]
        second_mesh = cmds.polyCube(name="bodyB")[0]
        cmds.skinCluster(second_root, second_tip, second_mesh, name="skinB")

        first = mtou_scene.capture_subject(first_root)
        second = mtou_scene.capture_subject(second_root)

        self.assertEqual(
            [first_root, first_mid, first_tip],
            [bone["path"] for bone in first["bones"]])
        self.assertEqual(
            [second_root, second_mid, second_tip],
            [bone["path"] for bone in second["bones"]])

    # ------------------------------------------------- linkage scene safety
    def _linkage_host(self):
        from mtou_livelink import link_host

        return link_host.MayaLinkageHost(None, ["character"], 24.0)

    # Each evaluation mode keeps a different part of the last evaluated pose
    # until the scene is evaluated again: DG mode the bones, the Evaluation
    # Manager modes the Morph weights. A stale sample is certain in every mode.
    def test_linkage_samples_the_pose_of_the_applied_frame_in_dg_mode(self):
        self._assert_linkage_samples_the_applied_frame("off")

    def test_linkage_samples_the_pose_of_the_applied_frame_in_serial_mode(self):
        self._assert_linkage_samples_the_applied_frame("serial")

    def test_linkage_samples_the_pose_of_the_applied_frame_in_parallel_mode(self):
        self._assert_linkage_samples_the_applied_frame("parallel")

    def _assert_linkage_samples_the_applied_frame(self, evaluation_mode):
        import test_mtou_camera as payloads
        from mtou_livelink import link_host

        group, root, unused_display = self._create_character_group()
        del unused_display
        base = cmds.polyCube(name="body")[0]
        target = cmds.duplicate(base, name="blinkTarget")[0]
        blendshape = cmds.blendShape(target, base, name="faceBS")[0]
        cmds.aliasAttr("Blink", blendshape + ".weight[0]")
        cmds.skinCluster(root, base, name="bodySkin")
        for plug, values in ((blendshape + ".Blink", (0.0, 1.0)),
                             (root + ".translateX", (0.0, 10.0))):
            for frame, value in zip((0, 100), values):
                cmds.setKeyframe(plug, time=frame, value=value,
                                 inTangentType="linear", outTangentType="linear")
        mode = cmds.evaluationManager(query=True, mode=True)[0]
        self.addCleanup(cmds.evaluationManager, mode=mode)
        cmds.evaluationManager(mode=evaluation_mode)
        self.assertEqual(evaluation_mode,
                         cmds.evaluationManager(query=True, mode=True)[0])
        cmds.currentTime(0, edit=True)
        scene = mtou_scene.CharacterScene.capture(cmds.ls(root, long=True)[0])
        self.addCleanup(scene.close)
        scene.sample()
        host = link_host.MayaLinkageHost(scene, ["character"], 24.0)
        host.set_origin_frame(0.0)
        self.addCleanup(lambda: cmds.objExists(mtou_config.LINK_CAMERA_NAME)
                        and cmds.delete(mtou_config.LINK_CAMERA_NAME))
        for frame, weight, translate_x in ((50.0, 0.5, 5.0), (100.0, 1.0, 10.0)):
            applied, unused_readback = host.apply_target(
                {"display_frame": frame, "playback_range": {"start": 0, "end": 100},
                 "display_rate": {"numerator": 24, "denominator": 1}},
                payloads.camera_payload(), payloads.gate_payload())
            self.assertEqual(frame, applied["maya_frame"])
            row = host.sample_group(["character"])[0]
            self.assertAlmostEqual(weight, row["curves"][0], places=5)
            self.assertAlmostEqual(translate_x, row["transforms"][0][0], places=4)

    def _foreign_scene_file(self):
        """A saved scene with its own MtoU_LinkCamera, gate and frame."""
        folder = tempfile.mkdtemp(prefix="mtou-linkage-scene-")
        self.addCleanup(lambda: __import__("shutil").rmtree(folder, ignore_errors=True))
        path = str(pathlib.Path(folder) / "foreign.ma")
        cmds.file(new=True, force=True)
        self._create_foreign_state()
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)
        return path

    def _create_foreign_state(self):
        transform = cmds.createNode("transform", name=mtou_config.LINK_CAMERA_NAME)
        cmds.createNode("camera", name=mtou_config.LINK_CAMERA_NAME + "Shape",
                        parent=transform)
        cmds.setAttr("defaultResolution.width", 2048)
        cmds.playbackOptions(minTime=1, maxTime=100)
        cmds.currentTime(48)

    def _assert_foreign_state_kept(self):
        self.assertTrue(cmds.objExists(mtou_config.LINK_CAMERA_NAME))
        self.assertEqual(2048, cmds.getAttr("defaultResolution.width"))
        self.assertEqual(48.0, cmds.currentTime(query=True))

    def _start_linkage_state(self, host):
        cmds.setAttr("defaultResolution.width", 960)
        cmds.currentTime(1)
        state = host.capture_state()
        # The tool camera exists, as after the first applied target.
        host._camera_adapter()._ensure_camera()
        cmds.setAttr("defaultResolution.width", 1280)
        return state

    def test_linkage_restore_reports_a_locked_camera_it_cannot_delete(self):
        host = self._linkage_host()
        state = self._start_linkage_state(host)
        camera_node = mtou_config.LINK_CAMERA_NAME
        cmds.lockNode(camera_node, lock=True)
        self.addCleanup(lambda: cmds.objExists(camera_node)
                        and cmds.lockNode(camera_node, lock=False))
        residual = host.restore_state(state)
        self.assertTrue(any(entry.startswith("camera:") and camera_node in entry
                            for entry in residual), residual)
        # The other resources are still restored.
        self.assertEqual(960, cmds.getAttr("defaultResolution.width"))

    def test_linkage_restore_writes_nothing_into_an_opened_scene(self):
        path = self._foreign_scene_file()
        cmds.file(new=True, force=True)
        host = self._linkage_host()
        state = self._start_linkage_state(host)
        cmds.file(path, open=True, force=True)
        residual = host.restore_state(state)
        self.assertEqual(1, len(residual))
        self.assertTrue(residual[0].startswith("scene:"))
        self._assert_foreign_state_kept()

    def test_linkage_restore_writes_nothing_into_a_new_scene(self):
        host = self._linkage_host()
        state = self._start_linkage_state(host)
        cmds.file(new=True, force=True)
        self._create_foreign_state()
        residual = host.restore_state(state)
        self.assertTrue(residual and residual[0].startswith("scene:"))
        self._assert_foreign_state_kept()

    def test_linkage_refuses_locked_or_driven_inputs_and_restores_every_resource(self):
        # Real camera, gate and timeline: a locked or driven input is refused
        # before anything is written; a target that applies is undone by the
        # restore (time, playback range, gate, undo state, tool camera).
        import test_mtou_camera as payloads
        from mtou_livelink import camera as mtou_camera
        host = self._linkage_host()
        cmds.playbackOptions(minTime=10, maxTime=90)
        cmds.currentTime(37)
        cmds.setAttr("defaultResolution.width", 1024)
        cmds.setAttr("defaultResolution.height", 768)
        undo_before = cmds.undoInfo(query=True, state=True)
        self.addCleanup(lambda: cmds.objExists(mtou_config.LINK_CAMERA_NAME)
                        and cmds.delete(mtou_config.LINK_CAMERA_NAME))
        state = host.capture_state()
        host.set_origin_frame(37.0)
        time_payload = {"display_frame": 12.0, "playback_range": {"start": 0, "end": 100},
                        "display_rate": {"numerator": 24, "denominator": 1}}

        def assert_refused(category):
            with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
                host.apply_target(time_payload, payloads.camera_payload(),
                                  payloads.gate_payload())
            self.assertEqual(category, caught.exception.category)
            self.assertEqual(37.0, cmds.currentTime(query=True))
            self.assertEqual(1024, cmds.getAttr("defaultResolution.width"))
            self.assertEqual(768, cmds.getAttr("defaultResolution.height"))

        cmds.setAttr("defaultResolution.width", lock=True)
        assert_refused(mtou_camera.CATEGORY_LOCKED_ATTRIBUTE)
        self.assertFalse(cmds.objExists(mtou_config.LINK_CAMERA_NAME))
        cmds.setAttr("defaultResolution.width", lock=False)

        driver = cmds.expression(
            string="defaultResolution.deviceAspectRatio = 1.5;", name="gateDriver")
        assert_refused(mtou_camera.CATEGORY_CAMERA_INPUT_DRIVEN)
        cmds.delete(driver)

        applied, unused_readback = host.apply_target(
            time_payload, payloads.camera_payload(), payloads.gate_payload())
        self.assertEqual(49.0, applied["maya_frame"])
        self.assertEqual(49.0, cmds.currentTime(query=True))
        self.assertEqual(1920, cmds.getAttr("defaultResolution.width"))
        self.assertTrue(cmds.objExists(mtou_config.LINK_CAMERA_NAME))

        self.assertEqual([], host.restore_state(state))
        self.assertEqual(37.0, cmds.currentTime(query=True))
        self.assertEqual((10.0, 90.0), (cmds.playbackOptions(query=True, minTime=True),
                                        cmds.playbackOptions(query=True, maxTime=True)))
        self.assertEqual(1024, cmds.getAttr("defaultResolution.width"))
        self.assertEqual(768, cmds.getAttr("defaultResolution.height"))
        self.assertEqual(undo_before, cmds.undoInfo(query=True, state=True))
        self.assertFalse(cmds.objExists(mtou_config.LINK_CAMERA_NAME))

        # A camera of the tool's name the scene owns, with a locked focal
        # length, is refused and kept exactly as it was.
        transform, shape = self._camera(mtou_config.LINK_CAMERA_NAME, 35.0, 4.0)
        cmds.setAttr(shape + ".focalLength", lock=True)
        host = self._linkage_host()
        state = host.capture_state()
        host.set_origin_frame(37.0)
        assert_refused(mtou_camera.CATEGORY_LOCKED_ATTRIBUTE)
        self.assertEqual((35.0, 4.0), self._camera_values(transform))
        self.assertEqual([], host.restore_state(state))
        self.assertEqual((35.0, 4.0), self._camera_values(transform))

    def _camera(self, name, focal, x):
        transform = cmds.createNode("transform", name=name)
        shape = cmds.createNode("camera", name=name + "Shape", parent=transform)
        cmds.setAttr(shape + ".focalLength", focal)
        cmds.setAttr(transform + ".translateX", x)
        return transform, shape

    def _camera_values(self, transform):
        shape = cmds.listRelatives(transform, shapes=True, type="camera", fullPath=True)[0]
        return (round(cmds.getAttr(shape + ".focalLength"), 3),
                round(cmds.getAttr(transform + ".translateX"), 3))

    def test_borrowed_camera_restore_follows_a_rename(self):
        from mtou_livelink import camera as mtou_camera

        name = mtou_config.LINK_CAMERA_NAME
        self._camera(name, 35.0, 0.0)
        link = mtou_camera.LinkCamera()
        snapshot = link.capture_state()
        cmds.rename(name, "BorrowedRenamed")
        renamed = "BorrowedRenamed"
        cmds.setAttr(cmds.listRelatives(renamed, shapes=True, fullPath=True)[0]
                     + ".focalLength", 50.0)
        cmds.setAttr(renamed + ".translateX", 20.0)
        # Another camera now carries the old name.
        self._camera(name, 85.0, 100.0)
        residual = link.restore_state(snapshot)
        self.assertEqual([], residual)
        self.assertEqual((85.0, 100.0), self._camera_values(name))
        self.assertEqual((35.0, 0.0), self._camera_values(renamed))

    def test_borrowed_camera_with_a_replaced_shape_is_refused(self):
        from mtou_livelink import camera as mtou_camera

        name = mtou_config.LINK_CAMERA_NAME
        transform, shape = self._camera(name, 35.0, 0.0)
        link = mtou_camera.LinkCamera()
        snapshot = link.capture_state()
        cmds.delete(shape)
        replacement = cmds.createNode("camera", name="ReplacementShape", parent=transform)
        cmds.setAttr(replacement + ".focalLength", 85.0)
        cmds.setAttr(transform + ".translateX", 100.0)
        residual = link.restore_state(snapshot)
        self.assertEqual(1, len(residual))
        self.assertIn("shape was replaced", residual[0])
        self.assertEqual((85.0, 100.0), self._camera_values(name))

    def _captured_and_applied_camera(self):
        """A borrowed tool camera after capture and one real apply."""
        import test_mtou_camera as payloads
        from mtou_livelink import camera as mtou_camera

        self._camera(mtou_config.LINK_CAMERA_NAME, 35.0, 0.0)
        link = mtou_camera.LinkCamera()
        snapshot = link.capture_state()
        link.apply(payloads.camera_payload(), payloads.gate_payload())
        return link, snapshot, payloads

    def test_apply_keeps_writing_the_borrowed_camera_after_a_rename(self):
        name = mtou_config.LINK_CAMERA_NAME
        link, snapshot, payloads = self._captured_and_applied_camera()
        cmds.rename(name, "OriginalRenamed")
        self._camera(name, 85.0, 1000.0)
        foreign_matrix = cmds.xform(name, query=True, worldSpace=True, matrix=True)
        report, unused_values = link.apply(
            payloads.camera_payload(focal_length_mm=60.0), payloads.gate_payload())
        self.assertEqual(cmds.ls("OriginalRenamed", long=True)[0],
                         cmds.ls(report["camera"], long=True)[0])
        self.assertEqual((60.0, 100.0), self._camera_values("OriginalRenamed"))
        self.assertEqual((85.0, 1000.0), self._camera_values(name))
        self.assertEqual(foreign_matrix,
                         cmds.xform(name, query=True, worldSpace=True, matrix=True))
        self.assertEqual([], link.restore_state(snapshot))
        self.assertEqual((35.0, 0.0), self._camera_values("OriginalRenamed"))
        self.assertEqual((85.0, 1000.0), self._camera_values(name))

    def test_apply_refuses_a_camera_that_replaced_the_deleted_one(self):
        from mtou_livelink import camera as mtou_camera

        name = mtou_config.LINK_CAMERA_NAME
        link, snapshot, payloads = self._captured_and_applied_camera()
        cmds.delete(name)
        self._camera(name, 85.0, 1000.0)
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            link.apply(payloads.camera_payload(focal_length_mm=60.0),
                       payloads.gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_CAMERA_REPLACED, caught.exception.category)
        self.assertEqual((85.0, 1000.0), self._camera_values(name))
        self.assertEqual([], link.restore_state(snapshot))
        self.assertEqual((85.0, 1000.0), self._camera_values(name))

    def test_apply_refuses_a_borrowed_camera_whose_shape_was_replaced(self):
        from mtou_livelink import camera as mtou_camera

        name = mtou_config.LINK_CAMERA_NAME
        link, snapshot, payloads = self._captured_and_applied_camera()
        transform = cmds.ls(name, long=True)[0]
        cmds.delete(cmds.listRelatives(transform, shapes=True, fullPath=True))
        replacement = cmds.createNode("camera", name="ReplacementShape", parent=transform)
        cmds.setAttr(replacement + ".focalLength", 85.0)
        cmds.setAttr(transform + ".translateX", 1000.0)
        with self.assertRaises(mtou_camera.CameraSyncRefused) as caught:
            link.apply(payloads.camera_payload(focal_length_mm=60.0),
                       payloads.gate_payload())
        self.assertEqual(mtou_camera.CATEGORY_CAMERA_REPLACED, caught.exception.category)
        self.assertEqual((85.0, 1000.0), self._camera_values(name))
        residual = link.restore_state(snapshot)
        self.assertTrue(residual and "shape was replaced" in residual[0], residual)
        self.assertEqual((85.0, 1000.0), self._camera_values(name))

    def test_panel_restores_linkage_before_the_scene_is_replaced(self):
        import maya.api.OpenMaya as om
        from mtou_livelink import linkage

        class Session(object):
            messages = []

            def submit_linkage(self, message):
                self.messages.append(message)
                return True

        path = self._foreign_scene_file()
        cmds.file(new=True, force=True)
        host = self._linkage_host()
        controller = mtou_controller.Controller()
        session = linkage.LinkageSession(host, Session())
        session.set_session_id(1)
        cmds.setAttr("defaultResolution.width", 960)
        cmds.currentTime(1)
        session.enter(1)
        host._camera_adapter()._ensure_camera()
        controller._linkage = session
        callback = om.MSceneMessage.addCallback(
            om.MSceneMessage.kBeforeOpen, controller._on_before_scene_replaced)
        self.addCleanup(om.MMessage.removeCallback, callback)
        cmds.file(path, open=True, force=True)
        # The old scene was restored while it was still open...
        self.assertIsNone(controller._linkage)
        self.assertEqual((), session.recovery_records())
        # ...and the opened scene keeps its own camera, gate and frame.
        self._assert_foreign_state_kept()

    def test_releasing_the_panel_theme_deletes_its_python_event_filters(self):
        import shiboken2
        from mtou_livelink.ui.layout import release_panel_theme
        from PySide2 import QtCore

        events = []

        class Decoration(QtCore.QObject):
            def eventFilter(self, watched, event):
                events.append(event.type())
                return False

        card = QtCore.QObject()
        self.addCleanup(shiboken2.delete, card)
        decoration = Decoration(card)
        card.installEventFilter(decoration)
        closed_card = QtCore.QObject()
        gone = Decoration(closed_card)
        shiboken2.delete(closed_card)  # Qt deleted it with its window
        self.assertFalse(shiboken2.isValid(gone))

        release_panel_theme([decoration, gone])
        release_panel_theme([decoration, gone])

        self.assertFalse(shiboken2.isValid(decoration))
        self.assertTrue(shiboken2.isValid(card))
        self.assertEqual([], card.children())
        QtCore.QCoreApplication.sendEvent(card, QtCore.QEvent(QtCore.QEvent.User))
        self.assertEqual([], events)

    # ------------------------------------------- scene-reference fault matrix
    def _real_handoff(self, directory, mesh_name, anchor=None, transfer_id="tx-host"):
        """A handoff directory with an ASCII FBX Maya itself exported."""
        import hashlib
        import json

        import maya.mel as mel
        directory = pathlib.Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        cmds.loadPlugin("fbxmaya", quiet=True)
        cube = cmds.polyCube(name=mesh_name)[0]
        geometry = directory / "Lvl.fbx"
        cmds.select(cube, replace=True)
        mel.eval("FBXResetExport")
        mel.eval("FBXExportInAscii -v true")
        mel.eval('FBXExport -f "{0}" -s'.format(geometry.as_posix()))
        cmds.delete(cube)
        size = geometry.stat().st_size
        manifest = {
            "schema": "mtou-scene-ref-manifest/1",
            "world": {"name": "Lvl", "up_axis": "Z", "linear_unit": "cm"},
            "conventions": {"export_axis_option": "bForceFrontXAxis=false",
                            "engine_to_maya_point_map": "engine"},
            "scope": {"level": "/Game/Lvl", "completeness": "confirmed"},
            "counts": {"objects": 1, "triangles": 12, "bytes": size},
            "files": {"geometry": "Lvl.fbx", "geometry_bytes": size,
                      "image_data": False},
        }
        if anchor is not None:
            manifest["world"]["anchor"] = anchor
        text = json.dumps(manifest, indent=2).encode("utf-8")
        (directory / "Lvl.manifest.json").write_bytes(text)
        digest = hashlib.md5(text + geometry.read_bytes()).hexdigest()
        return {"transfer_id": transfer_id, "directory": str(directory),
                "digest": digest, "space_revision": 1,
                "scope": {"level": "/Game/Lvl", "sublevels": []},
                "counts": {"objects": 1, "triangles": 12, "bytes": size}}

    def _reference_scratch(self):
        import shutil
        root = pathlib.Path(__file__).resolve().parents[5]
        for candidate in (root.parent / ".tmp", root / ".tmp"):
            if candidate.is_dir():
                directory = pathlib.Path(tempfile.mkdtemp(
                    prefix="mtou_host_ref_", dir=str(candidate)))
                break
        else:
            directory = pathlib.Path(tempfile.mkdtemp(prefix="mtou_host_ref_"))
        self.addCleanup(shutil.rmtree, str(directory), True)
        return directory

    def _scene_snapshot(self):
        return sorted(cmds.ls(long=True) or []), sorted(cmds.ls(uuid=True) or [])

    def _container_group(self):
        name = mtou_config.REFERENCE_CONTAINER
        found = cmds.ls("{0}:{0}".format(name), long=True, type="transform") or []
        return found[0] if found else None

    def _container_meshes(self):
        name = mtou_config.REFERENCE_CONTAINER
        return sorted(path.rsplit("|", 1)[-1] for path in
                      cmds.ls(name + ":*", long=True, type="mesh") or [])

    def test_reference_fault_matrix_keeps_the_scene_and_the_previous_reference(self):
        # Real FBX handoffs through the real importer: a first import, an
        # update, refusals before any write, a failed import, a failure inside
        # the swap and a scene node under the reference. The production node,
        # the previous reference and its UUID survive every failure.
        import hashlib

        from mtou_livelink import reference
        scratch = self._reference_scratch()
        d003 = {"location_cm": [60.0, 40.0, 0.0],
                "rotation_deg": {"roll": 3.0, "pitch": -4.0, "yaw": 37.0}}
        hero = cmds.createNode("transform", name="HeroCtrl")
        cmds.setAttr(hero + ".translateX", 12.5)

        first = reference.import_handoff(
            self._real_handoff(scratch / "a", "SM_First", anchor=d003))
        self.assertTrue(first["accepted"], first["problems"])
        self.assertTrue(any("SM_First" in name for name in self._container_meshes()))
        self.assertIsNone(reference.stale_reference(d003))
        moved = dict(d003, location_cm=[160.0, 40.0, 0.0])
        self.assertIn("100.000 cm", reference.stale_reference(moved))

        update = reference.import_handoff(
            self._real_handoff(scratch / "b", "SM_Second", anchor=moved,
                               transfer_id="tx-host-2"))
        self.assertTrue(update["accepted"], update["problems"])
        meshes = self._container_meshes()
        self.assertTrue(any("SM_Second" in name for name in meshes), meshes)
        self.assertFalse(any("SM_First" in name for name in meshes), meshes)
        self.assertIsNone(reference.stale_reference(moved))
        previous_uuid = cmds.ls(self._container_group(), uuid=True)[0]

        # Stale, digest-mismatched and over-budget handoffs are refused before
        # any node is created.
        good = self._real_handoff(scratch / "c", "SM_Third")
        before = self._scene_snapshot()
        refusals = {
            "REFERENCE_STALE": dict(good, session_space_revision=7),
            "REFERENCE_DIGEST_MISMATCH": dict(good, digest="0" * 32),
        }
        for code, handoff in refusals.items():
            with self.assertRaises(reference.ReferenceRefused) as caught:
                reference.import_handoff(handoff)
            self.assertEqual(code, caught.exception.code)
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.import_handoff(good, {"bytes": 1, "objects": 1, "triangles": 1})
        self.assertEqual("REFERENCE_OVER_BUDGET", caught.exception.code)
        self.assertEqual(before, self._scene_snapshot())

        # A handoff whose FBX the importer cannot read fails without touching
        # the previous reference.
        broken = self._real_handoff(scratch / "d", "SM_Broken")
        fbx = pathlib.Path(broken["directory"]) / "Lvl.fbx"
        fbx.write_bytes(b"; FBX 7.7.0 project file\nObjects:  {\n\tModel: 1, garbage")
        manifest = pathlib.Path(broken["directory"]) / "Lvl.manifest.json"
        broken["digest"] = hashlib.md5(manifest.read_bytes() + fbx.read_bytes()).hexdigest()
        failed = reference.import_handoff(broken)
        self.assertFalse(failed["accepted"])
        self.assertEqual(previous_uuid, cmds.ls(self._container_group(), uuid=True)[0])
        self.assertTrue(any("SM_Second" in name for name in self._container_meshes()))

        # A failure inside the swap rolls back to the previous reference.
        original = reference.ReferenceImporter._rehome_paths

        def fail_rehome(importer):
            raise RuntimeError("the injected rehome failed")

        reference.ReferenceImporter._rehome_paths = fail_rehome
        try:
            rolled = reference.import_handoff(self._real_handoff(scratch / "e", "SM_Fifth"))
        finally:
            reference.ReferenceImporter._rehome_paths = original
        self.assertFalse(rolled["accepted"])
        self.assertTrue(rolled["rolled_back"], rolled["problems"])
        self.assertEqual(previous_uuid, cmds.ls(self._container_group(), uuid=True)[0])
        self.assertTrue(any("SM_Second" in name for name in self._container_meshes()))
        self.assertIsNone(reference.stale_reference(moved))

        # A scene node a person parented under the reference keeps it: the
        # update and the removal are refused and nothing is deleted.
        mesh_transform = cmds.listRelatives(
            cmds.ls(mtou_config.REFERENCE_CONTAINER + ":*", long=True, type="mesh")[0],
            parent=True, fullPath=True)[0]
        prop = cmds.createNode("transform", name="HeroProp", parent=mesh_transform)
        prop_uuid = cmds.ls(prop, uuid=True)[0]
        before = self._scene_snapshot()
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.import_handoff(self._real_handoff(scratch / "f", "SM_Sixth"))
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertIn("HeroProp", caught.exception.detail)
        self.assertEqual(before, self._scene_snapshot())
        with self.assertRaises(reference.ReferenceRefused) as caught:
            reference.ReferenceContainer().remove()
        self.assertEqual("CONTAINER_NOT_OWNED", caught.exception.code)
        self.assertEqual(before, self._scene_snapshot())
        self.assertEqual(1, len(cmds.ls(prop_uuid)))

        # Once the node is moved out, a locked node makes the removal fail with
        # its residual reported; unlocked, the removal completes.
        cmds.parent(cmds.ls(prop_uuid, long=True)[0], world=True)
        locked = cmds.ls(mtou_config.REFERENCE_CONTAINER + ":*", long=True,
                         type="mesh")[0]
        cmds.lockNode(locked, lock=True)
        report = reference.ReferenceContainer().remove()
        self.assertFalse(report["removed"])
        self.assertTrue(any("CONTAINER_REMOVAL_FAILED" in entry
                            for entry in report["problems"]), report["problems"])
        self.assertTrue(report["residual"]["nodes"])
        cmds.lockNode(locked, lock=False)
        report = reference.ReferenceContainer().remove()
        self.assertTrue(report["removed"], report["problems"])
        self.assertIsNone(self._container_group())
        self.assertIsNone(reference.stale_reference(moved))

        # The production nodes never changed.
        self.assertEqual(12.5, cmds.getAttr(hero + ".translateX"))
        self.assertEqual(1, len(cmds.ls("HeroCtrl")))
        self.assertEqual(1, len(cmds.ls("HeroProp")))


if __name__ == "__main__":
    unittest.main()
