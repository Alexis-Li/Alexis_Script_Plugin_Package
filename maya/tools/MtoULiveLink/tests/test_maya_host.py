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

    def test_captures_evaluated_joints_and_blendshape_alias(self):
        cmds.namespace(add="Hero")
        root = cmds.joint(name="Hero:root", position=(1, 2, 3))
        child = cmds.joint(name="Hero:spine", position=(1, 5, 3))
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


if __name__ == "__main__":
    unittest.main()
