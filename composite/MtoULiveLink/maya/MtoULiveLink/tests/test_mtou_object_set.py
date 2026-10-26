# ruff: noqa: E402
"""Object-set tests: a primary character and one independent object (no Maya host).

Covers the v11 set negotiation of the sender worker, the realtime frame of a
two-object set, the in-connection removal of the second object, the linkage
group of the whole set and the panel's pairing window and set limits.
"""

import contextlib
import pathlib
import select
import sys
import types
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from mtou_livelink import config as mtou_config
from mtou_livelink import controller as mtou_controller
from mtou_livelink import link_host as mtou_link_host
from mtou_livelink import linkage as mtou_linkage
from mtou_livelink import mayaapi as mtou_mayaapi
from mtou_livelink import protocol as mtou_protocol
from mtou_livelink import scene as mtou_scene
from mtou_livelink import session as mtou_session
from mtou_livelink.ui import objects_window as mtou_objects_window

CHARACTER = mtou_config.CHARACTER_OBJECT_ID
SECONDARY = mtou_config.SECONDARY_OBJECT_ID
IDENTITY = [0, 0, 0, 0, 0, 0, 1, 1, 1, 1]
BIND = (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0)


def snapshot(revision, root, bones, curves, outfit="Clothes01"):
    return mtou_scene.CharacterSnapshot(
        revision, root, outfit, bones, curves, (), (), [BIND for unused in bones], 0)


class FakeScene(object):
    """One sampleable object scene with a fixed snapshot."""

    def __init__(self, root, bones, curves, value=0.0, outfit="Clothes01"):
        self._snapshot = snapshot(1, root, bones, curves, outfit)
        self.value = value
        self.revision = 1
        self.closed = False
        self.samples = 0

    def snapshot(self):
        return self._snapshot

    def sample(self):
        self.samples += 1
        transforms = tuple(
            (self.value, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0)
            for unused in self._snapshot.bones)
        curves = tuple(self.value for unused in self._snapshot.curve_names)
        return mtou_scene.CharacterFrame(self.revision, transforms, curves)

    def close(self):
        self.closed = True


def primary_scene():
    return FakeScene("|Hero|root", [("root", -1), ("spine", 0)], ["Smile"], value=1.0)


def secondary_scene():
    # Same bone and Morph names as the primary: they must never cross objects.
    return FakeScene("|Sword|root", [("root", -1), ("blade", 0)], ["Smile"], value=2.0,
                     outfit="")


def ready(object_id, revision=1, set_revision=1, session_id=4242, **extra):
    reply = {
        "type": "ready", "revision": revision,
        "object_id": object_id, "set_revision": set_revision, "space_revision": 1,
        "session_id": session_id,
        "missing_in_unreal": [], "missing_in_maya": [], "bone_name_remaps": [],
        "workflow": "animation", "target_morph_count": 1, "accepted_morph_count": 1,
    }
    reply.update(extra)
    return reply


GREETING = {
    "type": "hello_ack", "version": mtou_config.PROTOCOL_VERSION, "session_id": 4242,
    "capabilities": dict(mtou_protocol.DEFAULT_CAPABILITIES),
    "targets": [
        {"object_id": CHARACTER, "kind": "binding_actor",
         "binding_path": "/Game/Level.Level:PersistentLevel.Hero", "generation": "1"},
        {"object_id": SECONDARY, "kind": "binding_actor",
         "binding_path": "/Game/Level.Level:PersistentLevel.Sword", "generation": "2"},
    ],
    "space": {"space_revision": 1,
              "anchor": {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1],
                         "scale": [1, 1, 1]}},
}


def run_worker(init_messages, replies):
    """Run one sender worker against scripted Unreal replies; return its status."""
    worker = mtou_session.SenderWorker(
        init_messages, mtou_protocol.make_hello_message("2024", "cm", 1.0, 24.0))
    stream = b"".join(mtou_protocol.encode_message(reply) for reply in replies)

    class HandshakeSocket(object):
        def __init__(self):
            self.data = stream

        def settimeout(self, timeout):
            pass

        def recv(self, size):
            chunk, self.data = self.data[:size], self.data[size:]
            if not chunk:
                raise EOFError("closed")
            return chunk

        def sendall(self, packet):
            pass

        def close(self):
            pass

    observed = []

    def selectable(unused_r, unused_w, unused_x, unused_timeout):
        if not observed:
            observed.append(worker.status())
        worker.stop()
        return ([], [], [])

    with mock.patch.object(worker, "_connect", return_value=HandshakeSocket()), \
            mock.patch.object(worker, "_send_initial", return_value=True), \
            mock.patch.object(select, "select", side_effect=selectable):
        worker.run()
    return worker, (observed[0] if observed else worker.status())


def init_pair():
    pair = [CHARACTER, SECONDARY]
    return [
        mtou_protocol.make_init_message(
            [["root", -1, IDENTITY], ["spine", 0, IDENTITY]], ["Smile"], 1,
            object_id=CHARACTER, set_objects=pair),
        mtou_protocol.make_init_message(
            [["root", -1, IDENTITY], ["blade", 0, IDENTITY]], ["Smile"], 1,
            object_id=SECONDARY, set_objects=pair),
    ]


class ProtocolShapeTests(unittest.TestCase):
    def test_set_objects_start_with_the_primary_and_hold_at_most_two(self):
        self.assertEqual([CHARACTER, SECONDARY],
                         mtou_protocol.validate_set_objects([CHARACTER, SECONDARY]))
        for bad, keyword in (([SECONDARY, CHARACTER], "starts with"),
                             ([CHARACTER, CHARACTER], "duplicate"),
                             ([CHARACTER, SECONDARY, "third"], "max_set_objects"),
                             ([CHARACTER, "prop"], "unknown")):
            with self.subTest(bad=bad):
                with self.assertRaisesRegex(ValueError, keyword):
                    mtou_protocol.validate_set_objects(bad)

    def test_model_init_keeps_the_single_character_set(self):
        with self.assertRaisesRegex(ValueError, "Model"):
            mtou_protocol.make_init_message(
                [["root", -1, IDENTITY]], [], 1, mtou_config.WORKFLOW_MODEL,
                object_id=CHARACTER, set_objects=[CHARACTER, SECONDARY])

    def test_two_object_frame_carries_the_second_object_beside_the_primary(self):
        message = mtou_protocol.make_frame_message(
            [IDENTITY], [0.5],
            [{"object_id": SECONDARY, "transforms": [IDENTITY], "curves": [0.25]}])
        self.assertEqual([IDENTITY], message["transforms"])
        self.assertEqual(SECONDARY, message["objects"][0]["object_id"])
        with self.assertRaisesRegex(ValueError, "beside the primary"):
            mtou_protocol.make_frame_message(
                [IDENTITY], [0.5],
                [{"object_id": CHARACTER, "transforms": [IDENTITY], "curves": [0.5]}])
        # A one-object set keeps the v9 frame shape.
        self.assertNotIn("objects", mtou_protocol.make_frame_message([IDENTITY], [0.5]))

    def test_group_refuses_a_duplicated_object(self):
        row = {"object_id": CHARACTER, "transforms": [IDENTITY], "curves": [0.0]}
        with self.assertRaisesRegex(ValueError, "duplicate"):
            mtou_protocol.make_group_message(
                1, 1, 1, 1, "eval", 1, 1, [row, dict(row)], {}, {})

    def test_only_the_second_object_can_be_removed_inside_a_connection(self):
        message = mtou_protocol.make_remove_object_message(4242, SECONDARY, 2, "done")
        self.assertEqual({"type": "remove_object", "session_id": 4242,
                          "object_id": SECONDARY, "set_revision": 2,
                          "detail": "done"}, message)
        with self.assertRaisesRegex(ValueError, "character"):
            mtou_protocol.make_remove_object_message(4242, CHARACTER, 2)


class SetNegotiationTests(unittest.TestCase):
    def test_the_connection_streams_only_after_every_object_is_ready(self):
        worker, status = run_worker(
            init_pair(), [GREETING, ready(CHARACTER), ready(SECONDARY)])
        self.assertEqual("ready", status[0])
        self.assertEqual(2, len(worker.hello_reply()["targets"]))

    def test_a_refused_second_object_ends_the_whole_set(self):
        error = {"type": "error", "code": "ANCHOR_MISMATCH",
                 "message": "The second target does not share the primary placement.",
                 "details": "Sword stands 120 cm away from Hero"}
        unused, status = run_worker(init_pair(), [GREETING, ready(CHARACTER), error])
        state, unused_detail, unused_warning, diagnostic = status
        self.assertEqual("error", state)
        self.assertEqual("ANCHOR_MISMATCH", diagnostic["code"])

    def test_each_ready_must_name_its_own_object(self):
        unused, status = run_worker(
            init_pair(), [GREETING, ready(CHARACTER), ready(CHARACTER)])
        state, unused_detail, unused_warning, diagnostic = status
        self.assertEqual("error", state)
        self.assertEqual("INVALID_MESSAGE", diagnostic["code"])
        self.assertIn("object_id", diagnostic["details"])

    def test_the_second_objects_differences_are_listed_apart(self):
        unused, status = run_worker(
            init_pair(),
            [GREETING, ready(CHARACTER, missing_in_maya=["Blink"]),
             ready(SECONDARY, missing_in_unreal=["Smile"])])
        warning = status[2]
        self.assertTrue(warning["has_warning"])
        self.assertEqual(["Blink"], warning["missing_in_maya"])
        self.assertEqual([mtou_session.SECONDARY_WARNING_PREFIX + "Smile"],
                         warning["missing_in_unreal"])


class _FakeOpenMaya(object):
    """The callback surface a streaming session registers, recorded."""

    def __init__(self):
        self.timers = []
        test = self

        class _SceneMessage(object):
            kBeforeNew, kBeforeOpen, kMayaExiting = 1, 2, 3

            @staticmethod
            def addCallback(message, callback):
                return 10 + message

        class _EventMessage(object):
            @staticmethod
            def addEventCallback(event, callback):
                return 30

        class _TimerMessage(object):
            @staticmethod
            def addTimerCallback(interval, callback):
                test.timers.append(callback)
                return 20 + len(test.timers)

        class _Message(object):
            @staticmethod
            def removeCallback(callback_id):
                return None

        self.MSceneMessage = _SceneMessage
        self.MEventMessage = _EventMessage
        self.MTimerMessage = _TimerMessage
        self.MMessage = _Message


class FakeWorker(object):
    """The sender worker surface a streaming session drives."""

    def __init__(self, init_messages, hello_message=None):
        self.init_messages = init_messages
        self.hello_message = hello_message
        self.frames = []
        self.controls = []
        self.state = "ready"

    def start(self):
        pass

    def status(self):
        return (self.state, "Connected", {"missing_in_unreal": [], "missing_in_maya": [],
                                          "bone_name_remaps": [], "has_warning": False},
                None)

    def submit(self, message):
        self.frames.append(message)

    def submit_control(self, message):
        self.controls.append(message)

    def hello_reply(self):
        return dict(GREETING)

    def stop(self):
        pass

    def join(self, timeout=None):
        pass

    def end_ordered(self, discard_pending=True):
        pass


class StreamingSetTests(unittest.TestCase):
    def setUp(self):
        self.fake_om = _FakeOpenMaya()
        self.workers = []
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.object(mtou_session, "om", self.fake_om))
        stack.enter_context(mock.patch.object(mtou_session, "cmds", mock.MagicMock()))
        stack.enter_context(mock.patch.object(
            mtou_session, "SenderWorker",
            side_effect=lambda *args, **kwargs: self.workers.append(
                FakeWorker(*args, **kwargs)) or self.workers[-1]))
        stack.enter_context(mock.patch.object(
            mtou_session.mayaapi, "run_deferred", lambda action, unused: action()))
        self.addCleanup(stack.close)
        self.primary = primary_scene()
        self.secondary = secondary_scene()

    def start(self, **kwargs):
        events = []
        session = mtou_session.StreamingSession.start(
            self.primary, 24.0, events.append, playback_state=lambda: False,
            secondary_scene=self.secondary, **kwargs)
        return session, self.workers[-1], events

    def test_one_init_per_object_with_the_same_set(self):
        session, worker, unused = self.start()
        self.assertEqual([CHARACTER, SECONDARY],
                         [message["object_id"] for message in worker.init_messages])
        for message in worker.init_messages:
            self.assertEqual([CHARACTER, SECONDARY], message["set_objects"])
            self.assertEqual(1, message["set_revision"])
        self.assertEqual((CHARACTER, SECONDARY), session.set_objects)
        self.assertEqual({CHARACTER: self.primary, SECONDARY: self.secondary},
                         session.object_scenes())

    def test_the_model_workflow_refuses_a_second_object(self):
        with self.assertRaises(mtou_session.StreamingSessionError) as caught:
            self.start(workflow=mtou_config.WORKFLOW_MODEL)
        self.assertEqual("OBJECT_SET_UNSUPPORTED", caught.exception.code)

    def test_realtime_frames_sample_both_objects_in_one_message(self):
        session, worker, events = self.start()
        session._sample_and_submit(force=True)
        self.assertEqual("ready", events[0].kind)
        frame = worker.frames[-1]
        self.assertEqual(1.0, frame["transforms"][0][0])
        self.assertEqual([0.0 + 1.0], frame["curves"])
        row = frame["objects"][0]
        self.assertEqual(SECONDARY, row["object_id"])
        # Each object streams its own values: no name crosses objects.
        self.assertEqual(2.0, row["transforms"][1][0])
        self.assertEqual([2.0], row["curves"])
        self.assertEqual(1, self.primary.samples)
        self.assertEqual(1, self.secondary.samples)

    def test_a_changed_second_object_ends_the_session(self):
        session, worker, events = self.start()
        self.secondary.revision = 2
        session._sample_and_submit(force=True)
        self.assertEqual("failed", events[-1].kind)
        self.assertIn("副对象", events[-1].diagnostic["message"])

    def test_removal_closes_the_old_set_until_unreal_confirms(self):
        session, worker, unused = self.start()
        session._sample_and_submit(force=True)
        revision = session.request_secondary_removal(4242, "pairing window")
        self.assertEqual(2, revision)
        self.assertEqual({"type": "remove_object", "session_id": 4242,
                          "object_id": SECONDARY, "set_revision": 2,
                          "detail": "pairing window"}, worker.controls[-1])
        self.assertTrue(session.removal_pending)
        frames = len(worker.frames)
        session._sample_and_submit(force=True)
        self.assertEqual(frames, len(worker.frames), "no frame of the closed set")
        # A repeated request waits for the same confirmation.
        self.assertEqual(2, session.request_secondary_removal(4242))
        self.assertEqual(1, len(worker.controls))

        removed = session.adopt_removal({
            "type": "object_removed", "session_id": 4242, "object_id": SECONDARY,
            "set_revision": 2, "restored": True, "detail": "restored"})
        self.assertIs(self.secondary, removed)
        self.assertFalse(session.removal_pending)
        self.assertEqual(2, session.set_revision)
        self.assertEqual((CHARACTER,), session.set_objects)
        # The primary continues at once, in the one-object frame shape.
        self.assertNotIn("objects", worker.frames[-1])
        self.assertEqual(frames + 1, len(worker.frames))

    def test_an_unreal_started_removal_is_adopted_too(self):
        session, worker, unused = self.start()
        removed = session.adopt_removal({
            "type": "object_removed", "session_id": 4242, "object_id": SECONDARY,
            "set_revision": 2, "restored": True, "detail": "removed in Unreal"})
        self.assertIs(self.secondary, removed)
        self.assertEqual((CHARACTER,), session.set_objects)
        # A repeated confirmation of the set this session already has is noted.
        self.assertIsNone(session.adopt_removal({
            "type": "object_removed", "session_id": 4242, "object_id": SECONDARY,
            "set_revision": 2, "restored": True, "detail": ""}))
        with self.assertRaises(mtou_session.StreamingSessionError):
            session.adopt_removal({"type": "object_removed", "object_id": CHARACTER,
                                   "set_revision": 3})


class SetHost(object):
    """A linkage host whose sample reports which objects it read."""

    def __init__(self):
        self.sampled = []
        self.on_sample = None

    def capture_state(self):
        return {"time": 1.0}

    def restore_state(self, state):
        return []

    def apply_target(self, time_payload, camera, gate):
        return ({"maya_frame": 1.0, "ue_display_frame": time_payload["display_frame"]},
                {"camera": "readback"})

    def sample_group(self, object_ids):
        self.sampled.append(list(object_ids))
        if self.on_sample is not None:
            self.on_sample()
        return [{"object_id": object_id, "transforms": [IDENTITY], "curves": []}
                for object_id in object_ids]


class SetLinkageSession(object):
    def __init__(self):
        self.messages = []

    def submit_linkage(self, message):
        self.messages.append(message)


def camera_time(eval_id="seq@1/cam#a", eval_serial=1, set_revision=1, mode_revision=1):
    return {
        "type": "camera_time", "session_id": 4242, "set_revision": set_revision,
        "mode_revision": mode_revision, "space_revision": 1, "eval_id": eval_id,
        "eval_serial": eval_serial, "transport_serial": eval_serial,
        "frame_serial": eval_serial,
        "time": {"display_frame": 10.0, "source_frame": 10, "seconds": 0.0, "tick": 0,
                 "display_rate": {"numerator": 24, "denominator": 1},
                 "tick_resolution": {"numerator": 24000, "denominator": 1},
                 "playback_range": {"start": 0, "end": 100}},
        "camera": {"focal_length": 35.0}, "gate": {"width": 1920, "height": 1080},
    }


class LinkageSetTests(unittest.TestCase):
    def build(self):
        host = SetHost()
        stream = SetLinkageSession()
        linkage = mtou_linkage.LinkageSession(
            host, stream, object_ids=[CHARACTER, SECONDARY], clock=lambda: 10.0)
        linkage.set_session_id(4242)
        linkage.enter(1)
        return linkage, host, stream

    def groups(self, stream):
        return [message for message in stream.messages if message["type"] == "group"]

    def test_one_group_answers_a_target_for_the_whole_set(self):
        linkage, host, stream = self.build()
        linkage.offer_target(camera_time())
        self.assertEqual(1, linkage.pump())
        self.assertEqual([[CHARACTER, SECONDARY]], host.sampled)
        group = self.groups(stream)[-1]
        self.assertEqual([CHARACTER, SECONDARY],
                         [row["object_id"] for row in group["objects"]])
        self.assertEqual(1, group["set_revision"])

    def test_no_group_is_sent_while_the_second_object_is_removed(self):
        linkage, host, stream = self.build()
        linkage.begin_set_removal()
        linkage.offer_target(camera_time())
        self.assertEqual(0, linkage.pump())
        self.assertEqual([], host.sampled)
        self.assertTrue(linkage.status().removing)
        # Unreal confirms the new set and issues the target again.
        self.assertTrue(linkage.adopt_set(2, [CHARACTER]))
        self.assertFalse(linkage.status().removing)
        linkage.offer_target(camera_time(eval_serial=2, set_revision=2))
        self.assertEqual(1, linkage.pump())
        group = self.groups(stream)[-1]
        self.assertEqual(2, group["set_revision"])
        self.assertEqual([CHARACTER], [row["object_id"] for row in group["objects"]])

    def test_a_set_change_while_sampling_answers_the_target_with_the_new_set(self):
        linkage, host, stream = self.build()
        linkage.offer_target(camera_time())
        host.on_sample = lambda: (linkage.begin_set_removal(),
                                  linkage.adopt_set(2, [CHARACTER]))
        self.assertEqual(0, linkage.pump())
        self.assertEqual([], self.groups(stream))
        host.on_sample = None
        self.assertEqual(1, linkage.pump())
        self.assertEqual([CHARACTER], [row["object_id"]
                                       for row in self.groups(stream)[-1]["objects"]])

    def test_adopting_the_new_set_releases_the_answer_of_the_old_set(self):
        linkage, host, stream = self.build()
        linkage.offer_target(camera_time())
        linkage.pump()
        self.assertEqual(1, linkage.status().issued)
        linkage.adopt_set(2, [CHARACTER])
        self.assertEqual(0, linkage.status().issued)
        self.assertEqual((CHARACTER,), linkage.object_ids())
        # An older confirmation never rolls the set back.
        self.assertFalse(linkage.adopt_set(1, [CHARACTER, SECONDARY]))

    def test_a_target_of_the_closed_set_is_stale_noise(self):
        linkage, host, stream = self.build()
        linkage.adopt_set(2, [CHARACTER])
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            linkage.offer_target(camera_time(set_revision=1))
        self.assertEqual("SET_REVISION_STALE", caught.exception.code)
        self.assertTrue(caught.exception.stale)
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            linkage.offer_target(camera_time(set_revision=3))
        self.assertFalse(caught.exception.stale)


class HostSamplingTests(unittest.TestCase):
    def test_each_object_is_read_from_its_own_scene(self):
        primary = primary_scene()
        secondary = secondary_scene()
        host = mtou_link_host.MayaLinkageHost(
            primary, [CHARACTER, SECONDARY], 24.0,
            scenes={CHARACTER: primary, SECONDARY: secondary})
        rows = host.sample_group([CHARACTER, SECONDARY])
        self.assertEqual([CHARACTER, SECONDARY], [row["object_id"] for row in rows])
        self.assertEqual(1.0, rows[0]["transforms"][0][0])
        self.assertEqual(2.0, rows[1]["transforms"][0][0])
        with self.assertRaisesRegex(ValueError, "prop"):
            host.sample_group(["prop"])

    def test_the_primary_scene_is_the_default(self):
        primary = primary_scene()
        host = mtou_link_host.MayaLinkageHost(primary, [CHARACTER], 24.0)
        self.assertEqual(CHARACTER, host.sample_group([CHARACTER])[0]["object_id"])


class PanelSession(object):
    """The streaming session surface the panel's object actions use."""

    def __init__(self, secondary=None):
        self.secondary_scene = secondary
        self.set_objects = (CHARACTER, SECONDARY) if secondary else (CHARACTER,)
        self.set_revision = 1
        self.removal_pending = False
        self.requests = []
        self.adopted = []
        self.stopped = False
        self.hello_reply = dict(GREETING)

    def is_ready(self):
        return True

    def request_secondary_removal(self, session_id, detail=""):
        self.requests.append((session_id, detail))
        self.removal_pending = True
        return 2

    def adopt_removal(self, message):
        self.adopted.append(message)
        removed, self.secondary_scene = self.secondary_scene, None
        self.removal_pending = False
        self.set_revision = message["set_revision"]
        self.set_objects = (CHARACTER,)
        return removed

    def stop(self):
        self.stopped = True

    def set_cached_reply_listener(self, listener):
        pass


class PanelObjectTests(unittest.TestCase):
    def setUp(self):
        self.cmds = mock.MagicMock()
        self.cmds.control.return_value = True
        self.cmds.currentUnit.return_value = "film"
        windows = set()

        def window(*args, **kwargs):
            name = args[0] if args else None
            if kwargs.get("exists"):
                return name in windows
            windows.add(name)
            return name

        self.cmds.window.side_effect = window
        self.cmds.deleteUI.side_effect = lambda name, *a, **k: windows.discard(name)
        stack = contextlib.ExitStack()
        for module in (mtou_controller, mtou_mayaapi, mtou_scene):
            if hasattr(module, "cmds"):
                stack.enter_context(mock.patch.object(module, "cmds", self.cmds))
        stack.enter_context(mock.patch.object(mtou_controller, "require_maya", lambda: None))
        stack.enter_context(mock.patch.object(
            mtou_objects_window, "apply_panel_theme", lambda *a, **k: ([], {})))
        self.deferred = []
        stack.enter_context(mock.patch.object(
            mtou_controller.Controller, "_defer_ui",
            lambda controller, action: self.deferred.append(action)))
        self.errors = []
        stack.enter_context(mock.patch.object(
            mtou_controller.Controller, "_show_error",
            lambda controller, diagnostic, session=False: self.errors.append(diagnostic)))
        stack.enter_context(mock.patch.object(
            mtou_controller.Controller, "_on_cached_playback_view", lambda *a: None))
        self.addCleanup(stack.close)
        self.controller = mtou_controller.Controller()
        self.controller._scene = primary_scene()

    def flush(self):
        while self.deferred:
            self.deferred.pop(0)()

    def select(self, *paths, types_by_path=None):
        types_by_path = types_by_path or {}
        self.cmds.ls.return_value = list(paths)
        self.cmds.nodeType.side_effect = lambda path: types_by_path.get(path, "joint")

    def test_the_second_object_must_not_overlap_the_primary(self):
        self.select("|Hero|root|spine")
        self.controller.set_secondary_object()
        self.assertIsNone(self.controller._secondary_scene)
        self.assertEqual("ROLE_SETUP_FAILED", self.errors[-1]["code"])
        self.assertTrue(mtou_controller.roots_overlap("|A|root", "|A|root|b"))
        self.assertFalse(mtou_controller.roots_overlap("|A|root", "|A|root2"))

    def test_setting_the_second_object_while_connected_ends_the_connection(self):
        session = PanelSession()
        self.controller._session = session
        self.select("|Sword|root")
        captured = secondary_scene()
        with mock.patch.object(mtou_controller.CharacterScene, "capture",
                               return_value=captured) as capture:
            self.controller.set_secondary_object()
        self.assertTrue(session.stopped)
        self.assertIs(captured, self.controller._secondary_scene)
        self.assertTrue(capture.call_args[1]["display_optional"])

    def test_connecting_the_model_workflow_with_a_second_object_is_refused(self):
        self.controller._secondary_scene = secondary_scene()
        self.controller._workflow = mtou_config.WORKFLOW_MODEL
        with mock.patch.object(mtou_controller.StreamingSession, "start") as start:
            self.controller.connect()
        start.assert_not_called()
        self.assertEqual("OBJECT_SET_UNSUPPORTED", self.errors[-1]["code"])

    def test_cached_playback_is_unavailable_for_a_two_object_set(self):
        self.controller._session = PanelSession(secondary_scene())
        self.controller._on_mode_changed(mtou_config.CACHED_MODE)
        self.assertEqual(mtou_config.REALTIME_MODE, self.controller._mode)
        self.assertEqual("OBJECT_SET_UNSUPPORTED", self.errors[-1]["code"])

    def test_removing_while_connected_asks_unreal_and_adopts_its_answer(self):
        secondary = secondary_scene()
        session = PanelSession(secondary)
        self.controller._session = session
        self.controller._secondary_scene = secondary
        linkage = mock.Mock()
        linkage.recovery_records.return_value = ()
        linkage.status.return_value = None
        linkage.convergence_overdue.return_value = False
        linkage.takeover_held.return_value = False
        self.controller._linkage = linkage
        self.controller.remove_secondary_object()
        self.assertEqual([(4242, "removed in the Maya pairing window")], session.requests)
        linkage.begin_set_removal.assert_called_once_with()
        self.assertEqual("正在移除副对象，等待 Unreal 确认",
                         self.controller.objects_state()["state"])
        self.assertFalse(self.controller.objects_window_controls()["remove"][0])

        reply = {"type": "object_removed", "session_id": 4242, "object_id": SECONDARY,
                 "set_revision": 2, "restored": True, "detail": "restored"}
        self.assertTrue(self.controller._on_runtime_reply(reply))
        # The linkage adopts the new set on the sender thread at once.
        linkage.adopt_set.assert_called_once_with(2, [CHARACTER])
        with mock.patch.object(mtou_controller, "CachedPlayback"):
            self.flush()
        self.assertEqual([reply], session.adopted)
        self.assertIsNone(self.controller._secondary_scene)
        self.assertTrue(secondary.closed)
        self.assertIn("已移除副对象", self.controller._object_set_detail)

    def test_removing_without_a_connection_only_forgets_the_object(self):
        secondary = secondary_scene()
        self.controller._secondary_scene = secondary
        self.controller.remove_secondary_object()
        self.assertIsNone(self.controller._secondary_scene)
        self.assertTrue(secondary.closed)

    def test_a_removal_that_could_not_restore_is_reported(self):
        secondary = secondary_scene()
        session = PanelSession(secondary)
        self.controller._session = session
        self.controller._secondary_scene = secondary
        self.controller._on_runtime_reply({
            "type": "object_removed", "session_id": 4242, "object_id": SECONDARY,
            "set_revision": 2, "restored": False,
            "detail": "the muted writer 'transform:Sword' no longer exists"})
        with mock.patch.object(mtou_controller, "CachedPlayback"):
            self.flush()
        self.assertIn("未能完整恢复", self.controller._object_set_detail)
        self.assertIn("transform:Sword", self.controller._object_set_detail)

    def test_the_pairing_window_is_reused_and_closing_it_keeps_the_set(self):
        first = self.controller.open_objects_window()
        second = self.controller.open_objects_window()
        self.assertIs(first, second)
        self.controller._secondary_scene = secondary_scene()
        self.controller.close_objects_window()
        self.assertIsNone(self.controller._objects_window)
        self.assertIsNotNone(self.controller._secondary_scene)
        third = self.controller.open_objects_window()
        self.assertIsNot(first, third)
        # Maya closing the window itself only forgets it.
        third._on_closed()
        self.assertIsNone(self.controller._objects_window)

    def test_diagnostics_name_the_set_without_scene_paths(self):
        self.controller._secondary_scene = secondary_scene()
        self.controller._session = PanelSession(self.controller._secondary_scene)
        text = self.controller._objects_diagnostic_text()
        self.assertIn("character、secondary", text)
        self.assertIn("set：1", text)
        self.assertNotIn("|Sword|root", text)

    def test_the_objects_state_tells_what_the_set_does(self):
        self.assertEqual("只连接主角色", self.controller.objects_state()["state"])
        self.controller._secondary_scene = secondary_scene()
        state = self.controller.objects_state()
        self.assertEqual("|Sword|root", state["secondary"])
        self.assertIn("无 Display 控制器", state["secondary_detail"])
        self.assertIn("连接后", state["state"])
        self.controller._workflow = mtou_config.WORKFLOW_MODEL
        self.assertIn("模型工作流只支持单个角色",
                      self.controller.objects_state()["state"])


class SceneDisplayOptionalTests(unittest.TestCase):
    def test_a_root_without_display_resolves_without_an_outfit(self):
        with mock.patch.object(mtou_scene, "display_candidates", return_value=[]):
            self.assertEqual(
                (None, None),
                mtou_scene.CharacterScene._resolve_display("|Sword|root", None, None,
                                                           display_optional=True))
            with self.assertRaises(mtou_scene.CharacterSceneError) as caught:
                mtou_scene.CharacterScene._resolve_display("|Hero|root", None, None)
        self.assertEqual("AMBIGUOUS_DISPLAY", caught.exception.code)


if __name__ == "__main__":
    unittest.main()
