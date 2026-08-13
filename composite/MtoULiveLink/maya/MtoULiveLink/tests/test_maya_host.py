import importlib.util
import pathlib
import unittest

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

    def _create_display(self, parent, entries="Clothes01:Clothes02"):
        add_group = cmds.createNode("transform", name="Add_Ctrl_grp", parent=parent)
        display_group = cmds.createNode(
            "transform", name="Display_ctrl_grp", parent=add_group
        )
        display = cmds.createNode("transform", name="Display_ctrl", parent=display_group)
        cmds.addAttr(display, longName="clothes", attributeType="enum", enumName=entries)
        return display

    def test_discovers_display_enum_and_visible_skinned_meshes(self):
        group = cmds.createNode("transform", name="Group")
        motion = cmds.createNode("transform", name="MotionSystem", parent=group)
        display = self._create_display(motion)
        geometry = cmds.createNode("transform", name="Geometry", parent=group)
        high_mesh = cmds.createNode("transform", name="HighMesh", parent=geometry)
        root = cmds.joint(name="root")
        cmds.parent(root, group)
        visible = cmds.polyCube(name="SM_C01_Clothes01")[0]
        hidden = cmds.polyCube(name="SM_C01_Clothes02")[0]
        cmds.parent(visible, hidden, high_mesh)
        cmds.skinCluster(root, visible, name="visibleSkin")
        cmds.skinCluster(root, hidden, name="hiddenSkin")
        cmds.setAttr(hidden + ".visibility", False)

        self.assertEqual(["|Group|MotionSystem|Add_Ctrl_grp|Display_ctrl_grp|Display_ctrl"],
                         module._display_candidates("|Group|root"))
        enum = module._enum_attributes(display)
        self.assertEqual(["clothes"], [item["attribute"] for item in enum])
        self.assertEqual("Clothes01", module._current_enum_label(enum[0]))
        self.assertEqual(["|Group|Geometry|HighMesh|SM_C01_Clothes01"],
                         module._visible_skinned_meshes(["|Group|root"]))

    def test_capture_rejects_character_without_visible_skinned_mesh(self):
        root = cmds.joint(name="root")
        mesh = cmds.polyCube(name="hiddenBody")[0]
        cmds.skinCluster(root, mesh)
        cmds.setAttr(mesh + ".visibility", False)
        with self.assertRaisesRegex(ValueError, "NO_VISIBLE_SKINNED_MESH"):
            module._capture_subject(root)

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

        controller = module._Controller()
        controller._set_duplicate_paths(paths)
        controller.select_duplicate_bones()

        self.assertEqual(sorted(paths), sorted(cmds.ls(selection=True, long=True)))

    def test_captures_evaluated_joints_and_blendshape_alias(self):
        cmds.namespace(add="Hero")
        root = cmds.joint(name="Hero:root", position=(1, 2, 3))
        cmds.joint(name="Hero:spine", position=(1, 5, 3))
        base = cmds.polyCube(name="body")[0]
        target = cmds.duplicate(base, name="smileTarget")[0]
        blendshape = cmds.blendShape(target, base, name="faceBS")[0]
        cmds.aliasAttr("Smile", blendshape + ".weight[0]")
        cmds.skinCluster(root, base, name="bodySkin")
        cmds.setAttr(blendshape + ".Smile", 0.25)
        cmds.select(root, replace=True)

        subject = module._capture_subject()
        transforms, curves = module._sample_pose(subject)

        self.assertEqual(["root", "spine"], [bone["name"] for bone in subject["bones"]])
        self.assertEqual([1.0, 3.0, 2.0], transforms[0][:3])
        self.assertEqual([0.0, 0.0, 3.0], transforms[1][:3])
        self.assertEqual(["Smile"], [curve["name"] for curve in subject["curves"]])
        self.assertEqual([0.25], curves)

    def test_same_alias_on_multiple_mesh_parts_streams_one_curve(self):
        root = cmds.joint(name="root")
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

        subject = module._capture_subject()
        _, curves = module._sample_pose(subject)

        self.assertEqual(["Smile"], [curve["name"] for curve in subject["curves"]])
        self.assertEqual([0.25], curves)

    def test_same_alias_with_conflicting_values_is_rejected(self):
        root = cmds.joint(name="root")
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

        with self.assertRaisesRegex(
            ValueError, "conflicting values for BlendShape alias Smile"
        ):
            module._sample_pose(module._capture_subject())


if __name__ == "__main__":
    unittest.main()
