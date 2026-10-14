"""Maya host adapter for the linkage state machine.

`LinkageSession` owns the state and the scheduling; this adapter owns every
Maya call it needs: capturing the state linkage must restore, applying one
camera/time target, sampling the objects, and reporting what the host really
did. Keeping the Maya calls here lets the state machine stay host-free and
testable, and keeps one place responsible for the restore order.
"""

from .mayaapi import cmds
from .scene import require_maya


def camera_module():
    """The camera adapter, imported when linkage actually starts.

    Keeping the import inside the call lets this module stay importable while
    the panel is built, and keeps the Maya camera dependency in one place.
    """
    from . import camera

    return camera


class MayaLinkageHost(object):
    """Applies linkage targets in one Maya scene."""

    def __init__(self, scene, object_ids, fps, camera=None, link_camera_name=None):
        self._scene = scene
        self._object_ids = tuple(object_ids)
        self._fps = float(fps)
        self._camera = camera
        self._link_camera_name = link_camera_name

    # ------------------------------------------------------------- capture
    def capture_state(self):
        """Capture everything a later restore must put back."""
        require_maya()
        state = {
            "time": self._current_time(),
            "playback": self._playback_state(),
            "gate": self._resolution_gate(),
            "camera": self._camera_adapter().capture_state(),
        }
        return state

    def restore_state(self, state):
        """Restore the captured state; returns the resources it could not."""
        residual = []
        camera = self._camera_adapter()
        try:
            camera.restore_state(state.get("camera"))
        except Exception as exc:
            residual.append("camera: {0}".format(exc))
        try:
            self._restore_resolution_gate(state.get("gate"))
        except Exception as exc:
            residual.append("gate: {0}".format(exc))
        try:
            self._restore_playback_state(state.get("playback"))
        except Exception as exc:
            residual.append("playback: {0}".format(exc))
        try:
            self._set_time(state.get("time"))
        except Exception as exc:
            residual.append("time: {0}".format(exc))
        return residual

    # --------------------------------------------------------------- apply
    def apply_target(self, time_payload, camera_payload, gate_payload):
        """Apply one linkage target and report what Maya actually holds."""
        require_maya()
        camera = self._camera_adapter()
        camera.apply(camera_payload, gate_payload)
        maya_time = camera_module().maya_time_for_frame(
            time_payload["display_frame"],
            time_payload["playback_range"]["start"],
            time_payload["display_rate"],
            self._scene_fps(),
            self._origin_frame())
        self._set_time(maya_time)
        return self._time_report(maya_time, time_payload), camera.readback()

    def sample_group(self, object_ids):
        """Sample the complete pose of every object of the current set."""
        rows = []
        for object_id in object_ids:
            frame = self._scene.sample()
            rows.append({
                "object_id": object_id,
                "transforms": [list(transform) for transform in frame.transforms],
                "curves": list(frame.curves),
            })
        return rows

    def set_origin_frame(self, frame):
        self._origin = float(frame)

    # ------------------------------------------------------------ internals
    def _origin_frame(self):
        return getattr(self, "_origin", self._current_time())

    def _scene_fps(self):
        return self._fps

    def _camera_adapter(self):
        if self._camera is None:
            self._camera = camera_module().LinkCamera(self._link_camera_name)
        return self._camera

    def _current_time(self):
        return float(cmds.currentTime(query=True))

    def _set_time(self, maya_time):
        if maya_time is None:
            return
        cmds.currentTime(float(maya_time), edit=True, update=False)

    def _time_report(self, maya_time, time_payload):
        applied = self._current_time()
        return {
            "maya_frame": applied,
            "ue_display_frame": float(time_payload["display_frame"]),
            "requested_maya_frame": float(maya_time),
            "subframe": applied != int(applied),
        }

    def _playback_state(self):
        return {
            "playing": bool(cmds.play(query=True, state=True)),
            "range": [float(cmds.playbackOptions(query=True, minTime=True)),
                      float(cmds.playbackOptions(query=True, maxTime=True))],
            "current": self._current_time(),
        }

    def _restore_playback_state(self, state):
        if not state:
            return
        if not state.get("playing", False):
            cmds.play(state=False)
        self._set_time(state.get("current"))

    def _resolution_gate(self):
        return {
            "width": int(cmds.getAttr("defaultResolution.width")),
            "height": int(cmds.getAttr("defaultResolution.height")),
            "device_aspect_ratio": float(
                cmds.getAttr("defaultResolution.deviceAspectRatio")),
            "pixel_aspect_ratio": float(
                cmds.getAttr("defaultResolution.pixelAspect")),
        }

    def _restore_resolution_gate(self, state):
        if not state:
            return
        for name, value in (("width", state.get("width")),
                            ("height", state.get("height"))):
            if value is not None:
                cmds.setAttr("defaultResolution." + name, int(value))
        if state.get("device_aspect_ratio") is not None:
            cmds.setAttr("defaultResolution.deviceAspectRatio",
                         float(state["device_aspect_ratio"]))
        if state.get("pixel_aspect_ratio") is not None:
            cmds.setAttr("defaultResolution.pixelAspect",
                         float(state["pixel_aspect_ratio"]))
