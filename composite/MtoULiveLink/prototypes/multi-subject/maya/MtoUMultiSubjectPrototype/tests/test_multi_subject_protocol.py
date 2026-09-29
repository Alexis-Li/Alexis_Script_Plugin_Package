"""Pure unit tests for the two-subject wire contract.

Standard library only: these run under CPython and under ``mayapy`` without
starting Maya.

    python -m unittest discover \
        -s composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype/tests \
        -t composite/MtoULiveLink/prototypes/multi-subject/maya/MtoUMultiSubjectPrototype
"""

import importlib.util
import json
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPTS = HERE.parent / "scripts"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


protocol = load_module("mtou_multi_subject_protocol", SCRIPTS / "mtou_multi_subject_protocol.py")

ROW = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0]


def subject(subject_id, root, bones, curves=("Shared",), rows=None):
    return {
        "id": subject_id,
        "root": root,
        "bones": [{"name": name, "parent": parent} for name, parent in bones],
        "curves": list(curves),
        "bind": rows if rows is not None else [list(ROW) for _ in bones],
    }


def pair():
    return [
        subject("character", "|character:Group|character:Root",
                [("Root", -1), ("Spine", 0)]),
        subject("prop", "|prop:Root", [("Root", -1), ("PropBody", 0), ("PropTip", 1)]),
    ]


class MessageBuildingTests(unittest.TestCase):
    def test_init_round_trip(self):
        message = protocol.make_init(pair(), 30.0)
        self.assertEqual(message["type"], "init")
        self.assertEqual(message["version"], 1)
        summary = protocol.validate_init(message)
        self.assertEqual(summary["ids"], ["character", "prop"])
        self.assertEqual(summary["bones"]["prop"], ["Root", "PropBody", "PropTip"])
        self.assertEqual(summary["parents"]["prop"], [-1, 0, 1])
        self.assertEqual(summary["rows"]["character"], 2)
        self.assertEqual(summary["active"], ["character", "prop"])

    def test_frame_round_trip_and_echo_validation(self):
        init = protocol.make_init(pair(), 30.0)
        summary = protocol.validate_init(init)
        frame = protocol.make_frame(1, 2.5, [
            {"id": "character", "transforms": [list(ROW), list(ROW)], "curves": [0.5]},
            {"id": "prop", "transforms": [list(ROW)] * 3, "curves": [1.0]},
        ])
        validated = protocol.validate_frame(frame, summary)
        self.assertEqual(validated, {"serial": 1, "time": 2.5,
                                     "ids": ["character", "prop"]})
        echo = {"type": "applied", "session": 7, "serial": 1, "time": 2.5,
                "subjects": [{"id": "character", "status": "applied"},
                             {"id": "prop", "status": "applied"}]}
        statuses = protocol.validate_applied(echo, 7, 1, 2.5,
                                             protocol.active_subject_ids(summary))
        self.assertEqual(statuses, {"character": "applied", "prop": "applied"})

    def test_line_framing_is_bounded_and_line_oriented(self):
        framer = protocol.LineFramer(max_line_bytes=64)
        framer.feed(b'{"type":"init"}\n{"ty')
        self.assertEqual(framer.take_lines(), ['{"type":"init"}'])
        self.assertEqual(framer.pending_bytes, len(b'{"ty'))
        framer.feed(b'pe":"frame"}\n')
        self.assertEqual(framer.take_lines(), ['{"type":"frame"}'])
        with self.assertRaises(protocol.ProtocolError) as caught:
            framer.feed(b"x" * 65)
        self.assertEqual(caught.exception.code, protocol.CODE_LINE_TOO_LONG)

    def test_encode_line_rejects_oversized_and_non_finite(self):
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.encode_line({"type": "frame", "value": float("nan")})
        self.assertEqual(caught.exception.code, protocol.CODE_NOT_AN_OBJECT)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.encode_line({"type": "init", "pad": "x" * protocol.MAX_LINE_BYTES})
        self.assertEqual(caught.exception.code, protocol.CODE_LINE_TOO_LONG)


class InitValidationTests(unittest.TestCase):
    def _refuse(self, subjects, code, fps=30.0):
        message = {"type": "init", "version": 1, "fps": fps, "subjects": subjects}
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_init(message)
        self.assertEqual(caught.exception.code, code)
        return caught.exception

    def test_subject_count_and_duplicate_ids(self):
        self._refuse(pair()[:1], protocol.CODE_SUBJECT_COUNT)
        duplicated = pair()
        duplicated[1]["id"] = "character"
        self._refuse(duplicated, protocol.CODE_SUBJECT_ID_DUPLICATE)
        invalid = pair()
        invalid[0]["id"] = "1bad"
        self._refuse(invalid, protocol.CODE_SUBJECT_ID_INVALID)

    def test_bones_parents_and_curves(self):
        broken = pair()
        broken[1]["bones"] = [{"name": "Root", "parent": -1},
                              {"name": "Root", "parent": 0},
                              {"name": "Tip", "parent": 1}]
        self._refuse(broken, protocol.CODE_BONE_NAME_DUPLICATE)
        broken = pair()
        broken[0]["bones"] = [{"name": "Root", "parent": -1},
                              {"name": "Spine", "parent": 5}]
        self._refuse(broken, protocol.CODE_BONE_PARENT_INVALID)
        broken = pair()
        broken[0]["curves"] = ["Shared", "Shared"]
        self._refuse(broken, protocol.CODE_CURVE_NAME_DUPLICATE)
        broken = pair()
        broken[0]["bones"] = []
        self._refuse(broken, protocol.CODE_BONES_EMPTY)

    def test_bind_rows_and_quaternion(self):
        broken = pair()
        broken[0]["bind"] = [list(ROW)]
        self._refuse(broken, protocol.CODE_BIND_COUNT)
        broken = pair()
        broken[0]["bind"] = [[1.0] * 9, list(ROW)]
        self._refuse(broken, protocol.CODE_BIND_ROW_INVALID)
        broken = pair()
        broken[0]["bind"] = [[0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0],
                             list(ROW)]
        self._refuse(broken, protocol.CODE_BIND_QUATERNION_INVALID)

    def test_version_and_fps(self):
        message = {"type": "init", "version": 9, "fps": 30.0, "subjects": pair()}
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_init(message)
        self.assertEqual(caught.exception.code, protocol.CODE_VERSION_UNSUPPORTED)
        message = {"type": "init", "version": 1, "fps": 0.0, "subjects": pair()}
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_init(message)
        self.assertEqual(caught.exception.code, protocol.CODE_FPS_INVALID)

    def test_root_must_be_a_non_empty_path(self):
        broken = pair()
        broken[1]["root"] = ""
        self._refuse(broken, protocol.CODE_ROOT_INVALID)


class FrameValidationTests(unittest.TestCase):
    def setUp(self):
        self.summary = protocol.validate_init(protocol.make_init(pair(), 30.0))

    def test_serial_starts_at_one(self):
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.make_frame(0, 1.0, [
                {"id": "character", "transforms": [list(ROW)] * 2, "curves": [0.0]},
                {"id": "prop", "transforms": [list(ROW)] * 3, "curves": [0.0]},
            ])
        self.assertEqual(caught.exception.code, protocol.CODE_SERIAL_INVALID)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_frame(
                {"type": "frame", "serial": 0, "time": 1.0, "subjects": []},
                self.summary)
        self.assertEqual(caught.exception.code, protocol.CODE_SERIAL_INVALID)

    def test_serial_and_time_must_increase(self):
        frame = protocol.make_frame(3, 1.0, [
            {"id": "character", "transforms": [list(ROW)] * 2, "curves": [0.0]},
            {"id": "prop", "transforms": [list(ROW)] * 3, "curves": [0.0]},
        ])
        protocol.validate_frame(frame, self.summary, previous_serial=2, previous_time=0.5)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_frame(frame, self.summary, previous_serial=3)
        self.assertEqual(caught.exception.code, protocol.CODE_SERIAL_NOT_INCREASING)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_frame(frame, self.summary, previous_time=1.0)
        self.assertEqual(caught.exception.code, protocol.CODE_TIME_NOT_INCREASING)

    def test_ids_order_and_counts(self):
        frame = protocol.make_frame(1, 1.0, [
            {"id": "prop", "transforms": [list(ROW)] * 3, "curves": [0.0]},
            {"id": "character", "transforms": [list(ROW)] * 2, "curves": [0.0]},
        ])
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_frame(frame, self.summary)
        self.assertEqual(caught.exception.code, protocol.CODE_SUBJECT_MISMATCH)
        frame = protocol.make_frame(1, 1.0, [
            {"id": "character", "transforms": [list(ROW)], "curves": [0.0]},
            {"id": "prop", "transforms": [list(ROW)] * 3, "curves": [0.0]},
        ])
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_frame(frame, self.summary)
        self.assertEqual(caught.exception.code, protocol.CODE_TRANSFORM_COUNT)
        frame = protocol.make_frame(1, 1.0, [
            {"id": "character", "transforms": [list(ROW)] * 2, "curves": []},
            {"id": "prop", "transforms": [list(ROW)] * 3, "curves": [0.0]},
        ])
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_frame(frame, self.summary)
        self.assertEqual(caught.exception.code, protocol.CODE_CURVE_COUNT)

    def test_after_remove_only_the_remaining_subject_is_expected(self):
        summary = dict(self.summary)
        summary["active"] = list(self.summary["active"])
        remaining = protocol.disable_subject(summary, "prop")
        self.assertEqual(remaining, ["character"])
        frame = protocol.make_frame(2, 2.0, [
            {"id": "character", "transforms": [list(ROW)] * 2, "curves": [0.25]},
        ])
        protocol.validate_frame(frame, summary, previous_serial=1, previous_time=1.0)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.disable_subject(summary, "character")
        self.assertEqual(caught.exception.code, protocol.CODE_REMOVE_ID_INACTIVE)


class ReplyValidationTests(unittest.TestCase):
    def test_ready_session_must_increase(self):
        self.assertEqual(protocol.validate_ready({"type": "ready", "session": 2}), 2)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_ready({"type": "ready", "session": 2}, previous_session=2)
        self.assertEqual(caught.exception.code, protocol.CODE_SESSION_INVALID)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_ready({"type": "ready", "session": 0})
        self.assertEqual(caught.exception.code, protocol.CODE_SESSION_INVALID)

    def test_error_reply_becomes_remote_error(self):
        with self.assertRaises(protocol.RemoteError) as caught:
            protocol.validate_reply_type(
                {"type": "error", "code": "skeleton_mismatch", "details": "Chest"}, "ready")
        self.assertEqual(caught.exception.code, "skeleton_mismatch")
        self.assertEqual(caught.exception.details, "Chest")

    def test_unexpected_reply_type(self):
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_reply_type({"type": "ready", "session": 1}, "applied")
        self.assertEqual(caught.exception.code, protocol.CODE_UNKNOWN_TYPE)

    def test_applied_echo_strictness(self):
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_applied({"type": "applied", "session": 1, "serial": 2,
                                       "time": 1.0,
                                       "subjects": [{"id": "character",
                                                     "status": "applied"}]},
                                      1, 1, 1.0, ["character"])
        self.assertEqual(caught.exception.code, protocol.CODE_SERIAL_MISMATCH)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_applied({"type": "applied", "session": 1, "serial": 1,
                                       "time": 1.5,
                                       "subjects": [{"id": "character",
                                                     "status": "applied"}]},
                                      1, 1, 1.0, ["character"])
        self.assertEqual(caught.exception.code, protocol.CODE_TIME_MISMATCH)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_applied({"type": "applied", "session": 2, "serial": 1,
                                       "time": 1.0,
                                       "subjects": [{"id": "character",
                                                     "status": "applied"}]},
                                      1, 1, 1.0, ["character"])
        self.assertEqual(caught.exception.code, protocol.CODE_SESSION_MISMATCH)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_applied({"type": "applied", "session": 1, "serial": 1,
                                       "time": 1.0,
                                       "subjects": [{"id": "character",
                                                     "status": "disabled"}]},
                                      1, 1, 1.0, ["character"])
        self.assertEqual(caught.exception.code, protocol.CODE_STATUS_INVALID)

    def test_remove_event_reports_the_whole_subject_set(self):
        reply = {"type": "applied", "session": 4, "serial": 3, "time": 2.5,
                 "subjects": [{"id": "character", "status": "applied"},
                              {"id": "prop", "status": "disabled"}]}
        statuses = protocol.validate_remove_reply(
            reply, 4, "prop", ["character"], last_serial=3, last_time=2.5)
        self.assertEqual(statuses, {"character": "applied", "prop": "disabled"})
        rewind = dict(reply, serial=2)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_remove_reply(rewind, 4, "prop", ["character"],
                                           last_serial=3)
        self.assertEqual(caught.exception.code, protocol.CODE_SERIAL_NOT_INCREASING)
        wrong_status = dict(reply, subjects=[
            {"id": "character", "status": "disabled"},
            {"id": "prop", "status": "disabled"}])
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_remove_reply(wrong_status, 4, "prop", ["character"])
        self.assertEqual(caught.exception.code, protocol.CODE_STATUS_INVALID)
        missing = dict(reply, subjects=[{"id": "prop", "status": "disabled"}])
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.validate_remove_reply(missing, 4, "prop", ["character"])
        self.assertEqual(caught.exception.code, protocol.CODE_SUBJECT_MISMATCH)


class ParseAndEvidenceTests(unittest.TestCase):
    def test_parse_rejects_junk_and_unknown_types(self):
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.parse_message("{not json")
        self.assertEqual(caught.exception.code, protocol.CODE_INVALID_JSON)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.parse_message("[1, 2]")
        self.assertEqual(caught.exception.code, protocol.CODE_NOT_AN_OBJECT)
        with self.assertRaises(protocol.ProtocolError) as caught:
            protocol.parse_message('{"type": "cache_begin"}')
        self.assertEqual(caught.exception.code, protocol.CODE_UNKNOWN_TYPE)

    def test_encoding_is_newline_delimited_compact_json(self):
        data = protocol.encode_line({"type": "remove", "id": "prop"})
        self.assertTrue(data.endswith(b"\n"))
        self.assertEqual(json.loads(data.decode("utf-8")),
                         {"type": "remove", "id": "prop"})
        self.assertNotIn(b" ", data)

    def test_evidence_finalisation(self):
        evidence = protocol.new_evidence("TestTool", host="127.0.0.1", port=1)
        protocol.add_check(evidence, "one", True)
        protocol.finalise_evidence(evidence)
        self.assertTrue(evidence["ok"])
        protocol.add_check(evidence, "two", False, "detail")
        protocol.add_error(evidence, "SOMETHING", "detail")
        protocol.finalise_evidence(evidence)
        self.assertFalse(evidence["ok"])
        self.assertEqual(evidence["errors"][0]["code"], "SOMETHING")


if __name__ == "__main__":
    unittest.main()
