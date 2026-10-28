# ruff: noqa: E402
"""Linkage state machine, scheduling and rejection tests (no Maya host)."""

import pathlib
import sys
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from mtou_livelink import config as mtou_config  # noqa: E402
from mtou_livelink import linkage as mtou_linkage  # noqa: E402
from mtou_livelink import protocol as mtou_protocol  # noqa: E402


def camera_time(eval_id="seq@10+0/cam#c1", eval_serial=1, transport_serial=1,
                frame_serial=1, display_frame=10.0, session_id=5, mode_revision=1,
                set_revision=1, space_revision=1):
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
                    mode_revision=1, session_id=5, reason=""):
    return {
        "type": "applied",
        "session_id": session_id,
        "mode_revision": mode_revision,
        "eval_id": eval_id,
        "sample_serial": sample_serial,
        "applied": applied,
        "applied_display_frame": 10.0,
        "lag_seconds": 0.02,
        "components": [{"object_id": "character", "bones": 3, "morphs": 1, "parts": 0}],
        "reason": reason,
    }


IDENTITY_ANCHOR = {"location_cm": [0.0, 0.0, 0.0],
                   "rotation_deg": {"roll": 0.0, "pitch": 0.0, "yaw": 0.0}}


def mode_state(mode_revision, mode=mtou_config.LINKAGE_MODE, session_id=5, detail="",
               anchor=IDENTITY_ANCHOR):
    message = {
        "type": "mode_state", "session_id": session_id,
        "mode_revision": mode_revision, "mode": mode,
        "control": (mtou_config.CONTROL_UNREAL if mode == mtou_config.LINKAGE_MODE
                    else mtou_config.CONTROL_MAYA),
        "detail": detail,
    }
    if mode == mtou_config.LINKAGE_MODE:
        message["anchor"] = anchor
    return message


class FakeHost(object):
    def __init__(self):
        self.captured = 0
        self.restored = []
        self.applied = []
        self.sampled = 0
        self.restore_residual = ()
        self.apply_error = None
        self.stale = None
        self.anchors = []

    def stale_reference(self, anchor):
        self.anchors.append(anchor)
        return self.stale

    def capture_state(self):
        self.captured += 1
        return {"time": 42.0, "camera": "persp"}

    def restore_state(self, state):
        self.restored.append(state)
        return list(self.restore_residual)

    def apply_target(self, time_payload, camera, gate):
        if self.apply_error is not None:
            raise self.apply_error
        self.applied.append((time_payload, camera, gate))
        return ({"maya_frame": time_payload["display_frame"], "ue_display_frame":
                 time_payload["display_frame"]}, {"world_matrix": [1.0]})

    def sample_group(self, object_ids):
        self.sampled += 1
        return [{"object_id": object_ids[0], "transforms": [[0, 0, 0, 0, 0, 0, 1, 1, 1, 1]],
                 "curves": [0.25]}]


class FakeSession(object):
    def __init__(self):
        self.messages = []
        self.ready = True

    def submit_linkage(self, message):
        self.messages.append(message)
        return True

    def is_ready(self):
        return self.ready


def build(host=None, session=None, events=None):
    host = host or FakeHost()
    session = session or FakeSession()
    events = events if events is not None else []
    instance = mtou_linkage.LinkageSession(
        host, session, on_event=events.append, clock=lambda: 100.0)
    instance.set_session_id(5)
    return instance, host, session, events


class LinkageModeTests(unittest.TestCase):
    def test_enter_captures_state_and_announces_the_mode(self):
        instance, host, session, events = build()
        instance.enter(1)
        self.assertEqual(1, host.captured)
        self.assertTrue(instance.is_active)
        message = session.messages[0]
        self.assertEqual("mode", message["type"])
        self.assertEqual(mtou_config.LINKAGE_MODE, message["mode"])
        self.assertEqual(mtou_config.CONTROL_UNREAL, message["control"])
        self.assertEqual("ready", message["transition"])
        self.assertEqual(5, message["session_id"])
        self.assertEqual(["mode"], [event.kind for event in events])

    def test_enter_refuses_a_stale_or_repeated_revision(self):
        instance, _, _, _ = build()
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            instance.enter(0)
        self.assertEqual("INVALID_MESSAGE", caught.exception.code)
        instance.enter(2)
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            instance.enter(2)
        self.assertEqual("MODE_CONFLICT", caught.exception.code)
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            instance.enter(1)
        self.assertEqual("MODE_CONFLICT", caught.exception.code)

    def test_enter_failure_keeps_realtime_and_records_the_reason(self):
        class RefusingHost(FakeHost):
            def capture_state(self):
                raise RuntimeError("gate is locked")

        instance, _, session, events = build(host=RefusingHost())
        with self.assertRaises(RuntimeError):
            instance.enter(1)
        self.assertFalse(instance.is_active)
        self.assertEqual([], [message for message in session.messages
                              if message["type"] == "mode"])
        self.assertEqual(["capture"], [record.split(":")[0]
                                       for record in instance.recovery_records()])
        self.assertEqual("mode", events[-1].kind)
        self.assertEqual(mtou_config.REALTIME_MODE, events[-1].mode)

    def test_leave_restores_state_and_reports_the_exit(self):
        instance, host, session, events = build()
        instance.enter(1)
        residual = instance.leave("realtime", 2)
        self.assertEqual((), residual)
        self.assertEqual([{"time": 42.0, "camera": "persp"}], host.restored)
        self.assertFalse(instance.is_active)
        message = session.messages[-1]
        self.assertEqual("leave_linkage", message["type"])
        self.assertEqual("realtime", message["target"])
        self.assertEqual(2, message["mode_revision"])
        self.assertEqual(mtou_config.REALTIME_MODE, events[-1].mode)

    def test_leave_refuses_while_linkage_is_not_active(self):
        instance, _, _, _ = build()
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            instance.leave("end", 1)
        self.assertEqual("LINKAGE_NOT_ACTIVE", caught.exception.code)

    def test_restore_residuals_are_reported_and_kept(self):
        host = FakeHost()
        host.restore_residual = ("panel camera missing",)
        instance, _, session, _ = build(host=host)
        instance.enter(1)
        residual = instance.leave("end", 2)
        self.assertEqual(("panel camera missing",), residual)
        self.assertIn("residual", instance.recovery_records()[0])
        self.assertIn("panel camera missing", session.messages[-1]["detail"])


class LinkageTargetTests(unittest.TestCase):
    def setUp(self):
        self.instance, self.host, self.session, self.events = build()
        self.instance.enter(1)

    def test_target_is_refused_outside_linkage(self):
        instance, _, _, _ = build()
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            instance.offer_target(camera_time())
        self.assertEqual("LINKAGE_NOT_ACTIVE", caught.exception.code)

    def test_target_identity_mismatches_are_refused_with_their_codes(self):
        for field, value, code in (
                ("session_id", 9, "INVALID_MESSAGE"),
                ("mode_revision", 7, "MODE_CONFLICT"),
                ("set_revision", 4, "SET_REVISION_STALE"),
                ("space_revision", 4, "SPACE_REVISION_STALE")):
            message = camera_time()
            message[field] = value
            with self.assertRaises(mtou_linkage.LinkageError) as caught:
                self.instance.offer_target(message)
            self.assertEqual(code, caught.exception.code)

    def test_only_an_older_mode_revision_is_stale(self):
        current = self.instance.mode_revision()
        for revision, stale in ((current - 1, True), (current + 1, False)):
            message = camera_time()
            message["mode_revision"] = revision
            with self.assertRaises(mtou_linkage.LinkageError) as caught:
                self.instance.offer_target(message)
            self.assertEqual("MODE_CONFLICT", caught.exception.code)
            self.assertEqual(stale, caught.exception.stale)

    def test_malformed_targets_are_refused(self):
        broken = {
            "missing time": {"time": None},
            "non finite display frame": {"time": dict(camera_time()["time"],
                                                      display_frame=float("nan"))},
            "zero tick rate": {"time": dict(camera_time()["time"],
                                            tick_resolution={"numerator": 0,
                                                             "denominator": 1})},
            "missing camera": {"camera": None},
            "missing gate": {"gate": None},
            "empty eval id": {"eval_id": ""},
            "float serial": {"eval_serial": 1.5},
        }
        for label, patch in broken.items():
            message = camera_time()
            message.update(patch)
            with self.subTest(case=label):
                with self.assertRaises(mtou_linkage.LinkageError) as caught:
                    self.instance.offer_target(message)
                self.assertEqual("INVALID_MESSAGE", caught.exception.code)

    def test_pump_applies_samples_and_answers_with_one_group(self):
        self.instance.offer_target(camera_time())
        self.assertEqual(1, self.instance.pump())
        message = self.session.messages[-1]
        self.assertEqual("group", message["type"])
        self.assertEqual(5, message["session_id"])
        self.assertEqual(1, message["set_revision"])
        self.assertEqual(1, message["mode_revision"])
        self.assertEqual(1, message["space_revision"])
        self.assertEqual("seq@10+0/cam#c1", message["eval_id"])
        self.assertEqual(1, message["sample_serial"])
        self.assertEqual("character", message["objects"][0]["object_id"])
        self.assertEqual(1, self.host.sampled)
        self.assertEqual(1, len(self.host.applied))
        self.assertFalse(self.instance.settled())

    def test_second_sample_of_one_target_gets_a_new_serial(self):
        self.instance.offer_target(camera_time())
        self.instance.pump()
        self.instance.offer_target(camera_time())
        self.instance.pump()
        serials = [message["sample_serial"] for message in self.session.messages
                   if message["type"] == "group"]
        self.assertEqual([1, 2], serials)

    def test_advancing_desired_never_cancels_the_issued_target(self):
        first = camera_time(eval_serial=1)
        second = camera_time(eval_id="seq@11+0/cam#c2", eval_serial=2, display_frame=11.0)
        third = camera_time(eval_id="seq@12+0/cam#c3", eval_serial=3, display_frame=12.0)
        self.instance.offer_target(first)
        self.assertEqual(1, self.instance.pump())
        # While the first target is in flight, later desires only replace the
        # waiting slot; they never cancel the issued work.
        self.instance.offer_target(second)
        self.instance.offer_target(third)
        self.assertEqual(1, self.instance.pump())
        groups = [message for message in self.session.messages
                  if message["type"] == "group"]
        self.assertEqual(["seq@10+0/cam#c1", "seq@12+0/cam#c3"],
                         [message["eval_id"] for message in groups])
        # The lagging first result is still applied and acknowledged.
        self.instance.note_applied(applied_message(sample_serial=1))
        self.assertEqual("seq@10+0/cam#c1", self.instance.status().applied_eval_id)

    def test_older_target_than_the_issued_one_is_refused(self):
        self.instance.offer_target(camera_time(eval_serial=5))
        self.instance.pump()
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            self.instance.offer_target(camera_time(eval_serial=4))
        self.assertEqual("EVAL_STALE", caught.exception.code)

    def test_apply_failure_is_reported_without_sending_a_group(self):
        self.host.apply_error = RuntimeError("camera attribute is locked")
        self.instance.offer_target(camera_time())
        self.assertEqual(0, self.instance.pump())
        self.assertEqual([], [message for message in self.session.messages
                              if message["type"] == "group"])
        self.assertEqual("failed", self.events[-1].kind)
        self.assertEqual("LINKAGE_APPLY_FAILED", self.events[-1].diagnostic["code"])

    def test_oversized_group_is_refused_before_it_is_sent(self):
        # The frozen budget is 32 MiB; the test lowers the module constant
        # instead of allocating a real oversized group.
        instance, _, session, events = build()
        instance.enter(1)
        instance.offer_target(camera_time())
        with mock.patch.object(mtou_linkage, "MAX_LINKAGE_GROUP_BYTES", 64):
            with self.assertRaises(mtou_linkage.LinkageError) as caught:
                instance.pump()
        self.assertEqual("RESOURCE_LIMIT", caught.exception.code)
        self.assertEqual([], [message for message in session.messages
                              if message["type"] == "group"])
        self.assertEqual("RESOURCE_LIMIT", events[-1].diagnostic["code"])
        self.assertFalse(instance.settled())


class LinkageEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.instance, self.host, self.session, self.events = build()
        self.instance.enter(1)

    def test_applied_evidence_marks_convergence(self):
        self.instance.offer_target(camera_time())
        self.instance.pump()
        self.instance.note_applied(applied_message())
        status = self.instance.status()
        self.assertTrue(status.converged)
        self.assertEqual(1, status.applied_count)
        self.assertEqual("seq@10+0/cam#c1", status.applied_eval_id)
        self.assertTrue(self.instance.settled())
        self.assertEqual("applied", self.events[-1].kind)

    def test_failed_application_keeps_the_session_unsynchronised(self):
        self.instance.offer_target(camera_time())
        self.instance.pump()
        self.instance.note_applied(applied_message(applied=False, reason="writer busy"))
        status = self.instance.status()
        self.assertFalse(status.converged)
        self.assertEqual(0, status.applied_count)
        self.assertIn("writer busy", status.detail)

    def test_applied_evidence_must_match_the_session_identity(self):
        message = applied_message(session_id=9)
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            self.instance.note_applied(message)
        self.assertEqual("INVALID_MESSAGE", caught.exception.code)
        message = applied_message(mode_revision=4)
        with self.assertRaises(mtou_linkage.LinkageError) as caught:
            self.instance.note_applied(message)
        self.assertEqual("MODE_CONFLICT", caught.exception.code)

    def test_waiting_target_reports_an_overdue_convergence(self):
        instance, host, session, events = build()
        clock = {"now": 0.0}
        instance = mtou_linkage.LinkageSession(
            host, session, on_event=events.append, clock=lambda: clock["now"])
        instance.set_session_id(5)
        instance.enter(1)
        instance.offer_target(camera_time())
        instance.pump()
        self.assertFalse(instance.convergence_overdue())
        clock["now"] = mtou_config.LINKAGE_CONVERGENCE_SECONDS + 0.1
        self.assertTrue(instance.convergence_overdue())
        self.assertAlmostEqual(
            mtou_config.LINKAGE_CONVERGENCE_SECONDS + 0.1, instance.wait_seconds())

    def test_mode_state_confirms_the_linkage_maya_started(self):
        revision = self.instance.mode_revision()
        outcome = self.instance.note_mode_state(mode_state(revision))
        self.assertEqual("confirmed", outcome)
        self.assertEqual(revision, self.instance.mode_revision())
        self.assertTrue(self.instance.is_active)
        self.assertFalse(self.instance.confirmation_overdue())


class RemoteTransitionTests(unittest.TestCase):
    """Unreal's mode_state is followed, not merely recorded."""

    def test_unreal_started_linkage_is_entered_and_confirmed(self):
        instance, host, session, events = build()
        self.assertEqual("enter", instance.note_mode_state(mode_state(1)))
        # Nothing changes until the caller prepared the session and entered.
        self.assertFalse(instance.is_active)
        self.assertEqual(0, host.captured)
        instance.enter(1, remote=True)
        self.assertTrue(instance.is_active)
        self.assertEqual(1, host.captured)
        self.assertEqual(
            [("mode", "linkage", 1, "ready")],
            [(m["type"], m["mode"], m["mode_revision"], m["transition"])
             for m in session.messages])
        # Unreal echoes Maya's answer; the echo confirms the linkage.
        self.assertEqual("confirmed", instance.note_mode_state(mode_state(1)))
        self.assertTrue(instance.is_active)

    def test_unreal_failing_its_own_transition_rolls_maya_back(self):
        # Unreal timed out before Maya's answer arrived: its realtime statement
        # at the same revision fails the transition Maya has not seen echoed.
        instance, host, session, events = build()
        instance.enter(1, remote=True)
        self.assertEqual(
            "left", instance.note_mode_state(mode_state(1, mode=mtou_config.REALTIME_MODE)))
        self.assertFalse(instance.is_active)
        self.assertEqual(1, len(host.restored))
        self.assertFalse(instance.takeover_held())

    def test_a_stale_reference_refuses_unreals_transition_before_maya_changes(self):
        instance, host, session, events = build()
        host.stale = "the reference was placed for another anchor"
        moved = {"location_cm": [60.0, 40.0, 0.0],
                 "rotation_deg": {"roll": 3.0, "pitch": -4.0, "yaw": 37.0}}
        self.assertEqual("enter", instance.note_mode_state(mode_state(1, anchor=moved)))
        with self.assertRaises(mtou_linkage.LinkageError) as raised:
            instance.enter(1, remote=True, anchor=moved)
        self.assertEqual("REFERENCE_STALE", raised.exception.code)
        self.assertEqual([moved], host.anchors)
        self.assertEqual(0, host.captured)
        self.assertFalse(instance.is_active)
        # Unreal hears the failed transition and hands its writers back.
        self.assertEqual(
            [("mode", "realtime", 1, "failed")],
            [(m["type"], m["mode"], m["mode_revision"], m["transition"])
             for m in session.messages])
        self.assertEqual("REFERENCE_STALE", events[-1].diagnostic["code"])

    def test_a_stale_reference_fails_the_linkage_maya_started_at_confirmation(self):
        instance, host, session, events = build()
        instance.enter(2)
        host.stale = "the reference was placed for another anchor"
        self.assertEqual("refused", instance.note_mode_state(mode_state(2)))
        self.assertFalse(instance.is_active)
        self.assertEqual(1, len(host.restored))
        self.assertFalse(instance.takeover_held())
        self.assertEqual(
            [("mode", "linkage", "ready"), ("mode", "realtime", "failed")],
            [(m["type"], m["mode"], m["transition"]) for m in session.messages])
        self.assertEqual("REFERENCE_STALE", events[-1].diagnostic["code"])

    def test_a_current_reference_lets_the_confirmation_through(self):
        instance, host, session, events = build()
        instance.enter(2)
        self.assertEqual("confirmed", instance.note_mode_state(mode_state(2)))
        self.assertEqual([IDENTITY_ANCHOR], host.anchors)
        self.assertTrue(instance.is_active)

    def test_an_echo_after_the_confirmation_checks_nothing_again(self):
        instance, host, session, events = build()
        instance.enter(2)
        instance.note_mode_state(mode_state(2))
        host.stale = "moved after the confirmation"
        self.assertEqual("noted", instance.note_mode_state(mode_state(1)))
        self.assertTrue(instance.is_active)
        self.assertEqual(1, len(host.anchors))

    def test_a_stale_realtime_statement_keeps_the_confirmed_linkage(self):
        instance, host, session, events = build()
        instance.enter(3)
        self.assertEqual("confirmed", instance.note_mode_state(mode_state(3)))
        outcome = instance.note_mode_state(mode_state(2, mode=mtou_config.REALTIME_MODE))
        self.assertEqual("noted", outcome)
        self.assertTrue(instance.is_active)
        self.assertEqual(3, instance.mode_revision())
        self.assertEqual([], host.restored)

    def test_unreal_started_linkage_announces_a_capture_refusal(self):
        instance, host, session, events = build()
        host.capture_state = mock.Mock(side_effect=RuntimeError("panel locked"))
        with self.assertRaises(RuntimeError):
            instance.enter(1, remote=True)
        self.assertFalse(instance.is_active)
        self.assertEqual(
            [("mode", "realtime", 1, "failed")],
            [(m["type"], m["mode"], m["mode_revision"], m["transition"])
             for m in session.messages])
        self.assertIn("panel locked", session.messages[0]["detail"])

    def test_a_stale_linkage_statement_is_only_noted(self):
        instance, host, session, events = build()
        instance.enter(1)
        instance.leave("realtime", 2)
        self.assertEqual("noted", instance.note_mode_state(mode_state(1)))
        self.assertFalse(instance.is_active)
        self.assertEqual(1, host.captured)

    def test_unreal_leave_restores_maya_and_keeps_the_takeover_endable(self):
        instance, host, session, events = build()
        instance.enter(1)
        instance.note_mode_state(mode_state(1))
        session.messages[:] = []
        outcome = instance.note_mode_state(
            mode_state(2, mode=mtou_config.REALTIME_MODE, detail="UE returned"))
        self.assertEqual("left", outcome)
        self.assertFalse(instance.is_active)
        self.assertEqual(mtou_config.CONTROL_MAYA, instance.status().control)
        self.assertEqual(2, instance.mode_revision())
        self.assertEqual(1, len(host.restored))
        # Unreal already moved; Maya does not answer with a second exit.
        self.assertEqual([], session.messages)
        self.assertTrue(instance.takeover_held())
        # 结束预览 still hands the writers back and then clears the hold.
        instance.leave("end", 3)
        self.assertFalse(instance.takeover_held())
        self.assertEqual("leave_linkage", session.messages[-1]["type"])
        self.assertEqual("end", session.messages[-1]["target"])

    def test_unreal_refusing_maya_entry_rolls_maya_back(self):
        instance, host, session, events = build()
        instance.enter(1)
        outcome = instance.note_mode_state(
            mode_state(0, mode=mtou_config.REALTIME_MODE, detail="PAIRING_REQUIRED"))
        self.assertEqual("left", outcome)
        self.assertFalse(instance.is_active)
        self.assertEqual(1, len(host.restored))
        # The refused revision is Unreal's, so the next entry is newer again.
        self.assertEqual(0, instance.mode_revision())
        self.assertFalse(instance.takeover_held())
        self.assertIn("PAIRING_REQUIRED", instance.status().detail)

    def test_restore_residual_of_a_remote_exit_is_recorded(self):
        instance, host, session, events = build()
        host.restore_residual = ("camera: cannot delete locked node",)
        instance.enter(1)
        instance.note_mode_state(mode_state(2, mode=mtou_config.REALTIME_MODE))
        self.assertIn("residual: camera: cannot delete locked node",
                      instance.recovery_records())
        self.assertEqual("RECOVERY_REFUSED", events[-1].diagnostic["code"])

    def test_unconfirmed_entry_rolls_back_and_announces_failure(self):
        host, session, events = FakeHost(), FakeSession(), []
        clock = {"now": 0.0}
        instance = mtou_linkage.LinkageSession(
            host, session, on_event=events.append, clock=lambda: clock["now"])
        instance.set_session_id(5)
        instance.enter(1)
        self.assertFalse(instance.confirmation_overdue())
        clock["now"] = mtou_config.LINKAGE_MODE_CONFIRMATION_SECONDS + 0.1
        self.assertTrue(instance.confirmation_overdue())
        self.assertEqual((), instance.expire_unconfirmed())
        self.assertFalse(instance.is_active)
        self.assertEqual(1, len(host.restored))
        failed = session.messages[-1]
        self.assertEqual(("mode", "failed", 1),
                         (failed["type"], failed["transition"], failed["mode_revision"]))
        self.assertIsNone(instance.expire_unconfirmed())

    def test_a_later_session_continues_the_connection_revision(self):
        instance = mtou_linkage.LinkageSession(
            FakeHost(), FakeSession(), mode_revision=4)
        self.assertEqual(4, instance.mode_revision())


class LinkageLifecycleTests(unittest.TestCase):
    def test_close_restores_once_and_is_idempotent(self):
        instance, host, _, _ = build()
        instance.enter(1)
        first = instance.close()
        second = instance.close()
        self.assertEqual((), first)
        self.assertEqual((), second)
        self.assertEqual(1, len(host.restored))
        self.assertFalse(instance.is_active)

    def test_close_after_leave_does_not_restore_twice(self):
        instance, host, _, _ = build()
        instance.enter(1)
        instance.leave("end", 2)
        instance.close()
        self.assertEqual(1, len(host.restored))

    def test_status_reports_the_control_owner_and_targets(self):
        instance, _, _, _ = build()
        status = instance.status()
        self.assertEqual(mtou_config.REALTIME_MODE, status.mode)
        self.assertEqual(mtou_config.CONTROL_MAYA, status.control)
        self.assertEqual(0, status.issued)
        instance.enter(1)
        instance.offer_target(camera_time())
        instance.pump()
        status = instance.status()
        self.assertEqual(mtou_config.LINKAGE_MODE, status.mode)
        self.assertEqual(mtou_config.CONTROL_UNREAL, status.control)
        self.assertEqual("seq@10+0/cam#c1", status.target)
        self.assertEqual(10.0, status.target_display_frame)
        self.assertEqual(0, status.pending)
        self.assertFalse(status.converged)


class LinkageProtocolTests(unittest.TestCase):
    def test_group_message_meters_the_whole_group(self):
        message = mtou_protocol.make_group_message(
            5, 1, 1, 1, "seq@1+0/cam#c", 1, 1,
            [{"object_id": "character",
              "transforms": [[0, 0, 0, 0, 0, 0, 1, 1, 1, 1]], "curves": [0.5]}],
            {"world_matrix": [1.0]}, {"maya_frame": 1.0, "ue_display_frame": 1.0})
        size = mtou_protocol.group_wire_size(message)
        self.assertGreater(size, 0)
        self.assertLess(size, mtou_config.MAX_LINKAGE_GROUP_BYTES)
        self.assertEqual(size, len(mtou_protocol.encode_message(message)) - 8)

    def test_mode_and_leave_messages_validate_their_arguments(self):
        with self.assertRaises(ValueError):
            mtou_protocol.make_mode_message(5, 1, "preview", "unreal", "ready")
        with self.assertRaises(ValueError):
            mtou_protocol.make_mode_message(5, 1, mtou_config.LINKAGE_MODE,
                                            "engine", "ready")
        with self.assertRaises(ValueError):
            mtou_protocol.make_mode_message(5, 1, mtou_config.LINKAGE_MODE,
                                            "unreal", "done")
        with self.assertRaises(ValueError):
            mtou_protocol.make_leave_linkage_message(5, 1, "pause")

    def test_hello_message_declares_units_and_capabilities(self):
        message = mtou_protocol.make_hello_message(
            "2024", "cm", 1.0, 24.0)
        self.assertEqual(mtou_config.PROTOCOL_VERSION, message["version"])
        self.assertEqual("cm", message["scene"]["linear_unit"])
        self.assertTrue(message["capabilities"]["linkage"])
        with self.assertRaises(ValueError):
            mtou_protocol.make_hello_message("2024", "cm", 0.0, 24.0)
        with self.assertRaises(ValueError):
            mtou_protocol.make_hello_message("2024", "cm", 1.0, float("inf"))


if __name__ == "__main__":
    unittest.main()
