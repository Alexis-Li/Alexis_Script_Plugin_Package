import errno
import importlib.util
import itertools
import json
import math
import pathlib
import struct
import threading
import unittest
from unittest import mock

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "MtoULiveLink.py"
SPEC = importlib.util.spec_from_file_location("MtoULiveLink", str(SCRIPT))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

BIND_IDENTITY = (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0)


def character_snapshot(revision, root, outfit, bones, curves,
                       duplicate_paths=(), mesh_paths=(), bind_conflict_count=0):
    return MODULE._CharacterSnapshot(
        revision,
        root,
        outfit,
        bones,
        curves,
        duplicate_paths,
        mesh_paths,
        [BIND_IDENTITY for unused in bones],
        bind_conflict_count,
    )
CORPUS = json.loads(
    (pathlib.Path(__file__).resolve().parents[3] / "protocol" / "conformance-v7.json")
    .read_text(encoding="utf-8")
)


class LatestFrameTests(unittest.TestCase):
    def test_only_newest_unsent_frame_is_kept(self):
        slot = MODULE._LatestFrame()
        slot.put(b"first")
        slot.put(b"second")
        self.assertEqual(b"second", slot.take())
        self.assertIsNone(slot.take())



class PlaybackCapPersistenceTests(unittest.TestCase):
    def _option_var_cmds(self, stored=None):
        state = {"stored": stored}

        def option_var(**kwargs):
            if "exists" in kwargs:
                return state["stored"] is not None
            if "query" in kwargs:
                return state["stored"]
            if "stringValue" in kwargs:
                _, value = kwargs["stringValue"]
                state["stored"] = value
                return None
            raise AssertionError("unexpected optionVar call: {0}".format(kwargs))

        return type("FakeCmds", (), {"optionVar": staticmethod(option_var)}), state

    def test_missing_and_invalid_values_fall_back_to_twenty_fps(self):
        fake_cmds, state = self._option_var_cmds()
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            self.assertEqual(MODULE.DEFAULT_PLAYBACK_CAP, MODULE.load_playback_cap())
            self.assertEqual(MODULE.DEFAULT_PLAYBACK_CAP, state["stored"])

            state["stored"] = "120 fps"
            self.assertEqual(MODULE.DEFAULT_PLAYBACK_CAP, MODULE.load_playback_cap())
            self.assertEqual(MODULE.DEFAULT_PLAYBACK_CAP, state["stored"])

    def test_valid_values_round_trip_through_native_option_storage(self):
        fake_cmds, state = self._option_var_cmds("15 fps")
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            self.assertEqual("15 fps", MODULE.load_playback_cap())
            self.assertEqual("15 fps", state["stored"])
            MODULE.save_playback_cap("Follow Scene")
            self.assertEqual("Follow Scene", state["stored"])


class PlaybackSchedulingTests(unittest.TestCase):
    def _runtime(self, state, intervals, callbacks, worker):
        def timer_factory(interval, callback):
            intervals.append(interval)
            callbacks.append(callback)
            return 20 + len(callbacks)

        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(lambda event, callback: 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(timer_factory),
            }),
            "MConditionMessage": type("FakeConditionMessage", (), {
                "addConditionCallback": staticmethod(
                    lambda condition, callback: 40),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(lambda callback_id: None),
            }),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])
        scene.sample.return_value = MODULE._CharacterFrame(7, [[1, 2, 3]], [])
        return fake_om, scene, lambda: state["playing"]

    def test_playback_cap_applies_only_while_playing_and_stale_timer_is_ignored(self):
        state = {"playing": False}
        intervals = []
        callbacks = []
        worker = mock.Mock()
        worker.status.return_value = (
            "ready", "Connected", {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False,
            }, None)
        fake_om, scene, playback_state = self._runtime(
            state, intervals, callbacks, worker)

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker), \
             mock.patch.object(MODULE.time, "time", return_value=100.0):
            session = MODULE._StreamingSession.start(
                scene, 30.0, playback_cap="20 fps", playback_state=playback_state)
            self.assertEqual([1.0 / 30.0], intervals)

            state["playing"] = True
            callbacks[0](0.0, 0.0, None)
            self.assertEqual([1.0 / 30.0, 1.0 / 20.0], intervals)
            self.assertEqual(1, scene.sample.call_count)

            stale_callback = callbacks[0]
            state["playing"] = False
            callbacks[1](0.0, 0.0, None)
            self.assertEqual([1.0 / 30.0, 1.0 / 20.0, 1.0 / 30.0], intervals)
            self.assertEqual(2, scene.sample.call_count)

            stale_callback(0.0, 0.0, None)
            self.assertEqual(2, scene.sample.call_count)
            session.stop()

    def test_follow_scene_and_numeric_caps_never_exceed_scene_rate(self):
        state = {"playing": True}
        intervals = []
        callbacks = []
        worker = mock.Mock()
        worker.status.return_value = (
            "ready", "Connected", {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False,
            }, None)
        fake_om, scene, playback_state = self._runtime(
            state, intervals, callbacks, worker)

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker):
            session = MODULE._StreamingSession.start(
                scene, 15.0, playback_cap="30 fps", playback_state=playback_state)
            self.assertEqual([1.0 / 15.0], intervals)
            session.change_cap("Follow Scene")
            self.assertEqual([1.0 / 15.0, 1.0 / 15.0], intervals)
            session.stop()
    def test_cap_change_keeps_snapshot_and_submits_current_pose_immediately(self):
        state = {"playing": True}
        intervals = []
        callbacks = []
        worker = mock.Mock()
        worker.status.return_value = (
            "ready", "Connected", {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False,
            }, None)
        fake_om, scene, playback_state = self._runtime(
            state, intervals, callbacks, worker)

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker), \
             mock.patch.object(MODULE.time, "time", return_value=100.0):
            session = MODULE._StreamingSession.start(
                scene, 30.0, playback_cap="20 fps", playback_state=playback_state)
            self.assertEqual([1.0 / 20.0], intervals)
            session.change_cap("15 fps")
            self.assertEqual([1.0 / 20.0, 1.0 / 15.0], intervals)
            self.assertEqual(1, scene.snapshot.call_count)
            self.assertEqual(1, worker.submit.call_count)
            session.stop()

class CharacterSceneTests(unittest.TestCase):
    def _capture(self, on_event=None, sample_side_effect=None):
        removed = []
        deferred = []
        callbacks = {"destroyed": [], "changed": []}

        class FakeDagPath(object):
            def node(self):
                return object()

        class FakeNodeMessage(object):
            kAttributeSet = 1

            @staticmethod
            def addNodeDestroyedCallback(node, callback):
                del node
                callbacks["destroyed"].append(callback)
                return len(callbacks["destroyed"])

            @staticmethod
            def addAttributeChangedCallback(node, callback):
                del node
                callbacks["changed"].append(callback)
                return 100 + len(callbacks["changed"])

        fake_om = type("FakeOpenMaya", (), {
            "MNodeMessage": FakeNodeMessage,
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(removed.append),
            }),
        })
        fake_cmds = type("FakeCmds", (), {
            "evalDeferred": staticmethod(deferred.append),
            "warning": staticmethod(lambda message: None),
        })
        subject = {
            "root": "|Group|root",
            "bones": [
                {"name": "root", "parent": -1, "path": "|Group|root",
                 "dag_path": FakeDagPath()},
                {"name": "spine", "parent": 0, "path": "|Group|root|spine",
                 "dag_path": FakeDagPath()},
            ],
            "meshes": ["|Group|body"],
            "curves": [{"name": "Smile", "plugs": ["face.Smile"]}],
            "unit_scale": 1.0,
            "bind_local_transforms": [
                [0, 0, 0, 0, 0, 0, 1, 1, 1, 1],
                [0, 0, 0, 0, 0, 0, 1, 1, 1, 1],
            ],
        }
        display = {"attribute": "clothes", "entries": ["Clothes01", "Clothes02"],
                   "plug": "|Group|Display_ctrl.clothes"}
        patches = [
            mock.patch.object(MODULE, "cmds", fake_cmds),
            mock.patch.object(MODULE, "om", fake_om),
            mock.patch.object(MODULE._CharacterScene, "_resolve_root",
                              return_value="|Group|root"),
            mock.patch.object(MODULE._CharacterScene, "_resolve_display",
                              return_value=("|Group|Display_ctrl", "clothes")),
            mock.patch.object(MODULE, "_capture_subject", return_value=subject),
            mock.patch.object(MODULE, "_enum_attributes", return_value=[display]),
            mock.patch.object(MODULE, "_current_enum_label", return_value="Clothes01"),
            mock.patch.object(MODULE, "_dag_path", return_value=FakeDagPath()),
            mock.patch.object(
                MODULE, "_sample_pose",
                side_effect=sample_side_effect
                if sample_side_effect is not None
                else lambda captured: ([[1, 2, 3]], [0.25])),
        ]
        for patcher in patches:
            patcher.start()
            self.addCleanup(patcher.stop)
        scene = MODULE._CharacterScene.capture("|Group|root", on_event=on_event)
        return scene, callbacks, deferred, removed, subject

    def test_snapshot_and_frame_are_values_at_one_revision(self):
        scene, _, _, _, _ = self._capture()

        snapshot = scene.snapshot()
        frame = scene.sample()

        self.assertEqual(1, snapshot.revision)
        self.assertEqual((('root', -1), ('spine', 0)), snapshot.bones)
        self.assertEqual(("Smile",), snapshot.curve_names)
        self.assertEqual(1, frame.revision)
        self.assertEqual(((1, 2, 3),), frame.transforms)
        self.assertEqual((0.25,), frame.curves)
        with self.assertRaises(AttributeError):
            snapshot.revision = 2

    def test_display_change_pauses_sampling_and_coalesces_refresh(self):
        events = []
        scene, callbacks, deferred, _, _ = self._capture(events.append)

        class FakePlug(object):
            @staticmethod
            def partialName(**kwargs):
                del kwargs
                return "clothes"

        changed = callbacks["changed"][0]
        with mock.patch.object(MODULE, "_current_enum_label", return_value="Clothes02"):
            changed(1, FakePlug(), None, None)
            changed(1, FakePlug(), None, None)

        self.assertEqual(["character_change_started"], [event.kind for event in events])
        with self.assertRaises(MODULE._CharacterSceneError) as caught:
            scene.sample()
        self.assertEqual("CHARACTER_SCENE_REFRESHING", caught.exception.code)
        self.assertEqual(2, len(deferred))

        deferred[0]()
        self.assertEqual(["character_change_started"], [event.kind for event in events])
        deferred[1]()
        self.assertEqual(
            ["character_change_started", "outfit_changed"],
            [event.kind for event in events])
        self.assertEqual(1, scene.snapshot().revision)

    def test_sampling_failure_does_not_terminate_scene(self):
        scene, _, _, _, _ = self._capture(
            sample_side_effect=[ValueError("bad pose"), ([[0]], [])])

        with self.assertRaises(MODULE._CharacterSceneError) as caught:
            scene.sample()
        self.assertEqual("SAMPLING_FAILED", caught.exception.code)
        self.assertEqual(1, scene.sample().revision)

    def test_destroyed_character_keeps_terminal_reason_and_cleans_callbacks(self):
        events = []
        scene, callbacks, _, removed, _ = self._capture(events.append)

        callbacks["destroyed"][0]()

        self.assertEqual(["character_invalidated"], [event.kind for event in events])
        for operation in (scene.snapshot, scene.sample):
            with self.assertRaises(MODULE._CharacterSceneError) as caught:
                operation()
            self.assertEqual("CHARACTER_ROOT_DESTROYED", caught.exception.code)
        self.assertEqual([1, 2, 101], removed)
        scene.close()
        self.assertEqual([1, 2, 101], removed)

    def test_close_invalidates_deferred_refresh(self):
        scene, callbacks, deferred, removed, _ = self._capture()

        class FakePlug(object):
            @staticmethod
            def partialName(**kwargs):
                del kwargs
                return "clothes"

        with mock.patch.object(MODULE, "_current_enum_label", return_value="Clothes02"):
            callbacks["changed"][0](1, FakePlug(), None, None)
        scene.close()
        deferred[0]()

        self.assertEqual([1, 2, 101], removed)
        with self.assertRaises(MODULE._CharacterSceneError) as caught:
            scene.snapshot()
        self.assertEqual("CHARACTER_SCENE_CLOSED", caught.exception.code)

    def test_refresh_failure_enters_terminal_state(self):
        events = []
        scene, callbacks, deferred, removed, _ = self._capture(events.append)

        class FakePlug(object):
            @staticmethod
            def partialName(**kwargs):
                del kwargs
                return "clothes"

        with mock.patch.object(MODULE, "_current_enum_label", return_value="Clothes02"):
            callbacks["changed"][0](1, FakePlug(), None, None)
        with mock.patch.object(
                scene, "_capture_values",
                side_effect=MODULE._CharacterSceneError(
                    "NO_VISIBLE_SKINNED_MESH", "no visible mesh")):
            deferred[0]()

        self.assertEqual(
            ["character_change_started", "character_invalidated"],
            [event.kind for event in events])
        with self.assertRaises(MODULE._CharacterSceneError) as caught:
            scene.snapshot()
        self.assertEqual("NO_VISIBLE_SKINNED_MESH", caught.exception.code)
        self.assertEqual([1, 2, 101], removed)

    def test_event_consumer_failure_does_not_break_scene(self):
        scene, callbacks, _, _, _ = self._capture(
            lambda event: (_ for _ in ()).throw(RuntimeError("consumer failed")))

        class FakePlug(object):
            @staticmethod
            def partialName(**kwargs):
                del kwargs
                return "clothes"

        with mock.patch.object(MODULE, "_current_enum_label", return_value="Clothes02"):
            callbacks["changed"][0](1, FakePlug(), None, None)

        with self.assertRaises(MODULE._CharacterSceneError) as caught:
            scene.sample()
        self.assertEqual("CHARACTER_SCENE_REFRESHING", caught.exception.code)

    def test_callback_registration_failure_rolls_back_registered_callback(self):
        removed = []

        class FakeDagPath(object):
            def node(self):
                return object()

        class FailingNodeMessage(object):
            calls = 0

            @classmethod
            def addNodeDestroyedCallback(cls, node, callback):
                del node, callback
                cls.calls += 1
                if cls.calls == 2:
                    raise RuntimeError("registration failed")
                return 41

            @staticmethod
            def addAttributeChangedCallback(node, callback):
                del node, callback
                return 42

        fake_om = type("FakeOpenMaya", (), {
            "MNodeMessage": FailingNodeMessage,
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(removed.append),
            }),
        })
        fake_cmds = object()
        subject = {
            "root": "|Group|root",
            "bones": [{"name": "root", "parent": -1, "path": "|Group|root",
                       "dag_path": FakeDagPath()}],
            "meshes": ["|Group|body"],
            "curves": [],
            "unit_scale": 1.0,
            "bind_local_transforms": [[0, 0, 0, 0, 0, 0, 1, 1, 1, 1]],
        }
        display = {"attribute": "clothes", "entries": ["Clothes01"],
                   "plug": "|Group|Display_ctrl.clothes"}
        with mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE._CharacterScene, "_resolve_root",
                               return_value="|Group|root"), \
             mock.patch.object(MODULE._CharacterScene, "_resolve_display",
                               return_value=("|Group|Display_ctrl", "clothes")), \
             mock.patch.object(MODULE, "_capture_subject", return_value=subject), \
             mock.patch.object(MODULE, "_enum_attributes", return_value=[display]), \
             mock.patch.object(MODULE, "_current_enum_label", return_value="Clothes01"), \
             mock.patch.object(MODULE, "_dag_path", return_value=FakeDagPath()):
            with self.assertRaises(MODULE._CharacterSceneError) as caught:
                MODULE._CharacterScene.capture("|Group|root")

        self.assertEqual("ROLE_SETUP_FAILED", caught.exception.code)
        self.assertEqual([41], removed)


class StreamingSessionTests(unittest.TestCase):
    def _runtime(self, timer_callbacks=None,
                 deferred=None, removed=None, timer_factory=None):
        timer_callbacks = timer_callbacks if timer_callbacks is not None else []
        deferred = deferred if deferred is not None else []
        removed = removed if removed is not None else []
        if timer_factory is None:
            def timer_factory(interval, callback):
                del interval
                timer_callbacks.append(callback)
                return 20 + len(timer_callbacks)
        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(lambda event, callback: 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(timer_factory),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(removed.append),
            }),
        })
        fake_cmds = type("FakeCmds", (), {
            "evalDeferred": staticmethod(deferred.append),
            "warning": staticmethod(lambda message: None),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])
        return fake_om, fake_cmds, scene

    def test_start_and_stop_are_one_complete_session_lifecycle(self):
        events = []
        removed = []
        registered = []

        class FakeWorker(object):
            def __init__(self, init_message):
                self.init_message = init_message
                self.started = False
                self.stopped = False
                self.joined = None

            def start(self):
                self.started = True

            def stop(self):
                self.stopped = True

            def join(self, timeout):
                self.joined = timeout

            def begin_ordered(self):
                pass

            def submit_ordered(self, frame):
                pass

            def ordered_pending(self):
                return 0

            def end_ordered(self, discard_pending=True):
                pass

            def set_reply_listener(self, listener):
                pass

        class FakeSceneMessage(object):
            kBeforeNew = 1
            kBeforeOpen = 2
            kMayaExiting = 3

            @staticmethod
            def addCallback(message, callback):
                registered.append((message, callback))
                return 10 + message

        class FakeTimerMessage(object):
            @staticmethod
            def addTimerCallback(interval, callback):
                registered.append((interval, callback))
                return 20

        class FakeEventMessage(object):
            @staticmethod
            def addEventCallback(event, callback):
                registered.append((event, callback))
                return 30

        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": FakeSceneMessage,
            "MEventMessage": FakeEventMessage,
            "MTimerMessage": FakeTimerMessage,
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(removed.append),
            }),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], ["Smile"])
        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "_SenderWorker", FakeWorker):
            session = MODULE._StreamingSession.start(scene, 24.0, events.append)
            session.stop()
            session.stop()

        self.assertEqual(["stopped"], [event.kind for event in events])
        self.assertEqual([11, 12, 13, 30, 20], removed)

    def test_ready_is_emitted_once_and_each_tick_submits_a_scene_frame(self):
        events = []
        timer_callbacks = []

        class FakeWorker(object):
            instance = None

            def __init__(self, init_message):
                del init_message
                self.submitted = []
                self.__class__.instance = self

            def start(self):
                pass

            def status(self):
                return ("ready", "Connected", {
                    "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "has_warning": False,
                }, None)

            def submit(self, frame):
                self.submitted.append(frame)

            def begin_ordered(self):
                pass

            def submit_ordered(self, frame):
                self.submitted.append(frame)

            def ordered_pending(self):
                return 0

            def end_ordered(self, discard_pending=True):
                pass

            def set_reply_listener(self, listener):
                pass

            def stop(self):
                pass

            def join(self, timeout):
                del timeout

        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(lambda event, callback: 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(
                    lambda interval, callback: timer_callbacks.append(callback) or 20),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(lambda callback_id: None),
            }),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], ["Smile"])
        scene.sample.return_value = MODULE._CharacterFrame(7, [[1, 2, 3]], [0.25])

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "_SenderWorker", FakeWorker), \
             mock.patch.object(MODULE.time, "time", side_effect=[100.0, 100.05]):
            session = MODULE._StreamingSession.start(scene, 24.0, events.append)
            timer_callbacks[0](0.0, 0.0, None)
            timer_callbacks[0](0.0, 0.0, None)
            session.stop()

        self.assertEqual(["ready", "stopped"], [event.kind for event in events])
        self.assertEqual(2, scene.sample.call_count)
        self.assertEqual(2, len(FakeWorker.instance.submitted))

    def test_time_changed_event_streams_without_idle_timer_and_caps_burst(self):
        time_callbacks = []
        timer_callbacks = []
        worker = mock.Mock()
        worker.status.return_value = (
            "ready", "Connected", {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False,
            }, None)
        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(
                    lambda event, callback: time_callbacks.append(
                        (event, callback)) or 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(
                    lambda interval, callback: timer_callbacks.append(callback) or 20),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(lambda callback_id: None),
            }),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])
        scene.sample.return_value = MODULE._CharacterFrame(7, [[1, 2, 3]], [])

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker), \
             mock.patch.object(
                 MODULE.time, "time",
                 side_effect=[100.0, 100.001, 100.01, 100.05]):
            session = MODULE._StreamingSession.start(scene, 30.0, None)
            self.assertEqual(1, len(time_callbacks))
            if time_callbacks:
                self.assertEqual("timeChanged", time_callbacks[0][0])
                for unused in range(4):
                    time_callbacks[0][1](None)
            session.stop()

        self.assertEqual(2, scene.sample.call_count)
        self.assertEqual(2, worker.submit.call_count)

    def test_worker_failure_defers_cleanup_and_wins_over_later_stop(self):
        events = []
        deferred = []
        removed = []
        timer_callbacks = []
        diagnostic = MODULE.make_diagnostic(
            "STREAM_INTERRUPTED", "connection lost", details="socket closed")

        class FakeWorker(object):
            instance = None

            def __init__(self, init_message):
                del init_message
                self.stop_count = 0
                self.join_count = 0
                self.__class__.instance = self

            def start(self):
                pass

            def status(self):
                return ("error", "connection lost", None, diagnostic)

            def stop(self):
                self.stop_count += 1

            def join(self, timeout):
                del timeout
                self.join_count += 1

            def begin_ordered(self):
                pass

            def submit_ordered(self, frame):
                pass

            def ordered_pending(self):
                return 0

            def end_ordered(self, discard_pending=True):
                pass

            def set_reply_listener(self, listener):
                pass

        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(lambda event, callback: 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(
                    lambda interval, callback: timer_callbacks.append(callback) or 20),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(removed.append),
            }),
        })
        fake_cmds = type("FakeCmds", (), {
            "evalDeferred": staticmethod(deferred.append),
            "warning": staticmethod(lambda message: None),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", FakeWorker):
            session = MODULE._StreamingSession.start(scene, 24.0, events.append)
            timer_callbacks[0](0.0, 0.0, None)
            self.assertEqual([], events)
            self.assertEqual([], removed)
            session.stop()
            deferred[0]()

        self.assertEqual(["failed"], [event.kind for event in events])
        self.assertEqual("STREAM_INTERRUPTED", events[0].diagnostic["code"])
        self.assertFalse(events[0].recapture_scene)
        self.assertEqual([11, 12, 13, 30, 20], removed)
        self.assertEqual(1, FakeWorker.instance.stop_count)
        self.assertEqual(1, FakeWorker.instance.join_count)

    def test_sampling_failure_requires_scene_recapture(self):
        events = []
        deferred = []
        timer_callbacks = []

        class FakeWorker(object):
            def __init__(self, init_message):
                del init_message

            def start(self):
                pass

            def status(self):
                return ("ready", "Connected", {
                    "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "has_warning": False,
                }, None)

            def stop(self):
                pass

            def join(self, timeout):
                del timeout

            def begin_ordered(self):
                pass

            def submit_ordered(self, frame):
                pass

            def ordered_pending(self):
                return 0

            def end_ordered(self, discard_pending=True):
                pass

            def set_reply_listener(self, listener):
                pass

        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(lambda event, callback: 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(
                    lambda interval, callback: timer_callbacks.append(callback) or 20),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(lambda callback_id: None),
            }),
        })
        fake_cmds = type("FakeCmds", (), {
            "evalDeferred": staticmethod(deferred.append),
            "warning": staticmethod(lambda message: None),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])
        scene.sample.side_effect = MODULE._CharacterSceneError(
            "SAMPLING_FAILED", "bad pose", details="bad plug")

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", FakeWorker):
            MODULE._StreamingSession.start(scene, 24.0, events.append)
            timer_callbacks[0](0.0, 0.0, None)
            deferred[0]()

        self.assertEqual(["ready", "failed"], [event.kind for event in events])
        self.assertTrue(events[1].recapture_scene)
        self.assertEqual("SAMPLING_FAILED", events[1].diagnostic["code"])

    def test_change_rate_replaces_timer_and_invalid_rate_fails_session(self):
        events = []
        deferred = []
        intervals = []
        removed = []

        class FakeWorker(object):
            def __init__(self, init_message):
                del init_message

            def start(self):
                pass

            def stop(self):
                pass

            def join(self, timeout):
                del timeout

            def begin_ordered(self):
                pass

            def submit_ordered(self, frame):
                pass

            def ordered_pending(self):
                return 0

            def end_ordered(self, discard_pending=True):
                pass

            def set_reply_listener(self, listener):
                pass

        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(lambda event, callback: 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(
                    lambda interval, callback: intervals.append(interval) or (20 + len(intervals))),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(removed.append),
            }),
        })
        fake_cmds = type("FakeCmds", (), {
            "evalDeferred": staticmethod(deferred.append),
            "warning": staticmethod(lambda message: None),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", FakeWorker):
            session = MODULE._StreamingSession.start(scene, 24.0, events.append)
            session.change_rate(30.0)
            session.change_rate(120.0)
            deferred[0]()

        self.assertEqual([1.0 / 24.0, 1.0 / 30.0], intervals)
        self.assertIn(21, removed)
        self.assertEqual(["failed"], [event.kind for event in events])
        self.assertEqual("INVALID_FRAME_RATE", events[0].diagnostic["code"])

    def test_change_rate_fails_and_removes_new_timer_when_old_timer_cannot_be_removed(self):
        events = []
        deferred = []
        timers = []
        removal_attempts = []
        failed_once = [False]

        def remove_callback(callback_id):
            removal_attempts.append(callback_id)
            if callback_id == 21 and not failed_once[0]:
                failed_once[0] = True
                raise RuntimeError("old timer removal failed")

        fake_om, fake_cmds, scene = self._runtime(timers, deferred)
        fake_om.MMessage.removeCallback = staticmethod(remove_callback)
        worker = mock.Mock()
        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker):
            session = MODULE._StreamingSession.start(scene, 24.0, events.append)
            session.change_rate(30.0)
            deferred[0]()

        self.assertEqual(2, len(timers))
        self.assertEqual([21, 22, 11, 12, 13, 30, 21], removal_attempts)
        self.assertEqual(["failed"], [event.kind for event in events])
        self.assertEqual("INTERNAL_ERROR", events[0].diagnostic["code"])

    def test_event_payloads_copy_dicts_and_freeze_nested_sequences(self):
        warning = {
            "missing_in_unreal": ["Jaw"],
            "missing_in_maya": [],
            "bone_name_remaps": [["Root", "root"]],
            "has_warning": True,
        }
        event = MODULE._StreamingSessionEvent("ready", warning=warning)
        warning["missing_in_unreal"].append("Neck")
        warning["bone_name_remaps"][0].append("extra")
        first_read = event.warning
        first_read["has_warning"] = False

        self.assertEqual(("Jaw",), event.warning["missing_in_unreal"])
        self.assertEqual((("Root", "root"),), event.warning["bone_name_remaps"])
        self.assertTrue(event.warning["has_warning"])

    def test_unexpected_worker_disconnect_is_stream_interrupted(self):
        events = []
        deferred = []
        timers = []

        class DisconnectedWorker(object):
            def __init__(self, init_message):
                del init_message

            def start(self):
                pass

            def status(self):
                return ("disconnected", "peer closed", None, None)

            def stop(self):
                pass

            def join(self, timeout):
                del timeout

            def begin_ordered(self):
                pass

            def submit_ordered(self, frame):
                pass

            def ordered_pending(self):
                return 0

            def end_ordered(self, discard_pending=True):
                pass

            def set_reply_listener(self, listener):
                pass

        fake_om, fake_cmds, scene = self._runtime(timers, deferred)
        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", DisconnectedWorker):
            MODULE._StreamingSession.start(scene, 24.0, events.append)
            timers[0](0.0, 0.0, None)
            deferred[0]()

        self.assertEqual(["failed"], [event.kind for event in events])
        self.assertEqual("STREAM_INTERRUPTED", events[0].diagnostic["code"])
        self.assertFalse(events[0].recapture_scene)

    def test_timer_start_failure_rolls_back_worker_and_callbacks(self):
        removed = []
        worker = mock.Mock()
        worker.is_alive.return_value = False
        fake_worker_type = mock.Mock(return_value=worker)
        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(lambda event, callback: 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(
                    lambda interval, callback: (_ for _ in ()).throw(
                        RuntimeError("timer failed"))),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(removed.append),
            }),
        })
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "_SenderWorker", fake_worker_type):
            with self.assertRaises(MODULE._StreamingSessionError) as caught:
                MODULE._StreamingSession.start(scene, 24.0, None)

        self.assertEqual("INTERNAL_ERROR", caught.exception.code)
        self.assertEqual([11, 12, 13, 30], removed)
        worker.stop.assert_called_once_with()
        worker.join.assert_called_once_with(1.0)

    def test_worker_start_failure_preserves_original_error(self):
        worker = mock.Mock()
        worker.start.side_effect = RuntimeError("thread start failed")
        fake_worker_type = mock.Mock(return_value=worker)
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])

        with mock.patch.object(MODULE, "_SenderWorker", fake_worker_type):
            with self.assertRaises(MODULE._StreamingSessionError) as caught:
                MODULE._StreamingSession.start(scene, 24.0, None)

        self.assertEqual("INTERNAL_ERROR", caught.exception.code)
        self.assertEqual("thread start failed", caught.exception.details)
        worker.stop.assert_called_once_with()
        worker.join.assert_not_called()

    def test_each_callback_registration_failure_rolls_back_prior_resources(self):
        for failing_call in (1, 2, 3):
            removed = []
            calls = [0]
            worker = mock.Mock()

            def add_callback(message, callback):
                del message, callback
                calls[0] += 1
                if calls[0] == failing_call:
                    raise RuntimeError("callback failed")
                return 10 + calls[0]

            fake_om, fake_cmds, scene = self._runtime(removed=removed)
            fake_om.MSceneMessage.addCallback = staticmethod(add_callback)
            with self.subTest(failing_call=failing_call), \
                 mock.patch.object(MODULE, "om", fake_om), \
                 mock.patch.object(MODULE, "cmds", fake_cmds), \
                 mock.patch.object(MODULE, "_SenderWorker", return_value=worker):
                with self.assertRaises(MODULE._StreamingSessionError):
                    MODULE._StreamingSession.start(scene, 24.0, None)

            self.assertEqual(
                [11 + offset for offset in range(failing_call - 1)], removed)
            worker.stop.assert_called_once_with()
            worker.join.assert_called_once_with(1.0)

    def test_cleanup_failures_do_not_mask_terminal_event_or_skip_worker_join(self):
        events = []
        removal_attempts = []
        worker = mock.Mock()
        worker.stop.side_effect = RuntimeError("worker stop failed")

        def remove_callback(callback_id):
            removal_attempts.append(callback_id)
            if callback_id == 11:
                raise RuntimeError("callback removal failed")

        fake_om, fake_cmds, scene = self._runtime()
        fake_om.MMessage.removeCallback = staticmethod(remove_callback)
        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker):
            session = MODULE._StreamingSession.start(scene, 24.0, events.append)
            session.stop()

        self.assertEqual([11, 12, 13, 30, 21], removal_attempts)
        worker.join.assert_called_once_with(1.0)
        self.assertEqual(["stopped"], [event.kind for event in events])

    def test_revision_change_fails_after_ready_and_requires_recapture(self):
        events = []
        timers = []
        deferred = []
        worker = mock.Mock()
        worker.status.return_value = ("ready", "Connected", None, None)
        fake_om, fake_cmds, scene = self._runtime(timers, deferred)
        scene.sample.return_value = MODULE._CharacterFrame(8, [], [])

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker):
            MODULE._StreamingSession.start(scene, 24.0, events.append)
            timers[0](0.0, 0.0, None)
            deferred[0]()

        self.assertEqual(["ready", "failed"], [event.kind for event in events])
        self.assertEqual("SAMPLING_FAILED", events[1].diagnostic["code"])
        self.assertTrue(events[1].recapture_scene)

    def test_rate_timer_creation_failure_keeps_old_timer_until_terminal_cleanup(self):
        events = []
        deferred = []
        removed = []
        timer_calls = [0]

        def timer_factory(interval, callback):
            del interval, callback
            timer_calls[0] += 1
            if timer_calls[0] == 2:
                raise RuntimeError("new timer failed")
            return 20

        fake_om, fake_cmds, scene = self._runtime(
            deferred=deferred, removed=removed, timer_factory=timer_factory)
        worker = mock.Mock()
        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker):
            session = MODULE._StreamingSession.start(scene, 24.0, events.append)
            session.change_rate(30.0)
            self.assertEqual([], removed)
            deferred[0]()

        self.assertEqual([11, 12, 13, 30, 20], removed)
        self.assertEqual(["failed"], [event.kind for event in events])
        self.assertEqual("INTERNAL_ERROR", events[0].diagnostic["code"])

    def test_event_consumer_failure_does_not_break_frame_submission(self):
        timers = []
        warnings = []
        worker = mock.Mock()
        worker.status.return_value = ("ready", "Connected", None, None)
        fake_om, fake_cmds, scene = self._runtime(timers)
        fake_cmds.warning = staticmethod(warnings.append)
        scene.sample.return_value = MODULE._CharacterFrame(7, [], [])

        def reject_event(event):
            del event
            raise RuntimeError("consumer failed")

        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker):
            session = MODULE._StreamingSession.start(scene, 24.0, reject_event)
            timers[0](0.0, 0.0, None)
            session.stop()

        worker.submit.assert_called_once()
        self.assertEqual(2, len(warnings))

    def test_late_timer_tick_after_stop_is_ignored(self):
        timers = []
        worker = mock.Mock()
        fake_om, fake_cmds, scene = self._runtime(timers)
        with mock.patch.object(MODULE, "om", fake_om), \
             mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_SenderWorker", return_value=worker):
            session = MODULE._StreamingSession.start(scene, 24.0, None)
            session.stop()
            timers[0](0.0, 0.0, None)

        worker.status.assert_not_called()
        scene.sample.assert_not_called()


class SenderLifecycleTests(unittest.TestCase):
    def test_ready_reply_must_echo_the_init_revision(self):
        worker = MODULE._SenderWorker({"type": "init", "revision": 7})

        class MismatchSocket(object):
            def __init__(self):
                self.reply = MODULE.encode_message({
                    "type": "ready", "revision": 8,
                    "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "workflow": "animation",
                    "target_morph_count": 0, "accepted_morph_count": 0,
                })

            def settimeout(self, timeout):
                pass

            def recv(self, size):
                chunk, self.reply = self.reply[:size], self.reply[size:]
                return chunk

            def sendall(self, packet):
                pass

            def close(self):
                pass

        fake_socket = MismatchSocket()

        def selectable(unused_rlist, unused_wlist, unused_xlist, unused_timeout):
            worker.stop()
            return ([], [], [])

        with mock.patch.object(worker, "_connect", return_value=fake_socket), \
             mock.patch.object(worker, "_send_initial", return_value=True), \
             mock.patch.object(MODULE.select, "select", side_effect=selectable):
            worker.run()

        state, _, _, diagnostic = worker.status()
        self.assertEqual("error", state)
        self.assertEqual("INVALID_MESSAGE", diagnostic["code"])
        self.assertIn("revision", diagnostic["details"])

    def test_bone_driven_outfit_treats_unreal_only_morphs_as_expected(self):
        identity = [0, 0, 0, 0, 0, 0, 1, 1, 1, 1]

        def connect_with(manifest_curves):
            init_message = MODULE.make_init_message(
                [["root", -1, identity]], manifest_curves, 7,
                MODULE.WORKFLOW_MODEL, True)
            worker = MODULE._SenderWorker(init_message)

            class ReadySocket(object):
                def __init__(self):
                    self.reply = MODULE.encode_message({
                        "type": "ready", "revision": 7,
                        "missing_in_unreal": [],
                        "missing_in_maya": ["Projected"],
                        "bone_name_remaps": [], "workflow": "model",
                        "target_morph_count": 1, "accepted_morph_count": 0,
                    })

                def settimeout(self, timeout):
                    pass

                def recv(self, size):
                    chunk, self.reply = self.reply[:size], self.reply[size:]
                    return chunk

                def sendall(self, packet):
                    pass

                def close(self):
                    pass

            fake_socket = ReadySocket()
            observed = []

            def selectable(unused_rlist, unused_wlist, unused_xlist, unused_timeout):
                if not observed:
                    # The first poll runs after the ready reply was processed.
                    observed.append(worker.status())
                worker.stop()
                return ([], [], [])

            with mock.patch.object(worker, "_connect", return_value=fake_socket), \
                 mock.patch.object(worker, "_send_initial", return_value=True), \
                 mock.patch.object(MODULE.select, "select", side_effect=selectable):
                worker.run()
            return observed[0]

        # A zero-name manifest is intentionally bone-driven: UE-only generated
        # Morphs are expected and must not raise the expression-coverage
        # warning.
        state, detail, warning, diagnostic = connect_with([])
        self.assertEqual("ready", state)
        self.assertEqual("Connected", detail)
        self.assertFalse(warning["has_warning"])
        self.assertEqual([], warning["missing_in_maya"])
        self.assertIsNone(diagnostic)

        # A declared manifest keeps the expression-coverage warning.
        _, _, warning, _ = connect_with(["Smile"])
        self.assertTrue(warning["has_warning"])
        self.assertEqual(["Projected"], warning["missing_in_maya"])

    def test_runtime_structured_error_is_preserved(self):
        worker = MODULE._SenderWorker({"type": "init", "revision": 7})
        runtime_error = MODULE.encode_message({
            "type": "error",
            "code": "INVALID_MESSAGE",
            "message": "bad frame",
            "details": "transform count mismatch",
        })

        class RuntimeErrorSocket(object):
            def __init__(self):
                self.reply = MODULE.encode_message({
                    "type": "ready", "revision": 7,
                    "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "workflow": "animation",
                    "target_morph_count": 0, "accepted_morph_count": 0,
                }) + runtime_error

            def settimeout(self, timeout):
                pass

            def recv(self, size):
                chunk, self.reply = self.reply[:size], self.reply[size:]
                return chunk

            def sendall(self, packet):
                pass

            def close(self):
                pass

        fake_socket = RuntimeErrorSocket()
        with mock.patch.object(worker, "_connect", return_value=fake_socket), \
             mock.patch.object(worker, "_send_initial", return_value=True), \
             mock.patch.object(MODULE.select, "select", return_value=([fake_socket], [], [])):
            worker.run()

        state, _, _, diagnostic = worker.status()
        self.assertEqual("error", state)
        self.assertEqual("INVALID_MESSAGE", diagnostic["code"])
        self.assertEqual("transform count mismatch", diagnostic["details"])

    def test_reply_listener_receives_cache_replies_and_keeps_connection(self):
        worker = MODULE._SenderWorker({"type": "init", "revision": 7})
        received = []
        worker.set_reply_listener(received.append)
        cache_ready = MODULE.encode_message({
            "type": "cache_ready", "upload_id": 1, "revision": 7,
            "frame_count": 321})

        class ListenerSocket(object):
            def __init__(self):
                self.reply = MODULE.encode_message({
                    "type": "ready", "revision": 7,
                    "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "workflow": "animation",
                    "target_morph_count": 0, "accepted_morph_count": 0,
                }) + cache_ready

            def settimeout(self, timeout):
                pass

            def recv(self, size):
                chunk, self.reply = self.reply[:size], self.reply[size:]
                return chunk or b"\x00"

            def sendall(self, packet):
                pass

            def close(self):
                pass

        fake_socket = ListenerSocket()

        def selectable(unused_rlist, unused_wlist, unused_xlist, unused_timeout):
            if fake_socket.reply:
                return ([fake_socket], [], [])
            worker.stop()
            return ([], [], [])

        with mock.patch.object(worker, "_connect", return_value=fake_socket), \
             mock.patch.object(worker, "_send_initial", return_value=True), \
             mock.patch.object(MODULE.select, "select", side_effect=selectable):
            worker.run()

        self.assertEqual(
            [{"type": "cache_ready", "upload_id": 1, "revision": 7,
              "frame_count": 321}], received)
        self.assertEqual("disconnected", worker.status()[0])

    def test_ordered_controls_survive_the_switch_back_to_latest_mode(self):
        worker = MODULE._SenderWorker({"type": "init", "revision": 7})

        class RecordingSocket(object):
            def __init__(self):
                self.sent = []
                self.reply = MODULE.encode_message({
                    "type": "ready", "revision": 7,
                    "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "workflow": "animation",
                    "target_morph_count": 0, "accepted_morph_count": 0})

            def settimeout(self, timeout):
                pass

            def recv(self, size):
                if not self.reply:
                    raise EOFError("done")
                chunk, self.reply = self.reply[:size], self.reply[size:]
                return chunk or b"\x00"

            def sendall(self, packet):
                self.sent.append(bytes(packet))

            def close(self):
                pass

        fake_socket = RecordingSocket()
        worker.begin_ordered()
        worker.submit_ordered({"type": "cache_clear"})
        worker.submit_ordered({"type": "frame", "order": "first-live"})
        # Switch back to Latest mode without discarding queued controls, then
        # queue a fresh live pose that must follow them.
        worker.end_ordered(discard_pending=False)
        worker.submit({"type": "frame", "order": "second-live"})

        def selectable(unused_rlist, unused_wlist, unused_xlist, unused_timeout):
            if len(fake_socket.sent) >= 3:
                worker.stop()
            # Never report readability so the runtime read path stays idle
            # while the queued controls and live frames drain in order.
            return ([], [], [])

        with mock.patch.object(worker, "_connect", return_value=fake_socket), \
             mock.patch.object(worker, "_send_initial", return_value=True), \
             mock.patch.object(MODULE.select, "select", side_effect=selectable):
            worker.run()

        self.assertEqual(3, len(fake_socket.sent))
        self.assertIn(b'"cache_clear"', fake_socket.sent[0])
        self.assertIn(b'first-live', fake_socket.sent[1])
        self.assertIn(b'second-live', fake_socket.sent[2])

    def test_stop_during_connection_prevents_init_and_terminates(self):
        entered = threading.Event()
        released = threading.Event()

        class BlockingSocket(object):
            def __init__(self):
                self.sent = []
                self._reply = MODULE.encode_message({
                    "type": "ready", "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "workflow": "animation",
                    "target_morph_count": 0, "accepted_morph_count": 0})

            def settimeout(self, timeout):
                pass

            def setblocking(self, blocking):
                pass

            def connect(self, address):
                entered.set()
                released.wait(2.0)

            def connect_ex(self, address):
                entered.set()
                released.wait(2.0)
                return errno.EINPROGRESS

            def shutdown(self, how):
                released.set()

            def close(self):
                released.set()

            def sendall(self, packet):
                self.sent.append(packet)

            def recv(self, size):
                chunk, self._reply = self._reply[:size], self._reply[size:]
                return chunk

        fake_socket = BlockingSocket()
        with mock.patch.object(MODULE.socket, "socket", return_value=fake_socket):
            worker = MODULE._SenderWorker({"type": "init", "revision": 7})
            worker.start()
            self.assertTrue(entered.wait(1.0))
            worker.stop()
            try:
                worker.join(0.2)
                self.assertFalse(worker.is_alive())
                self.assertEqual([], fake_socket.sent)
            finally:
                released.set()
                worker.join(1.0)

    def test_stop_after_connection_check_prevents_init(self):
        checked = threading.Event()
        released = threading.Event()

        class PausingEvent(object):
            def __init__(self):
                self._event = threading.Event()
                self._checks = 0

            def is_set(self):
                result = self._event.is_set()
                self._checks += 1
                if self._checks == 3:
                    checked.set()
                    released.wait(1.0)
                return result

            def set(self):
                self._event.set()

        class ReadySocket(object):
            def __init__(self):
                self.sent = []
                self._reply = MODULE.encode_message({
                    "type": "ready", "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "workflow": "animation",
                    "target_morph_count": 0, "accepted_morph_count": 0})

            def settimeout(self, timeout):
                pass

            def setblocking(self, blocking):
                pass

            def connect_ex(self, address):
                return 0

            def shutdown(self, how):
                pass

            def close(self):
                pass

            def sendall(self, packet):
                self.sent.append(packet)

            def recv(self, size):
                chunk, self._reply = self._reply[:size], self._reply[size:]
                return chunk

        fake_socket = ReadySocket()
        with mock.patch.object(MODULE.socket, "socket", return_value=fake_socket):
            worker = MODULE._SenderWorker({"type": "init", "revision": 7})
            worker._stop_event = PausingEvent()
            worker.start()
            self.assertTrue(checked.wait(1.0))
            worker.stop()
            released.set()
            worker.join(0.2)
            self.assertFalse(worker.is_alive())
            self.assertEqual([], fake_socket.sent)

    def test_stop_after_send_decision_prevents_init_write(self):
        decision_released = threading.Event()
        resume_write = threading.Event()

        class PausingLock(object):
            def __init__(self):
                self._lock = threading.Lock()
                self._releases = 0

            def __enter__(self):
                self._lock.acquire()
                return self

            def __exit__(self, exc_type, exc_value, traceback):
                self._releases += 1
                pause = self._releases == 2
                self._lock.release()
                if pause:
                    decision_released.set()
                    resume_write.wait(1.0)

        class PausingWriteSocket(object):
            def __init__(self):
                self.sent = []

            def settimeout(self, timeout):
                pass

            def setblocking(self, blocking):
                pass

            def connect_ex(self, address):
                return 0

            def shutdown(self, how):
                pass

            def close(self):
                pass

            def sendall(self, packet):
                self.sent.append(bytes(packet))

            def send(self, packet):
                if not resume_write.is_set():
                    raise OSError(errno.EWOULDBLOCK, "send would block")
                self.sent.append(bytes(packet))
                return len(packet)

        fake_socket = PausingWriteSocket()

        def pause_writable(readable, writable, errors, timeout):
            resume_write.wait(timeout)
            return [], writable, []

        with mock.patch.object(MODULE.socket, "socket", return_value=fake_socket), \
             mock.patch.object(MODULE.select, "select", side_effect=pause_writable):
            worker = MODULE._SenderWorker({"type": "init", "revision": 7})
            worker._lock = PausingLock()
            worker.start()
            self.assertTrue(decision_released.wait(1.0))
            worker.stop()
            resume_write.set()
            worker.join(0.2)
            self.assertFalse(worker.is_alive())
            self.assertEqual([], fake_socket.sent)

    def test_stop_closes_socket_while_initial_send_blocks(self):
        send_started = threading.Event()
        release_send = threading.Event()
        stop_finished = threading.Event()

        class BlockingSendSocket(object):
            def __init__(self):
                self._reply = MODULE.encode_message({
                    "type": "ready", "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": [], "workflow": "animation",
                    "target_morph_count": 0, "accepted_morph_count": 0})
                self.closed = False

            def settimeout(self, timeout):
                pass

            def setblocking(self, blocking):
                pass

            def connect_ex(self, address):
                return 0

            def shutdown(self, how):
                self.closed = True
                release_send.set()

            def close(self):
                self.closed = True
                release_send.set()

            def sendall(self, packet):
                send_started.set()
                release_send.wait(2.0)

            def send(self, packet):
                send_started.set()
                raise OSError(errno.EWOULDBLOCK, "send would block")

            def recv(self, size):
                chunk, self._reply = self._reply[:size], self._reply[size:]
                return chunk

        fake_socket = BlockingSendSocket()

        def block_writable(readable, writable, errors, timeout):
            release_send.wait(2.0)
            return [], writable, []

        with mock.patch.object(MODULE.socket, "socket", return_value=fake_socket), \
             mock.patch.object(MODULE.select, "select", side_effect=block_writable):
            worker = MODULE._SenderWorker({"type": "init", "revision": 7})
            worker.start()
            self.assertTrue(send_started.wait(1.0))
            stopper = threading.Thread(target=lambda: (worker.stop(), stop_finished.set()))
            stopper.start()
            try:
                self.assertTrue(stop_finished.wait(0.2))
                self.assertTrue(fake_socket.closed)
                worker.join(0.2)
                self.assertFalse(worker.is_alive())
            finally:
                release_send.set()
                stopper.join(1.0)
                worker.join(1.0)

    def test_initial_send_times_out_during_continuous_partial_writes(self):
        class FakeClock(object):
            now = 0.0

            def time(self):
                return self.now

        clock = FakeClock()

        class PartialSendSocket(object):
            def send(self, packet):
                clock.now += 1.0
                return 1

        worker = MODULE._SenderWorker({"type": "init", "revision": 7})
        with mock.patch.object(MODULE.time, "time", side_effect=clock.time):
            with self.assertRaisesRegex(MODULE.socket.timeout,
                                        "timed out sending init message"):
                worker._send_initial(PartialSendSocket())

        self.assertEqual(5.0, clock.now)



class PlaybackCacheTests(unittest.TestCase):
    def _frame(self, value):
        return {"type": "frame", "transforms": [[value] * 10], "curves": []}

    def test_incremental_cache_finalizes_atomically_and_replays_in_order(self):
        with __import__("tempfile").TemporaryDirectory() as directory:
            cache = MODULE._PlaybackCache.begin(
                snapshot_revision=7, capture_start=10, capture_end=11,
                scene_fps=30.0, temp_dir=directory, capture_time=123.0)
            cache.append(self._frame(10))
            self.assertFalse(cache.completed)
            self.assertTrue(__import__("os").path.exists(cache._partial_frames_path))
            cache.append(self._frame(11))
            cache.finalize()
            self.assertTrue(cache.completed)
            self.assertEqual((10, 11), cache.capture_range)
            self.assertEqual(2, cache.frame_count)
            self.assertEqual([10, 11], [frame["transforms"][0][0]
                                        for frame in cache.iter_frames()])
            loaded = MODULE._PlaybackCache.load(cache.metadata_path)
            self.assertEqual(cache.metadata, loaded.metadata)
            cache.delete()
            self.assertFalse(__import__("os").path.exists(cache.metadata_path))
            self.assertFalse(__import__("os").path.exists(cache.frames_path))

    def test_unstarted_frame_iterator_does_not_open_the_cache_file(self):
        with __import__("tempfile").TemporaryDirectory() as directory:
            cache = MODULE._PlaybackCache.begin(
                snapshot_revision=7, capture_start=10, capture_end=10,
                scene_fps=30.0, temp_dir=directory, capture_time=123.0)
            cache.append(self._frame(10))
            cache.finalize()

            with mock.patch("builtins.open", mock.mock_open()) as open_file:
                frames = cache.iter_frames()
                frames.close()

            open_file.assert_not_called()

    def test_incomplete_frame_range_never_becomes_replayable(self):
        with __import__("tempfile").TemporaryDirectory() as directory:
            cache = MODULE._PlaybackCache.begin(
                snapshot_revision=7, capture_start=10, capture_end=12,
                scene_fps=30.0, temp_dir=directory, capture_time=123.0)
            cache.append(self._frame(10))
            with self.assertRaises(MODULE._PlaybackCacheError):
                cache.finalize()
            self.assertFalse(__import__("os").path.exists(cache.metadata_path))
            self.assertFalse(__import__("os").path.exists(cache.frames_path))
            self.assertFalse(__import__("os").path.exists(cache._partial_frames_path))

    def test_incremental_cache_does_not_retain_appended_frame_objects(self):
        import gc
        import weakref

        class TrackedFrame(dict):
            pass

        with __import__("tempfile").TemporaryDirectory() as directory:
            cache = MODULE._PlaybackCache.begin(
                snapshot_revision=7, capture_start=0, capture_end=9,
                scene_fps=30.0, temp_dir=directory, capture_time=123.0)
            collected = []
            for value in range(10):
                frame = TrackedFrame(self._frame(value))
                reference = weakref.ref(frame)
                cache.append(frame)
                del frame
                gc.collect()
                collected.append(reference() is None)
            self.assertTrue(all(collected))
            cache.finalize()
            cache.delete()

    def test_load_rejects_metadata_pointing_at_another_frame_file(self):
        with __import__("tempfile").TemporaryDirectory() as directory:
            cache = MODULE._PlaybackCache.begin(
                snapshot_revision=7, capture_start=10, capture_end=10,
                scene_fps=30.0, temp_dir=directory, capture_time=123.0)
            cache.append(self._frame(10))
            cache.finalize()
            metadata = json.loads(pathlib.Path(cache.metadata_path).read_text())
            metadata["frames_file"] = "foreign.frames"
            pathlib.Path(directory, "foreign.frames").write_text(
                json.dumps(self._frame(10)) + "\n")
            pathlib.Path(cache.metadata_path).write_text(json.dumps(metadata) + "\n")
            with self.assertRaises(MODULE._PlaybackCacheError):
                MODULE._PlaybackCache.load(cache.metadata_path)

    def test_truncated_completed_frame_file_is_not_replayable(self):
        with __import__("tempfile").TemporaryDirectory() as directory:
            cache = MODULE._PlaybackCache.begin(
                snapshot_revision=7, capture_start=10, capture_end=11,
                scene_fps=30.0, temp_dir=directory, capture_time=123.0)
            cache.append(self._frame(10))
            cache.append(self._frame(11))
            cache.finalize()
            lines = pathlib.Path(cache.frames_path).read_text().splitlines()
            pathlib.Path(cache.frames_path).write_text(lines[0] + "\n")
            with self.assertRaises(MODULE._PlaybackCacheError):
                list(cache.iter_frames())

    def test_stale_cleanup_only_removes_valid_owned_caches(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            cache = MODULE._PlaybackCache.begin(
                snapshot_revision=1, capture_start=0, capture_end=0,
                scene_fps=24.0, temp_dir=directory, capture_time=1.0)
            cache.append(self._frame(0))
            cache.finalize()
            for path in (cache.metadata_path, cache.frames_path):
                os.utime(path, (1.0, 1.0))
            unrelated = os.path.join(directory, "MtoULiveLinkPlayback-not-owned.metadata.json")
            with open(unrelated, "w", encoding="utf-8") as stream:
                stream.write("not a cache")
            removed = MODULE._PlaybackCache.cleanup_stale(
                temp_dir=directory, now=MODULE.CACHE_STALE_SECONDS + 2)
            self.assertIn(cache.metadata_path, removed)
            self.assertFalse(os.path.exists(cache.metadata_path))
            self.assertTrue(os.path.exists(unrelated))

    def test_stale_cleanup_requires_owned_metadata_filename(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            cache = MODULE._PlaybackCache.begin(
                snapshot_revision=1, capture_start=0, capture_end=0,
                scene_fps=24.0, temp_dir=directory, capture_time=1.0)
            cache.append(self._frame(0))
            cache.finalize()
            foreign_metadata = os.path.join(directory, "foreign.metadata.json")
            os.replace(cache.metadata_path, foreign_metadata)
            os.utime(cache.frames_path, (1.0, 1.0))
            MODULE._PlaybackCache.cleanup_stale(
                temp_dir=directory, now=MODULE.CACHE_STALE_SECONDS + 2)
            self.assertTrue(os.path.exists(foreign_metadata))
            self.assertTrue(os.path.exists(cache.frames_path))


class CachedPlaybackTests(unittest.TestCase):
    class Timeline(object):
        def __init__(self):
            self.current = 42
            self.ranges = (0, 3)
            self.playing = True
            self.stopped = 0
            self.set_frames = []

        def current_frame(self):
            return self.current

        def playback_range(self):
            return self.ranges

        def is_playing(self):
            return self.playing

        def stop(self):
            self.playing = False
            self.stopped += 1

        def set_frame(self, frame):
            self.current = frame
            self.set_frames.append(frame)

    class Timer(object):
        def __init__(self):
            self.callbacks = {}
            self.next_id = 1

        def addTimerCallback(self, unused_interval, callback):
            timer_id = self.next_id
            self.next_id += 1
            self.callbacks[timer_id] = callback
            return timer_id

        def removeCallback(self, timer_id):
            self.callbacks.pop(timer_id, None)

        def fire(self):
            for callback in list(self.callbacks.values()):
                callback()

        def fire_until(self, predicate, limit=100):
            for unused in range(limit):
                if predicate():
                    return
                self.fire()
            raise AssertionError("fake timers did not reach the expected state")

    class Stream(object):
        def __init__(self, revision=7):
            self.is_ready = True
            self.revision = revision
            self.listener = None
            self.submitted = []
            self.actions = []
            self.paused = 0
            self.resumed = 0
            self.stopped = 0
            self.drained = True
            self.upload_id = 0
            self.play_id = 0

        def pause_for_cached(self, listener=None):
            self.listener = listener
            self.paused += 1

        def resume_from_cached(self):
            self.resumed += 1
            self.actions.append("resume")

        def submit_cached(self, message):
            self.submitted.append(message)
            self.actions.append(message["type"])
            if message["type"] == "cache_begin":
                self.upload_id = message["upload_id"]
                self.play_id = 0
            elif message["type"] == "cache_clear":
                if self.listener is not None:
                    self.emit({"type": "cache_cleared", "upload_id": self.upload_id,
                               "play_id": self.play_id})
                self.upload_id = 0
                self.play_id = 0

        def cached_delivery_drained(self):
            return self.drained

        def stop(self):
            self.stopped += 1
            self.is_ready = False

        def emit(self, message):
            if self.listener is None:
                raise AssertionError("Cached Playback did not attach a reply listener")
            if message["type"] == "cache_playing":
                self.play_id = message["play_id"]
            self.listener(message)

    def setUp(self):
        self.temp = __import__("tempfile").TemporaryDirectory()
        self.timer = self.Timer()
        self.timeline = self.Timeline()
        self.cached_playbacks = []

    def tearDown(self):
        for cached in self.cached_playbacks:
            cached.discard()
        self.temp.cleanup()

    def _scene(self, revision=7):
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            revision, "|root", "Clothes01", [("root", -1)], [])

        def sample():
            value = float(self.timeline.current)
            return MODULE._CharacterFrame(revision, [[value] * 10], [])

        scene.sample.side_effect = sample
        return scene

    def _attached(self, on_change=None, stream=None, scene=None, retention=None,
                  disk_usage=None, cache_factory=None):
        stream = stream or self.Stream()
        cached = MODULE._CachedPlayback(
            scene or self._scene(stream.revision), stream, on_change=on_change,
            timeline=self.timeline, temp_dir=self.temp.name, timer_api=self.timer,
            disk_usage=(disk_usage
                        or (lambda unused: type("Usage", (), {"free": 1 << 40})())),
            cache_factory=cache_factory, retention=retention)
        self.cached_playbacks.append(cached)
        return cached, stream

    @staticmethod
    def _types(stream):
        return [message["type"] for message in stream.submitted]

    @staticmethod
    def _capabilities(view):
        return (view.can_capture, view.can_upload, view.can_cancel, view.can_leave)

    def _capture_to_upload(self, cached, stream):
        cached.capture(2.0, lambda unused_size, unused_frames: True)
        self.timer.fire_until(lambda: "cache_end" in self._types(stream))
        self.assertEqual(MODULE._CachedPlayback.UPLOADING, cached.view.state)
        self.assertEqual((True, False, False, True),
                         self._capabilities(cached.view))
        return [message for message in stream.submitted
                if message["type"] == "cache_begin"][-1]

    def _capture_to_replay(self, cached, stream):
        begin = self._capture_to_upload(cached, stream)
        stream.emit({
            "type": "cache_ready", "upload_id": begin["upload_id"],
            "revision": begin["revision"], "frame_count": begin["frame_count"],
        })
        self.timer.fire_until(
            lambda: cached.view.state == MODULE._CachedPlayback.READY)
        self.assertNotIn("cache_play", self._types(stream))
        stream.emit({"type": "cache_playing", "upload_id": begin["upload_id"],
                     "play_id": cached._play_id + 1})
        self.timer.fire_until(
            lambda: cached.view.state == MODULE._CachedPlayback.REPLAYING)
        self.assertEqual((True, False, False, True),
                         self._capabilities(cached.view))
        return begin

    def test_ready_attachment_exposes_immutable_realtime_view_without_pausing(self):
        cached, stream = self._attached()

        self.assertEqual(MODULE._CachedPlayback.REALTIME, cached.view.state)
        self.assertEqual((False, False, False, False),
                         self._capabilities(cached.view))
        self.assertEqual(0, stream.paused)
        with self.assertRaises(AttributeError):
            cached.view.state = MODULE._CachedPlayback.CACHED_IDLE

    def test_enter_publishes_one_coherent_cached_idle_view(self):
        changes = []
        cached, stream = self._attached(changes.append)

        cached.enter()
        cached.enter()

        self.assertEqual(MODULE._CachedPlayback.CACHED_IDLE, cached.view.state)
        self.assertEqual((True, False, False, True),
                         self._capabilities(cached.view))
        self.assertEqual([MODULE._CachedPlayback.CACHED_IDLE], [view.state for view in changes])
        self.assertEqual(1, stream.paused)
        self.assertEqual(["cache_enter"], self._types(stream))

    def test_capture_uses_inclusive_playback_range_and_publishes_summary(self):
        changes = []
        cached, stream = self._attached(changes.append)

        self._capture_to_upload(cached, stream)

        self.assertEqual([0, 1, 2, 3, 42], self.timeline.set_frames)
        self.assertEqual(42, self.timeline.current)
        summary = cached.view.cache_summary
        self.assertEqual((0, 3), summary.capture_range)
        self.assertEqual(4, summary.frame_count)
        self.assertEqual(2.0, summary.scene_fps)
        self.assertEqual([0.0, 1.0, 2.0, 3.0], [
            message["transforms"][0][0] for message in stream.submitted
            if message["type"] == "cache_frame"])
        self.assertIn(MODULE._CachedPlayback.CAPTURING, [view.state for view in changes])
        self.assertIn(MODULE._CachedPlayback.UPLOADING, [view.state for view in changes])

    def test_custom_range_freezes_negative_single_frame_without_editing_playback_range(self):
        cached, stream = self._attached()
        with mock.patch.object(self.timeline, "playback_range",
                               side_effect=AssertionError("custom range must not query playback")):
            cached.capture(30.0, lambda unused_size, unused_frames: True,
                           capture_range=(-2, -2))
        self.timeline.ranges = (100, 120)
        self.timer.fire_until(lambda: "cache_end" in self._types(stream))
        begin = [message for message in stream.submitted
                 if message["type"] == "cache_begin"][-1]
        self.assertEqual((-2, -2, 1, 30.0),
                         (begin["start_frame"], begin["end_frame"],
                          begin["frame_count"], begin["fps"]))
        self.assertEqual([-2, 42], self.timeline.set_frames)
        self.assertEqual((100, 120), self.timeline.ranges)
        self.assertEqual((-2, -2), cached.view.cache_summary.capture_range)
        stream.emit({"type": "cache_ready", "upload_id": begin["upload_id"],
                     "revision": begin["revision"], "frame_count": 1})
        self.timer.fire_until(lambda: cached.view.state == cached.READY)
        stream.emit({"type": "cache_playing", "upload_id": begin["upload_id"],
                     "play_id": 1})
        stream.emit({"type": "cache_complete", "play_id": 1,
                     "applied_frame_count": 1, "elapsed_seconds": 0.0})
        self.timer.fire_until(lambda: cached.view.state == cached.COMPLETED)
        self.assertEqual(1, cached.view.current)

    def test_invalid_custom_range_fails_before_cache_or_scene_changes(self):
        cached, stream = self._attached()
        for selection, code in (((3, 2), "CACHED_PLAYBACK_CAPTURE_RANGE"),
                                ((False, 1), "CACHED_PLAYBACK_CAPTURE_RANGE"),
                                ((-20000, 1), "CACHED_PLAYBACK_FRAME_LIMIT")):
            with self.assertRaises(MODULE._CachedPlaybackError) as caught:
                cached.capture(24.0, capture_range=selection)
            self.assertEqual(code, caught.exception.code)
        self.assertEqual([], self.timeline.set_frames)
        self.assertEqual([], list(pathlib.Path(self.temp.name).iterdir()))
        self.assertNotIn("cache_begin", self._types(stream))

    def test_recapture_waits_for_unreal_to_end_old_playback(self):
        cached, stream = self._attached()
        self._capture_to_replay(cached, stream)
        old_upload = cached._active_upload_id
        old_play = cached._active_play_id
        before = len(self.timeline.set_frames)
        # Hold the clear acknowledgement while the old playback is active.
        real_listener = stream.listener
        queued = []
        stream.listener = lambda reply: queued.append(reply)
        cached.capture(24.0, capture_range=(-1, 0))
        self.timer.fire()
        self.assertEqual(before, len(self.timeline.set_frames))
        self.assertEqual((old_upload, old_play), cached._awaiting_clear)
        stream.listener = real_listener
        real_listener({"type": "cache_cleared", "upload_id": old_upload,
                       "play_id": old_play - 1})
        self.timer.fire()
        self.assertEqual(before, len(self.timeline.set_frames))
        for reply in queued:
            real_listener(reply)
        self.timer.fire_until(lambda: len(self.timeline.set_frames) > before)
        self.assertEqual(-1, self.timeline.set_frames[before])
        self.assertNotIn("cache_play", self._types(stream))

    def test_cancel_restores_frame_deletes_partial_cache_and_clears_before_resume(self):
        changes = []
        cached, stream = self._attached(changes.append)
        cached.enter()
        cached.capture(24.0, lambda unused_size, unused_frames: True)
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.CAPTURING, cached.view.state)
        self.assertEqual((False, False, True, False),
                         self._capabilities(cached.view))

        cached.cancel_capture()
        self.timer.fire()

        # Cancellation already resumed Real-time Preview, so the controller
        # completes that transition: a REALTIME view carrying the truthful
        # cancellation diagnostic, not a FAILED view implying disconnection.
        self.assertEqual(MODULE._CachedPlayback.REALTIME, cached.view.state)
        self.assertEqual((False, False, False, False),
                         self._capabilities(cached.view))
        self.assertEqual(42, self.timeline.current)
        self.assertIsNone(cached.view.cache_summary)
        self.assertLess(stream.actions.index("cache_clear"), stream.actions.index("resume"))
        self.assertEqual([], list(pathlib.Path(self.temp.name).iterdir()))
        cancelled = [view.diagnostic for view in changes if view.diagnostic]
        self.assertEqual("CACHED_PLAYBACK_CANCELLED", cancelled[-1]["code"])

    def test_large_cache_decline_and_disk_preflight_publish_stable_failures(self):
        confirmations = []
        cached, unused_stream = self._attached()
        cached.enter()
        with mock.patch.object(MODULE, "CACHE_CONFIRMATION_BYTES", 1):
            cached.capture(24.0, lambda size, frames: confirmations.append((size, frames)) or False)
            self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.REALTIME, cached.view.state)
        self.assertEqual(4, confirmations[0][1])

        diagnostics = []
        cached, unused_stream = self._attached(
            lambda view: diagnostics.append(view.diagnostic),
            disk_usage=lambda unused: type("Usage", (), {"free": 0})())
        cached.enter()
        cached.capture(24.0, lambda unused_size, unused_frames: True)
        self.timer.fire()
        # The space failure deleted the partial cache, cleared Unreal
        # ownership, and resumed Real-time Preview; the view says exactly that.
        self.assertEqual(MODULE._CachedPlayback.REALTIME, cached.view.state)
        diagnostic = [item for item in diagnostics if item][-1]
        self.assertEqual("CACHED_PLAYBACK_SPACE", diagnostic["code"])

    def test_upload_is_driven_in_bounded_timer_chunks(self):
        cached, stream = self._attached()
        cached.enter()
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 2):
            cached.capture(2.0, lambda unused_size, unused_frames: True)
            self.timer.fire_until(
                lambda: cached.view.state == MODULE._CachedPlayback.UPLOADING)
            self.timer.fire()
            self.assertEqual(2, self._types(stream).count("cache_frame"))
            self.timer.fire()
            self.assertEqual(4, self._types(stream).count("cache_frame"))
            self.timer.fire()
        self.assertIn("cache_end", self._types(stream))

    def test_ready_requires_matching_identity_and_drained_delivery(self):
        cached, stream = self._attached()
        begin = self._capture_to_upload(cached, stream)
        stream.emit({
            "type": "cache_ready", "upload_id": begin["upload_id"] + 1,
            "revision": 7, "frame_count": 4,
        })
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.UPLOADING, cached.view.state)

        stream.drained = False
        stream.emit({
            "type": "cache_ready", "upload_id": begin["upload_id"],
            "revision": 7, "frame_count": 4,
        })
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.UPLOADING, cached.view.state)
        stream.drained = True
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.READY, cached.view.state)

    def test_ready_rejects_non_exact_application_evidence(self):
        changes = []
        cached, stream = self._attached(changes.append)
        begin = self._capture_to_upload(cached, stream)
        stream.emit({
            "type": "cache_ready", "upload_id": begin["upload_id"],
            "revision": begin["revision"], "frame_count": True,
        })

        self.timer.fire()

        # Upload rejection dropped Unreal ownership and resumed Real-time
        # Preview, so the truthful post-failure view is REALTIME with the
        # upload-failure diagnostic, and the workflow stays usable.
        self.assertEqual(MODULE._CachedPlayback.REALTIME, cached.view.state)
        self.assertEqual(1, stream.resumed)
        self.assertLess(stream.actions.index("cache_clear"), stream.actions.index("resume"))
        diagnostic = [view.diagnostic for view in changes if view.diagnostic][-1]
        self.assertEqual("CACHED_UPLOAD_FAILED", diagnostic["code"])

    def test_unreal_stop_and_replay_again_reuse_the_verified_upload(self):
        cached, stream = self._attached()
        self._capture_to_replay(cached, stream)
        play_id = cached._active_play_id

        stream.emit({"type": "cache_stopped", "play_id": play_id + 1})
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.REPLAYING, cached.view.state)
        stream.emit({"type": "cache_progress", "play_id": play_id, "applied": 2})
        stream.emit({"type": "cache_stopped", "play_id": play_id})
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.STOPPED, cached.view.state)
        self.assertEqual(2, cached.view.current)
        self.assertEqual((True, False, False, True),
                         self._capabilities(cached.view))

        begin_count = self._types(stream).count("cache_begin")
        stream.emit({"type": "cache_playing", "upload_id": cached._active_upload_id,
                     "play_id": play_id + 1})
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.REPLAYING, cached.view.state)
        self.assertEqual((True, False, False, True),
                         self._capabilities(cached.view))
        self.assertEqual(begin_count, self._types(stream).count("cache_begin"))

    def test_completion_and_performance_failure_are_view_outcomes(self):
        changes = []
        cached, stream = self._attached(changes.append)
        self._capture_to_replay(cached, stream)
        play_id = cached._active_play_id
        stream.emit({
            "type": "cache_complete", "play_id": play_id,
            "applied_frame_count": 4, "elapsed_seconds": 1.5,
        })
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.COMPLETED, cached.view.state)
        self.assertEqual((True, False, False, True),
                         self._capabilities(cached.view))

        stream.emit({"type": "cache_playing", "upload_id": cached._active_upload_id,
                     "play_id": play_id + 1})
        self.timer.fire()
        stream.emit({
            "type": "error", "code": "CACHED_PLAYBACK_PERFORMANCE",
            "message": "Local playback fell behind.", "details": "frame 3",
            "play_id": play_id + 1,
        })
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.FAILED, cached.view.state)
        diagnostic_views = [view for view in changes if view.diagnostic]
        self.assertEqual("CACHED_PLAYBACK_PERFORMANCE", diagnostic_views[-1].diagnostic["code"])
        self.assertIsNotNone(cached.view.cache_summary)

    def test_leave_queues_clear_before_resumed_live_pose_and_retains_cache(self):
        cached, stream = self._attached()
        self._capture_to_replay(cached, stream)

        cached.leave()
        cached.leave()

        self.assertEqual(MODULE._CachedPlayback.REALTIME, cached.view.state)
        self.assertIsNotNone(cached.view.cache_summary)
        self.assertLess(stream.actions.index("cache_clear"), stream.actions.index("resume"))
        self.assertEqual(1, stream.resumed)

    def test_transport_detach_returns_opaque_retention_adopted_by_new_attachment(self):
        cached, stream = self._attached()
        self._capture_to_replay(cached, stream)
        diagnostic = MODULE.make_diagnostic("STREAM_INTERRUPTED")

        retention = cached.detach(MODULE._StreamingSessionEvent(
            "failed", diagnostic=diagnostic))

        self.assertEqual(MODULE._CachedPlayback.DETACHED, cached.view.state)
        self.assertTrue({"cache", "path", "delete"}.isdisjoint(dir(retention)))
        next_cached, next_stream = self._attached(retention=retention)
        self.assertEqual(4, next_cached.view.cache_summary.frame_count)
        self.assertTrue(next_cached.view.cache_summary.capture_time > 0.0)
        next_cached.enter()
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 2):
            next_cached.upload_retained_cache()
            self.timer.fire()
            self.assertNotIn("cache_begin", self._types(next_stream))
            self.timer.fire()
            self.assertNotIn("cache_begin", self._types(next_stream))
            self.timer.fire()
        self.assertIn("cache_begin", self._types(next_stream))

    def test_cached_detected_transport_loss_retains_cache_on_stopped_outcome(self):
        cached, stream = self._attached()
        self._capture_to_upload(cached, stream)
        stream.is_ready = False

        self.timer.fire()
        retention = cached.detach(MODULE._StreamingSessionEvent("stopped"))

        self.assertEqual(MODULE._CachedPlayback.DETACHED, cached.view.state)
        self.assertIsNotNone(retention)
        self.assertEqual(1, stream.stopped)

    def test_incompatible_retention_and_partial_capture_are_discarded(self):
        cached, stream = self._attached()
        self._capture_to_replay(cached, stream)
        retention = cached.detach(MODULE._StreamingSessionEvent(
            "failed", diagnostic=MODULE.make_diagnostic("STREAM_INTERRUPTED")))
        next_cached, unused_stream = self._attached(
            stream=self.Stream(8), scene=self._scene(8), retention=retention)
        self.assertIsNone(next_cached.view.cache_summary)
        self.assertEqual([], list(pathlib.Path(self.temp.name).iterdir()))

        partial, unused_stream = self._attached()
        partial.enter()
        partial.capture(24.0, lambda unused_size, unused_frames: True)
        self.timer.fire()
        self.assertIsNone(partial.detach(MODULE._StreamingSessionEvent(
            "failed", diagnostic=MODULE.make_diagnostic("STREAM_INTERRUPTED"))))
        self.assertIsNone(partial.view.cache_summary)

    def test_corrupt_completed_cache_is_rejected_through_the_view(self):
        class CorruptCache(object):
            def __init__(self, options):
                self.snapshot_revision = options["snapshot_revision"]
                self.capture_range = (
                    options["capture_start"], options["capture_end"])
                self.scene_fps = options["scene_fps"]
                self.capture_time = options["capture_time"]
                self.frame_count = 0
                self.completed = False
                self.deleted = False

            @property
            def metadata(self):
                return {"frame_count": self.frame_count}

            def append(self, unused_frame):
                self.frame_count += 1

            def finalize(self):
                self.completed = True

            def compatible_with(self, revision):
                return revision == self.snapshot_revision

            def iter_frames(self):
                raise MODULE._PlaybackCacheError("corrupt frame file")

            def delete(self):
                self.deleted = True

        class CorruptFactory(object):
            cache = None

            @classmethod
            def begin(cls, **options):
                cls.cache = CorruptCache(options)
                return cls.cache

        changes = []
        cached, stream = self._attached(
            changes.append, cache_factory=CorruptFactory)
        cached.enter()
        cached.capture(24.0, lambda unused_size, unused_frames: True)

        def failed_realtime_view():
            return any(
                view.state == MODULE._CachedPlayback.REALTIME and view.diagnostic
                for view in changes)

        self.timer.fire_until(failed_realtime_view)

        self.assertEqual(MODULE._CachedPlayback.REALTIME, cached.view.state)
        self.assertIsNone(cached.view.cache_summary)
        self.assertTrue(CorruptFactory.cache.deleted)
        self.assertEqual(["cache_enter", "cache_clear", "cache_enter",
                          "cache_begin", "cache_clear"],
                         self._types(stream))
        diagnostic = [view.diagnostic for view in changes if view.diagnostic][-1]
        self.assertEqual("CACHED_PLAYBACK_NO_CACHE", diagnostic["code"])

    def test_capture_stops_before_start_when_range_exceeds_frame_limit(self):
        cached, stream = self._attached()
        cached.enter()
        self.timeline.ranges = (0, MODULE.MAX_CACHE_FRAME_COUNT)

        with self.assertRaises(MODULE._CachedPlaybackError) as caught:
            cached.capture(24.0, lambda unused_size, unused_frames: True)

        self.assertEqual("CACHED_PLAYBACK_FRAME_LIMIT", caught.exception.code)
        # Zero frames were captured, no owned data exists, and the cached
        # workflow remains usable for a shorter range or leaving.
        self.assertEqual([], list(pathlib.Path(self.temp.name).iterdir()))
        self.assertNotIn("cache_frame", self._types(stream))
        self.assertEqual(MODULE._CachedPlayback.CACHED_IDLE, cached.view.state)
        self.assertEqual((True, False, False, True),
                         self._capabilities(cached.view))

    def test_capture_stops_as_soon_as_encoded_bytes_cross_the_payload_limit(self):
        changes = []
        cached, stream = self._attached(changes.append)
        cached.enter()
        first_frame_bytes = MODULE.cache_frame_wire_size(
            0, [[0.0] * 10], [])
        budget = first_frame_bytes * 2
        with mock.patch.object(MODULE, "MAX_CACHE_PAYLOAD_BYTES", budget):
            cached.capture(24.0, lambda unused_size, unused_frames: True)
            self.timer.fire_until(
                lambda: any(view.diagnostic for view in changes))

        self.assertEqual(MODULE._CachedPlayback.REALTIME, cached.view.state)
        diagnostic = [view.diagnostic for view in changes if view.diagnostic][-1]
        self.assertEqual("CACHED_PLAYBACK_PAYLOAD_LIMIT", diagnostic["code"])
        self.assertEqual([], list(pathlib.Path(self.temp.name).iterdir()))
        self.assertLess(stream.actions.index("cache_clear"), stream.actions.index("resume"))

    def test_capture_observes_transport_loss_before_the_next_sample(self):
        changes = []
        scene = self._scene()
        cached, stream = self._attached(changes.append, scene=scene)
        cached.enter()
        cached.capture(24.0, lambda unused_size, unused_frames: True)
        self.timer.fire()
        self.assertEqual(1, scene.sample.call_count)
        self.assertTrue(list(pathlib.Path(self.temp.name).iterdir()))

        stream.is_ready = False
        self.timer.fire()

        self.assertEqual(MODULE._CachedPlayback.DETACHED, cached.view.state)
        diagnostic = [view.diagnostic for view in changes if view.diagnostic][-1]
        self.assertEqual("STREAM_INTERRUPTED", diagnostic["code"])
        self.assertEqual(1, scene.sample.call_count)
        self.assertEqual(42, self.timeline.current)
        self.assertEqual([], list(pathlib.Path(self.temp.name).iterdir()))
        self.assertEqual(1, stream.stopped)
        self.assertEqual(0, stream.resumed)

    def test_cached_idle_observes_transport_termination_without_user_action(self):
        changes = []
        cached, stream = self._attached(changes.append)
        cached.enter()
        self.assertEqual(MODULE._CachedPlayback.CACHED_IDLE, cached.view.state)

        stream.is_ready = False
        self.timer.fire()

        detached = [view for view in changes
                    if view.state == MODULE._CachedPlayback.DETACHED]
        self.assertEqual("STREAM_INTERRUPTED", detached[-1].diagnostic["code"])
        self.assertEqual(1, stream.stopped)

    def test_completed_and_stopped_states_keep_observing_the_connection(self):
        cached, stream = self._attached()
        self._capture_to_replay(cached, stream)
        stream.emit({
            "type": "cache_complete", "play_id": cached._active_play_id,
            "applied_frame_count": 4, "elapsed_seconds": 1.5,
        })
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.COMPLETED, cached.view.state)

        stream.is_ready = False
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.DETACHED, cached.view.state)
        self.assertEqual(1, stream.stopped)

    def test_stopped_cached_state_observes_transport_termination(self):
        cached, stream = self._attached()
        self._capture_to_replay(cached, stream)
        stream.emit({"type": "cache_stopped", "play_id": cached._active_play_id})
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.STOPPED, cached.view.state)

        stream.is_ready = False
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.DETACHED, cached.view.state)

    def test_runtime_playback_failure_stays_cached_with_leave_and_retry(self):
        cached, stream = self._attached()
        self._capture_to_replay(cached, stream)
        stream.emit({
            "type": "error", "code": "CACHED_PLAYBACK_PERFORMANCE",
            "message": "Local playback fell behind.", "details": "frame 3",
            "play_id": cached._active_play_id,
        })

        self.timer.fire()

        # A failure that keeps Unreal cache ownership stays cached and must
        # leave retrying and leaving possible, and the connection is real.
        self.assertEqual(MODULE._CachedPlayback.FAILED, cached.view.state)
        self.assertEqual((True, False, False, True),
                         self._capabilities(cached.view))
        self.assertIsNotNone(cached.view.cache_summary)
        self.assertEqual(0, stream.resumed)
        self.timer.fire()
        self.assertEqual(MODULE._CachedPlayback.FAILED, cached.view.state)

    def test_discard_is_idempotent_and_rejected_actions_keep_stable_errors(self):
        cached, unused_stream = self._attached()
        with self.assertRaises(MODULE._CachedPlaybackError) as caught:
            cached.upload_retained_cache()
        self.assertEqual("CACHED_PLAYBACK_NO_CACHE", caught.exception.code)

        cached.discard()
        cached.discard()
        with self.assertRaises(MODULE._CachedPlaybackError) as caught:
            cached.enter()
        self.assertEqual("CACHED_PLAYBACK_NOT_READY", caught.exception.code)
        self.assertEqual([], list(pathlib.Path(self.temp.name).iterdir()))

    def test_observer_failure_is_contained_and_diagnostic_is_copied_once(self):
        observed = []

        def broken_observer(view):
            observed.append(view)
            raise RuntimeError("observer failed")

        cached, stream = self._attached(broken_observer)
        cached.enter()
        cached.enter()
        self.assertEqual(1, len(observed))
        self._capture_to_replay(cached, stream)
        stream.emit({
            "type": "error", "code": "CACHED_PLAYBACK_PERFORMANCE",
            "message": "late", "details": "late", "play_id": cached._active_play_id,
        })
        self.timer.fire()
        diagnostic_view = [view for view in observed if view.diagnostic][-1]
        copied = diagnostic_view.diagnostic
        copied["code"] = "MUTATED"
        self.assertEqual("CACHED_PLAYBACK_PERFORMANCE", diagnostic_view.diagnostic["code"])
        stream.emit({"type": "cache_playing", "upload_id": cached._active_upload_id,
                     "play_id": cached._active_play_id + 1})
        self.timer.fire()
        self.assertIsNone(observed[-1].diagnostic)

    def test_old_event_and_public_driving_interfaces_are_deleted(self):
        cached, unused_stream = self._attached()
        for name in ("tick", "capture_step", "capture_and_replay",
                     "begin_capture", "enter_cached_mode", "leave_cached_mode",
                     "phase", "cache"):
            self.assertFalse(hasattr(cached, name), name)
        self.assertFalse(hasattr(MODULE, "_CachedPlaybackEvent"))
        self.assertFalse(hasattr(MODULE, "_CachedPlaybackSession"))

class CacheUploadTests(unittest.TestCase):
    """Focused start/advance/close coverage for the internal upload module."""
    UPLOAD_ID = 9
    REVISION = 7

    class _TrackingIterator(object):
        def __init__(self, frames):
            self._it = iter(frames)
            self.closed = False

        def __next__(self):
            return next(self._it)

        def next(self):
            return self.__next__()

        def close(self):
            self.closed = True

    class _FakeCache(object):
        def __init__(self, frames, fps=24.0):
            self._frames = list(frames)
            self.scene_fps = fps
            self.frame_count = len(frames)
            self.capture_range = (0, len(frames) - 1)
            self.opens = 0
            self.iters = []

        def iter_frames(self):
            self.opens += 1
            iterator = CacheUploadTests._TrackingIterator(self._frames)
            self.iters.append(iterator)
            return iterator

    @staticmethod
    def _frame(value):
        return {"transforms": [[float(value)] * 10], "curves": []}

    def _expected_bytes(self, frames):
        return sum(
            MODULE.cache_frame_wire_size(
                index, frame["transforms"], frame["curves"])
            for index, frame in enumerate(frames))

    def _upload(self, submitted, drained):
        return MODULE._CacheUpload(
            submit=submitted.append, is_drained=lambda: drained[0])

    def test_declaration_reopens_cache_and_declares_exact_bytes_in_chunks(self):
        frames = [self._frame(value) for value in range(5)]
        cache = self._FakeCache(frames)
        submitted = []
        upload = self._upload(submitted, [True])
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 2):
            outcome = upload.start(cache, (self.UPLOAD_ID, self.REVISION))
            self.assertIsNone(outcome.action)
            self.assertEqual([], submitted)
            self.assertEqual(1, cache.opens)
            declaration_steps = 0
            while not any(
                    message["type"] == "cache_begin" for message in submitted):
                before = len(submitted)
                outcome = upload.advance()
                declaration_steps += 1
                self.assertIsNone(outcome.action)
                self.assertTrue(outcome.processed)
                self.assertLessEqual(len(submitted) - before, 1)
                self.assertLessEqual(declaration_steps, 3)
            self.assertEqual(3, declaration_steps)
            begin = [message for message in submitted
                     if message["type"] == "cache_begin"][-1]
            self.assertEqual(
                self._expected_bytes(frames), begin["payload_size"])
            self.assertEqual(2, cache.opens)
            self.assertTrue(cache.iters[0].closed)
            send_steps = 0
            while not any(
                    message["type"] == "cache_end" for message in submitted):
                before = [message for message in submitted
                          if message["type"] == "cache_frame"]
                outcome = upload.advance()
                send_steps += 1
                self.assertIsNone(outcome.action)
                sent = [message for message in submitted
                        if message["type"] == "cache_frame"][len(before):]
                self.assertLessEqual(len(sent), 2)
                self.assertLessEqual(send_steps, 3)
            sent_frames = [message for message in submitted
                           if message["type"] == "cache_frame"]
            self.assertEqual(
                [0, 1, 2, 3, 4],
                [message["index"] for message in sent_frames])
            self.assertEqual(
                [0.0, 1.0, 2.0, 3.0, 4.0],
                [message["transforms"][0][0] for message in sent_frames])
            self.assertTrue(upload.end_sent)
            self.assertTrue(cache.iters[1].closed)

    def test_send_waits_for_drain_and_arms_deadline_only_after_drain(self):
        frames = [self._frame(value) for value in range(2)]
        cache = self._FakeCache(frames)
        submitted = []
        drained = [True]
        now = [1000.0]
        upload = self._upload(submitted, drained)
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 8), \
                mock.patch.object(MODULE.time, "time", lambda: now[0]):
            outcome = upload.start(
                cache, (self.UPLOAD_ID, self.REVISION),
                self._expected_bytes(frames))
            self.assertIsNone(outcome.action)
            self.assertEqual(["cache_begin"],
                             [message["type"] for message in submitted])
            drained[0] = False
            outcome = upload.advance()
            self.assertFalse(outcome.processed)
            self.assertIsNone(outcome.action)
            self.assertEqual(["cache_begin"],
                             [message["type"] for message in submitted])
            drained[0] = True
            outcome = upload.advance()
            self.assertTrue(outcome.processed)
            self.assertIsNone(outcome.action)
            self.assertTrue(upload.end_sent)
            drained[0] = False
            now[0] += MODULE.UPLOAD_READY_TIMEOUT_SECONDS + 10.0
            outcome = upload.advance()
            self.assertFalse(outcome.processed)
            self.assertIsNone(outcome.action)
            drained[0] = True
            outcome = upload.advance()
            self.assertIsNone(outcome.action)
            armed = upload._deadline
            self.assertIsNotNone(armed)
            now[0] = armed - 0.5
            outcome = upload.advance()
            self.assertIsNone(outcome.action)
            now[0] = armed + 0.5
            outcome = upload.advance()
            self.assertFalse(outcome.processed)
            self.assertEqual(MODULE._CacheUpload.TRANSPORT, outcome.action)
            self.assertEqual("STREAM_INTERRUPTED", outcome.detail["code"])

    def test_open_failure_is_corrupt(self):
        class BadCache(self._FakeCache):
            def iter_frames(self):
                raise MODULE._PlaybackCacheError("corrupt frame file")

        upload = self._upload([], [True])
        outcome = upload.start(
            BadCache([self._frame(0.0)]), (self.UPLOAD_ID, self.REVISION), 8)
        self.assertEqual(MODULE._CacheUpload.CORRUPT, outcome.action)
        upload.close()

    def test_iteration_failure_is_corrupt_and_closes_the_file(self):
        frames = [self._frame(0.0), self._frame(1.0)]
        cache = self._FakeCache(frames)

        class FailingIterator(CacheUploadTests._TrackingIterator):
            def __init__(self, frames):
                super(FailingIterator, self).__init__(frames)
                self.calls = 0

            def __next__(self):
                self.calls += 1
                if self.calls > 1:
                    raise MODULE._PlaybackCacheError("torn frame row")
                return super(FailingIterator, self).__next__()

        def failing_iter():
            iterator = FailingIterator(cache._frames)
            cache.iters.append(iterator)
            cache.opens += 1
            return iterator
        cache.iter_frames = failing_iter
        submitted = []
        upload = self._upload(submitted, [True])
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 8):
            outcome = upload.start(
                cache, (self.UPLOAD_ID, self.REVISION),
                self._expected_bytes(frames))
            self.assertIsNone(outcome.action)
            outcome = upload.advance()
            self.assertEqual(MODULE._CacheUpload.CORRUPT, outcome.action)
            self.assertTrue(cache.iters[0].closed)

    def test_unencodable_frame_is_corrupt_and_closes_the_file(self):
        frames = [self._frame(0.0),
                  {"transforms": [[float("nan")] * 10], "curves": []}]
        cache = self._FakeCache(frames)
        submitted = []
        upload = self._upload(submitted, [True])
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 8):
            outcome = upload.start(
                cache, (self.UPLOAD_ID, self.REVISION),
                self._expected_bytes([frames[0]]) + 8)
            self.assertIsNone(outcome.action)
            outcome = upload.advance()
            self.assertTrue(outcome.processed)
            self.assertEqual(MODULE._CacheUpload.CORRUPT, outcome.action)
            self.assertTrue(cache.iters[0].closed)

    def test_submit_failures_are_transport(self):
        frames = [self._frame(0.0)]

        def failing_submit(unused_message):
            raise MODULE._StreamingSessionError("connection lost")

        upload = MODULE._CacheUpload(
            submit=failing_submit, is_drained=lambda: True)
        outcome = upload.start(
            self._FakeCache(frames), (self.UPLOAD_ID, self.REVISION),
            self._expected_bytes(frames))
        self.assertEqual(MODULE._CacheUpload.TRANSPORT, outcome.action)
        submitted = []
        cache = self._FakeCache(frames)
        calls = {"count": 0}

        def fail_on_frame(message):
            calls["count"] += 1
            if message["type"] == "cache_frame":
                raise MODULE._StreamingSessionError("connection lost")
            submitted.append(message)
        upload = MODULE._CacheUpload(
            submit=fail_on_frame, is_drained=lambda: True)
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 8):
            outcome = upload.start(
                cache, (self.UPLOAD_ID, self.REVISION),
                self._expected_bytes(frames))
            self.assertIsNone(outcome.action)
            outcome = upload.advance()
            self.assertEqual(MODULE._CacheUpload.TRANSPORT, outcome.action)

    def test_declared_overflow_is_failed(self):
        frames = [self._frame(value) for value in range(2)]
        first_bytes = MODULE.cache_frame_wire_size(
            0, frames[0]["transforms"], frames[0]["curves"])
        cache = self._FakeCache(frames)
        upload = self._upload([], [True])
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 8), \
                mock.patch.object(
                    MODULE, "MAX_CACHE_PAYLOAD_BYTES", first_bytes):
            outcome = upload.start(cache, (self.UPLOAD_ID, self.REVISION))
            self.assertIsNone(outcome.action)
            outcome = upload.advance()
            self.assertTrue(outcome.processed)
            self.assertEqual(MODULE._CacheUpload.FAILED, outcome.action)
            self.assertEqual("CACHED_UPLOAD_FAILED", outcome.detail["code"])
            upload.close()

    def test_close_during_declaration_and_send_closes_file_and_is_idempotent(self):
        frames = [self._frame(value) for value in range(4)]
        cache = self._FakeCache(frames)
        upload = self._upload([], [True])
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 2):
            outcome = upload.start(cache, (self.UPLOAD_ID, self.REVISION))
            self.assertIsNone(outcome.action)
            outcome = upload.advance()
            self.assertTrue(outcome.processed)
            self.assertFalse(cache.iters[0].closed)
            upload.close()
            self.assertTrue(cache.iters[0].closed)
            upload.close()
            outcome = upload.advance()
            self.assertFalse(outcome.processed)
            self.assertIsNone(outcome.action)
        send_cache = self._FakeCache(frames)
        send_upload = self._upload([], [True])
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 2):
            outcome = send_upload.start(
                send_cache, (self.UPLOAD_ID, self.REVISION),
                self._expected_bytes(frames))
            self.assertIsNone(outcome.action)
            outcome = send_upload.advance()
            self.assertTrue(outcome.processed)
            send_upload.close()
            self.assertTrue(send_cache.iters[0].closed)
            send_upload.close()
            outcome = send_upload.advance()
            self.assertFalse(outcome.processed)
            self.assertIsNone(outcome.action)


class ControllerLifecycleTests(unittest.TestCase):
    def test_main_window_offers_persisted_playback_caps(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.currentUnit.return_value = "film"
        fake_cmds.control.return_value = True
        stored = {"value": "15 fps"}

        def option_var(**kwargs):
            if "exists" in kwargs:
                return True
            if "query" in kwargs:
                return stored["value"]
            if "stringValue" in kwargs:
                _, stored["value"] = kwargs["stringValue"]
                return None
            raise AssertionError("unexpected optionVar call: {0}".format(kwargs))

        fake_cmds.optionVar.side_effect = option_var
        controller = MODULE._Controller()
        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "om", mock.MagicMock()):
            controller.build_ui()

        labels = [call[1]["label"] for call in fake_cmds.menuItem.call_args_list]
        self.assertEqual(list(MODULE.PLAYBACK_CAP_CHOICES), labels)
        self.assertEqual("15 fps", controller._playback_cap)
        self.assertTrue(any(
            call[1].get("edit") and call[1].get("value") == "15 fps"
            for call in fake_cmds.optionMenu.call_args_list))

    def test_changing_playback_cap_updates_existing_session_without_reconnect(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.control.return_value = True
        controller = MODULE._Controller()
        controller._playback_cap_menu = "capMenu"
        controller._session = mock.Mock()

        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller._on_playback_cap_changed("30 fps")

        self.assertEqual("30 fps", controller._playback_cap)
        controller._session.change_cap.assert_called_once_with("30 fps")


    def test_session_ready_requires_public_is_ready_never_probes_phase(self):
        class PhaseOnlySession(object):
            _phase = "ready"

        class GuardedPhaseSession(object):
            @property
            def is_ready(self):
                return True

            @property
            def _phase(self):
                raise AssertionError("Controller must not probe the private _phase")

        controller = MODULE._Controller()
        controller._session = PhaseOnlySession()
        self.assertFalse(controller._session_ready())

        controller._session = GuardedPhaseSession()
        self.assertTrue(controller._session_ready())

    def test_disconnecting_discards_cached_state_before_stopping_session(self):
        controller = MODULE._Controller()
        controller._session = mock.Mock()
        controller._discard_cached_playback = mock.Mock()
        controller._set_connected = mock.Mock()

        controller.disconnect()

        controller._discard_cached_playback.assert_called_once_with()
        controller._session.stop.assert_called_once_with()

    def test_ready_eagerly_attaches_cached_playback_with_retention(self):
        controller = MODULE._Controller()
        session = object()
        retention = object()
        cached = mock.Mock()
        cached.view = MODULE._CachedPlaybackView(MODULE._CachedPlayback.REALTIME)
        controller._session = session
        controller._scene = object()
        controller._cached_retention = retention
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()
        controller._on_cached_playback_view = mock.Mock()
        fake_cmds = mock.Mock()
        fake_cmds.checkBox.return_value = False

        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "_CachedPlayback", return_value=cached) as factory:
            controller._on_streaming_session_event(
                session, MODULE._StreamingSessionEvent("ready"))

        factory.assert_called_once_with(
            controller._scene, session, on_change=controller._on_cached_playback_view,
            retention=retention)
        self.assertIs(cached, controller._cached_playback)
        self.assertIsNone(controller._cached_retention)
        controller._on_cached_playback_view.assert_called_once_with(cached.view)

    def test_terminal_outcome_is_forwarded_to_detach_and_retention_is_opaque(self):
        controller = MODULE._Controller()
        session = object()
        cached = mock.Mock()
        retention = object()
        cached.detach.return_value = retention
        cached.view = MODULE._CachedPlaybackView(MODULE._CachedPlayback.DETACHED)
        controller._session = session
        controller._cached_playback = cached
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()
        diagnostic = MODULE.make_diagnostic("STREAM_INTERRUPTED")

        event = MODULE._StreamingSessionEvent("failed", diagnostic=diagnostic)
        controller._on_streaming_session_event(session, event)

        cached.detach.assert_called_once_with(event)
        self.assertIsNone(controller._session)
        self.assertIs(retention, controller._cached_retention)
        self.assertFalse(hasattr(controller, "_cached_cache"))

    def test_failed_capture_view_clears_cache_text_and_restores_realtime_mode(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.control.return_value = True
        controller = MODULE._Controller()
        session = mock.Mock()
        session.is_ready = True
        controller._session = session
        controller._mode = MODULE.CACHED_MODE
        controller._cache_text = "cacheText"
        controller._light = "light"
        controller._status_text = "statusText"
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            # A recoverable capture failure that already resumed Real-time
            # Preview publishes a REALTIME view with the truthful diagnostic;
            # the controller completes the same transition in the UI.
            controller._on_cached_playback_view(MODULE._CachedPlaybackView(
                MODULE._CachedPlayback.REALTIME,
                diagnostic=MODULE.make_diagnostic("CACHED_PLAYBACK_CANCELLED")))

        self.assertEqual(MODULE.REALTIME_MODE, controller._mode)
        self.assertIn("缓存：无", [call[1]["label"]
                                  for call in fake_cmds.text.call_args_list])
        labels = [call[1].get("label") for call in fake_cmds.text.call_args_list]
        self.assertIn("●  已连接", [label for label in labels if label])

    def test_switching_cached_mode_pauses_and_resumes_the_same_connection(self):
        class ReadySession(object):
            is_ready = True

        cached = mock.Mock()
        controller = MODULE._Controller()
        controller._session = ReadySession()
        controller._cached_playback = cached
        controller._ensure_cached_playback = mock.Mock(return_value=cached)
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()

        controller._on_mode_changed(MODULE.CACHED_MODE)
        self.assertEqual(MODULE.CACHED_MODE, controller._mode)
        cached.enter.assert_called_once_with()

        controller._on_mode_changed(MODULE.REALTIME_MODE)
        self.assertEqual(MODULE.REALTIME_MODE, controller._mode)
        cached.leave.assert_called_once_with()

    def test_controller_forwards_capture_and_uses_view_capabilities(self):
        cached = mock.Mock()
        controller = MODULE._Controller()
        controller._mode = MODULE.CACHED_MODE
        controller._cached_playback = cached
        controller._ensure_cached_playback = mock.Mock(return_value=cached)
        controller._refresh_fps = mock.Mock(return_value=24.0)
        controller._set_text = mock.Mock()
        controller._update_mode_controls = mock.Mock()

        controller._capture_cached_playback()

        cached.capture.assert_called_once_with(
            24.0, controller._confirm_large_cache, capture_range=None)
        self.assertFalse(hasattr(controller, "_cached_cache"))

        controller._set_enabled = mock.Mock()
        controller._set_tooltip = mock.Mock()
        controller._mode = MODULE.CACHED_MODE
        controller._capture_button = "capture"
        controller._upload_button = "upload"
        controller._cancel_capture_button = "cancel"
        controller._realtime_mode_button = "realtime"
        controller._cached_mode_button = "cached"
        controller._connect_button = "connect"
        controller._disconnect_button = "disconnect"
        controller._playback_cap_menu = "cap"
        controller._cached_view = MODULE._CachedPlaybackView(
            MODULE._CachedPlayback.REPLAYING, can_capture=True,
            can_leave=True)
        controller._update_mode_selection = mock.Mock()
        MODULE._Controller._update_mode_controls(controller)
        enabled = {recorded_call[0][0]: recorded_call[0][1]
                   for recorded_call in controller._set_enabled.call_args_list}
        self.assertTrue(enabled["capture"])
        self.assertFalse(enabled["upload"])
        self.assertFalse(enabled["cancel"])
        self.assertTrue(enabled["realtime"])
        tooltips = {recorded_call[0][0]: recorded_call[0][1]
                    for recorded_call in controller._set_tooltip.call_args_list}
        self.assertEqual("", tooltips["capture"])
        self.assertNotEqual("", tooltips["upload"])
        self.assertNotEqual("", tooltips["cancel"])

    def test_same_warning_signature_does_not_interrupt_twice(self):
        controller = MODULE._Controller()
        session = object()
        controller._session = session
        controller._workflow = MODULE.WORKFLOW_ANIMATION
        controller._scene = object()
        controller._cached_retention = None
        controller._set_connected = mock.Mock()
        controller._on_cached_playback_view = mock.Mock()
        controller._show_warning = mock.Mock()
        cached = mock.Mock()
        cached.view = MODULE._CachedPlaybackView(MODULE._CachedPlayback.REALTIME)
        fake_cmds = mock.Mock()
        fake_cmds.checkBox.return_value = True
        warning = {"missing_in_unreal": ["Jaw"], "missing_in_maya": [],
                   "bone_name_remaps": [], "has_warning": True}
        event = MODULE._StreamingSessionEvent("ready", warning=warning)
        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "_CachedPlayback", return_value=cached):
            controller._on_streaming_session_event(session, event)
            controller._on_streaming_session_event(session, event)
        self.assertEqual(1, controller._show_warning.call_count)
        self.assertIn("UE 显示实时姿势", controller._set_connected.call_args[0][1])


    def test_diagnostic_details_only_include_error_specific_information(self):
        snapshot = character_snapshot(
            1, "|Group|root", "Clothes09", [("root", -1)], ["Smile"],
            mesh_paths=["|Group|Geometry|body"])
        scene = mock.Mock()
        scene.snapshot.return_value = snapshot
        controller = MODULE._Controller()
        controller._scene = scene
        controller._last_warning = {
            "missing_in_unreal": ["MayaSmile"],
            "missing_in_maya": ["UnrealSmile"],
            "bone_name_remaps": ["arm -> arm_1"],
            "has_warning": True,
        }
        diagnostic = MODULE.make_diagnostic(
            "INTERNAL_ERROR", "socket worker failed", details="traceback line")

        with mock.patch.object(controller, "_refresh_fps", return_value=30.0):
            text = controller._diagnostic_text(diagnostic)

        self.assertIn("MtoU_LiveLink \u53d1\u751f\u5185\u90e8\u9519\u8bef", text)
        self.assertIn("INTERNAL_ERROR", text)
        self.assertIn("traceback line", text)
        self.assertNotIn("|Group|root", text)
        self.assertNotIn("Clothes09", text)
        self.assertNotIn("30 fps", text)
        self.assertNotIn("MayaSmile", text)
        self.assertNotIn("UnrealSmile", text)
        self.assertNotIn("arm -> arm_1", text)
        self.assertNotIn("|Group|Geometry|body", text)

    def test_diagnostic_window_wraps_text_and_fills_resized_window(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.window.side_effect = [False, "diagnosticWindow"]
        fake_cmds.formLayout.return_value = "diagnosticForm"
        fake_cmds.scrollField.return_value = "diagnosticField"
        fake_cmds.button.return_value = "copyButton"
        controller = MODULE._Controller()
        controller._diagnostic_text = mock.Mock(return_value="long diagnostic details")

        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller.show_diagnostics()

        create_window = fake_cmds.window.call_args_list[1]
        self.assertLessEqual(create_window[1]["widthHeight"][1], 400)
        create_field = fake_cmds.scrollField.call_args_list[0]
        self.assertTrue(create_field[1]["wordWrap"])
        self.assertNotIn("height", create_field[1])
        layout_edit = fake_cmds.formLayout.call_args_list[-1]
        self.assertTrue(layout_edit[1]["edit"])
        self.assertIn(
            ("diagnosticField", "bottom", 6, "copyButton"),
            layout_edit[1]["attachControl"])
        self.assertIn(
            ("copyButton", "bottom", 8),
            layout_edit[1]["attachForm"])

    def test_connect_presents_session_start_failure_without_retaining_session(self):
        snapshot = character_snapshot(
            1, "|root", "Clothes01", [("root", -1)], [], [], [])

        class FakeScene(object):
            @staticmethod
            def snapshot():
                return snapshot

            @staticmethod
            def sample():
                return MODULE._CharacterFrame(1, [], [])

        controller = MODULE._Controller()
        controller._scene = FakeScene()
        controller._show_error = mock.Mock()
        controller._set_connected = mock.Mock()
        fake_cmds = type("FakeCmds", (), {
            "currentUnit": staticmethod(lambda **kwargs: "film"),
        })
        with mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "om", object()), \
             mock.patch.object(
                 MODULE._StreamingSession, "start",
                 side_effect=MODULE._StreamingSessionError(
                     "INTERNAL_ERROR", "session setup failed")):
            controller.connect()

        self.assertIsNone(controller._session)
        diagnostic = controller._show_error.call_args[0][0]
        self.assertEqual("INTERNAL_ERROR", diagnostic["code"])

    def test_stale_session_event_does_not_change_current_controller_state(self):
        controller = MODULE._Controller()
        current_session = object()
        controller._session = current_session
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()
        stale_event = MODULE._StreamingSessionEvent(
            "failed", diagnostic=MODULE.make_diagnostic("STREAM_INTERRUPTED"))

        controller._on_streaming_session_event(object(), stale_event)

        self.assertIs(current_session, controller._session)
        controller._set_connected.assert_not_called()
        controller._show_error.assert_not_called()

    def test_manual_display_setup_reports_resolved_bind_conflicts(self):
        snapshot = character_snapshot(
            1, "|root", "Clothes01", [("root", -1)], [],
            bind_conflict_count=2)
        scene = mock.Mock()
        scene.snapshot.return_value = snapshot
        controller = MODULE._Controller()
        controller._pending_root = "|root"
        controller._clear_scene = mock.Mock()
        controller._selected_display = mock.Mock(return_value="|Display_ctrl")
        controller._capture_scene = mock.Mock(return_value=scene)
        controller._set_connected = mock.Mock()

        controller.set_display_controller()

        status = controller._set_connected.call_args[0][1]
        self.assertIn("已按绑定姿势解析 2 处蒙皮绑定矩阵冲突", status)

    def test_outfit_refresh_reports_resolved_bind_conflicts(self):
        snapshot = character_snapshot(
            2, "|root", "Clothes02", [("root", -1)], [],
            bind_conflict_count=3)
        controller = MODULE._Controller()
        controller._render_snapshot = mock.Mock()
        controller._set_connected = mock.Mock()

        controller._on_character_scene_event(
            MODULE._CharacterSceneEvent("outfit_changed", snapshot=snapshot))

        status = controller._set_connected.call_args[0][1]
        self.assertIn("已按绑定姿势解析 3 处蒙皮绑定矩阵冲突", status)

    def test_display_controller_replacement_terminates_active_session_first(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.control.return_value = True
        calls = []
        session = mock.Mock()
        session.is_ready = True
        session.stop.side_effect = lambda: calls.append("stop")
        snapshot = character_snapshot(
            1, "|root", "Clothes01", [("root", -1)], [])
        scene = mock.Mock()
        scene.snapshot.return_value = snapshot

        controller = MODULE._Controller()
        controller._session = session
        controller._pending_root = "|root"
        controller._selected_display = mock.Mock(return_value="|Display_ctrl")

        def clear_scene(**kwargs):
            calls.append("clear")

        def capture_scene(*args, **kwargs):
            calls.append("capture")
            return scene

        controller._clear_scene = clear_scene
        controller._capture_scene = capture_scene
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller.set_display_controller()

        self.assertEqual(["stop", "clear", "capture"], calls)

    def test_invalid_display_selection_keeps_live_session_and_scene(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.control.return_value = True
        session = mock.Mock()
        session.is_ready = True
        controller = MODULE._Controller()
        controller._session = session
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            1, "|root", "Clothes01", [("root", -1)], [])
        controller._scene = scene
        controller._clear_scene = mock.Mock()
        controller._capture_scene = mock.Mock()
        controller._show_error = mock.Mock()
        controller._selected_display = mock.Mock(
            side_effect=ValueError("请只选择一个 Display 曲线控制器。"))

        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller.set_display_controller()

        session.stop.assert_not_called()
        controller._clear_scene.assert_not_called()
        controller._capture_scene.assert_not_called()
        self.assertIs(scene, controller._scene)
        controller._show_error.assert_called_once()

    def test_replaced_scene_stale_failure_leaves_new_state_untouched(self):
        controller = MODULE._Controller()
        old_session = object()
        new_session = object()
        controller._session = new_session
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()
        stale_event = MODULE._StreamingSessionEvent(
            "failed", diagnostic=MODULE.make_diagnostic("CHARACTER_SCENE_CLOSED"),
            recapture_scene=True)

        controller._on_streaming_session_event(old_session, stale_event)

        self.assertIs(new_session, controller._session)
        controller._set_connected.assert_not_called()
        controller._show_error.assert_not_called()

    def test_duplicate_bone_diagnostic_keeps_true_connection_state(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.control.return_value = True
        fake_cmds.objExists.return_value = True
        duplicate_paths = ["|root|arm|joint", "|root|arm|joint_1"]
        snapshot = character_snapshot(
            1, "|root", "Clothes01", [("root", -1)], [],
            duplicate_paths=duplicate_paths)
        scene = mock.Mock()
        scene.snapshot.return_value = snapshot

        class ReadySession(object):
            is_ready = True

        controller = MODULE._Controller()
        controller._scene = scene
        controller._session = ReadySession()
        controller._duplicate_button = "duplicateButton"
        controller._set_connected = mock.Mock()

        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller.select_duplicate_bones()

        self.assertTrue(controller._set_connected.call_args[0][0])
        fake_cmds.select.assert_called_once_with(duplicate_paths, replace=True)

        fake_cmds.objExists.return_value = False
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller.select_duplicate_bones()

        self.assertTrue(controller._set_connected.call_args[0][0])

        controller._session = None
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller.select_duplicate_bones()

        self.assertFalse(controller._set_connected.call_args[0][0])

    def test_bind_pose_diagnostic_describes_current_failure_causes(self):
        diagnostic = MODULE.make_diagnostic("BIND_POSE_INVALID")

        self.assertIn("非有限值", diagnostic["solution"])
        self.assertIn("不可逆", diagnostic["solution"])
        self.assertNotIn("完整且一致", diagnostic["solution"])


class WorkflowTests(unittest.TestCase):
    def _fake_ui_cmds(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.currentUnit.return_value = "film"
        fake_cmds.control.return_value = True
        counter = itertools.count()
        for name in ("text", "optionMenu", "button", "checkBox",
                     "menuItem", "scriptJob"):
            getattr(fake_cmds, name).side_effect = (
                lambda *args, __name=name, **kwargs:
                    "{0}-{1}".format(__name, next(counter)))
        return fake_cmds


    def test_model_workflow_hides_cached_playback_and_shows_blendshape_toggle(self):
        fake_cmds = self._fake_ui_cmds()
        controller = MODULE._Controller()
        visibility = {}

        def set_visible(control, **kwargs):
            visibility[control] = kwargs["visible"]

        fake_cmds.control.side_effect = lambda control, **kwargs: (
            set_visible(control, **kwargs) if "visible" in kwargs else True)

        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "om", mock.MagicMock()):
            controller.build_ui()
            animation_visibility = dict(visibility)
            visibility.clear()
            controller._on_workflow_changed(MODULE.WORKFLOW_MODEL)
            model_visibility = dict(visibility)
            model_workflow = controller._workflow
            visibility.clear()
            controller._on_workflow_changed(MODULE.WORKFLOW_ANIMATION)
            back_visibility = dict(visibility)

        self.assertEqual(MODULE.WORKFLOW_MODEL, model_workflow)
        self.assertEqual(MODULE.WORKFLOW_ANIMATION, controller._workflow)
        for state in (animation_visibility, back_visibility):
            self.assertTrue(state[controller._realtime_mode_button])
            self.assertFalse(state[controller._bs_checkbox])
        self.assertFalse(model_visibility[controller._realtime_mode_button])
        self.assertFalse(model_visibility[controller._cached_mode_button])
        self.assertFalse(model_visibility[controller._cache_text])
        self.assertTrue(model_visibility[controller._bs_checkbox])

    def test_switching_workflow_disconnects_and_clears_cached_playback(self):
        controller = MODULE._Controller()
        session = mock.Mock()
        cached = mock.Mock()
        controller._session = session
        controller._cached_playback = cached
        controller._mode = MODULE.CACHED_MODE
        snapshot = character_snapshot(
            3, "|Group|root", "Clothes01", [("root", -1)], ["Smile"])
        scene = mock.Mock()
        scene.snapshot.return_value = snapshot
        controller._scene = scene
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()

        controller._on_workflow_changed(MODULE.WORKFLOW_MODEL)

        self.assertEqual(MODULE.WORKFLOW_MODEL, controller._workflow)
        self.assertEqual(MODULE.REALTIME_MODE, controller._mode)
        session.stop.assert_called_once_with()
        cached.discard.assert_called_once_with()
        self.assertIs(scene, controller._scene)
        self.assertIs(snapshot, controller._scene.snapshot())
        self.assertEqual("Clothes01", controller._scene.snapshot().outfit)
        self.assertEqual("|Group|root", controller._scene.snapshot().root)

    def test_switching_back_to_animation_keeps_model_disconnected_state(self):
        controller = MODULE._Controller()
        controller._workflow = MODULE.WORKFLOW_MODEL
        controller._set_connected = mock.Mock()

        controller._on_workflow_changed(MODULE.WORKFLOW_ANIMATION)

        self.assertEqual(MODULE.WORKFLOW_ANIMATION, controller._workflow)

    def test_toggling_blendshapes_while_connected_requires_new_negotiation(self):
        fake_cmds = self._fake_ui_cmds()
        controller = MODULE._Controller()
        controller._bs_checkbox = "bsCheckBox"
        controller._session = mock.Mock()
        controller._blendshapes_enabled = True

        fake_cmds.checkBox.side_effect = None
        fake_cmds.checkBox.return_value = False
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller._on_blendshapes_toggled()

        self.assertFalse(controller._blendshapes_enabled)
        controller._session.stop.assert_called_once_with()

    def test_toggling_blendshapes_while_disconnected_keeps_state_only(self):
        fake_cmds = self._fake_ui_cmds()
        controller = MODULE._Controller()
        controller._bs_checkbox = "bsCheckBox"

        fake_cmds.checkBox.side_effect = None
        fake_cmds.checkBox.return_value = False
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller._on_blendshapes_toggled()

        self.assertFalse(controller._blendshapes_enabled)

    def test_connect_sends_selected_workflow_and_blendshape_choice(self):
        started = []

        class FakeScene(object):
            snapshot = staticmethod(lambda: character_snapshot(
                1, "|root", "Clothes01", [("root", -1)], []))

            sample = staticmethod(lambda: MODULE._CharacterFrame(1, [], []))

        def record_start(*args, **kwargs):
            del args
            started.append(kwargs)
            return mock.Mock()

        controller = MODULE._Controller()
        controller._scene = FakeScene()
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()
        fake_cmds = type("FakeCmds", (), {
            "currentUnit": staticmethod(lambda **kwargs: "film"),
        })
        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "om", object()), \
                mock.patch.object(MODULE._StreamingSession, "start",
                                  side_effect=record_start):
            controller.connect()
            controller._session = None
            controller._workflow = MODULE.WORKFLOW_MODEL
            controller.connect()

        self.assertEqual(2, len(started))
        self.assertEqual(MODULE.WORKFLOW_ANIMATION, started[0]["workflow"])
        self.assertTrue(started[0]["blendshapes_enabled"])
        self.assertEqual(MODULE.WORKFLOW_MODEL, started[1]["workflow"])
        self.assertTrue(started[1]["blendshapes_enabled"])

    def test_streaming_session_init_carries_protocol_v7_workflow_fields(self):
        captured = {}
        worker = mock.Mock()
        worker.status.return_value = ("connecting", "Connecting", None, None)

        def worker_factory(init_message):
            captured["init"] = init_message
            return worker

        fake_om = type("FakeOpenMaya", (), {
            "MSceneMessage": type("FakeSceneMessage", (), {
                "kBeforeNew": 1, "kBeforeOpen": 2, "kMayaExiting": 3,
                "addCallback": staticmethod(lambda message, callback: 10 + message),
            }),
            "MEventMessage": type("FakeEventMessage", (), {
                "addEventCallback": staticmethod(lambda event, callback: 30),
            }),
            "MTimerMessage": type("FakeTimerMessage", (), {
                "addTimerCallback": staticmethod(
                    lambda interval, callback: timer_callbacks.append(callback) or 20),
            }),
            "MConditionMessage": type("FakeConditionMessage", (), {
                "addConditionCallback": staticmethod(
                    lambda condition, callback: 40),
            }),
            "MMessage": type("FakeMessage", (), {
                "removeCallback": staticmethod(lambda callback_id: None),
            }),
        })
        timer_callbacks = []
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], ["Smile"])

        with mock.patch.object(MODULE, "om", fake_om), \
                mock.patch.object(MODULE, "_SenderWorker",
                                  side_effect=worker_factory):
            session = MODULE._StreamingSession.start(
                scene, 24.0, None, workflow=MODULE.WORKFLOW_MODEL,
                blendshapes_enabled=False)
            session.stop()

        init_message = captured["init"]
        self.assertEqual(7, init_message["version"])
        self.assertEqual(MODULE.WORKFLOW_MODEL, init_message["workflow"])
        self.assertFalse(init_message["blendshapes_enabled"])
        self.assertEqual(["Smile"], init_message["curves"])
        self.assertEqual(MODULE.WORKFLOW_MODEL, session.workflow)
        self.assertFalse(session.blendshapes_enabled)

    def test_invalid_streaming_workflow_is_rejected_before_worker_start(self):
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])

        with self.assertRaises(MODULE._StreamingSessionError) as caught:
            MODULE._StreamingSession.start(
                scene, 24.0, None, workflow="preview")

        self.assertEqual("INVALID_MESSAGE", caught.exception.code)

    def test_clothes_change_disconnects_session_and_refreshes_outfit_manifest(self):
        controller = MODULE._Controller()
        session = mock.Mock()
        controller._session = session
        controller._outfit_change_was_connected = True
        before = character_snapshot(
            1, "|root", "Clothes01", [("root", -1)], ["OldSmile"])
        after = character_snapshot(
            2, "|root", "Clothes02", [("root", -1)], ["NewSmile"])
        scene = mock.Mock()
        scene.snapshot.return_value = before
        controller._scene = scene
        controller._render_snapshot = mock.Mock()
        controller._set_connected = mock.Mock()

        controller._on_character_scene_event(
            MODULE._CharacterSceneEvent("character_change_started", snapshot=before))

        session.stop.assert_called_once_with()

        controller._on_character_scene_event(
            MODULE._CharacterSceneEvent("outfit_changed", snapshot=after))

        self.assertIs(scene, controller._scene)
        self.assertEqual("|root", controller._scene.snapshot().root)
        controller._render_snapshot.assert_called_once()
        rendered = controller._render_snapshot.call_args[0][0]
        self.assertEqual("Clothes02", rendered.outfit)
        self.assertEqual(("NewSmile",), rendered.curve_names)
        status = controller._set_connected.call_args[0][1]
        self.assertIn("Clothes02", status)


class ProtocolTests(unittest.TestCase):
    def test_canonical_conformance_cases_for_maya_adapter(self):
        exercised = []
        for case in CORPUS["cases"]:
            if "maya" not in case["applies_to"]:
                continue
            exercised.append(case["id"])
            with self.subTest(case=case["id"]):
                self._assert_maya_conformance_case(case)
        self.assertEqual(
            [case["id"] for case in CORPUS["cases"] if "maya" in case["applies_to"]],
            exercised,
        )

    def test_maya_framing_ceiling_matches_the_shared_corpus_limit(self):
        # The single source of truth for the frozen wire limits is the
        # conformance corpus `limits` block. The framing ceiling is the one
        # value both hosts hardcode independently, so pin it against the
        # corpus in each adapter: editing the constant without the corpus (or
        # vice versa) fails Maya here and Unreal in the shared corpus test.
        self.assertEqual(
            CORPUS["limits"]["max_message_bytes"], MODULE.MAX_MESSAGE_BYTES)

    def test_maya_cache_limits_match_the_shared_corpus_limits(self):
        self.assertEqual(
            CORPUS["limits"]["max_cache_payload_bytes"],
            MODULE.MAX_CACHE_PAYLOAD_BYTES)
        self.assertEqual(
            CORPUS["limits"]["max_cache_frame_count"],
            MODULE.MAX_CACHE_FRAME_COUNT)

    def _assert_maya_conformance_case(self, case):
        operation = case["operation"]
        expected = case["expected"]
        if operation == "framing":
            if "raw_hex" in case:
                raw = bytes.fromhex(case["raw_hex"])

                class RawSocket(object):
                    def recv(self, size):
                        chunk, self.data = self.data[:size], self.data[size:]
                        return chunk

                sock = RawSocket()
                sock.data = raw
                with self.assertRaisesRegex(ValueError, "|".join(expected["keywords"])):
                    MODULE.recv_message(sock)
                return
            packets = [MODULE.encode_message(payload) for payload in case["payloads"]]
            stream = b"".join(packets)

            class ChunkSocket(object):
                def __init__(self, data, sizes):
                    self.data = data
                    self.sizes = list(sizes or [len(data)])

                def recv(self, size):
                    if not self.data:
                        return b""
                    limit = self.sizes.pop(0) if self.sizes else size
                    count = min(size, limit, len(self.data))
                    chunk, self.data = self.data[:count], self.data[count:]
                    return chunk

            sock = ChunkSocket(stream, case.get("chunk_sizes"))
            decoded = [MODULE.recv_message(sock) for unused in packets]
            self.assertEqual(case["payloads"], decoded)
            self.assertEqual(expected["message_count"], len(decoded))
            return
        if operation == "init":
            payload = case["payload"]
            actual = MODULE.make_init_message(
                payload["bones"], payload["curves"], payload["revision"],
                payload["workflow"], payload["blendshapes_enabled"])
            actual.update({key: value for key, value in payload.items() if key == "future"})
            self.assertEqual(payload, actual)
            self.assertTrue(MODULE.encode_message(actual))
            return
        if operation == "frame":
            payload = case["payload"]
            actual = MODULE.make_frame_message(payload["transforms"], payload["curves"])
            actual.update({key: value for key, value in payload.items() if key == "future"})
            self.assertEqual(payload, actual)
            self.assertTrue(MODULE.encode_message(actual))
            return
        if operation in ("ready", "error", "cache_ready", "cache_playing", "cache_complete",
                         "cache_progress", "cache_stopped", "cache_cleared"):
            if expected["accepted"]:
                self.assertEqual(case["payload"], MODULE.validate_reply(case["payload"]))
            else:
                with self.assertRaisesRegex(ValueError, "|".join(expected["keywords"])):
                    MODULE.validate_reply(case["payload"])
            return
        if operation == "cache_begin":
            payload = case["payload"]
            actual = MODULE.make_cache_begin_message(
                payload["revision"], payload["start_frame"], payload["end_frame"],
                payload["fps"], payload["payload_size"], payload["upload_id"])
            actual.update({key: value for key, value in payload.items() if key == "future"})
            self.assertEqual(payload, actual)
            self.assertTrue(MODULE.encode_message(actual))
            return
        if operation == "cache_frame":
            payload = case["payload"]
            counts = case.get("expected_counts")
            transforms = payload.get(
                "transforms",
                [[0, 0, 0, 0, 0, 0, 1, 1, 1, 1]
                 for unused in range((counts or {}).get("transforms", 0))])
            curves = payload.get(
                "curves", [0.0 for unused in range((counts or {}).get("curves", 0))])
            actual = MODULE.make_cache_frame_message(
                payload.get("index", 0), transforms, curves)
            actual.update({key: value for key, value in payload.items()
                           if key not in ("index", "transforms", "curves")})
            self.assertEqual(payload, actual)
            self.assertTrue(MODULE.encode_message(actual))
            return
        if operation == "cache_end":
            payload = dict(case["payload"])
            actual = MODULE.make_cache_end_message()
            actual.update({key: value for key, value in payload.items() if key != "type"})
            self.assertEqual(payload, actual)
            self.assertTrue(MODULE.encode_message(actual))
            return
        if operation == "cache_play":
            payload = case["payload"]
            actual = MODULE.make_cache_play_message(payload["play_id"])
            actual.update({key: value for key, value in payload.items()
                           if key != "play_id"})
            self.assertEqual(payload, actual)
            self.assertTrue(MODULE.encode_message(actual))
            return
        if operation in ("cache_stop", "cache_clear", "cache_enter"):
            payload = dict(case["payload"])
            builder = {
                "cache_stop": MODULE.make_cache_stop_message,
                "cache_clear": MODULE.make_cache_clear_message,
                "cache_enter": MODULE.make_cache_enter_message,
            }[operation]
            actual = builder()
            actual.update({key: value for key, value in payload.items() if key != "type"})
            self.assertEqual(payload, actual)
            self.assertTrue(MODULE.encode_message(actual))
            return
        self.fail("Unsupported Maya conformance operation: " + operation)

    def test_protocol_v7_init_and_structured_diagnostics(self):
        identity = [0, 0, 0, 0, 0, 0, 1, 1, 1, 1]
        message = MODULE.make_init_message([["root", -1, identity]], ["Smile"], 7)
        self.assertEqual(7, message["version"])
        self.assertEqual("animation", message["workflow"])
        self.assertTrue(message["blendshapes_enabled"])
        model_message = MODULE.make_init_message(
            [["root", -1, identity]], ["Smile"], 7,
            MODULE.WORKFLOW_MODEL, False)
        self.assertEqual("model", model_message["workflow"])
        self.assertFalse(model_message["blendshapes_enabled"])
        with self.assertRaisesRegex(ValueError, "workflow"):
            MODULE.make_init_message([["root", -1, identity]], [], 7, "preview", True)
        for code in ("CACHED_UPLOAD_FAILED", "CACHED_PLAYBACK_PERFORMANCE"):
            cached_diagnostic = MODULE.make_diagnostic(code)
            self.assertEqual(code, cached_diagnostic["code"])
            self.assertNotEqual(
                "MtoU_LiveLink 发生内部错误", cached_diagnostic["summary"])
        diagnostic = MODULE.make_diagnostic(
            "SKELETON_MISMATCH", "Skeleton differs", details="details"
        )
        self.assertEqual("SKELETON_MISMATCH", diagnostic["code"])
        self.assertEqual("骨架与 Unreal Skeletal Mesh 不匹配", diagnostic["summary"])
        self.assertIn("UE", diagnostic["solution"])
        for code in ("PREVIEW_NOT_READY", "PREVIEW_BUILD_FAILED",
                     "PREVIEW_MORPH_MISMATCH"):
            preview_diagnostic = MODULE.make_diagnostic(code)
            self.assertEqual(code, preview_diagnostic["code"])
            self.assertNotEqual(
                "MtoU_LiveLink 发生内部错误", preview_diagnostic["summary"])

    def test_maya_time_units_keep_exact_animation_frame_rates(self):
        expected = {
            "game": 15.0,
            "film": 24.0,
            "pal": 25.0,
            "ntsc": 30.0,
            "show": 48.0,
            "palf": 50.0,
            "ntscf": 60.0,
            "23.976fps": 23.976,
            "29.97fps": 29.97,
            "59.94fps": 59.94,
            "29.97df": 29.97,
        }
        for unit, fps in expected.items():
            with self.subTest(unit=unit):
                self.assertAlmostEqual(fps, MODULE.frames_per_second(unit))
        self.assertEqual("23.976 fps", MODULE.format_fps(23.976))
        self.assertEqual("24 fps", MODULE.format_fps(24.0))
        with self.assertRaisesRegex(ValueError, "1.*60"):
            MODULE.validate_frame_rate(120.0)

    def test_ready_reply_keeps_both_blendshape_difference_lists(self):
        reply = {
            "type": "ready",
            "missing_in_unreal": ["MayaOnly"],
            "missing_in_maya": ["UnrealOnly"],
        }
        warning = MODULE.blendshape_warning_from_reply(reply)
        self.assertEqual(["MayaOnly"], warning["missing_in_unreal"])
        self.assertEqual(["UnrealOnly"], warning["missing_in_maya"])
        self.assertTrue(warning["has_warning"])

    def test_ready_reply_reports_unreal_bone_name_remapping(self):
        warning = MODULE.blendshape_warning_from_reply({
            "type": "ready",
            "missing_in_unreal": [],
            "missing_in_maya": [],
            "bone_name_remaps": ["hair_7/tip_2 -> tip_21"],
        })
        self.assertEqual(
            ["hair_7/tip_2 -> tip_21"], warning["bone_name_remaps"])
        self.assertTrue(warning["has_warning"])

    def test_namespace_normalization(self):
        self.assertEqual("spine_01", MODULE.normalize_name("|Rig|Hero:spine_01"))
        self.assertEqual("jaw", MODULE.normalize_name("|Rig|show:Hero:jaw"))

    def test_hierarchy_is_parent_first_and_rejects_duplicates(self):
        children = {"|root": ["|root|a", "|root|b"], "|root|a": ["|root|a|tip"]}
        records = MODULE.build_hierarchy("|root", lambda path: children.get(path, []))
        self.assertEqual(["root", "a", "tip", "b"], [record["name"] for record in records])
        self.assertEqual([-1, 0, 1, 0], [record["parent"] for record in records])
        with self.assertRaisesRegex(
                MODULE.DuplicateBoneNamesError,
                "duplicate normalized bone names: arm") as caught:
            MODULE.build_hierarchy(
                "|root",
                lambda path: ["|root|A:arm", "|root|B:arm"]
                if path == "|root" else [],
            )
        self.assertEqual(["|root|A:arm", "|root|B:arm"], caught.exception.paths)
        diagnostic = MODULE.duplicate_bone_diagnostic(caught.exception)
        self.assertEqual("DUPLICATE_BONE_NAMES", diagnostic["code"])
        self.assertEqual(caught.exception.paths, diagnostic["duplicate_paths"])
        self.assertIn("|root|A:arm", diagnostic["details"])
        allowed = MODULE.build_hierarchy(
            "|root",
            lambda path: ["|root|A:arm", "|root|B:arm"]
            if path == "|root" else [],
            allow_duplicates=True,
        )
        self.assertEqual(["root", "arm", "arm"], [item["name"] for item in allowed])

    def test_units_and_basis_conversion(self):
        self.assertEqual(100.0, MODULE.centimeters_per_unit("m"))
        transform = MODULE.convert_transform((1, 2, 3), (0, 0, 0, 1), (1, 2, 3), 100.0)
        self.assertEqual([100.0, 300.0, 200.0], transform[:3])
        self.assertEqual([1.0, 3.0, 2.0], transform[7:])
        half = math.sqrt(0.5)
        rotated = MODULE.convert_transform((0, 0, 0), (0, half, 0, half), (1, 1, 1), 1.0)
        self.assertAlmostEqual(-half, rotated[5])
        self.assertAlmostEqual(half, rotated[6])

    def test_length_prefix_and_large_dynamic_message(self):
        bones = [["bone_{0}".format(index), index - 1] for index in range(701)]
        packet = MODULE.encode_message(MODULE.make_init_message(bones, [], 7))
        self.assertEqual(len(packet) - 8, struct.unpack(">Q", packet[:8])[0])
        self.assertIn(b"bone_700", packet)

    def test_non_finite_frame_is_rejected(self):
        for value in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "finite"):
                    MODULE.make_frame_message([[0, 0, 0, 0, 0, 0, 1, 1, 1, value]], [])


class CharacterPartDiscoveryTests(unittest.TestCase):
    """A separated Head mesh publishes its BlendShapes like the Body's."""

    BONES = ("|Group|root", "|Group|root|head")
    MESHES = ("|Group|geometry|body", "|Group|geometry|head")
    SHAPES = {
        "|Group|geometry|body": "bodyShape",
        "|Group|geometry|head": "headShape",
    }
    CLUSTERS = {
        "|Group|root": ("bodyCluster",),
        "|Group|root|head": ("bodyCluster", "headCluster"),
    }
    GEOMETRY = {
        "bodyCluster": ("|Group|geometry|body",),
        "headCluster": ("|Group|geometry|head",),
    }
    DEFORMERS = {
        "|Group|geometry|body": ("bodyBlendShape", "bodyCluster"),
        "|Group|geometry|head": ("headBlendShape", "headCluster"),
    }
    ALIASES = {
        "bodyBlendShape": ("Smile", "weight[0]", "Blink", "weight[1]"),
        "headBlendShape": ("Jaw", "weight[0]", "Smile", "weight[1]"),
    }

    def _fake_cmds(self):
        shapes = self.SHAPES
        parents = {}
        for path in self.MESHES:
            current = path
            while "|" in current:
                parent = current.rsplit("|", 1)[0]
                parents[current] = [parent] if parent else []
                current = parent
        parents["|Group"] = []

        def list_relatives(path, **kwargs):
            if kwargs.get("parent"):
                return list(parents.get(path, []))
            if kwargs.get("shapes"):
                return [shapes[path]]
            return []

        def list_connections(plug, **kwargs):
            if kwargs.get("type") == "skinCluster":
                return list(self.CLUSTERS.get(plug, ()))
            return []

        def list_history(node, **kwargs):
            return list(self.DEFORMERS.get(node, ()))

        def skin_cluster(cluster, **kwargs):
            if kwargs.get("query") and kwargs.get("geometry"):
                return list(self.GEOMETRY.get(cluster, ()))
            return []

        def node_type(path):
            if path in shapes.values():
                return "mesh"
            if path in self.ALIASES:
                return "blendShape"
            if path in self.GEOMETRY:
                return "skinCluster"
            return "transform"

        def get_attr(plug, **kwargs):
            if plug.endswith(".intermediateObject"):
                return False
            return True

        def alias_attr(node, **kwargs):
            return list(self.ALIASES.get(node, ()))

        def ls(*names, **kwargs):
            known = set(shapes.values()) | set(self.MESHES)
            return [name for name in names if name in known]

        fake = type("FakeCmds", (), {
            "listRelatives": staticmethod(list_relatives),
            "listConnections": staticmethod(list_connections),
            "listHistory": staticmethod(list_history),
            "skinCluster": staticmethod(skin_cluster),
            "nodeType": staticmethod(node_type),
            "getAttr": staticmethod(get_attr),
            "aliasAttr": staticmethod(alias_attr),
            "ls": staticmethod(ls),
            "attributeQuery": staticmethod(lambda attribute, **kwargs: False),
        })
        return fake

    def test_head_mesh_is_a_visible_skinned_mesh_of_the_same_character(self):
        fake_cmds = self._fake_cmds()
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            meshes = MODULE._visible_skinned_meshes(list(self.BONES))
        self.assertEqual(list(self.MESHES), meshes)

    def test_separated_head_blendshapes_join_one_manifest(self):
        fake_cmds = self._fake_cmds()
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            curves = MODULE._discover_curve_plugs(
                list(self.BONES), list(self.MESHES))
        self.assertEqual(["Smile", "Blink", "Jaw"],
                         [curve["name"] for curve in curves])
        by_name = {curve["name"]: curve["plugs"] for curve in curves}
        # A name owned by both meshes becomes one curve with both plugs; the
        # sampler then requires every plug to agree on one value.
        self.assertEqual(["bodyBlendShape.weight[0]", "headBlendShape.weight[1]"],
                         by_name["Smile"])
        self.assertEqual(["bodyBlendShape.weight[1]"], by_name["Blink"])
        self.assertEqual(["headBlendShape.weight[0]"], by_name["Jaw"])


class StructuralHierarchyTests(unittest.TestCase):
    def _fake_cmds(self, children, types, joint_descendants=None,
                   parents=None, connections=None):
        joint_descendants = joint_descendants or {}
        parents = parents or {}
        connections = connections or {}
        calls = {"list_connections": []}

        def list_relatives(path, **kwargs):
            if kwargs.get("parent"):
                if path in parents:
                    return [parents[path]]
                if "|" in path:
                    parent = path.rsplit("|", 1)[0]
                    return [parent] if parent else []
                return []
            if kwargs.get("allDescendents") or kwargs.get("allDescendants") \
                    or kwargs.get("ad"):
                result = joint_descendants.get(path, None)
                return list(result) if result else None
            return list(children.get(path, []))

        def node_type(path):
            return types[path]

        def list_connections(plug, **kwargs):
            calls["list_connections"].append(plug)
            return connections.get(plug, [])

        fake = type("FakeCmds", (), {
            "listRelatives": staticmethod(list_relatives),
            "nodeType": staticmethod(node_type),
            "listConnections": staticmethod(list_connections),
        })
        return fake, calls

    def test_joint_transform_joint_is_published_in_order(self):
        children = {
            "|root": ["|root|spine_02"],
            "|root|spine_02": [
                "|root|spine_02|spine_03",
                "|root|spine_02|joints_grp",
            ],
            "|root|spine_02|joints_grp": [
                "|root|spine_02|joints_grp|MHHead:spine_04",
            ],
        }
        types = {
            "|root": "joint",
            "|root|spine_02": "joint",
            "|root|spine_02|spine_03": "joint",
            "|root|spine_02|joints_grp": "transform",
            "|root|spine_02|joints_grp|MHHead:spine_04": "joint",
        }
        joint_descendants = {
            "|root|spine_02|joints_grp": [
                "|root|spine_02|joints_grp|MHHead:spine_04",
            ],
        }
        fake_cmds, _ = self._fake_cmds(children, types, joint_descendants)
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            records = MODULE.build_hierarchy(
                "|root", MODULE._maya_children, allow_duplicates=True)
        self.assertEqual(
            ["root", "spine_02", "spine_03", "joints_grp", "spine_04"],
            [record["name"] for record in records])
        self.assertEqual(
            [-1, 0, 1, 1, 3],
            [record["parent"] for record in records])
        self.assertEqual(
            "|root|spine_02|joints_grp|MHHead:spine_04",
            records[4]["path"])

    def test_multilevel_chain_kept_and_unrelated_branch_excluded(self):
        children = {
            "|root": ["|root|j1"],
            "|root|j1": [
                "|root|j1|T1",
                "|root|j1|unrelated_grp",
                "|root|j1|extraMesh",
            ],
            "|root|j1|T1": ["|root|j1|T1|T2"],
            "|root|j1|T1|T2": ["|root|j1|T1|T2|j2"],
            "|root|j1|unrelated_grp": ["|root|j1|unrelated_grp|geo"],
        }
        types = {
            "|root": "joint",
            "|root|j1": "joint",
            "|root|j1|T1": "transform",
            "|root|j1|T1|T2": "transform",
            "|root|j1|T1|T2|j2": "joint",
            "|root|j1|unrelated_grp": "transform",
            "|root|j1|unrelated_grp|geo": "mesh",
            "|root|j1|extraMesh": "mesh",
        }
        joint_descendants = {
            "|root|j1|T1": ["|root|j1|T1|T2|j2"],
            "|root|j1|T1|T2": ["|root|j1|T1|T2|j2"],
            "|root|j1|unrelated_grp": None,
        }
        fake_cmds, _ = self._fake_cmds(children, types, joint_descendants)
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            records = MODULE.build_hierarchy(
                "|root", MODULE._maya_children, allow_duplicates=True)
        paths = [record["path"] for record in records]
        self.assertEqual(
            ["|root", "|root|j1", "|root|j1|T1",
             "|root|j1|T1|T2", "|root|j1|T1|T2|j2"],
            paths)
        self.assertNotIn("|root|j1|unrelated_grp", paths)
        self.assertNotIn("|root|j1|extraMesh", paths)

    def test_second_character_isolation_without_name_hardcoding(self):
        children = {
            "|Group|Hero:root": ["|Group|Hero:root|Hero:grp"],
            "|Group|Hero:root|Hero:grp": ["|Group|Hero:root|Hero:grp|Hero:arm"],
            "|Group|Villain:root": ["|Group|Villain:root|Villain:grp"],
            "|Group|Villain:root|Villain:grp": [
                "|Group|Villain:root|Villain:grp|Villain:arm",
            ],
        }
        types = {
            "|Group|Hero:root": "joint",
            "|Group|Hero:root|Hero:grp": "transform",
            "|Group|Hero:root|Hero:grp|Hero:arm": "joint",
            "|Group|Villain:root": "joint",
            "|Group|Villain:root|Villain:grp": "transform",
            "|Group|Villain:root|Villain:grp|Villain:arm": "joint",
        }
        joint_descendants = {
            "|Group|Hero:root|Hero:grp": [
                "|Group|Hero:root|Hero:grp|Hero:arm",
            ],
            "|Group|Villain:root|Villain:grp": [
                "|Group|Villain:root|Villain:grp|Villain:arm",
            ],
        }
        fake_cmds, _ = self._fake_cmds(children, types, joint_descendants)
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            hero = MODULE.build_hierarchy(
                "|Group|Hero:root", MODULE._maya_children,
                allow_duplicates=True)
            villain = MODULE.build_hierarchy(
                "|Group|Villain:root", MODULE._maya_children,
                allow_duplicates=True)
        self.assertEqual(
            ["|Group|Hero:root", "|Group|Hero:root|Hero:grp",
             "|Group|Hero:root|Hero:grp|Hero:arm"],
            [record["path"] for record in hero])
        self.assertEqual(
            ["|Group|Villain:root", "|Group|Villain:root|Villain:grp",
             "|Group|Villain:root|Villain:grp|Villain:arm"],
            [record["path"] for record in villain])
        self.assertEqual(["root", "grp", "arm"],
                         [record["name"] for record in hero])

    def test_pure_joint_hierarchy_keeps_order_and_parents(self):
        children = {
            "|root": ["|root|a", "|root|b"],
            "|root|a": ["|root|a|tip"],
        }
        types = {
            "|root": "joint",
            "|root|a": "joint",
            "|root|a|tip": "joint",
            "|root|b": "joint",
        }
        fake_cmds, _ = self._fake_cmds(children, types, {})
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            records = MODULE.build_hierarchy(
                "|root", MODULE._maya_children, allow_duplicates=True)
        self.assertEqual(["root", "a", "tip", "b"],
                         [record["name"] for record in records])
        self.assertEqual([-1, 0, 1, 0],
                         [record["parent"] for record in records])

    def test_complete_capture_accepts_full_set(self):
        bones = [{"path": "|root"}, {"path": "|root|a"}]
        joint_descendants = {"|root": ["|root|a"]}
        fake_cmds, _ = self._fake_cmds({}, {"|root": "joint"}, joint_descendants)
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            MODULE._ensure_complete_capture("|root", bones)

    def test_complete_capture_reports_first_missing_path(self):
        bones = [{"path": "|root"}, {"path": "|root|a"}]
        joint_descendants = {"|root": ["|root|a", "|root|a|tip"]}
        parents = {"|root|a|tip": "|root|a", "|root|a": "|root"}
        types = {"|root|a|tip": "joint", "|root|a": "joint", "|root": "joint"}
        fake_cmds, _ = self._fake_cmds(
            {}, types, joint_descendants, parents)
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            with self.assertRaises(MODULE._CharacterSceneError) as caught:
                MODULE._ensure_complete_capture("|root", bones)
        self.assertEqual("INCOMPLETE_SKELETON", caught.exception.code)
        self.assertIn("|root|a|tip", caught.exception.details)
        self.assertIn("|root|a", caught.exception.details)
        self.assertEqual("|root|a|tip",
                         caught.exception.context["missing_joint"])

    def test_complete_capture_names_non_transform_intermediate(self):
        bones = [{"path": "|root"}]
        joint_descendants = {"|root": ["|root|meshGrp|tip"]}
        parents = {"|root|meshGrp|tip": "|root|meshGrp",
                   "|root|meshGrp": "|root"}
        types = {"|root|meshGrp|tip": "joint",
                 "|root|meshGrp": "mesh",
                 "|root": "joint"}
        fake_cmds, _ = self._fake_cmds(
            {}, types, joint_descendants, parents)
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            with self.assertRaises(MODULE._CharacterSceneError) as caught:
                MODULE._ensure_complete_capture("|root", bones)
        self.assertEqual("INCOMPLETE_SKELETON", caught.exception.code)
        self.assertIn("|root|meshGrp|tip", caught.exception.details)
        self.assertIn("|root|meshGrp", caught.exception.details)
        self.assertIn("mesh", caught.exception.details)

    def test_bind_candidates_skip_structural_transforms(self):
        subject = {
            "bones": [
                {"path": "|root"},
                {"path": "|root|grp"},
                {"path": "|root|grp|tip"},
            ],
            "meshes": [],
        }
        types = {"|root": "joint", "|root|grp": "transform",
                 "|root|grp|tip": "joint"}
        fake_cmds, calls = self._fake_cmds({}, types, {}, {}, {})
        with mock.patch.object(MODULE, "cmds", fake_cmds), \
             mock.patch.object(MODULE, "_skin_clusters_for_meshes",
                               return_value=[]):
            candidates = MODULE._bind_world_candidates(subject)
        self.assertEqual(set(["|root", "|root|grp", "|root|grp|tip"]),
                         set(candidates))
        self.assertEqual([], candidates["|root|grp"])
        self.assertEqual(["|root.bindPose", "|root|grp|tip.bindPose"],
                         calls["list_connections"])

    def test_mh_duplicate_fragment_preserves_parents(self):
        children = {
            "|Group|root": ["|Group|root|pelvis"],
            "|Group|root|pelvis": ["|Group|root|pelvis|spine_01"],
            "|Group|root|pelvis|spine_01": ["|Group|root|pelvis|spine_01|spine_02"],
            "|Group|root|pelvis|spine_01|spine_02": [
                "|Group|root|pelvis|spine_01|spine_02|spine_03",
                "|Group|root|pelvis|spine_01|spine_02|joints_grp",
            ],
            "|Group|root|pelvis|spine_01|spine_02|spine_03": [
                "|Group|root|pelvis|spine_01|spine_02|spine_03|spine_04",
            ],
            "|Group|root|pelvis|spine_01|spine_02|joints_grp": [
                "|Group|root|pelvis|spine_01|spine_02|joints_grp|MHHead:spine_04",
            ],
            "|Group|root|pelvis|spine_01|spine_02|spine_03|spine_04": [
                "|Group|root|pelvis|spine_01|spine_02|spine_03|spine_04|clavicle_l",
            ],
            "|Group|root|pelvis|spine_01|spine_02|joints_grp|MHHead:spine_04": [
                "|Group|root|pelvis|spine_01|spine_02|joints_grp|MHHead:spine_04|MHHead:spine_05",
            ],
            "|Group|root|pelvis|spine_01|spine_02|joints_grp|MHHead:spine_04|MHHead:spine_05": [
                "|Group|root|pelvis|spine_01|spine_02|joints_grp|MHHead:spine_04|MHHead:spine_05|MHHead:clavicle_l",
            ],
        }
        records = MODULE.build_hierarchy(
            "|Group|root", lambda path: children.get(path, []),
            allow_duplicates=True)
        by_name = {}
        for record in records:
            by_name.setdefault(record["name"], []).append(record["path"])
        self.assertEqual(
            sorted([
                "|Group|root|pelvis|spine_01|spine_02|spine_03|spine_04",
                "|Group|root|pelvis|spine_01|spine_02|joints_grp|MHHead:spine_04",
            ]),
            sorted(by_name["spine_04"]))
        self.assertEqual(
            sorted([
                "|Group|root|pelvis|spine_01|spine_02|spine_03|spine_04|clavicle_l",
                "|Group|root|pelvis|spine_01|spine_02|joints_grp|MHHead:spine_04|MHHead:spine_05|MHHead:clavicle_l",
            ]),
            sorted(by_name["clavicle_l"]))
        duplicates = MODULE.duplicate_bone_paths(records)
        self.assertIn("spine_04", duplicates)
        self.assertIn("clavicle_l", duplicates)
        joints_grp = [record for record in records
                      if record["path"].endswith("|joints_grp")][0]
        mh_spine = [record for record in records
                    if record["path"].endswith("MHHead:spine_04")][0]
        self.assertEqual(joints_grp["name"], "joints_grp")
        self.assertEqual(mh_spine["parent"],
                         records.index(joints_grp))


if __name__ == "__main__":
    unittest.main()
