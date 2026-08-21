import errno
import importlib.util
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
    (pathlib.Path(__file__).resolve().parents[3] / "protocol" / "conformance-v3.json")
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
                    "type": "ready", "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": []
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

    def test_stop_during_connection_prevents_init_and_terminates(self):
        entered = threading.Event()
        released = threading.Event()

        class BlockingSocket(object):
            def __init__(self):
                self.sent = []
                self._reply = MODULE.encode_message({
                    "type": "ready", "missing_in_unreal": [], "missing_in_maya": [],
                    "bone_name_remaps": []})

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
                    "bone_name_remaps": []})

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
                    "bone_name_remaps": []})
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


class ControllerLifecycleTests(unittest.TestCase):
    def test_main_window_always_fits_its_controls(self):
        fake_cmds = mock.MagicMock()
        fake_cmds.currentUnit.return_value = "film"
        fake_cmds.control.return_value = True

        controller = MODULE._Controller()
        with mock.patch.object(MODULE, "cmds", fake_cmds):
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
        with mock.patch.object(MODULE, "cmds", fake_cmds):
            controller.build_ui()

        labels = [call.kwargs["label"] for call in fake_cmds.menuItem.call_args_list]
        self.assertEqual(list(MODULE.PLAYBACK_CAP_CHOICES), labels)
        self.assertEqual("15 fps", controller._playback_cap)
        self.assertTrue(any(
            call.kwargs.get("edit") and call.kwargs.get("value") == "15 fps"
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
            actual = MODULE.make_init_message(payload["bones"], payload["curves"])
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
        if operation in ("ready", "error"):
            if expected["accepted"]:
                self.assertEqual(case["payload"], MODULE.validate_reply(case["payload"]))
            else:
                with self.assertRaisesRegex(ValueError, "|".join(expected["keywords"])):
                    MODULE.validate_reply(case["payload"])
            return
        self.fail("Unsupported Maya conformance operation: " + operation)

    def test_protocol_v3_init_and_structured_diagnostics(self):
        identity = [0, 0, 0, 0, 0, 0, 1, 1, 1, 1]
        message = MODULE.make_init_message([["root", -1, identity]], ["Smile"])
        self.assertEqual(3, message["version"])
        diagnostic = MODULE.make_diagnostic(
            "SKELETON_MISMATCH", "Skeleton differs", details="details"
        )
        self.assertEqual("SKELETON_MISMATCH", diagnostic["code"])
        self.assertEqual("骨架与 Unreal Skeletal Mesh 不匹配", diagnostic["summary"])
        self.assertIn("UE", diagnostic["solution"])

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
        packet = MODULE.encode_message(MODULE.make_init_message(bones, []))
        self.assertEqual(len(packet) - 8, struct.unpack(">Q", packet[:8])[0])
        self.assertIn(b"bone_700", packet)

    def test_non_finite_frame_is_rejected(self):
        for value in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "finite"):
                    MODULE.make_frame_message([[0, 0, 0, 0, 0, 0, 1, 1, 1, value]], [])


if __name__ == "__main__":
    unittest.main()
