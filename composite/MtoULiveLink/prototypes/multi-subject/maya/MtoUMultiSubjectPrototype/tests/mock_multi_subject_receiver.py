"""Stand-in Unreal receiver for the bounded two-subject prototype.

It speaks the real wire protocol (newline JSON over TCP) and stands in for the
Unreal adapter when no editor is running: it verifies the declared skeletons
against a fixture manifest exactly like a strict target check would, answers
``ready``/``applied``/``error``, disables one subject on ``remove`` and clears
both on disconnect. Fault injection turns it into a negative-test peer for the
Maya side (wrong session, wrong serial, wrong time, silence, refusal).

Standard library only; runs under CPython and under ``mayapy``.

    python mock_multi_subject_receiver.py --manifest fixture.json --port 0
"""

import argparse
import json
import socket
import sys
import threading
import time
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import mtou_multi_subject_protocol as protocol  # noqa: E402

#: Receiver-facing codes, the same vocabulary the Unreal adapter documents.
CODE_VERSION_UNSUPPORTED = "version_unsupported"
CODE_SUBJECTS_SHAPE = "subjects_shape"
CODE_SUBJECT_DUPLICATE_ID = "subject_duplicate_id"
CODE_UNKNOWN_SUBJECT_ID = "unknown_subject_id"
CODE_SKELETON_MISMATCH = "skeleton_mismatch"
CODE_BONE_PARENT_MISMATCH = "bone_parent_mismatch"
CODE_CURVE_NOT_IN_TARGET = "curve_not_in_target"
CODE_NO_SESSION = "no_session"
CODE_SHARED_TARGET = "shared_target"
CODE_SESSION_MISMATCH = "session_mismatch"
CODE_SUBJECT_MISSING = "subject_missing"
CODE_MALFORMED_JSON = "malformed_json"
CODE_TOO_LARGE = "too_large"
CODE_INJECTED = "injected_fault"

_CODE_MAP = {
    protocol.CODE_VERSION_UNSUPPORTED: CODE_VERSION_UNSUPPORTED,
    protocol.CODE_SUBJECT_COUNT: CODE_SUBJECTS_SHAPE,
    protocol.CODE_SUBJECT_ID_INVALID: CODE_SUBJECTS_SHAPE,
    protocol.CODE_SUBJECT_ID_DUPLICATE: CODE_SUBJECT_DUPLICATE_ID,
    protocol.CODE_BONES_EMPTY: CODE_SKELETON_MISMATCH,
    protocol.CODE_BONE_NAME_INVALID: CODE_SKELETON_MISMATCH,
    protocol.CODE_BONE_NAME_DUPLICATE: CODE_SKELETON_MISMATCH,
    protocol.CODE_BONE_PARENT_INVALID: CODE_BONE_PARENT_MISMATCH,
    protocol.CODE_CURVE_NAME_INVALID: CODE_CURVE_NOT_IN_TARGET,
    protocol.CODE_CURVE_NAME_DUPLICATE: CODE_CURVE_NOT_IN_TARGET,
    protocol.CODE_BIND_COUNT: CODE_SKELETON_MISMATCH,
    protocol.CODE_BIND_ROW_INVALID: CODE_SKELETON_MISMATCH,
    protocol.CODE_BIND_QUATERNION_INVALID: CODE_SKELETON_MISMATCH,
    protocol.CODE_TRANSFORM_COUNT: CODE_SUBJECTS_SHAPE,
    protocol.CODE_TRANSFORM_ROW_INVALID: CODE_SUBJECTS_SHAPE,
    protocol.CODE_CURVE_COUNT: CODE_CURVE_NOT_IN_TARGET,
    protocol.CODE_SERIAL_INVALID: CODE_SUBJECTS_SHAPE,
    protocol.CODE_SERIAL_NOT_INCREASING: CODE_SUBJECTS_SHAPE,
    protocol.CODE_TIME_INVALID: CODE_SUBJECTS_SHAPE,
    protocol.CODE_INVALID_JSON: CODE_MALFORMED_JSON,
    protocol.CODE_NOT_AN_OBJECT: CODE_MALFORMED_JSON,
    protocol.CODE_LINE_TOO_LONG: CODE_TOO_LARGE,
    protocol.CODE_REMOVE_ID_INACTIVE: CODE_UNKNOWN_SUBJECT_ID,
}


def receiver_code(code):
    return _CODE_MAP.get(code, code)


def load_fingerprints(path):
    """Read ``fingerprints`` from a fixture manifest (or a bare mapping)."""
    with open(str(path), "r", encoding="utf-8") as stream:
        data = json.load(stream)
    return data.get("fingerprints", data)


class MockReceiver(object):
    """One-connection-at-a-time fake Unreal receiver with fault injection."""

    def __init__(self, fingerprints=None, faults=None, host="127.0.0.1", port=0,
                 log_path=None):
        self.fingerprints = fingerprints
        self.faults = dict(faults or {})
        self.host = host
        self.port = int(port)
        self.log_path = str(log_path) if log_path else None
        self._server = None
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self.records = {"received": [], "replies": [], "events": []}
        self.sessions = 0
        self.clients = 0

    # ------------------------------------------------------------------- life

    def start(self):
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind((self.host, self.port))
        self._server.listen(4)
        self._server.settimeout(0.2)
        self.port = self._server.getsockname()[1]
        self._thread = threading.Thread(target=self._serve_forever, name="mock-receiver")
        self._thread.daemon = True
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
        if self._server is not None:
            self._server.close()
            self._server = None
        return self

    def __enter__(self):
        return self.start()

    def __exit__(self, *unused):
        self.stop()
        return False

    # ---------------------------------------------------------------- records

    def _record(self, bucket, payload):
        with self._lock:
            entry = dict(payload)
            entry["at"] = time.monotonic()
            self.records[bucket].append(entry)
        return entry

    def received_messages(self):
        with self._lock:
            return [entry["message"] for entry in self.records["received"]]

    def replied_messages(self):
        with self._lock:
            return [entry["message"] for entry in self.records["replies"]]

    def events(self):
        with self._lock:
            return [entry["event"] for entry in self.records["events"]]

    def records_snapshot(self):
        """Deep copy of every record, safe to serialise while serving."""
        with self._lock:
            return json.loads(json.dumps(self.records, ensure_ascii=True))

    def write_log(self, path):
        """Write the records to ``path`` (best effort, for standalone runs)."""
        with open(str(path), "w", encoding="utf-8") as stream:
            json.dump(self.records_snapshot(), stream, indent=2, ensure_ascii=True)
            stream.write("\n")
        return path

    # ------------------------------------------------------------------ serve

    def _serve_forever(self):
        while not self._stop.is_set():
            try:
                connection, _ = self._server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.clients += 1
            self._record("events", {"event": "connect", "client": self.clients})
            try:
                self._serve_client(connection)
            finally:
                try:
                    connection.close()
                except OSError:
                    pass
                self._record("events", {"event": "disconnect",
                                        "cleared_subjects": True})
                if self.log_path:
                    # Written as soon as a client leaves, so an externally
                    # killed stand-in still has the receiver-side record.
                    self.write_log(self.log_path)

    def _serve_client(self, connection):
        connection.settimeout(0.2)
        framer = protocol.LineFramer()
        state = {
            "session": None,
            "summary": None,
            "last_serial": None,
            "last_time": None,
            "frames": 0,
            "refused_inits": 0,
        }
        while not self._stop.is_set():
            try:
                data = connection.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            try:
                framer.feed(data)
            except protocol.ProtocolError as error:
                self._reply(connection, {"type": "error",
                                         "code": receiver_code(error.code),
                                         "details": error.details})
                return
            for line in framer.take_lines():
                try:
                    message = protocol.parse_message(line)
                except protocol.ProtocolError as error:
                    self._record("received", {"message": line, "error": error.code})
                    self._reply(connection, {"type": "error",
                                             "code": receiver_code(error.code),
                                             "details": error.details})
                    if error.code in (protocol.CODE_INVALID_JSON,
                                      protocol.CODE_LINE_TOO_LONG,
                                      protocol.CODE_NOT_AN_OBJECT):
                        return
                    continue
                self._record("received", {"message": message})
                if message["type"] == protocol.INIT:
                    self._handle_init(connection, message, state)
                elif message["type"] == protocol.FRAME:
                    if not self._handle_frame(connection, message, state):
                        return
                elif message["type"] == protocol.REMOVE:
                    self._handle_remove(connection, message, state)
                else:
                    self._reply(connection, {"type": "error",
                                             "code": CODE_SUBJECTS_SHAPE,
                                             "details": "clients send init/frame/remove"})

    # ------------------------------------------------------------------ init

    def _handle_init(self, connection, message, state):
        faults = self.faults
        if state["session"] is not None and not faults.get("allow_reinit", True):
            self._reply(connection, {"type": "error", "code": CODE_UNKNOWN_SUBJECT_ID,
                                     "details": "a connection negotiates once"})
            return
        try:
            summary = protocol.validate_init(message)
        except protocol.ProtocolError as error:
            state["refused_inits"] += 1
            self._reply(connection, {"type": "error", "code": receiver_code(error.code),
                                     "details": error.details})
            return
        mismatch = self._fingerprint_mismatch(message, summary)
        if mismatch is None:
            roots = [subject.get("root") for subject in message["subjects"]]
            if len(set(roots)) != len(roots):
                mismatch = (CODE_SHARED_TARGET,
                            "two subjects cannot share one target root: {0}".format(roots))
        if mismatch is None and faults.get("refuse_inits"):
            mismatch = (CODE_INJECTED, "fault injection refused the init")
        if mismatch is not None:
            state["refused_inits"] += 1
            self._reply(connection, {"type": "error", "code": mismatch[0],
                                     "details": mismatch[1]})
            return
        self.sessions += 1
        state["session"] = self.sessions
        state["summary"] = summary
        state["last_serial"] = None
        state["last_time"] = None
        self._record("events", {"event": "negotiated", "session": self.sessions,
                                "subjects": summary["ids"]})
        self._reply(connection, {"type": "ready", "session": self.sessions})

    def _fingerprint_mismatch(self, message, summary):
        """Compare the declared subjects against the configured targets."""
        if not self.fingerprints:
            return None
        for subject in message["subjects"]:
            expected = self.fingerprints.get(subject["id"])
            if expected is None:
                return (CODE_SKELETON_MISMATCH,
                        "no target for subject {0!r}".format(subject["id"]))
            bones = [bone["name"] for bone in subject["bones"]]
            parents = [int(bone["parent"]) for bone in subject["bones"]]
            if bones != list(expected["bones"]):
                return (CODE_SKELETON_MISMATCH,
                        "{0} bones {1} are not the target skeleton".format(
                            subject["id"], bones))
            if parents != [int(value) for value in expected["parents"]]:
                return (CODE_BONE_PARENT_MISMATCH,
                        "{0} parents {1} are not the target hierarchy".format(
                            subject["id"], parents))
            if list(subject["curves"]) != list(expected["curves"]):
                return (CODE_CURVE_NOT_IN_TARGET,
                        "{0} curves {1} are not the target Morph set".format(
                            subject["id"], subject["curves"]))
        return None

    # ----------------------------------------------------------------- frames

    def _handle_frame(self, connection, message, state):
        faults = self.faults
        summary = state["summary"]
        if summary is None:
            self._reply(connection, {"type": "error", "code": CODE_NO_SESSION,
                                     "details": "init before frame"})
            return True
        ids = [subject.get("id") for subject in message.get("subjects") or []
               if isinstance(subject, dict)]
        unknown = [subject_id for subject_id in ids
                   if subject_id not in summary["ids"]]
        if unknown:
            self._reply(connection, {"type": "error", "code": CODE_UNKNOWN_SUBJECT_ID,
                                     "details": "unknown subject {0}".format(unknown)})
            return True
        missing = [subject_id for subject_id in protocol.active_subject_ids(summary)
                   if subject_id not in ids]
        if missing:
            self._reply(connection, {"type": "error", "code": CODE_SUBJECT_MISSING,
                                     "details": "missing subject {0}".format(missing)})
            return True
        if message.get("session") != state["session"]:
            self._reply(connection, {"type": "error", "code": CODE_SESSION_MISMATCH,
                                     "details": "frame session {0!r} is not the negotiated "
                                                "session {1!r}".format(
                                                message.get("session"), state["session"])})
            return True
        try:
            protocol.validate_frame(message, summary, session=state["session"],
                                    previous_serial=state["last_serial"])
        except protocol.ProtocolError as error:
            self._reply(connection, {"type": "error", "code": receiver_code(error.code),
                                     "details": error.details})
            return True
        silence = faults.get("silence_frames") or ()
        if message["serial"] in silence:
            return True
        state["last_serial"] = message["serial"]
        state["last_time"] = float(message["time"])
        state["frames"] += 1
        reply = {
            "type": "applied",
            "session": state["session"] + int(faults.get("session_offset", 0)),
            "serial": message["serial"] + int(faults.get("serial_offset", 0)),
            "time": float(message["time"]) + float(faults.get("time_offset", 0.0)),
            "subjects": [{"id": subject_id, "status": protocol.STATUS_APPLIED}
                         for subject_id in protocol.active_subject_ids(summary)],
        }
        self._record("events", {"event": "applied", "serial": message["serial"],
                                "time": message["time"]})
        self._reply(connection, reply)
        if faults.get("close_after_frames") is not None \
                and state["frames"] >= int(faults["close_after_frames"]):
            return False
        return True

    # ----------------------------------------------------------------- remove

    def _handle_remove(self, connection, message, state):
        summary = state["summary"]
        if summary is None:
            self._reply(connection, {"type": "error", "code": CODE_NO_SESSION,
                                     "details": "init before remove"})
            return
        if message.get("session") != state["session"]:
            self._reply(connection, {"type": "error", "code": CODE_SESSION_MISMATCH,
                                     "details": "remove session {0!r} is not the negotiated "
                                                "session {1!r}".format(
                                                message.get("session"), state["session"])})
            return
        subject_id = message.get("id")
        if subject_id not in summary["ids"] or subject_id not in summary["active"]:
            self._reply(connection, {"type": "error", "code": CODE_UNKNOWN_SUBJECT_ID,
                                     "details": "unknown subject {0!r}".format(subject_id)})
            return
        protocol.disable_subject(summary, subject_id)
        self._record("events", {"event": "removed", "id": subject_id})
        subjects = [{"id": subject_id, "status": protocol.STATUS_DISABLED}]
        subjects.extend({"id": remaining, "status": protocol.STATUS_APPLIED}
                        for remaining in protocol.active_subject_ids(summary))
        self._reply(connection, {
            "type": "applied",
            "session": state["session"],
            "serial": state["last_serial"] if state["last_serial"] is not None else 0,
            "time": state["last_time"] if state["last_time"] is not None else 0.0,
            "subjects": subjects,
        })

    # ------------------------------------------------------------------ reply

    def _reply(self, connection, message):
        try:
            connection.sendall(protocol.encode_line(message))
        except (OSError, protocol.ProtocolError):
            return
        self._record("replies", {"message": message})


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=protocol.DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--manifest", default=None,
                        help="fixture manifest whose fingerprints are the targets")
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--log", default=None, help="write the receiver records here")
    parser.add_argument("--session-offset", type=int, default=0)
    parser.add_argument("--serial-offset", type=int, default=0)
    parser.add_argument("--time-offset", type=float, default=0.0)
    parser.add_argument("--silence-frames", default="",
                        help="comma separated frame serials to leave unanswered")
    parser.add_argument("--refuse-inits", action="store_true")
    parser.add_argument("--close-after-frames", type=int, default=None)
    args = parser.parse_args(argv)

    fingerprints = load_fingerprints(args.manifest) if args.manifest else None
    faults = {
        "session_offset": args.session_offset,
        "serial_offset": args.serial_offset,
        "time_offset": args.time_offset,
        "close_after_frames": args.close_after_frames,
        "refuse_inits": args.refuse_inits,
        "silence_frames": tuple(int(part) for part in args.silence_frames.split(",")
                                if part.strip()),
    }
    receiver = MockReceiver(fingerprints, faults, args.host, args.port,
                            log_path=args.log).start()
    print("MtoUMultiSubjectMockReceiver: listening {0}:{1}".format(
        receiver.host, receiver.port), flush=True)
    deadline = time.monotonic() + float(args.duration)
    next_flush = time.monotonic()
    try:
        while time.monotonic() < deadline:
            time.sleep(0.1)
            if args.log and time.monotonic() >= next_flush:
                # Flushed periodically so an externally killed stand-in still
                # leaves the receiver-side record behind.
                receiver.write_log(args.log)
                next_flush = time.monotonic() + 0.5
    except KeyboardInterrupt:
        pass
    finally:
        receiver.stop()
        if args.log:
            receiver.write_log(args.log)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
