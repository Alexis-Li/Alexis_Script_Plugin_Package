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
        unused_group, root, unused_display = self._create_character_group()
        del unused_group, unused_display
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

        scene = module._CharacterScene.capture(root)
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

        scene = module._CharacterScene.capture(root)
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

        scene = module._CharacterScene.capture(root)
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

        scene = module._CharacterScene.capture(root)
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

        with self.assertRaises(module._CharacterSceneError) as caught:
            module._CharacterScene.capture(root)

        self.assertEqual("BIND_POSE_INVALID", caught.exception.code)
        self.assertIn("equally ranked bind matrices", caught.exception.details)
        self.assertIn("aSecondSkin.bindPreMatrix[0]", caught.exception.details)
        self.assertIn("zFirstSkin.bindPreMatrix[0]", caught.exception.details)

    def test_non_finite_bindpre_matrix_is_rejected_before_inverse(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, mesh, name="bodySkin")
        original_matrix_attr = module._matrix_attr

        def matrix_attr(plug):
            if ".bindPreMatrix[" in plug:
                values = [1.0, 0.0, 0.0, 0.0,
                          0.0, 1.0, 0.0, 0.0,
                          0.0, 0.0, 1.0, 0.0,
                          0.0, 0.0, 0.0, float("inf")]
                return module.om.MMatrix(values)
            return original_matrix_attr(plug)

        with mock.patch.object(module, "_matrix_attr", side_effect=matrix_attr):
            with self.assertRaises(module._CharacterSceneError) as caught:
                module._CharacterScene.capture(root)

        self.assertEqual("BIND_POSE_INVALID", caught.exception.code)
        self.assertIn("non-finite bind matrix", caught.exception.details)
        self.assertIn("bodySkin.bindPreMatrix[0]", caught.exception.details)

    def test_non_invertible_bindpre_matrix_is_rejected_before_inverse(self):
        group, root, unused_display = self._create_character_group()
        del group, unused_display
        mesh = cmds.polyCube(name="body")[0]
        cmds.skinCluster(root, mesh, name="bodySkin")
        original_matrix_attr = module._matrix_attr

        def matrix_attr(plug):
            if ".bindPreMatrix[" in plug:
                return module.om.MMatrix([0.0] * 16)
            return original_matrix_attr(plug)

        with mock.patch.object(module, "_matrix_attr", side_effect=matrix_attr):
            with self.assertRaises(module._CharacterSceneError) as caught:
                module._CharacterScene.capture(root)

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

        scene = module._CharacterScene.capture(root)
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

        scene = module._CharacterScene.capture(root)
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

        scene = module._CharacterScene.capture(root)
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
        self.assertEqual(4, FakeWorker.instance.init_message["version"])
        self.assertEqual("animation", FakeWorker.instance.init_message["workflow"])
        self.assertTrue(FakeWorker.instance.init_message["blendshapes_enabled"])
        self.assertEqual(3, len(FakeWorker.instance.init_message["bones"][0]))

    def test_cached_capture_uses_real_playback_range_and_restores_after_cancel(self):
        cmds.playbackOptions(min=10, max=12)
        cmds.currentTime(11, edit=True)
        playing = {"value": True}

        def play(**kwargs):
            if "state" in kwargs:
                playing["value"] = bool(kwargs["state"])

        with mock.patch.object(
                module, "_maya_is_playing", side_effect=lambda: playing["value"]), \
                mock.patch.object(module.cmds, "play", side_effect=play):
            timeline = module._MayaTimeline()
            self.assertTrue(timeline.is_playing())
            original_frame = timeline.current_frame()
            snapshot = module._CharacterSnapshot(
                1, "|root", "Clothes01", [("root", -1)], [],
                bind_local_transforms=[(0.0,) * 10])

            class FakeScene(object):
                @staticmethod
                def snapshot():
                    return snapshot

                @staticmethod
                def sample():
                    value = float(cmds.currentTime(query=True))
                    return module._CharacterFrame(1, [[value] * 10], [])

            class FakeStream(object):
                is_ready = True
                revision = 1

                def __init__(self):
                    self.resumed = 0

                def pause_for_cached(self):
                    pass

                def resume_from_cached(self):
                    self.resumed += 1

                def submit_cached(self, unused_frame):
                    pass

                def end_cached_replay(self):
                    pass

            stream = FakeStream()
            progress = []

            def cancel_after_two(current, total):
                progress.append((current, total))
                if current == 2:
                    session.cancel_capture()

            with tempfile.TemporaryDirectory() as directory:
                session = module._CachedPlaybackSession(
                    FakeScene(), stream, timeline=timeline, temp_dir=directory,
                    clock=lambda: 0.0,
                    disk_usage=lambda unused: (1, 1, 1 << 40), timer_api=None)
                self.assertIsNone(session.capture_and_replay(
                    scene_fps=24.0, progress=cancel_after_two))
                self.assertEqual([(1, 3), (2, 3)], progress)
                self.assertFalse(timeline.is_playing())
                self.assertEqual(original_frame, timeline.current_frame())
                self.assertEqual(1, stream.resumed)
                self.assertIsNone(session.cache)
                session.close()
            self.assertFalse(playing["value"])


    def test_playback_state_query_uses_maya_host_state_without_editing_scene(self):
        current_time = cmds.currentTime(query=True)
        with mock.patch.object(module.cmds, "play", return_value=True) as play:
            self.assertTrue(module._maya_is_playing())
        play.assert_called_once_with(query=True, state=True)
        self.assertEqual(current_time, cmds.currentTime(query=True))
if __name__ == "__main__":
    unittest.main()
