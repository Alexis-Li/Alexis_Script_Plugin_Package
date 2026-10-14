# ruff: noqa: E402
"""Maya linkage host adapter tests (fake Maya bindings, no Maya session)."""

import pathlib
import sys
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from mtou_livelink import link_host as mtou_link_host  # noqa: E402


class FakeScene(object):
    def __init__(self, transforms=None, curves=None):
        self.sampled = 0
        self._transforms = transforms or [[0, 0, 0, 0, 0, 0, 1, 1, 1, 1]]
        self._curves = curves or [0.25]

    def sample(self):
        self.sampled += 1

        class Frame(object):
            transforms = self._transforms
            curves = self._curves

        return Frame()


class FakeCamera(object):
    def __init__(self, name=None):
        self.name = name
        self.applied = []
        self.readback_value = {"world_matrix": [1.0], "focal_length": 35.0}
        self.captured = 0
        self.restored = []

    def apply(self, camera_payload, gate_payload):
        self.applied.append((camera_payload, gate_payload))

    def readback(self):
        return dict(self.readback_value)

    def capture_state(self):
        self.captured += 1
        return {"look_through": "persp"}

    def restore_state(self, state):
        self.restored.append(state)


class FakeCameraModule(object):
    def __init__(self, camera):
        self.LinkCamera = lambda name=None: camera

    @staticmethod
    def maya_time_for_frame(display_frame, playback_start, display_rate,
                            maya_fps, maya_origin):
        return maya_origin + (display_frame - playback_start) * maya_fps / (
            float(display_rate["numerator"]) / float(display_rate["denominator"]))


def fake_cmds(time_value=1.0, playing=False):
    state = {"time": float(time_value), "playing": playing, "width": 1920,
             "height": 1080, "deviceAspectRatio": 1.7777777777777777,
             "pixelAspect": 1.0}
    commands = mock.MagicMock()

    def current_time(*args, **kwargs):
        if kwargs.get("query"):
            return state["time"]
        state["time"] = float(args[0])
        return state["time"]

    def get_attr(name, *args, **kwargs):
        key = name.split(".")[-1]
        return state[key]

    def set_attr(name, value, *args, **kwargs):
        state[name.split(".")[-1]] = value

    def play(*args, **kwargs):
        if kwargs.get("query"):
            return state["playing"]
        if "state" in kwargs:
            state["playing"] = bool(kwargs["state"])
        return state["playing"]

    def playback_options(*args, **kwargs):
        return 1.0 if kwargs.get("minTime") else 24.0

    commands.currentTime.side_effect = current_time
    commands.getAttr.side_effect = get_attr
    commands.setAttr.side_effect = set_attr
    commands.play.side_effect = play
    commands.playbackOptions.side_effect = playback_options
    return commands, state


def target(display_frame=5.0):
    return {
        "display_frame": display_frame,
        "playback_range": {"start": 1, "end": 24},
        "display_rate": {"numerator": 24, "denominator": 1},
    }


def build(fps=24.0, time_value=1.0):
    camera = FakeCamera()
    commands, state = fake_cmds(time_value)
    host = mtou_link_host.MayaLinkageHost(FakeScene(), ["character"], fps)
    patchers = [
        mock.patch.object(mtou_link_host, "cmds", commands),
        mock.patch.object(mtou_link_host, "camera_module",
                          return_value=FakeCameraModule(camera)),
        mock.patch.object(mtou_link_host, "require_maya", return_value=None),
    ]
    return host, camera, commands, state, patchers


class LinkageHostTests(unittest.TestCase):
    def test_capture_and_restore_cover_every_resource(self):
        host, camera, commands, state, patchers = build(time_value=7.0)
        for patcher in patchers:
            patcher.start()
            self.addCleanup(patcher.stop)
        captured = host.capture_state()
        self.assertEqual(7.0, captured["time"])
        self.assertEqual({"look_through": "persp"}, captured["camera"])
        self.assertEqual(1920, captured["gate"]["width"])
        self.assertEqual(1, camera.captured)

        state["time"] = 20.0
        state["width"] = 1280
        residual = host.restore_state(captured)
        self.assertEqual([], residual)
        self.assertEqual(7.0, state["time"])
        self.assertEqual(1920, state["width"])
        self.assertEqual([{"look_through": "persp"}], camera.restored)

    def test_restore_reports_a_resource_it_cannot_put_back(self):
        host, camera, commands, state, patchers = build()
        for patcher in patchers:
            patcher.start()
            self.addCleanup(patcher.stop)
        captured = host.capture_state()
        camera.restore_state = mock.Mock(side_effect=RuntimeError("panel is gone"))
        residual = host.restore_state(captured)
        self.assertEqual(1, len(residual))
        self.assertTrue(residual[0].startswith("camera:"))
        self.assertIn("panel is gone", residual[0])

    def test_apply_maps_time_applies_camera_and_reports_readback(self):
        host, camera, commands, state, patchers = build(time_value=3.0)
        for patcher in patchers:
            patcher.start()
            self.addCleanup(patcher.stop)
        applied_time, readback = host.apply_target(
            target(5.0), {"focal_length_mm": 35.0}, {"output_width": 1920})
        # origin 3.0 + (5.0 - 1) * 24 / 24
        self.assertEqual(7.0, applied_time["maya_frame"])
        self.assertEqual(7.0, state["time"])
        self.assertEqual(5.0, applied_time["ue_display_frame"])
        self.assertEqual([({"focal_length_mm": 35.0}, {"output_width": 1920})],
                         camera.applied)
        self.assertEqual(35.0, readback["focal_length"])

    def test_apply_uses_the_captured_origin_not_the_current_frame(self):
        host, camera, commands, state, patchers = build(time_value=3.0)
        for patcher in patchers:
            patcher.start()
            self.addCleanup(patcher.stop)
        captured = host.capture_state()
        host.set_origin_frame(captured["time"])
        state["time"] = 11.0
        applied_time, unused_readback = host.apply_target(
            target(5.0), {}, {})
        self.assertEqual(7.0, applied_time["maya_frame"])

    def test_sample_group_returns_one_complete_row_per_object(self):
        scene = FakeScene()
        camera = FakeCamera()
        host = mtou_link_host.MayaLinkageHost(scene, ["character", "prop"], 24.0)
        with mock.patch.object(mtou_link_host, "camera_module",
                               return_value=FakeCameraModule(camera)):
            rows = host.sample_group(["character", "prop"])
        self.assertEqual(["character", "prop"],
                         [row["object_id"] for row in rows])
        for row in rows:
            self.assertEqual([[0, 0, 0, 0, 0, 0, 1, 1, 1, 1]],
                             [list(transform) for transform in row["transforms"]])
        self.assertEqual([0.25], list(rows[0]["curves"]))
        self.assertEqual(2, scene.sampled)

    def test_camera_module_is_imported_only_when_linkage_starts(self):
        host = mtou_link_host.MayaLinkageHost(FakeScene(), ["character"], 24.0)
        with mock.patch.object(mtou_link_host, "camera_module") as loader:
            loader.return_value = FakeCameraModule(FakeCamera())
            host.sample_group(["character"])
            loader.assert_not_called()
            host._camera_adapter()
            loader.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
