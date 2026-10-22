"""Panel, controller and window tests for the linkage and reference workflow.

Everything runs against fake ``cmds``/``om`` bindings and a fake streaming
session, so no timer, socket or Unreal peer is involved: the pump is driven by
invoking the recorded timer callback and deferred panel work is flushed by
hand. The controller's linkage state comes from the real ``LinkageSession``
state machine over a fake host, which is what the panel wires in production.
"""

import contextlib
import itertools
import pathlib
import sys
import types
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from mtou_livelink import cache as mtou_cache  # noqa: E402
from mtou_livelink import config as mtou_config  # noqa: E402
from mtou_livelink import controller as mtou_controller  # noqa: E402
from mtou_livelink import diagnostics as mtou_diagnostics  # noqa: E402
from mtou_livelink import linkage as mtou_linkage  # noqa: E402
from mtou_livelink import mayaapi as mtou_mayaapi  # noqa: E402
from mtou_livelink import protocol as mtou_protocol  # noqa: E402
from mtou_livelink import scene as mtou_scene  # noqa: E402
from mtou_livelink import session as mtou_session  # noqa: E402
from mtou_livelink import space as mtou_space  # noqa: E402
from mtou_livelink import timeline as mtou_timeline  # noqa: E402
from mtou_livelink.ui import link_window as mtou_link_window  # noqa: E402
from mtou_livelink.ui import reference_window as mtou_reference_window  # noqa: E402

# Every module that binds a Maya global at import time; the detail windows read
# theirs through `mayaapi`, so patching that module covers them too.
_PACKAGE_MODULES = (mtou_cache, mtou_config, mtou_controller, mtou_diagnostics,
                    mtou_linkage, mtou_mayaapi, mtou_protocol, mtou_scene,
                    mtou_session, mtou_space, mtou_timeline,
                    mtou_link_window, mtou_reference_window)


class _Bindings(object):
    """Patch the Maya bindings on every package module for one block."""

    def __init__(self, cmds=None, om=None):
        self._cmds = cmds
        self._om = om
        self._stack = None

    def __enter__(self):
        self._stack = contextlib.ExitStack()
        for name, value in (("cmds", self._cmds), ("om", self._om)):
            if value is None:
                continue
            for module in _PACKAGE_MODULES:
                if hasattr(module, name):
                    self._stack.enter_context(
                        mock.patch.object(module, name, value))
        return self

    def __exit__(self, *exc_info):
        self._stack.close()
        self._stack = None
        return False


def camera_time(eval_id="seq@10+0/cam#c1", eval_serial=1, transport_serial=1,
                frame_serial=1, display_frame=10.0, session_id=7,
                mode_revision=1, set_revision=1, space_revision=1):
    return {
        "type": "camera_time",
        "session_id": session_id,
        "set_revision": set_revision,
        "mode_revision": mode_revision,
        "space_revision": space_revision,
        "eval_id": eval_id,
        "eval_serial": eval_serial,
        "transport_serial": transport_serial,
        "frame_serial": frame_serial,
        "time": {
            "display_frame": display_frame,
            "source_frame": int(display_frame),
            "seconds": 0.0,
            "tick": 0,
            "display_rate": {"numerator": 24, "denominator": 1},
            "tick_resolution": {"numerator": 24000, "denominator": 1},
            "playback_range": {"start": 0, "end": 100},
        },
        "camera": {"focal_length": 35.0},
        "gate": {"width": 1920, "height": 1080},
        "cut": {"stage": "root", "name": "shotA"},
    }


def applied_message(eval_id="seq@10+0/cam#c1", sample_serial=1, applied=True,
                    mode_revision=1, session_id=7, reason=""):
    return {
        "type": "applied",
        "session_id": session_id,
        "mode_revision": mode_revision,
        "eval_id": eval_id,
        "sample_serial": sample_serial,
        "applied": applied,
        "applied_display_frame": 10.0,
        "lag_seconds": 0.02,
        "components": [{"object_id": "character", "bones": 3, "morphs": 1,
                        "parts": 0}],
        "reason": reason,
    }


class FakeLinkageHost(object):
    """The Maya host adapter's contract, without Maya."""

    def __init__(self, residual=(), capture_error=None):
        self.captured = 0
        self.restored = []
        self.applied = []
        self.restore_residual = list(residual)
        self.capture_error = capture_error

    def capture_state(self):
        self.captured += 1
        if self.capture_error is not None:
            raise self.capture_error
        return {"time": 42.0, "camera": "persp"}

    def restore_state(self, state):
        self.restored.append(state)
        return list(self.restore_residual)

    def apply_target(self, time_payload, camera, gate):
        self.applied.append((time_payload, camera, gate))
        return ({"maya_frame": time_payload["display_frame"],
                 "ue_display_frame": time_payload["display_frame"]},
                {"world_matrix": [1.0]})

    def sample_group(self, object_ids):
        return [{"object_id": object_ids[0],
                 "transforms": [[0.0] * 10], "curves": [0.25]}]


class FakeStreamingSession(object):
    """The streaming session surface the linkage and reference paths use."""

    def __init__(self, ready=True, session_id=7, set_revision=1,
                 space_revision=1):
        self._ready = ready
        self.hello = {"session_id": session_id, "version": 10}
        self.messages = []
        self.begun = []
        self.resumed = 0
        self.listeners = []
        self.object_id = mtou_config.CHARACTER_OBJECT_ID
        self.set_revision = set_revision
        self.space_revision = space_revision

    def is_ready(self):
        return self._ready

    @property
    def hello_reply(self):
        return dict(self.hello)

    def begin_linkage(self, reply_listener=None):
        self.begun.append(reply_listener)
        self.listeners.append(reply_listener)

    def submit_linkage(self, message):
        self.messages.append(message)
        return True

    def resume_after_linkage(self):
        self.resumed += 1

    def stop(self):
        self.stopped = True

    def set_cached_reply_listener(self, listener):
        self.listeners.append(listener)


class FakeCharacterScene(object):
    """One sampleable character scene."""

    def sample(self):
        return types.SimpleNamespace(
            transforms=((0.0,) * 10,), curves=(0.0,))

    def close(self):
        self.closed = True


class FakeReferenceModule(object):
    """The scene-reference module contract, recording every call."""

    class ReferenceRefused(RuntimeError):
        def __init__(self, code, detail=""):
            RuntimeError.__init__(self, detail or code)
            self.code = code
            self.detail = detail

    def __init__(self, inspect_report=None, import_report=None,
                 inspect_error=None, import_error=None):
        self.inspect_report = inspect_report or {"accepted": True, "detail": "ok"}
        self.import_report = import_report
        self.inspect_error = inspect_error
        self.import_error = import_error
        self.calls = []

    def inspect(self, handoff, budget=None):
        self.calls.append(("inspect", handoff, budget))
        if self.inspect_error is not None:
            raise self.inspect_error
        return dict(self.inspect_report)

    def import_handoff(self, handoff, budget=None):
        self.calls.append(("import_handoff", handoff, budget))
        if self.import_error is not None:
            raise self.import_error
        return dict(self.import_report or {})

    def ReferenceContainer(self, **kwargs):
        self.calls.append(("container", kwargs))
        return self

    def remove(self):
        self.calls.append(("remove",))
        return {"removed": True, "namespace_removed": True, "nodes_removed": 3,
                "detail": "removed", "residual": {}}

    def recover(self):
        self.calls.append(("recover",))
        return {"stale_staging_namespaces": [], "restored_previous_reference": True}


def stale_reference():
    """A stale-manifest refusal, with the frozen code the module raises."""
    class _Stale(FakeReferenceModule.ReferenceRefused):
        def __init__(self):
            FakeReferenceModule.ReferenceRefused.__init__(
                self, "REFERENCE_STALE", "manifest belongs to an older space")

    return _Stale()


class MainThreadDeferralTests(unittest.TestCase):
    """Unreal statements arrive on the sender thread and must reach the panel.

    An interactive Maya silently drops ``cmds.evalDeferred`` called from a
    worker thread, so inside Maya the thread-safe ``maya.utils`` queue is used.
    """

    def test_statements_use_the_thread_safe_queue_inside_maya(self):
        queued = []
        fake_cmds = mock.MagicMock()
        fake_utils = types.SimpleNamespace(executeDeferred=queued.append)
        with mock.patch.object(mtou_mayaapi, "utils", fake_utils), \
                mock.patch.object(mtou_controller, "cmds", fake_cmds):
            controller = mtou_controller.Controller()

            def action():
                return None

            controller._defer_ui(action)
        self.assertEqual([action], queued)
        fake_cmds.evalDeferred.assert_not_called()

    def test_without_maya_utils_the_cmds_queue_is_used(self):
        fake_cmds = mock.MagicMock()
        with mock.patch.object(mtou_mayaapi, "utils", None), \
                mock.patch.object(mtou_controller, "cmds", fake_cmds):
            controller = mtou_controller.Controller()

            def action():
                return None

            controller._defer_ui(action)
        fake_cmds.evalDeferred.assert_called_once_with(action)

    def test_a_late_group_refusal_is_counted_not_shown(self):
        # A group still in flight when linkage ended is refused by Unreal; the
        # panel counts it like a cancelled target instead of an error dialog.
        fake_cmds = mock.MagicMock()
        with mock.patch.object(mtou_mayaapi, "utils", None), \
                mock.patch.object(mtou_controller, "cmds", fake_cmds):
            controller = mtou_controller.Controller()
            controller._linkage = mock.Mock()
            controller._report_runtime_error({
                "type": "error", "code": "LINKAGE_NOT_ACTIVE",
                "message": "A group arrived while another host owns time."})
        controller._linkage.note_refused.assert_called_once()
        fake_cmds.evalDeferred.assert_not_called()


class LinkagePanelTests(unittest.TestCase):
    """Shared harness: one controller, fake bindings and no Unreal peer."""

    def setUp(self):
        self.timers = []
        self.removed_callbacks = []
        self.deferred = []
        self.host = FakeLinkageHost()
        self.session = FakeStreamingSession()
        self.reference = FakeReferenceModule()
        self.controller = mtou_controller.Controller()
        self.controller._session = self.session
        self.controller._scene = FakeCharacterScene()
        self.fake_cmds = self._fake_cmds()
        self.fake_om = self._fake_om()
        harness = self
        host_patch = mock.patch.object(
            mtou_controller.Controller, "_linkage_host",
            lambda controller, scene, object_ids, frame_rate: harness.host)
        host_patch.start()
        self.addCleanup(host_patch.stop)
        module_patch = mock.patch.object(
            mtou_controller.Controller, "_reference_module_or_report",
            lambda controller: harness.reference)
        module_patch.start()
        self.addCleanup(module_patch.stop)
        error_patch = mock.patch.object(
            mtou_controller.Controller, "_show_error")
        error_patch.start()
        self.addCleanup(error_patch.stop)
        self.show_error = mtou_controller.Controller._show_error

    # ----------------------------------------------------------- fake Maya
    def _fake_cmds(self):
        fake = mock.MagicMock()
        fake.control.return_value = True
        fake.currentUnit.return_value = "film"
        fake.confirmDialog.return_value = "确定"
        windows = set()

        def window(*args, **kwargs):
            name = args[0] if args else kwargs.get("name")
            if kwargs.get("exists"):
                return name in windows
            if kwargs.get("query"):
                return None
            windows.add(name)
            return name

        def delete_ui(name, *args, **kwargs):
            windows.discard(name)

        def eval_deferred(action, *args, **kwargs):
            self.deferred.append(action)

        fake.window.side_effect = window
        fake.deleteUI.side_effect = delete_ui
        fake.evalDeferred.side_effect = eval_deferred
        counter = itertools.count()
        for name in ("text", "button", "checkBox", "optionMenu", "menuItem",
                     "intField", "frameLayout", "rowLayout", "columnLayout",
                     "formLayout", "scriptJob", "separator"):
            getattr(fake, name).side_effect = (
                lambda *args, __name=name, **kwargs:
                    "{0}-{1}".format(__name, next(counter)))
        return fake

    def _fake_om(self):
        class _Timer(object):
            @staticmethod
            def addTimerCallback(interval, callback):
                self.timers.append((interval, callback))
                return len(self.timers)

        class _Message(object):
            @staticmethod
            def removeCallback(callback_id):
                self.removed_callbacks.append(callback_id)

        return type("FakeOpenMaya", (), {
            "MTimerMessage": _Timer,
            "MMessage": _Message,
        })

    # -------------------------------------------------------------- helpers
    @property
    def bindings(self):
        return _Bindings(self.fake_cmds, self.fake_om)

    def enter_linkage(self):
        with self.bindings:
            self.controller.enter_linkage()

    def pump(self):
        """Run the one pump tick the controller's timer would run."""
        with self.bindings:
            self.controller._pump_linkage()

    def run_timer(self):
        interval, callback = self.timers[-1]
        with self.bindings:
            callback()

    def flush(self):
        """Run the panel work the controller deferred to Maya's main thread."""
        while self.deferred:
            self.deferred.pop(0)()

    def labels(self):
        return [call[1].get("label") for call in self.fake_cmds.text.call_args_list
                if call[1].get("label")]

    def build_ui(self):
        with self.bindings:
            self.controller.build_ui()

    # --------------------------------------------------------------- tests
    def test_panel_offers_the_linkage_card_with_its_actions(self):
        self.build_ui()

        buttons = [call[1].get("label")
                   for call in self.fake_cmds.button.call_args_list
                   if call[1].get("label")]
        for label in ("进入联动", "返回实时", "结束预览",
                      "打开镜头联动窗口", "打开场景参考窗口"):
            self.assertIn(label, buttons)
        cards = [call[1].get("label")
                 for call in self.fake_cmds.frameLayout.call_args_list
                 if call[1].get("label")]
        self.assertIn("镜头联动", cards)
        self.assertEqual("控制方：Maya", self.controller._link_control_text
                         and self.controller.linkage_status()["labels"]["control"])
        self.assertIn("模式：实时预览", self.labels())
        self.assertIn("恢复结果：无", self.labels())
        # The disabled actions keep a reason exactly like the cache actions.
        tooltips = [call[1].get("annotation")
                    for call in self.fake_cmds.control.call_args_list
                    if call[1].get("annotation")]
        self.assertTrue(any("连接 Unreal" in tooltip for tooltip in tooltips))

    def test_cache_summary_stays_inside_its_column_with_the_full_text_as_tooltip(self):
        self.build_ui()
        created = [call[1] for call in self.fake_cmds.text.call_args_list
                   if call[1].get("label") == "缓存：无" and not call[1].get("edit")]
        self.assertEqual(1, len(created))
        self.assertEqual(mtou_controller.CACHE_TEXT_WIDTH, created[0].get("width"))
        self.assertLessEqual(mtou_controller.CACHE_TEXT_WIDTH
                             + mtou_controller.CACHE_TEXT_INDENT,
                             mtou_controller.CARD_HALF)

        def summary(start, end, fps):
            return types.SimpleNamespace(capture_range=(start, end),
                                         frame_count=end - start + 1,
                                         scene_fps=fps, capture_time=0.0)

        def render(value):
            self.fake_cmds.text.reset_mock()
            with self.bindings:
                self.controller._update_cache_text(value)
            edit = self.fake_cmds.text.call_args_list[-1][1]
            self.assertTrue(edit.get("edit"))
            return edit["label"], edit["annotation"]

        label, tooltip = render(summary(100, 160, 30.0))
        self.assertEqual("缓存：100-160（61 帧）", label)
        self.assertRegex(tooltip, r"^缓存：100-160，61 帧，30 fps，捕获于 \d{4}-\d\d-\d\d ")
        for start, end in ((-9999, 10000), (-123456789, -123436790),
                           (12345678901234, 12345678921233)):
            label, tooltip = render(summary(start, end, 23.976))
            self.assertLessEqual(mtou_controller._text_units(label),
                                 mtou_controller.CACHE_LABEL_UNITS, label)
            self.assertTrue(label.startswith("缓存："), label)
            self.assertIn("{0}-{1}，{2} 帧，23.976 fps".format(
                start, end, end - start + 1), tooltip)
        self.assertEqual(("缓存：无", "缓存：无"), render(None))

    def test_enter_linkage_refuses_without_a_ready_session(self):
        self.session._ready = False
        self.enter_linkage()

        diagnostic = self.show_error.call_args[0][0]
        self.assertEqual("STREAM_INTERRUPTED", diagnostic["code"])
        self.assertIn("连接", diagnostic["solution"])
        self.assertIsNone(self.controller._linkage)
        self.assertEqual([], self.timers)
        self.assertEqual(0, self.host.captured)
        self.assertEqual([], self.session.begun)
        self.assertEqual(mtou_config.REALTIME_MODE, self.controller._mode)

    def test_enter_linkage_refuses_while_cached_playback_owns_the_session(self):
        self.controller._mode = mtou_config.CACHED_MODE
        self.controller._cached_view = mtou_cache.CachedPlaybackView(
            mtou_cache.CachedPlayback.REPLAYING)
        self.enter_linkage()

        diagnostic = self.show_error.call_args[0][0]
        self.assertEqual("CACHE_NOT_CLEARED", diagnostic["code"])
        self.assertIn("实时预览", diagnostic["solution"])
        self.assertIsNone(self.controller._linkage)
        self.assertEqual([], self.timers)
        self.assertEqual(0, self.host.captured)

    def test_enter_linkage_reports_a_host_capture_refusal(self):
        self.host.capture_error = RuntimeError("panel camera missing")
        self.enter_linkage()

        self.assertIsNone(self.controller._linkage)
        self.assertEqual(mtou_config.REALTIME_MODE, self.controller._mode)
        self.assertEqual([], self.timers)
        diagnostic = self.show_error.call_args[0][0]
        self.assertIn("panel camera missing", diagnostic["details"])
        # The refused attempt leaves the session's sampling path untouched.
        self.assertEqual([], self.session.messages)

    def test_enter_linkage_starts_the_pump_and_announces_the_mode(self):
        self.enter_linkage()

        linkage = self.controller._linkage
        self.assertIsNotNone(linkage)
        self.assertTrue(linkage.is_active)
        self.assertEqual(mtou_config.LINKAGE_MODE, self.controller._mode)
        self.assertEqual(1, linkage.mode_revision())
        self.assertEqual(1, self.host.captured)
        self.assertEqual([self.controller._on_runtime_reply], self.session.begun)
        self.assertEqual(7, linkage.status().session_id)
        self.assertEqual(1, len(self.timers))
        self.assertLessEqual(self.timers[0][0], 1.0 / 60.0)
        modes = [message for message in self.session.messages
                 if message["type"] == "mode"]
        self.assertEqual(1, len(modes))
        self.assertEqual("linkage", modes[0]["mode"])
        self.assertEqual("unreal", modes[0]["control"])
        self.assertEqual("ready", modes[0]["transition"])
        self.assertEqual(7, modes[0]["session_id"])
        self.assertEqual("控制方：Unreal",
                         self.controller.linkage_status()["labels"]["control"])
        self.assertEqual("模式：镜头联动",
                         self.controller.linkage_status()["labels"]["mode"])

    def test_pump_applies_the_waiting_target_and_submits_one_group(self):
        self.enter_linkage()
        window = mock.Mock()
        self.controller._link_window = window

        with self.bindings:
            self.controller._on_runtime_reply(camera_time(display_frame=12.0))
        self.assertEqual(1, self.controller._linkage.pending_count())

        self.run_timer()

        groups = [message for message in self.session.messages
                  if message["type"] == "group"]
        self.assertEqual(1, len(groups))
        self.assertEqual("seq@10+0/cam#c1", groups[0]["eval_id"])
        self.assertEqual(1, groups[0]["sample_serial"])
        self.assertEqual(1, len(self.host.applied))
        status = self.controller.linkage_status()
        self.assertEqual(1, status["issued"])
        self.assertEqual(0, status["pending"])
        self.assertIn("目标时间：12.00",
                      self.controller.linkage_status()["labels"]["target"])
        window.refresh.assert_called()

    def test_runtime_reply_routes_every_linkage_message_type(self):
        self.enter_linkage()

        with self.bindings:
            self.controller._on_runtime_reply(camera_time(eval_serial=3))
        # A routed target waits for the pump; the status keeps it honest.
        self.assertEqual(1, self.controller._linkage.pending_count())

        with self.bindings:
            self.controller._on_runtime_reply(
                applied_message(eval_id="seq@10+0/cam#c1", sample_serial=1))
        status = self.controller.linkage_status()
        self.assertEqual(1, status["applied_count"])
        self.assertEqual("seq@10+0/cam#c1", status["applied_eval_id"])
        self.assertEqual(0.02, status["lag_seconds"])

        with self.bindings:
            self.controller._on_runtime_reply({
                "type": "mode_state", "session_id": 7, "mode_revision": 1,
                "mode": "linkage", "control": "unreal", "detail": "UE 已接管"})
            # Mode statements are followed on Maya's main thread.
            self.flush()
        self.assertEqual("UE 已接管", self.controller.linkage_status()["detail"])

    def test_messages_of_a_mode_maya_already_left_are_counted_not_reported(self):
        self.enter_linkage()
        self.show_error.reset_mock()
        current = self.controller._linkage.mode_revision()

        with self.bindings:
            self.controller._on_runtime_reply(
                camera_time(mode_revision=current - 1))
            self.controller._on_runtime_reply(
                applied_message(mode_revision=current - 1))
            self.flush()

        self.show_error.assert_not_called()
        self.assertEqual(2, self.controller.linkage_status()["refused_count"])
        self.assertEqual(0, self.controller._linkage.pending_count())

    def test_a_message_from_a_newer_mode_is_still_a_conflict(self):
        self.enter_linkage()
        self.show_error.reset_mock()
        current = self.controller._linkage.mode_revision()

        with self.bindings:
            self.controller._on_runtime_reply(
                camera_time(mode_revision=current + 1))
            self.flush()

        self.assertEqual("MODE_CONFLICT", self.show_error.call_args[0][0]["code"])

    def _mode_state(self, revision, mode="linkage", detail=""):
        return {"type": "mode_state", "session_id": 7, "mode_revision": revision,
                "mode": mode, "control": "unreal" if mode == "linkage" else "maya",
                "detail": detail}

    def _route(self, reply):
        with self.bindings:
            handled = self.controller._on_runtime_reply(reply)
            self.flush()
        return handled

    def test_unreal_started_linkage_enters_from_realtime(self):
        self._route(self._mode_state(1))

        linkage = self.controller._linkage
        self.assertIsNotNone(linkage)
        self.assertTrue(linkage.is_active)
        self.assertEqual(mtou_config.LINKAGE_MODE, self.controller._mode)
        self.assertEqual(1, self.host.captured)
        self.assertEqual(1, len(self.timers))
        modes = [(m["mode"], m["mode_revision"], m["transition"])
                 for m in self.session.messages if m["type"] == "mode"]
        self.assertEqual([("linkage", 1, "ready")], modes)

    def test_unreal_started_linkage_is_refused_without_a_role(self):
        self.controller._scene = None
        self._route(self._mode_state(1))

        self.assertIsNone(self.controller._linkage)
        self.assertEqual(0, self.host.captured)
        modes = [(m["mode"], m["mode_revision"], m["transition"])
                 for m in self.session.messages if m["type"] == "mode"]
        # Unreal hears the refusal and hands its writers back at once.
        self.assertEqual([("realtime", 1, "failed")], modes)
        self.assertEqual("ROLE_SETUP_FAILED", self.show_error.call_args[0][0]["code"])

    def test_unreal_leave_restores_maya_and_resumes_realtime(self):
        self.enter_linkage()
        self._route(self._mode_state(1))
        self._route(self._mode_state(2, mode="realtime", detail="UE 返回实时"))

        linkage = self.controller._linkage
        self.assertFalse(linkage.is_active)
        self.assertEqual(mtou_config.REALTIME_MODE, self.controller._mode)
        self.assertEqual(1, len(self.host.restored))
        self.assertEqual(1, self.session.resumed)
        self.assertEqual(2, self.controller._linkage_revision)
        # The pump no longer drives a linkage that ended.
        self.assertIn(1, self.removed_callbacks)
        # A later Maya entry continues the connection's revision.
        self.enter_linkage()
        self.assertEqual(3, self.controller._linkage.mode_revision())

    def test_unreal_refusing_maya_entry_rolls_the_panel_back(self):
        self.enter_linkage()
        self._route(self._mode_state(0, mode="realtime", detail="PAIRING_REQUIRED"))

        self.assertFalse(self.controller._linkage.is_active)
        self.assertEqual(mtou_config.REALTIME_MODE, self.controller._mode)
        self.assertEqual(1, len(self.host.restored))
        self.assertFalse(self.controller._linkage.takeover_held())

    def test_unconfirmed_entry_rolls_back_on_the_pump(self):
        self.enter_linkage()
        linkage = self.controller._linkage
        linkage._clock = lambda: 1e9
        self.pump()

        self.assertFalse(linkage.is_active)
        self.assertEqual(mtou_config.REALTIME_MODE, self.controller._mode)
        self.assertEqual(1, len(self.host.restored))
        failed = [m for m in self.session.messages
                  if m["type"] == "mode" and m["transition"] == "failed"]
        self.assertEqual(1, len(failed))

    def test_end_preview_stays_available_after_returning_to_realtime(self):
        self.enter_linkage()
        with self.bindings:
            self.controller.leave_linkage("realtime")
        enabled = {}
        self.controller._end_linkage_button = "end"
        self.controller._leave_linkage_button = "leave"
        with mock.patch.object(
                self.controller, "_set_enabled",
                side_effect=lambda control, value: enabled.__setitem__(control, value)):
            with self.bindings:
                self.controller._update_linkage_controls()
        self.assertTrue(enabled["end"])
        self.assertFalse(enabled["leave"])

        with self.bindings:
            self.controller.leave_linkage("end")
        exits = [m["target"] for m in self.session.messages
                 if m["type"] == "leave_linkage"]
        self.assertEqual(["realtime", "end"], exits)
        with mock.patch.object(
                self.controller, "_set_enabled",
                side_effect=lambda control, value: enabled.__setitem__(control, value)):
            with self.bindings:
                self.controller._update_linkage_controls()
        self.assertFalse(enabled["end"])

    def test_a_statement_for_another_session_starts_nothing(self):
        stale = self._mode_state(4)
        stale["session_id"] = 999
        self._route(stale)
        self.assertIsNone(self.controller._linkage)
        self.assertEqual(0, self.host.captured)
        self.assertEqual([], self.session.messages)

    def test_a_deferred_statement_of_an_ended_connection_is_dropped(self):
        with self.bindings:
            self.controller._on_runtime_reply(self._mode_state(1))
        # The connection ends and a new one (session 8) is ready before the
        # main thread runs the deferred statement.
        replacement = FakeStreamingSession(session_id=8)
        self.controller._session = replacement
        with self.bindings:
            self.flush()
        self.assertIsNone(self.controller._linkage)
        self.assertEqual(0, self.host.captured)
        self.assertEqual([], replacement.messages)

    def test_a_stale_exit_does_not_end_the_confirmed_linkage(self):
        self.enter_linkage()
        self._route(self._mode_state(1))
        self._route(self._mode_state(0, mode="realtime", detail="old"))
        self.assertTrue(self.controller._linkage.is_active)
        self.assertEqual(mtou_config.LINKAGE_MODE, self.controller._mode)
        self.assertEqual([], self.host.restored)

    def test_dispatcher_leaves_errors_to_the_session_before_linkage(self):
        # Without linkage an Unreal error still ends the realtime connection.
        handled = self._route({"type": "error", "code": "INVALID_MESSAGE",
                               "message": "bad frame"})
        self.assertFalse(handled)
        self.enter_linkage()
        handled = self._route({"type": "error", "code": "MODE_CONFLICT",
                               "message": "stale"})
        self.assertTrue(handled)

    def test_runtime_reply_stores_a_reference_offer_and_refreshes_its_window(self):
        self.enter_linkage()
        window = mock.Mock()
        self.controller._reference_window = window
        offer = {
            "type": "reference_offer", "session_id": 7, "transfer_id": "tx-1",
            "directory": "handoff/tx-1", "manifest_digest": "ab12",
            "scope": {"level": "L_Main", "sublevels": ["A", "B"]},
            "space_revision": 1, "counts": {"objects": 12, "triangles": 340,
                                            "bytes": 2048},
        }

        with self.bindings:
            self.controller._on_runtime_reply(offer)
        self.flush()

        stored = self.controller.reference_offer()
        self.assertEqual("tx-1", stored["transfer_id"])
        self.assertEqual("ab12", stored["manifest_digest"])
        window.refresh.assert_called_once_with()

    def test_runtime_reply_reports_an_error_reply_without_dropping_the_session(self):
        self.enter_linkage()

        with self.bindings:
            self.controller._on_runtime_reply({
                "type": "error", "code": "PAIRING_REQUIRED",
                "message": "target is not paired", "details": "no target"})
        self.flush()

        diagnostic = self.controller._last_diagnostic
        self.assertEqual("PAIRING_REQUIRED", diagnostic["code"])
        self.assertTrue(self.controller._linkage.is_active)

    def test_runtime_reply_swallows_a_stale_target_refusal(self):
        self.enter_linkage()

        with self.bindings:
            self.controller._on_runtime_reply(camera_time(eval_serial=5))
            # An older target than the one already waiting is refused.
            self.controller._on_runtime_reply(camera_time(eval_serial=4))
        self.flush()

        self.show_error.assert_not_called()
        self.assertTrue(self.controller._linkage.is_active)
        # The refused older target changed nothing; the newer one still waits.
        self.assertEqual(1, self.controller._linkage.pending_count())
        self.run_timer()
        groups = [message for message in self.session.messages
                  if message["type"] == "group"]
        self.assertEqual("seq@10+0/cam#c1", groups[-1]["eval_id"])

    def test_runtime_reply_reports_a_real_refusal(self):
        self.enter_linkage()

        with self.bindings:
            # A message for another session fails the identity check.
            self.controller._on_runtime_reply(applied_message(session_id=99))
        self.flush()

        self.show_error.assert_called()
        self.assertTrue(self.controller._linkage.is_active)

    def test_leave_linkage_restores_and_resumes_the_session(self):
        self.enter_linkage()

        with self.bindings:
            self.controller.leave_linkage("realtime")

        linkage = self.controller._linkage
        self.assertFalse(linkage.is_active)
        self.assertEqual([{"time": 42.0, "camera": "persp"}], self.host.restored)
        self.assertEqual(1, self.session.resumed)
        # The dispatcher stays installed for the session's lifetime: Unreal
        # answers a leave with a mode statement and may push a reference offer
        # while Maya is merely streaming realtime, and neither may end the
        # connection the user is still using.
        self.assertEqual([self.controller._on_runtime_reply], self.session.listeners)
        self.assertEqual(mtou_config.REALTIME_MODE, self.controller._mode)
        self.assertEqual(1, len(self.removed_callbacks))
        left = [message for message in self.session.messages
                if message["type"] == "leave_linkage"]
        self.assertEqual(1, len(left))
        self.assertEqual("realtime", left[0]["target"])
        self.assertEqual(2, left[0]["mode_revision"])

    def test_leave_linkage_end_asks_unreal_to_restore_its_writer(self):
        self.enter_linkage()

        with self.bindings:
            self.controller.leave_linkage("end")

        left = [message for message in self.session.messages
                if message["type"] == "leave_linkage"]
        self.assertEqual("end", left[-1]["target"])
        self.assertEqual(1, self.session.resumed)
        self.assertEqual(mtou_config.REALTIME_MODE, self.controller._mode)

    def test_a_failed_restore_is_reported_in_the_status(self):
        self.host.restore_residual = ["camera: persp is locked"]
        self.enter_linkage()
        connected = mock.Mock()
        self.controller._set_connected = connected

        with self.bindings:
            self.controller.leave_linkage("realtime")

        status = connected.call_args[0][1]
        self.assertIn("恢复残留：camera: persp is locked", status)
        self.assertIn("camera: persp is locked",
                      self.controller.linkage_status()["labels"]["recovery"])
        self.assertTrue(any("camera: persp is locked" in record for record
                            in self.controller.linkage_status()["recovery_records"]))

    def test_leaving_without_linkage_reports_the_refusal(self):
        with self.bindings:
            self.controller.leave_linkage("realtime")

        self.assertEqual("LINKAGE_NOT_ACTIVE", self.show_error.call_args[0][0]["code"])
        self.assertEqual([], self.host.restored)

    def test_disconnect_stops_the_pump_and_restores_every_resource(self):
        self.enter_linkage()
        with self.bindings:
            self.controller.open_link_window()
            self.controller.open_reference_window()

        with self.bindings:
            self.controller.disconnect()

        self.assertIsNone(self.controller._linkage)
        self.assertEqual([{"time": 42.0, "camera": "persp"}], self.host.restored)
        self.assertEqual([1], self.removed_callbacks)
        self.fake_cmds.deleteUI.assert_any_call(
            mtou_link_window.LINK_WINDOW_NAME)
        self.fake_cmds.deleteUI.assert_any_call(
            mtou_reference_window.REFERENCE_WINDOW_NAME)
        self.assertIsNone(self.controller._link_window)
        self.assertIsNone(self.controller._reference_window)
        self.assertIn("camera: persp" if self.host.restore_residual else "无",
                      self.controller.linkage_status()["labels"]["recovery"])

    def test_scene_change_releases_linkage_and_its_windows(self):
        self.enter_linkage()
        self.controller._link_window = mock.Mock()

        with self.bindings:
            self.controller._on_scene_change()

        self.assertIsNone(self.controller._linkage)
        self.assertEqual(1, len(self.removed_callbacks))
        self.assertIsNone(self.controller._link_window)

    def test_maya_exit_releases_linkage_and_its_windows(self):
        self.enter_linkage()
        self.controller._reference_window = mock.Mock()

        with self.bindings:
            self.controller._on_maya_exiting()

        self.assertIsNone(self.controller._linkage)
        self.assertIsNone(self.controller._reference_window)

    def test_a_target_after_leaving_is_dropped_without_a_diagnostic(self):
        self.enter_linkage()
        with self.bindings:
            self.controller.leave_linkage("realtime")
        reported = len(self.show_error.call_args_list)

        with self.bindings:
            # Unreal may still be answering the transition it already saw.
            self.controller._on_runtime_reply(camera_time(eval_serial=9))
        self.flush()

        self.assertEqual(reported, len(self.show_error.call_args_list))
        self.assertFalse(self.controller._linkage_active())

    def test_storing_an_offer_without_its_window_is_safe(self):
        with self.bindings:
            self.controller.offer_reference({
                "transfer_id": "tx-2", "directory": "handoff/tx-2",
                "manifest_digest": "9f9f", "space_revision": 1})
        self.flush()

        self.assertIsNone(self.controller._reference_window)
        self.assertEqual("tx-2", self.controller.reference_offer()["transfer_id"])

    def test_cached_mode_is_refused_while_linkage_holds_the_session(self):
        self.enter_linkage()

        with self.bindings:
            self.controller._on_mode_changed(mtou_config.CACHED_MODE)

        self.assertEqual("MODE_CONFLICT", self.show_error.call_args[0][0]["code"])
        self.assertEqual(mtou_config.LINKAGE_MODE, self.controller._mode)
        self.assertTrue(self.controller._linkage.is_active)


class ReferenceActionTests(unittest.TestCase):
    """The scene-reference actions validate before they touch the scene."""

    def setUp(self):
        self.timers = []
        self.removed_callbacks = []
        self.deferred = []
        self.host = FakeLinkageHost()
        self.session = FakeStreamingSession()
        self.reference = FakeReferenceModule()
        self.controller = mtou_controller.Controller()
        self.controller._session = self.session
        self.controller._scene = FakeCharacterScene()
        self.fake_cmds = self._fake_cmds()
        self.fake_om = self._fake_om()
        self.bindings = _Bindings(self.fake_cmds, self.fake_om)
        harness = self
        module_patch = mock.patch.object(
            mtou_controller.Controller, "_reference_module_or_report",
            lambda controller: harness.reference)
        module_patch.start()
        self.addCleanup(module_patch.stop)
        error_patch = mock.patch.object(
            mtou_controller.Controller, "_show_error")
        error_patch.start()
        self.addCleanup(error_patch.stop)

    def _fake_cmds(self):
        fake = mock.MagicMock()
        fake.control.return_value = True
        fake.currentUnit.return_value = "film"
        fake.confirmDialog.return_value = "确定"

        def window(*args, **kwargs):
            if kwargs.get("exists"):
                return False
            return args[0] if args else kwargs.get("name")

        fake.window.side_effect = window
        fake.evalDeferred.side_effect = lambda action, *a, **k: self.deferred.append(action)
        return fake

    def _fake_om(self):
        class _Timer(object):
            @staticmethod
            def addTimerCallback(interval, callback):
                self.timers.append((interval, callback))
                return len(self.timers)

        class _Message(object):
            @staticmethod
            def removeCallback(callback_id):
                self.removed_callbacks.append(callback_id)

        return type("FakeOpenMaya", (), {
            "MTimerMessage": _Timer,
            "MMessage": _Message,
        })

    def offer(self):
        self.controller.offer_reference({
            "type": "reference_offer", "session_id": 7, "transfer_id": "tx-9",
            "directory": "handoff/tx-9", "manifest_digest": "cd34",
            "scope": {"level": "L_Main", "sublevels": []}, "space_revision": 1,
            "counts": {"objects": 4, "triangles": 900, "bytes": 4096}})
        self.flush()

    def flush(self):
        while self.deferred:
            self.deferred.pop(0)()

    def test_reference_actions_are_safe_without_an_offer(self):
        with self.bindings:
            self.controller.import_reference()

        self.assertEqual([], self.reference.calls)
        self.assertTrue(self.fake_cmds.warning.called)
        self.assertIsNone(self.controller.reference_offer())
        self.assertEqual({"action": None, "report": {}},
                         self.controller.reference_report())

    def test_reference_budgets_are_the_frozen_ones(self):
        budget = self.controller.reference_budget()

        self.assertEqual(mtou_config.MAX_REFERENCE_BYTES, budget["bytes"])
        self.assertEqual(mtou_config.MAX_REFERENCE_OBJECTS, budget["objects"])
        self.assertEqual(mtou_config.MAX_REFERENCE_TRIANGLES, budget["triangles"])

    def test_import_validates_before_touching_the_scene(self):
        self.offer()
        self.reference.inspect_error = stale_reference()

        with self.bindings:
            self.controller.import_reference()

        self.assertEqual(["inspect"], [name for name, *_ in self.reference.calls])
        self.assertEqual("REFERENCE_STALE",
                         self.controller._last_diagnostic["code"])
        self.assertIsNone(self.controller.reference_report()["report"] or None)

    def test_import_passes_the_frozen_budgets_to_validation(self):
        self.offer()
        self.reference.import_report = {
            "accepted": True, "detail": "ok", "counts": {"objects": 4,
            "triangles": 900, "bytes": 4096}, "container": {"name": "MtoU_UE_SceneRef"},
            "discarded": False, "rolled_back": False, "residual": {}}

        with self.bindings:
            self.controller.import_reference()

        names = [name for name, *_ in self.reference.calls]
        self.assertEqual(["inspect", "import_handoff"], names)
        budgets = [budget for name, _, budget in self.reference.calls]
        self.assertEqual(self.controller.reference_budget(), budgets[0])
        self.assertEqual(self.controller.reference_budget(), budgets[1])
        reported = self.controller.reference_report()
        self.assertEqual("import", reported["action"])
        self.assertTrue(reported["report"]["accepted"])
        results = [message for message in self.session.messages
                   if message["type"] == "reference_result"]
        self.assertEqual(1, len(results))
        self.assertEqual("tx-9", results[0]["transfer_id"])
        self.assertTrue(results[0]["accepted"])
        self.assertEqual(4, results[0]["counts"]["objects"])

    def test_an_import_without_an_anchor_is_reported_in_the_status_line(self):
        text = self.controller._reference_outcome_text({
            "accepted": True, "counts": {"objects": 4},
            "anchor": {"source": "absent"}})
        self.assertIn("锚点", text)
        self.assertNotIn("锚点", self.controller._reference_outcome_text({
            "accepted": True, "counts": {"objects": 4},
            "anchor": {"source": "manifest"}}))

    def test_a_rolled_back_import_is_reported(self):
        self.offer()
        self.controller._status_text = "statusText"
        self.controller._set_text = mock.Mock()
        self.reference.import_report = {
            "accepted": False, "detail": "staged swap failed",
            "counts": {"objects": 0, "triangles": 0, "bytes": 0},
            "container": {"name": "MtoU_UE_SceneRef"}, "discarded": True,
            "rolled_back": True, "residual": {"staging": ["tx-9_staging"]}}

        with self.bindings:
            self.controller.import_reference()

        report = self.controller.reference_report()["report"]
        self.assertTrue(report["rolled_back"])
        self.assertTrue(report["discarded"])
        lines = mtou_reference_window._report_lines(
            self.controller.reference_report())
        self.assertTrue(any("回滚：是" in line for line in lines))
        results = [message for message in self.session.messages
                   if message["type"] == "reference_result"]
        self.assertFalse(results[-1]["accepted"])
        self.assertTrue(any("回滚" in call[0][1]
                            for call in self.controller._set_text.call_args_list
                            if call[0][0] == "statusText"))

    def test_remove_and_recover_call_the_tool_owned_container(self):
        with self.bindings:
            self.controller.remove_reference()
        self.assertEqual("remove", self.controller.reference_report()["action"])
        self.assertTrue(self.controller.reference_report()["report"]["removed"])

        with self.bindings:
            self.controller.recover_reference()

        names = [name for name, *_ in self.reference.calls]
        self.assertEqual(["container", "remove", "container", "recover"], names)
        self.assertEqual("recover", self.controller.reference_report()["action"])
        removed = [message for message in self.session.messages
                   if message["type"] == "reference_removed"]
        self.assertEqual(1, len(removed))
        self.assertEqual("removed", removed[0]["detail"])

    def test_reference_window_controls_carry_their_reasons(self):
        controls = self.controller.reference_window_controls()
        self.assertFalse(controls["import"][0])
        self.assertIn("导出", controls["import"][1])

        self.offer()
        controls = self.controller.reference_window_controls()
        self.assertTrue(controls["import"][0])
        self.assertTrue(controls["remove"][0])
        self.assertTrue(controls["recover"][0])

        self.session._ready = False
        controls = self.controller.reference_window_controls()
        self.assertFalse(controls["import"][0])
        self.assertIn("连接", controls["import"][1])


class DetailWindowTests(unittest.TestCase):
    """The sub-windows reuse one instance and never own the session."""

    def setUp(self):
        self.timers = []
        self.removed_callbacks = []
        self.deferred = []
        self.host = FakeLinkageHost()
        self.session = FakeStreamingSession()
        self.controller = mtou_controller.Controller()
        self.controller._session = self.session
        self.controller._scene = FakeCharacterScene()
        self.fake_cmds = self._fake_cmds()
        self.fake_om = self._fake_om()
        self.bindings = _Bindings(self.fake_cmds, self.fake_om)

    def _fake_cmds(self):
        fake = mock.MagicMock()
        fake.control.return_value = True
        fake.currentUnit.return_value = "film"
        fake.confirmDialog.return_value = "确定"
        windows = set()

        def window(*args, **kwargs):
            name = args[0] if args else kwargs.get("name")
            if kwargs.get("exists") or kwargs.get("query"):
                return name in windows if kwargs.get("exists") else None
            windows.add(name)
            return name

        def delete_ui(name, *args, **kwargs):
            windows.discard(name)

        fake.window.side_effect = window
        fake.deleteUI.side_effect = delete_ui
        fake.evalDeferred.side_effect = lambda action, *a, **k: self.deferred.append(action)
        return fake

    def _fake_om(self):
        class _Message(object):
            @staticmethod
            def removeCallback(callback_id):
                self.removed_callbacks.append(callback_id)

        return type("FakeOpenMaya", (), {"MMessage": _Message})

    def created_windows(self, name):
        """The create calls for one window, not its existence checks."""
        return [call for call in self.fake_cmds.window.call_args_list
                if call[0] and call[0][0] == name
                and not call[1].get("exists") and not call[1].get("query")]

    def test_repeated_open_reuses_one_linkage_window(self):
        with self.bindings:
            first = self.controller.open_link_window()
            second = self.controller.open_link_window()

        self.assertIs(first, second)
        self.assertEqual(1, len(self.created_windows(
            mtou_link_window.LINK_WINDOW_NAME)))

    def test_linkage_window_labels_keep_a_fixed_width(self):
        # Maya keeps a label at its creation width unless one is given, which
        # clipped every value that grew after the window opened.
        with self.bindings:
            self.controller.open_link_window()

        created = [call for call in self.fake_cmds.text.call_args_list
                   if not call[1].get("edit") and call[1].get("label")]
        self.assertEqual(14, len(created))
        # A label wider than its card body widens the window past its default.
        self.assertLessEqual(3 * mtou_link_window.STATUS_COLUMN,
                             mtou_link_window.CARD_BODY)
        for call in created:
            self.assertLessEqual(call[1].get("width", 0),
                                 mtou_link_window.CARD_BODY, call)
            self.assertGreaterEqual(call[1].get("width", 0),
                                    mtou_link_window.STATUS_LABEL, call)

    def test_linkage_window_elides_long_eval_ids_and_keeps_them_in_the_tooltip(self):
        eval_id = "Main@1200000+000//Game/Level/Cam.CameraComponent#" + \
            ";".join("field_{0}={0}.000000".format(index) for index in range(80)) + ";~e2"
        status = {"applied_eval_id": eval_id, "detail": "已应用 " + eval_id,
                  "recovery_records": ["camera: persp is locked"], "labels": {}}
        with self.bindings, mock.patch.object(
                self.controller, "linkage_status", return_value=status):
            self.controller.open_link_window()

        labels = {call[1]["label"]: call[1]["annotation"]
                  for call in self.fake_cmds.text.call_args_list
                  if call[1].get("edit") and call[1]["label"]}
        applied = [label for label in labels if label.startswith("已应用求值：")]
        self.assertEqual(1, len(applied))
        self.assertIn("…", applied[0])
        self.assertTrue(applied[0].startswith("已应用求值：Main@1200000+000"))
        self.assertTrue(applied[0].endswith(";~e2"))
        self.assertLessEqual(len(applied[0]), len("已应用求值：")
                             + mtou_link_window.TOKEN_LIMIT)
        self.assertEqual("已应用求值：" + eval_id, labels[applied[0]])
        detail = [label for label in labels if label.startswith("已应用 Main")]
        self.assertEqual(["已应用 " + mtou_link_window.compact(eval_id)], detail)
        self.assertEqual("camera: persp is locked",
                         labels["camera: persp is locked"])

    def test_repeated_open_reuses_one_reference_window(self):
        with self.bindings:
            first = self.controller.open_reference_window()
            second = self.controller.open_reference_window()

        self.assertIs(first, second)
        self.assertEqual(1, len(self.created_windows(
            mtou_reference_window.REFERENCE_WINDOW_NAME)))

    def test_closing_a_window_releases_only_that_window(self):
        with self.bindings:
            window = self.controller.open_link_window()
            window._on_closed()

        self.assertIsNone(self.controller._link_window)
        self.fake_cmds.deleteUI.assert_called_with(
            mtou_link_window.LINK_WINDOW_NAME)
        # The session and every other window survive a detail window's close.
        self.assertIs(self.session, self.controller._session)
        with self.bindings:
            reopened = self.controller.open_link_window()
        self.assertIsNot(window, reopened)

    def test_reference_window_close_keeps_the_offer_and_the_session(self):
        with self.bindings:
            self.controller.offer_reference({
                "transfer_id": "tx-3", "directory": "handoff/tx-3",
                "manifest_digest": "ef56", "space_revision": 1})
            self.flush()
            window = self.controller.open_reference_window()
            window._on_closed()

        self.assertIsNone(self.controller._reference_window)
        self.assertEqual("tx-3", self.controller.reference_offer()["transfer_id"])
        self.assertIs(self.session, self.controller._session)

    def flush(self):
        while self.deferred:
            self.deferred.pop(0)()

    def test_windows_render_the_controller_state(self):
        with self.bindings:
            self.controller.open_reference_window()
            self.controller.offer_reference({
                "transfer_id": "tx-4", "directory": "handoff/tx-4",
                "manifest_digest": "ab78",
                "scope": {"level": "L_Main", "sublevels": ["A"]},
                "space_revision": 2,
                "counts": {"objects": 5, "triangles": 60, "bytes": 128}})
            self.flush()

        labels = [call[1].get("label")
                  for call in self.fake_cmds.text.call_args_list
                  if call[1].get("label")]
        self.assertIn("交接：tx-4", labels)
        self.assertIn("摘要：ab78", labels)
        self.assertIn("计数：对象 5｜三角面 60｜字节 128", labels)
        self.assertTrue(any("字节上限" in label for label in labels))


if __name__ == "__main__":
    unittest.main()
