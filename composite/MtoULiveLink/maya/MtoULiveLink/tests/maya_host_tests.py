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

        scene = module._CharacterScene.capture(root)
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

            def end_ordered(self, discard_pending=True):
                pass

        with mock.patch.object(module, "_SenderWorker", FakeWorker):
            session = module._StreamingSession.start(scene, 24.0, events.append)
            session.change_rate(30.0)
            session.stop()
            session.stop()

        self.assertEqual(["stopped"], [event.kind for event in events])
        self.assertTrue(FakeWorker.instance.stopped)
        self.assertEqual(1.0, FakeWorker.instance.joined)
        self.assertEqual(6, FakeWorker.instance.init_message["version"])
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

                def pause_for_cached(self, reply_listener=None):
                    pass

                def resume_from_cached(self):
                    self.resumed += 1

                def submit_cached(self, unused_frame):
                    pass

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
                if view.state == module._CachedPlayback.CAPTURING and view.current:
                    progress.append((view.current, view.total))
                    if view.current == 2:
                        holder["cached"].cancel_capture()

            with tempfile.TemporaryDirectory() as directory:
                cached = module._CachedPlayback(
                    FakeScene(), stream, on_change=on_change, timeline=timeline,
                    temp_dir=directory,
                    disk_usage=lambda unused: (1, 1, 1 << 40), timer_api=timer)
                holder["cached"] = cached
                cached.enter()
                cached.capture(24.0, lambda unused_size, unused_frames: True)
                while cached.view.state == module._CachedPlayback.CAPTURING:
                    timer.fire()
                self.assertEqual([(1, 3), (2, 3)], progress)
                self.assertFalse(timeline.is_playing())
                self.assertEqual(original_frame, timeline.current_frame())
                self.assertEqual(1, stream.resumed)
                self.assertIsNone(cached.view.cache_summary)
                self.assertEqual(module._CachedPlayback.REALTIME, cached.view.state)
                self.assertEqual([1, 2], timer.removed)
                cached.discard()
            self.assertFalse(playing["value"])


    def test_playback_state_query_uses_maya_host_state_without_editing_scene(self):
        current_time = cmds.currentTime(query=True)
        with mock.patch.object(module.cmds, "play", return_value=True) as play:
            self.assertTrue(module._maya_is_playing())
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

        scene = module._CharacterScene.capture(root)
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
        expected = module._sample_matrix(correct, unit_scale)
        skipped = module._sample_matrix(wrong, unit_scale)
        for actual, wanted in zip(frame.transforms[3], expected):
            self.assertAlmostEqual(wanted, actual, places=4)
        self.assertTrue(
            any(abs(actual - bad) > 1.0e-3
                for actual, bad in zip(frame.transforms[3], skipped)))

    def test_published_trs_reconstructs_bind_and_animated_worlds(self):
        # Independent reconstruction: convert wire UE (X,Z,Y) back to Maya,
        # build S*R*T from the transmitted quaternion, then multiply parents.
        # Do not call _sample_matrix or convert_transform in the oracle.
        om = module.om
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
        scene = module._CharacterScene.capture(root)
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

        first = module._capture_subject(first_root)
        second = module._capture_subject(second_root)

        self.assertEqual(
            [first_root, first_mid, first_tip],
            [bone["path"] for bone in first["bones"]])
        self.assertEqual(
            [second_root, second_mid, second_tip],
            [bone["path"] for bone in second["bones"]])
if __name__ == "__main__":
    unittest.main()
