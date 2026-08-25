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
    (pathlib.Path(__file__).resolve().parents[3] / "protocol" / "conformance-v6.json")
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
    def test_runtime_structured_error_is_preserved(self):
        worker = MODULE._SenderWorker({"type": "init"})
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
        worker = MODULE._SenderWorker({"type": "init"})
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
        worker = MODULE._SenderWorker({"type": "init"})

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
            worker = MODULE._SenderWorker({"type": "init"})
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
            worker = MODULE._SenderWorker({"type": "init"})
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
            worker = MODULE._SenderWorker({"type": "init"})
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
            worker = MODULE._SenderWorker({"type": "init"})
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

        worker = MODULE._SenderWorker({"type": "init"})
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


class CachedPlaybackSessionTests(unittest.TestCase):
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

    class Worker(object):
        def __init__(self):
            self.state = "ready"
            self.pending = 0

        def status(self):
            return (self.state, "Connected", {
                "missing_in_unreal": [], "missing_in_maya": [],
                "bone_name_remaps": [], "has_warning": False}, None)

        def ordered_pending(self):
            return self.pending

    class Stream(object):
        def __init__(self):
            self.is_ready = True
            self.revision = 7
            self.paused = 0
            self.resumed = 0
            self.submitted = []
            self.ended = 0
            self.stopped = 0
            self.fail_submit = False
            self._worker = CachedPlaybackSessionTests.Worker()
            self.listener = None

        def pause_for_cached(self):
            self.paused += 1

        def resume_from_cached(self):
            self.resumed += 1

        def submit_cached(self, message):
            if self.fail_submit:
                raise RuntimeError("transport failed")
            self.submitted.append(message)

        def set_cached_reply_listener(self, listener):
            self.listener = listener

        def end_cached_replay(self, discard_pending=False):
            self.ended += 1

        def stop(self):
            self.stopped += 1

    def _scene(self, timeline):
        scene = mock.Mock()
        scene.snapshot.return_value = character_snapshot(
            7, "|root", "Clothes01", [("root", -1)], [])

        def sample():
            value = float(timeline.current)
            return MODULE._CharacterFrame(7, [[value] * 10], [])

        scene.sample.side_effect = sample
        return scene

    def _session(self, events=None):
        import tempfile
        timeline = self.Timeline()
        stream = self.Stream()
        scene = self._scene(timeline)
        session = MODULE._CachedPlaybackSession(
            scene, stream, on_event=events.append if events is not None else None,
            timeline=timeline, temp_dir=tempfile.gettempdir(),
            disk_usage=lambda unused: type("Usage", (), {"free": 1 << 40})(),
            timer_api=None)
        return session, timeline, stream, scene

    @staticmethod
    def _ready_reply(cache, upload_id=1):
        return {"type": "cache_ready", "upload_id": upload_id,
                "revision": 7, "frame_count": cache.frame_count}

    def _captured_session(self, events=None):
        """Run a full capture whose upload succeeds and enters local replay."""
        session, timeline, stream, scene = self._session(events)
        session._replies.put({"type": "cache_ready", "upload_id": 1,
                              "revision": 7, "frame_count": 4})
        cache = session.capture_and_replay(scene_fps=2.0)
        while session.phase == "uploading":
            session.tick()
        return session, timeline, stream, scene, cache

    def _submitted_types(self, stream):
        return [message["type"] for message in stream.submitted]

    def test_capture_uploads_complete_cache_then_unreal_drives_playback(self):
        events = []
        session, timeline, stream, scene, cache = self._captured_session(events)
        self.assertIs(cache, session.cache)
        self.assertTrue(cache.completed)
        self.assertEqual(4, cache.frame_count)
        self.assertEqual("replaying", session.phase)
        self.assertEqual(
            ["cache_enter", "cache_begin"]
            + ["cache_frame"] * 4 + ["cache_end", "cache_play"],
            self._submitted_types(stream))
        self.assertEqual("cache_enter", stream.submitted[0]["type"])
        begin_message = stream.submitted[1]
        self.assertEqual(7, begin_message["revision"])
        self.assertEqual(1, begin_message["upload_id"])
        self.assertEqual(2.0, begin_message["fps"])
        self.assertEqual((0, 3), (begin_message["start_frame"], begin_message["end_frame"]))
        self.assertEqual(4, begin_message["frame_count"])
        self.assertGreater(begin_message["payload_size"], 0)
        self.assertEqual([0, 1, 2, 3], [
            message["index"] for message in stream.submitted[2:-2]])
        self.assertEqual([0.0, 1.0, 2.0, 3.0], [
            message["transforms"][0][0] for message in stream.submitted[2:-2]])
        self.assertEqual(1, stream.submitted[-1]["play_id"])
        self.assertEqual([], [m for m in stream.submitted if m["type"] == "frame"])
        self.assertEqual(42, timeline.current)
        self.assertEqual(1, stream.paused)
        self.assertEqual(4, scene.sample.call_count)
        session._replies.put({
            "type": "cache_complete", "play_id": 1,
            "applied_frame_count": 4, "elapsed_seconds": 1.5})
        self.assertTrue(session.tick())
        self.assertEqual("completed", session.phase)
        self.assertIn("upload_completed", [event.kind for event in events])
        self.assertIn("replay_completed", [event.kind for event in events])
        completed_event = [e for e in events if e.kind == "replay_completed"][0]
        self.assertEqual(4, completed_event.current_frame)
        session.leave_cached_mode()
        self.assertEqual(1, stream.resumed)
        self.assertIsNotNone(session.cache)
        session.close()

    def test_transfer_progress_is_reported_separately_from_local_playback(self):
        events = []
        with mock.patch.object(MODULE, "UPLOAD_CHUNK_FRAMES", 2):
            session, unused_timeline, unused_stream, unused_scene, cache = \
                self._captured_session(events)
        kinds = [event.kind for event in events]
        self.assertIn("upload_started", kinds)
        self.assertIn("upload_progress", kinds)
        self.assertLess(kinds.index("upload_completed"), kinds.index("replay_started"))
        progress_events = [event for event in events if event.kind == "upload_progress"]
        self.assertEqual([2, 4], [event.current_frame for event in progress_events])
        self.assertEqual(4, cache.frame_count)
        session.close()

    def test_cancel_capture_deletes_partial_cache_and_restores_frame(self):
        events = []
        session, timeline, stream, unused_scene = self._session(events)

        def cancel_after_two(current, total):
            if current == 2:
                session.cancel_capture()

        self.assertIsNone(session.capture_and_replay(
            scene_fps=24.0, progress=cancel_after_two))
        self.assertIsNone(session.cache)
        self.assertEqual(42, timeline.current)
        self.assertEqual(1, stream.resumed)
        self.assertIn("capture_cancelled", [event.kind for event in events])
        session.close()

    def test_cancel_requested_by_final_progress_does_not_finalize_or_upload(self):
        events = []
        session, timeline, stream, unused_scene = self._session(events)

        def cancel_on_final_frame(current, total):
            if current == total:
                session.cancel_capture()

        self.assertIsNone(session.capture_and_replay(
            scene_fps=24.0, progress=cancel_on_final_frame))
        self.assertEqual(["cache_enter"],
                         [message["type"] for message in stream.submitted])
        self.assertIsNone(session.cache)
        self.assertEqual(42, timeline.current)
        self.assertEqual(1, stream.resumed)
        self.assertIn("capture_cancelled", [event.kind for event in events])
        session.close()

    def test_begin_capture_is_incremental_and_timer_cancelable(self):
        events = []
        session, timeline, stream, unused_scene = self._session(events)
        timer_callbacks = []

        class Timer(object):
            @staticmethod
            def addTimerCallback(interval, callback):
                timer_callbacks.append((interval, callback))
                return len(timer_callbacks)

        session._timer_api = Timer
        session.begin_capture(scene_fps=24.0)
        self.assertTrue(session.is_capturing)
        self.assertEqual(1, len(timer_callbacks))
        timer_callbacks[0][1]()
        session.cancel_capture()
        timer_callbacks[0][1]()
        self.assertFalse(session.is_capturing)
        self.assertIsNone(session.cache)
        self.assertEqual(42, timeline.current)
        self.assertEqual(1, stream.resumed)
        self.assertIn("capture_cancelled", [event.kind for event in events])
        session.close()

    def test_timer_registration_failure_cleans_up_capture_state(self):
        session, timeline, stream, unused_scene = self._session()
        with __import__("tempfile").TemporaryDirectory() as directory:
            session._temp_dir = directory

            class BrokenTimer(object):
                @staticmethod
                def addTimerCallback(unused_interval, unused_callback):
                    raise RuntimeError("timer unavailable")

            session._timer_api = BrokenTimer
            with self.assertRaises(MODULE._CachedPlaybackSessionError) as caught:
                session.begin_capture(scene_fps=24.0)
            self.assertEqual("INTERNAL_ERROR", caught.exception.code)
            self.assertFalse(session.is_capturing)
            self.assertIsNone(session.cache)
            self.assertEqual(42, timeline.current)
            self.assertEqual(1, stream.resumed)
            self.assertEqual([], list(pathlib.Path(directory).iterdir()))
        session.close()

    def test_recapture_stops_active_playback_and_replaces_coherently(self):
        session, unused_timeline, stream, unused_scene = self._session()
        self._captured_session_into(session)
        self.assertTrue(session.is_replaying)

        session.begin_capture(scene_fps=24.0)
        self.assertTrue(session.is_capturing)
        self.assertEqual("cache_stop", stream.submitted[-1]["type"])
        self.assertIsNone(session.cache)

        session.cancel_capture()
        session.capture_step()
        self.assertIsNone(session.cache)
        session.close()

    def _captured_session_into(self, session):
        session._replies.put({"type": "cache_ready",
                              "upload_id": session._upload_id + 1,
                              "revision": 7, "frame_count": 4})
        session.begin_capture(scene_fps=2.0)
        while session.is_capturing:
            session.capture_step()
        while session.phase == "uploading":
            session.tick()

    def test_transport_failure_during_upload_retains_completed_cache(self):
        events = []
        session, unused_timeline, stream, unused_scene = self._session(events)
        session.begin_capture(scene_fps=2.0)
        for unused_frame in range(3):
            session.capture_step()
        stream._worker.state = "error"

        session.capture_step()
        session.tick()

        self.assertEqual("transport_failed", session.phase)
        self.assertIsNotNone(session.cache)
        self.assertEqual(1, stream.stopped)
        self.assertIn("transport_failed", [event.kind for event in events])
        session.close()

    def test_upload_rejection_reports_stable_upload_failure(self):
        events = []
        session, unused_timeline, stream, unused_scene = self._session(events)
        session._replies.put({
            "type": "error",
            "code": "CACHE_METADATA_INVALID",
            "message": "The cache upload metadata is invalid.",
            "details": "frame_count mismatch",
            "upload_id": 1,
        })
        session.begin_capture(scene_fps=2.0)
        while session.is_capturing:
            session.capture_step()
        session.tick()

        self.assertEqual("upload_failed", session.phase)
        self.assertIsNotNone(session.cache)
        self.assertEqual(0, stream.stopped)
        failed_events = [event for event in events if event.kind == "failed"]
        self.assertEqual(1, len(failed_events))
        self.assertEqual(
            "CACHED_UPLOAD_FAILED", failed_events[0].diagnostic["code"])
        session.close()

    def test_playback_performance_failure_is_explicit_and_keeps_cache(self):
        events = []
        session, unused_timeline, stream, unused_scene, unused_cache = \
            self._captured_session(events)
        play_count = len(stream.submitted)
        session._replies.put({
            "type": "error",
            "code": "CACHED_PLAYBACK_PERFORMANCE",
            "message": "Local playback fell behind the captured scene rate.",
            "details": "frame 118 missed its schedule",
            "play_id": 1,
        })
        self.assertTrue(session.tick())

        self.assertEqual("playback_failed", session.phase)
        self.assertIsNotNone(session.cache)
        self.assertEqual(play_count, len(stream.submitted))
        performance_events = [
            event for event in events if event.kind == "playback_failed"]
        self.assertEqual(1, len(performance_events))
        self.assertEqual(
            "CACHED_PLAYBACK_PERFORMANCE",
            performance_events[0].diagnostic["code"])
        session.close()

    def test_manual_stop_holds_last_frame_and_replay_again_reuses_upload(self):
        session, unused_timeline, stream, unused_scene = self._session()
        self._captured_session_into(session)
        session.stop_replay()
        self.assertEqual("stopped", session.phase)
        self.assertEqual("cache_stop", stream.submitted[-1]["type"])
        self.assertIsNotNone(session.cache)
        uploads_before = self._submitted_types(stream).count("cache_begin")

        session.replay()

        self.assertEqual("replaying", session.phase)
        self.assertEqual(uploads_before, self._submitted_types(stream).count("cache_begin"))
        self.assertEqual("cache_play", stream.submitted[-1]["type"])
        session.close()

    def test_completed_review_replays_again_without_reupload(self):
        session, unused_timeline, stream, unused_scene = self._session()
        self._captured_session_into(session)
        session._replies.put({
            "type": "cache_complete", "play_id": 1,
            "applied_frame_count": 4, "elapsed_seconds": 1.5})
        session.tick()
        self.assertEqual("completed", session.phase)
        uploads_before = self._submitted_types(stream).count("cache_begin")

        session.replay()

        self.assertEqual("replaying", session.phase)
        self.assertEqual(uploads_before, self._submitted_types(stream).count("cache_begin"))
        session.close()

    def test_snapshot_change_invalidates_cache_before_replay_again(self):
        session, unused_timeline, stream, scene = self._session()
        self._captured_session_into(session)
        session.stop_replay()
        session.cache._metadata["snapshot_revision"] = 8

        with self.assertRaises(MODULE._CachedPlaybackSessionError) as caught:
            session.replay()

        self.assertEqual("CACHED_PLAYBACK_INCOMPATIBLE", caught.exception.code)
        self.assertIsNone(session.cache)
        self.assertEqual("idle", session.phase)
        self.assertEqual(1, stream.resumed)
        session.close()

    def test_leave_cached_mode_clears_unreal_buffer_and_resumes_streaming(self):
        session, unused_timeline, stream, unused_scene = self._session()
        self._captured_session_into(session)

        session.leave_cached_mode()

        types = self._submitted_types(stream)
        clear_index = len(types) - 1 - types[::-1].index("cache_clear")
        # The clear precedes every queued live pose in delivery order.
        self.assertEqual("cache_clear", types[clear_index])
        self.assertEqual("idle", session.phase)
        self.assertEqual(1, stream.resumed)
        session.close()

    def test_corrupt_completed_cache_is_deleted_before_upload_error(self):
        events = []
        session, unused_timeline, stream, unused_scene = self._session(events)
        with __import__("tempfile").TemporaryDirectory() as directory:
            session._temp_dir = directory
            self._captured_session_into(session)
            session.stop_replay()
            session._uploaded_revision = None
            cache = session.cache
            pathlib.Path(cache.frames_path).write_text("")

            session.replay()
            session.tick()

            self.assertIsNone(session.cache)
            self.assertEqual("idle", session.phase)
            self.assertEqual(1, stream.resumed)
            self.assertEqual([], list(pathlib.Path(directory).iterdir()))
        session.close()

    def test_three_capture_stop_cycles_leave_only_the_current_cache(self):
        session, unused_timeline, stream, unused_scene = self._session()
        with __import__("tempfile").TemporaryDirectory() as directory:
            session._temp_dir = directory
            for unused_cycle in range(3):
                self._captured_session_into(session)
                session.stop_replay()
                self.assertIsNotNone(session.cache)
                self.assertEqual("stopped", session.phase)

            self.assertEqual(2, len(list(pathlib.Path(directory).iterdir())))
            session.close()
            self.assertEqual([], list(pathlib.Path(directory).iterdir()))

    def test_large_cache_confirmation_and_space_rejection_restore_state(self):
        session, timeline, stream, unused_scene = self._session()
        with mock.patch.object(MODULE, "CACHE_CONFIRMATION_BYTES", 1):
            self.assertIsNone(session.capture_and_replay(
                scene_fps=24.0, confirm_large_cache=lambda *unused: False))
        self.assertEqual(42, timeline.current)
        self.assertEqual(1, stream.resumed)
        session.close()

        session, timeline, stream, unused_scene = self._session()
        session._disk_usage = lambda unused: type("Usage", (), {"free": 0})()
        with self.assertRaises(MODULE._CachedPlaybackSessionError) as caught:
            session.capture_and_replay(scene_fps=24.0)
        self.assertEqual("CACHED_PLAYBACK_SPACE", caught.exception.code)
        self.assertEqual(42, timeline.current)
        self.assertEqual(1, stream.resumed)
        session.close()

    def test_disk_usage_failure_cleans_up_partial_capture(self):
        session, timeline, stream, unused_scene = self._session()
        with __import__("tempfile").TemporaryDirectory() as directory:
            session._temp_dir = directory

            def disk_usage_failure(unused_directory):
                raise OSError("disk usage unavailable")

            session._disk_usage = disk_usage_failure
            with self.assertRaises(MODULE._CachedPlaybackSessionError) as caught:
                session.capture_and_replay(scene_fps=24.0)
            self.assertEqual("CACHED_PLAYBACK_SPACE", caught.exception.code)
            self.assertFalse(session.is_capturing)
            self.assertIsNone(session.cache)
            self.assertEqual(42, timeline.current)
            self.assertEqual(1, stream.resumed)
            self.assertEqual([], list(pathlib.Path(directory).iterdir()))
        session.close()

    def test_larger_later_frame_is_checked_before_append(self):
        session, timeline, stream, scene = self._session()
        timeline.ranges = (0, 1)
        small_message = MODULE.make_frame_message([[0.0]], [])
        small_size = MODULE._PlaybackCache.serialized_frame_size(small_message)

        def sample():
            if timeline.current == 0:
                return MODULE._CharacterFrame(7, [[0.0]], [])
            return MODULE._CharacterFrame(7, [[1.0e308] * 100], [])

        scene.sample.side_effect = sample
        session._disk_usage = lambda unused: type("Usage", (), {
            "free": small_size * 2,
        })()
        with __import__("tempfile").TemporaryDirectory() as directory:
            session._temp_dir = directory
            with self.assertRaises(MODULE._CachedPlaybackSessionError) as caught:
                session.capture_and_replay(scene_fps=24.0)
            self.assertEqual("CACHED_PLAYBACK_SPACE", caught.exception.code)
            self.assertEqual(["cache_enter"],
                         [message["type"] for message in stream.submitted])
            self.assertIsNone(session.cache)
            self.assertEqual(42, timeline.current)
            self.assertEqual(1, stream.resumed)
            self.assertEqual([], list(pathlib.Path(directory).iterdir()))
        session.close()

    def test_space_check_uses_remaining_estimate_after_each_write(self):
        session, timeline, stream, unused_scene = self._session()
        small_message = MODULE.make_frame_message([[0.0] * 10], [])
        small_size = MODULE._PlaybackCache.serialized_frame_size(small_message)
        free_values = [small_size * value for value in (4, 3, 2, 1)]

        def disk_usage(unused_directory):
            return type("Usage", (), {"free": free_values.pop(0)})()

        session._disk_usage = disk_usage
        with __import__("tempfile").TemporaryDirectory() as directory:
            session._temp_dir = directory
            session._replies.put({"type": "cache_ready", "upload_id": 1,
                                  "revision": 7, "frame_count": 4})
            cache = session.capture_and_replay(scene_fps=24.0)
            self.assertTrue(cache.completed)
            self.assertEqual(4, cache.frame_count)
            session.close()

    def test_oversized_cache_fails_fast_before_upload_starts(self):
        session, unused_timeline, stream, unused_scene = self._session()
        with mock.patch.object(MODULE, "MAX_CACHE_PAYLOAD_BYTES", 8):
            session._replies.put({"type": "cache_ready", "upload_id": 1,
                                  "revision": 7, "frame_count": 4})
            cache = session.capture_and_replay(scene_fps=24.0)
            self.assertTrue(cache.completed)
            self.assertEqual("upload_failed", session.phase)
            self.assertEqual(["cache_enter"],
                             [message["type"] for message in stream.submitted])
        session.close()

    def test_replay_requires_ready_connection_and_matching_revision(self):
        session, unused_timeline, stream, scene = self._session()
        stream.is_ready = False
        with self.assertRaises(MODULE._CachedPlaybackSessionError) as caught:
            session.enter_cached_mode()
        self.assertEqual("CACHED_PLAYBACK_NOT_READY", caught.exception.code)
        stream.is_ready = True
        stream.revision = 8
        with self.assertRaises(MODULE._CachedPlaybackSessionError) as caught:
            session.enter_cached_mode()
        self.assertEqual("CACHED_PLAYBACK_INCOMPATIBLE", caught.exception.code)
        session.close()

    def test_transport_failure_after_stop_does_not_overwrite_terminal_outcome(self):
        events = []
        session, unused_timeline, stream, unused_scene = self._session(events)
        self._captured_session_into(session)
        session.stop_replay()

        session.on_transport_failure(MODULE.make_diagnostic("STREAM_INTERRUPTED"))

        self.assertEqual("stopped", session.phase)
        self.assertEqual(0, stream.stopped)
        self.assertEqual(1, [event.kind for event in events].count("replay_stopped"))
        session.close()

    def test_stale_outcomes_from_older_attempts_are_ignored(self):
        events = []
        session, unused_timeline, stream, unused_scene = self._session(events)
        self._captured_session_into(session)

        # Outcomes carrying an older play identity must not complete or stop
        # the current attempt.
        session._replies.put({
            "type": "cache_complete", "play_id": 99,
            "applied_frame_count": 4, "elapsed_seconds": 1.5})
        session._replies.put({"type": "cache_stopped", "play_id": 99})
        self.assertTrue(session.tick())
        self.assertEqual("replaying", session.phase)
        self.assertNotIn("replay_completed", [event.kind for event in events])

        # The matching identity completes the attempt exactly once.
        session._replies.put({
            "type": "cache_complete", "play_id": 1,
            "applied_frame_count": 4, "elapsed_seconds": 1.5})
        self.assertTrue(session.tick())
        self.assertEqual("completed", session.phase)
        session.close()

    def test_inconsistent_ready_evidence_fails_the_upload(self):
        events = []
        session, unused_timeline, stream, unused_scene = self._session(events)
        session._replies.put({
            "type": "cache_ready", "upload_id": 1,
            "revision": 7, "frame_count": 3})
        cache = session.capture_and_replay(scene_fps=2.0)
        session.tick()

        self.assertTrue(cache.completed)
        self.assertEqual("upload_failed", session.phase)
        failed_events = [event for event in events if event.kind == "failed"]
        self.assertEqual(1, len(failed_events))
        self.assertIn(
            "inconsistent cache evidence",
            failed_events[0].diagnostic["message"])
        session.close()

    def test_playback_progress_is_informational_and_identity_scoped(self):
        events = []
        session, unused_timeline, stream, unused_scene = self._session(events)
        self._captured_session_into(session)

        session._replies.put({
            "type": "cache_progress", "play_id": 99, "applied": 99})
        session.tick()
        self.assertNotIn("replay_progress", [event.kind for event in events])
        self.assertEqual("replaying", session.phase)

        session._replies.put({
            "type": "cache_progress", "play_id": 1, "applied": 2})
        session.tick()
        progress_events = [
            event for event in events if event.kind == "replay_progress"]
        self.assertEqual(1, len(progress_events))
        self.assertEqual(2, progress_events[0].current_frame)
        self.assertEqual("replaying", session.phase)
        session.close()

    def test_entering_cached_mode_establishes_unreal_ownership_first(self):
        session, unused_timeline, stream, unused_scene = self._session()
        session.enter_cached_mode()
        self.assertEqual("cache_enter", stream.submitted[-1]["type"])
        session.close()



class ControllerLifecycleTests(unittest.TestCase):
    def test_main_window_always_fits_its_controls(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.currentUnit.return_value = "film"
        fake_cmds.control.return_value = True

        controller = MODULE._Controller()
        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "om", mock.MagicMock()):
            controller.build_ui()

        create_call = fake_cmds.window.call_args_list[0]
        self.assertEqual(MODULE.WINDOW_NAME, create_call[0][0])
        self.assertEqual(430, create_call[1]["width"])
        self.assertTrue(create_call[1]["resizeToFitChildren"])
        self.assertNotIn("height", create_call[1])
        self.assertNotIn("widthHeight", create_call[1])

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


    def test_main_window_offers_cached_playback_controls(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.currentUnit.return_value = "film"
        fake_cmds.control.return_value = True
        stored = {"value": MODULE.DEFAULT_PLAYBACK_CAP}

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

        button_labels = [call[1]["label"] for call in fake_cmds.button.call_args_list
                         if "label" in call[1]]
        for label in ("动画", "模型", "实时预览", "缓存播放", "捕获并回放",
                      "再次回放", "停止回放", "取消捕获"):
            self.assertIn(label, button_labels)

    def test_cached_mode_status_identifies_cached_unreal_state(self):
        class ReadySession(object):
            is_ready = True

        cached = mock.Mock()
        cached.is_capturing = False
        controller = MODULE._Controller()
        controller._session = ReadySession()
        controller._cached_playback = cached
        controller._ensure_cached_playback = mock.Mock(return_value=cached)
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()

        controller._on_mode_changed(MODULE.CACHED_MODE)

        status = controller._set_connected.call_args[0][1]
        self.assertIn("实时采样已暂停", status)
        self.assertIn("回放时显示缓存", status)

    def test_disconnecting_discards_cached_state_before_stopping_session(self):
        controller = MODULE._Controller()
        controller._session = mock.Mock()
        controller._discard_cached_playback = mock.Mock()
        controller._set_connected = mock.Mock()

        controller.disconnect()

        controller._discard_cached_playback.assert_called_once_with()
        controller._session.stop.assert_called_once_with()

    def test_stale_cached_session_event_does_not_change_current_controller_state(self):
        controller = MODULE._Controller()
        current_session = object()
        controller._cached_playback = current_session
        controller._set_connected = mock.Mock()
        controller._set_text = mock.Mock()
        controller._update_mode_controls = mock.Mock()

        controller._on_cached_playback_event(
            object(), MODULE._CachedPlaybackSessionEvent("replay_completed"))

        self.assertIs(current_session, controller._cached_playback)
        controller._set_connected.assert_not_called()
        controller._set_text.assert_not_called()

    def test_transport_failure_is_forwarded_to_cached_session_before_detach(self):
        controller = MODULE._Controller()
        session = object()
        cached = mock.Mock()
        cached.cache = object()
        controller._session = session
        controller._cached_playback = cached
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()
        diagnostic = MODULE.make_diagnostic("STREAM_INTERRUPTED")

        controller._on_streaming_session_event(
            session, MODULE._StreamingSessionEvent("failed", diagnostic=diagnostic))

        cached.on_transport_failure.assert_called_once_with(diagnostic)
        cached.stop_replay.assert_not_called()
        self.assertIsNone(controller._session)
        self.assertIs(cached.cache, controller._cached_cache)

    def test_deleted_cached_playback_clears_controller_cache_state(self):
        class EmptyCachedPlayback(object):
            cache = None
            is_capturing = False

        fake_cmds = mock.MagicMock()
        fake_cmds.control.return_value = True
        controller = MODULE._Controller()
        controller._cached_playback = EmptyCachedPlayback()
        controller._cached_cache = object()
        controller._cache_text = "cacheText"
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller._on_cached_playback_event(
                MODULE._CachedPlaybackSessionEvent("capture_cancelled"))

        self.assertIsNone(controller._cached_cache)
        self.assertIn("缓存：无", [call[1]["label"]
                                  for call in fake_cmds.text.call_args_list])

    def test_switching_cached_mode_pauses_and_resumes_the_same_connection(self):
        class ReadySession(object):
            is_ready = True

        cached = mock.Mock()
        cached.is_capturing = False
        controller = MODULE._Controller()
        controller._session = ReadySession()
        controller._cached_playback = cached
        controller._ensure_cached_playback = mock.Mock(return_value=cached)
        controller._set_connected = mock.Mock()
        controller._show_error = mock.Mock()

        controller._on_mode_changed(MODULE.CACHED_MODE)
        self.assertEqual(MODULE.CACHED_MODE, controller._mode)
        cached.enter_cached_mode.assert_called_once_with()

        controller._on_mode_changed(MODULE.REALTIME_MODE)
        self.assertEqual(MODULE.REALTIME_MODE, controller._mode)
        cached.leave_cached_mode.assert_called_once_with()

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

    def _toggle_buttons(self, fake_cmds):
        toggle_labels = ("动画", "模型", "实时预览", "缓存播放")
        return [(call[1]["label"],
                 call[1].get("backgroundColor") == MODULE.TOGGLE_ON_BACKGROUND)
                for call in fake_cmds.button.call_args_list
                if call[1].get("label") in toggle_labels]

    def test_startup_defaults_to_animation_workflow(self):
        fake_cmds = self._fake_ui_cmds()
        controller = MODULE._Controller()

        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "om", mock.MagicMock()):
            controller.build_ui()

        self.assertEqual(MODULE.WORKFLOW_ANIMATION, controller._workflow)
        self.assertEqual(
            [("动画", True), ("模型", False),
             ("实时预览", True), ("缓存播放", False)],
            self._toggle_buttons(fake_cmds))

    def test_build_ui_offers_blendshapes_toggle_defaulting_on(self):
        fake_cmds = self._fake_ui_cmds()
        controller = MODULE._Controller()

        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "om", mock.MagicMock()):
            controller.build_ui()

        checkbox_labels = [call[1]["label"]
                           for call in fake_cmds.checkBox.call_args_list]
        self.assertIn("传递 BS", checkbox_labels)
        self.assertIn("连接成功后弹出差异警告", checkbox_labels)
        self.assertTrue(controller._blendshapes_enabled)

    def test_workflow_and_mode_toggles_swap_background_colors_on_selection(self):
        fake_cmds = self._fake_ui_cmds()
        controller = MODULE._Controller()

        def last_background(control):
            edits = [call[1]["backgroundColor"]
                     for call in fake_cmds.button.call_args_list
                     if call[0][:1] == (control,) and "backgroundColor" in call[1]]
            return edits[-1] if edits else None

        with mock.patch.object(MODULE, "cmds", fake_cmds), \
                mock.patch.object(MODULE, "om", mock.MagicMock()):
            controller.build_ui()
            self.assertEqual(MODULE.TOGGLE_ON_BACKGROUND,
                             last_background(controller._animation_workflow_button))
            self.assertEqual(MODULE.TOGGLE_OFF_BACKGROUND,
                             last_background(controller._model_workflow_button))

            controller._session = mock.Mock()
            controller._set_connected = mock.Mock()
            controller._show_error = mock.Mock()
            controller._ensure_cached_playback = mock.Mock(
                return_value=mock.Mock())
            controller._on_mode_changed(MODULE.CACHED_MODE)
            self.assertEqual(MODULE.TOGGLE_OFF_BACKGROUND,
                             last_background(controller._realtime_mode_button))
            self.assertEqual(MODULE.TOGGLE_ON_BACKGROUND,
                             last_background(controller._cached_mode_button))
            controller._on_workflow_changed(MODULE.WORKFLOW_MODEL)

        self.assertEqual(
            MODULE.TOGGLE_OFF_BACKGROUND,
            last_background(controller._animation_workflow_button))
        self.assertEqual(
            MODULE.TOGGLE_ON_BACKGROUND,
            last_background(controller._model_workflow_button))
        self.assertEqual(MODULE.TOGGLE_ON_BACKGROUND,
                         last_background(controller._realtime_mode_button))
        self.assertEqual(MODULE.TOGGLE_OFF_BACKGROUND,
                         last_background(controller._cached_mode_button))

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
            self.assertTrue(state[controller._capture_button])
            self.assertTrue(state[controller._realtime_mode_button])
            self.assertFalse(state[controller._bs_checkbox])
        self.assertFalse(model_visibility[controller._capture_button])
        self.assertFalse(model_visibility[controller._replay_button])
        self.assertFalse(model_visibility[controller._stop_replay_button])
        self.assertFalse(model_visibility[controller._cancel_capture_button])
        self.assertFalse(model_visibility[controller._cache_text])
        self.assertFalse(model_visibility[controller._realtime_mode_button])
        self.assertFalse(model_visibility[controller._cached_mode_button])
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
        cached.close.assert_called_once_with(delete_cache=True)
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

    def test_streaming_session_init_carries_protocol_v6_workflow_fields(self):
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
        self.assertEqual(6, init_message["version"])
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
        if operation in ("ready", "error", "cache_ready", "cache_complete",
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

    def test_protocol_v6_init_and_structured_diagnostics(self):
        identity = [0, 0, 0, 0, 0, 0, 1, 1, 1, 1]
        message = MODULE.make_init_message([["root", -1, identity]], ["Smile"], 7)
        self.assertEqual(6, message["version"])
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


if __name__ == "__main__":
    unittest.main()
